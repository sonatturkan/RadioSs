"""Plan-bound sweeper and voice-track decisions with context invalidation.

This module stores reviewed transition data; it does not render audio. Cue and
controller APIs remain re-exported here for existing service callers.
"""

from __future__ import annotations

from datetime import datetime, timedelta
import math
from typing import Any, Mapping, Sequence
from uuid import NAMESPACE_URL, UUID, uuid4, uuid5

from .identity import Principal, require
from .readiness import readiness_at
from .storage import Conflict, Store, Transaction
from ._production_common import (ProductionError, _id, _body, _now, _sha,
                                 _version_in_station, _verified_media)

PLACEMENT_KINDS = frozenset({"sweeper", "voice_track"})
START_REFERENCES = frozenset({"from_end", "to_start"})


def _revision_in_station(store: Store, station_id: str, revision_id: str) -> tuple[dict, dict]:
    revision = store.get("plan_revisions", revision_id)
    if revision is None:
        raise ProductionError("plan revision is absent")
    plan = store.get("plans", revision["plan_id"])
    if plan is None or plan["station_id"] != station_id:
        raise ProductionError("plan revision is outside station")
    return revision, plan


def _transition_context(store: Store, station_id: str, revision_id: str,
                        from_entry_id: str, to_entry_id: str) -> tuple[dict, dict, str]:
    _revision_in_station(store, station_id, revision_id)
    entries = store.lookup("plan_entries", "revision", revision_id)
    entries.sort(key=lambda row: (row["ordinal"], row["scheduled_at"], row["_id"]))
    positions = {row["_id"]: index for index, row in enumerate(entries)}
    if from_entry_id not in positions or to_entry_id not in positions or \
            positions[to_entry_id] != positions[from_entry_id] + 1:
        raise Conflict("transition entries must be adjacent in one plan revision")
    first, second = entries[positions[from_entry_id]], entries[positions[to_entry_id]]
    if any(row.get("event_kind") != "audio" or row.get("state") != "planned" or
           not (row.get("payload") or {}).get("version_id")
           for row in (first, second)):
        raise Conflict("transition needs two planned audio entries with resolved versions")
    context = [{"entry_id": row["_id"], "revision": row["_rev"],
                "ordinal": row["ordinal"], "scheduled_at": row["scheduled_at"],
                "version_id": row["payload"]["version_id"],
                "timing": row.get("timing"), "transition": row.get("transition")}
               for row in (first, second)]
    return first, second, _sha(context)


def _transition_state(store: Store, transition: dict) -> tuple[bool, str]:
    try:
        _, _, current_hash = _transition_context(
            store, transition["station_id"], transition["plan_revision_id"],
            transition["from_entry_id"], transition["to_entry_id"])
    except (Conflict, ProductionError):
        return False, "entries_removed_or_no_longer_adjacent"
    return (current_hash == transition["context_fingerprint"],
            "current" if current_hash == transition["context_fingerprint"] else
            "plan_entry_context_changed")


def create_plan_transition(
    store: Store, principal: Principal, station_id: str | UUID,
    plan_revision_id: str | UUID, from_entry_id: str | UUID,
    to_entry_id: str | UUID, *, manual_lock: bool = False,
) -> dict:
    """Bind an explicit adjacent audio pair, with no inferred mix execution."""
    station, revision_id, first_id, second_id = (
        _id(station_id, "station_id"), _id(plan_revision_id, "plan_revision_id"),
        _id(from_entry_id, "from_entry_id"), _id(to_entry_id, "to_entry_id"))
    sequence = store.sequence
    require(store, principal, "edit", station)
    revision, _ = _revision_in_station(store, station, revision_id)
    if revision["status"] != "draft":
        raise Conflict("transition can only be configured in a draft plan")
    if not isinstance(manual_lock, bool):
        raise ProductionError("manual_lock must be boolean")
    first, second, fingerprint = _transition_context(
        store, station, revision_id, first_id, second_id)
    transition_id = str(uuid5(NAMESPACE_URL,
                              f"sdbase-transition:{revision_id}:{first_id}:{second_id}"))
    existing = store.get("plan_transitions", transition_id)
    if existing:
        if existing["context_fingerprint"] == fingerprint and \
                existing["status"] == "draft" and existing["manual_lock"] == manual_lock:
            return existing
        raise Conflict("transition already exists with an old context; rebind it explicitly")
    body = {"station_id": station, "plan_revision_id": revision_id,
            "from_entry_id": first_id, "to_entry_id": second_id,
            "from_version_id": first["payload"]["version_id"],
            "to_version_id": second["payload"]["version_id"],
            "context_fingerprint": fingerprint, "status": "draft",
            "manual_lock": manual_lock, "created_by": principal.user_id,
            "created_at": _now()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.expect("plan_revisions", revision_id, revision["_rev"])
    tx.expect("plan_entries", first_id, first["_rev"])
    tx.expect("plan_entries", second_id, second["_rev"])
    tx.put("plan_transitions", transition_id, body)
    tx.commit()
    return store.get("plan_transitions", transition_id)


def _ducking(points: Sequence[Mapping[str, Any]] | None) -> list[dict]:
    if points is None:
        return []
    if isinstance(points, (str, bytes)) or not isinstance(points, Sequence) or len(points) > 128:
        raise ProductionError("ducking envelope must be a bounded list")
    clean = []
    last_ms = -1
    for point in points:
        if not isinstance(point, Mapping):
            raise ProductionError("ducking point must be an object")
        offset, gain = point.get("offset_ms"), point.get("gain_db")
        if isinstance(offset, bool) or not isinstance(offset, int) or offset <= last_ms or offset < 0 or \
                offset > 24 * 60 * 60 * 1000 or isinstance(gain, bool) or \
                not isinstance(gain, (int, float)) or not math.isfinite(gain) or \
                not -90 <= gain <= 0:
            raise ProductionError("ducking times must increase and gains must be -90..0 dB")
        clean.append({"offset_ms": offset, "gain_db": float(gain)})
        last_ms = offset
    return clean


def create_production_placement(
    store: Store, principal: Principal, station_id: str | UUID,
    transition_id: str | UUID, content_version_id: str | UUID, *,
    kind: str, anchor_entry_id: str | UUID,
    start_reference: str, offset_ms: int,
    ducking_envelope: Sequence[Mapping[str, Any]] | None = None,
    cue_id: str | UUID | None = None, manual_lock: bool = False,
) -> dict:
    """Create a draft sweeper/voice-track placement for one concrete pair."""
    station, transition_id, version_id, anchor = (
        _id(station_id, "station_id"), _id(transition_id, "transition_id"),
        _id(content_version_id, "content_version_id"),
        _id(anchor_entry_id, "anchor_entry_id"))
    sequence = store.sequence
    require(store, principal, "edit", station)
    transition = store.get("plan_transitions", transition_id)
    if transition is None or transition["station_id"] != station:
        raise ProductionError("transition is outside station")
    revision, _ = _revision_in_station(store, station, transition["plan_revision_id"])
    current, reason = _transition_state(store, transition)
    if revision["status"] != "draft" or not current or transition["status"] != "draft":
        raise Conflict(f"transition is not an editable current draft: {reason}")
    if kind not in PLACEMENT_KINDS or start_reference not in START_REFERENCES or \
            anchor not in {transition["from_entry_id"], transition["to_entry_id"]}:
        raise ProductionError("placement kind, start reference or anchor is invalid")
    expected_anchor = (transition["from_entry_id"] if start_reference == "from_end"
                       else transition["to_entry_id"])
    if anchor != expected_anchor:
        raise ProductionError("placement anchor must match its start reference")
    if isinstance(offset_ms, bool) or not isinstance(offset_ms, int) or abs(offset_ms) > 24 * 60 * 60 * 1000:
        raise ProductionError("placement offset must be a bounded signed millisecond value")
    if not isinstance(manual_lock, bool):
        raise ProductionError("manual_lock must be boolean")
    version, item = _version_in_station(store, station, version_id)
    if item["kind"] != kind:
        raise ProductionError("placement content kind must match sweeper or voice track")
    selected_cue_id = _id(cue_id, "cue_id") if cue_id else None
    if selected_cue_id:
        cue = store.get("radio_cues", selected_cue_id)
        if cue is None or cue.get("version_id") != version_id or \
                cue.get("station_id") != station:
            raise ProductionError("placement cue belongs to another content version")
    body = {"station_id": station, "transition_id": transition_id,
            "plan_revision_id": transition["plan_revision_id"],
            "anchor_entry_id": anchor, "content_version_id": version_id,
            "kind": kind, "start_reference": start_reference,
            "offset_ms": offset_ms, "ducking_envelope": _ducking(ducking_envelope),
            "cue_id": selected_cue_id,
            "context_fingerprint": transition["context_fingerprint"],
            "status": "draft", "manual_lock": manual_lock,
            "created_by": principal.user_id, "created_at": _now()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.expect("plan_revisions", revision["_id"], revision["_rev"])
    tx.expect("plan_transitions", transition_id, transition["_rev"])
    tx.expect("versions", version_id, version["_rev"])
    tx.put("production_placements", str(uuid4()), body)
    return tx.commit()[0]


def _placement_readiness(store: Store, placement: Mapping[str, Any],
                         transition: Mapping[str, Any], media: Mapping[str, Any]) -> None:
    technical = media.get("technical") or {}
    duration = technical.get("duration_ms")
    if isinstance(duration, bool) or not isinstance(duration, int) or duration < 1:
        raise Conflict("placement media has no reviewed duration")
    envelope = placement.get("ducking_envelope", [])
    if envelope and envelope[-1]["offset_ms"] > duration:
        raise Conflict("ducking envelope exceeds placement media duration")
    if placement["start_reference"] == "from_end":
        entry = store.get("plan_entries", transition["from_entry_id"])
        previous_duration = entry.get("planned_duration_ms") if entry else None
        if isinstance(previous_duration, bool) or not isinstance(previous_duration, int) or previous_duration < 1:
            raise Conflict("from_end placement needs a known preceding duration")
        base = datetime.fromisoformat(entry["scheduled_at"].replace("Z", "+00:00")) + \
            timedelta(milliseconds=previous_duration)
    else:
        entry = store.get("plan_entries", transition["to_entry_id"])
        if entry is None:
            raise Conflict("to_start plan entry is missing")
        base = datetime.fromisoformat(entry["scheduled_at"].replace("Z", "+00:00"))
    start = base + timedelta(milliseconds=placement["offset_ms"])
    end = start + timedelta(milliseconds=duration)
    ready = readiness_at(store, placement["station_id"], placement["content_version_id"],
                         slot_start=start, slot_end=end, require_endpoint_copy=False)
    if not ready["ready"]:
        raise Conflict("placement cannot be used at its plan time: " +
                       ", ".join(ready["blocked_reasons"]))
    if not any(location.get("status") in {"present", "verified"} for location in
               store.lookup("media_locations", "file", media["_id"])):
        raise Conflict("selected placement media has no verified central location")


def approve_production_placement(
    store: Store, principal: Principal, station_id: str | UUID,
    placement_id: str | UUID, *, expected_revision: int,
    media_file_id: str | UUID | None = None,
) -> dict:
    station, placement_id = _id(station_id, "station_id"), _id(placement_id, "placement_id")
    sequence = store.sequence
    require(store, principal, "approve", station)
    placement = store.get("production_placements", placement_id)
    if placement is None or placement["station_id"] != station:
        raise ProductionError("placement is outside station")
    if placement["_rev"] != expected_revision or placement["status"] != "draft":
        raise Conflict("only the current draft placement may be approved")
    transition = store.get("plan_transitions", placement["transition_id"])
    current, reason = _transition_state(store, transition)
    revision, _ = _revision_in_station(store, station, transition["plan_revision_id"])
    if not current or transition["status"] != "draft" or revision["status"] != "draft" or \
            placement["context_fingerprint"] != transition["context_fingerprint"]:
        raise Conflict(f"placement context is stale: {reason}")
    version, item = _version_in_station(store, station, placement["content_version_id"])
    if version["status"] != "approved" or item["kind"] != placement["kind"]:
        raise Conflict("placement needs an approved matching content version")
    media = _verified_media(store, version["_id"],
                            _id(media_file_id, "media_file_id") if media_file_id else None)
    _placement_readiness(store, placement, transition, media)
    cue = None
    if placement.get("cue_id"):
        cue = store.get("radio_cues", placement["cue_id"])
        if cue is None or cue.get("status") != "approved" or cue.get("media_file_id") != media["_id"]:
            raise Conflict("placement cue must be approved for the selected media")
    body = _body(placement) | {"status": "approved", "media_file_id": media["_id"],
                               "media_sha256": media["sha256"],
                               "approved_by": principal.user_id,
                               "approved_at": _now()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    for collection, row in (("plan_revisions", revision), ("plan_transitions", transition),
                            ("versions", version), ("media_files", media)):
        tx.expect(collection, row["_id"], row["_rev"])
    if cue:
        tx.expect("radio_cues", cue["_id"], cue["_rev"])
    tx.put("production_placements", placement_id, body,
           expected_revision=expected_revision)
    tx.commit()
    return store.get("production_placements", placement_id)


def invalidate_production_for_revision(tx: Transaction, store: Store,
                                       revision_id: str | UUID, reason: str) -> int:
    """Append stale writes to the same transaction as a plan entry edit.

    Callers must invoke this before committing *any* edit in a draft revision,
    including insertion that changes adjacency without changing existing rows.
    A locked placement is retained intact for explicit human review.
    """
    revision_id = _id(revision_id, "revision_id")
    if not isinstance(reason, str) or not reason.strip():
        raise ProductionError("invalidation reason is required")
    count = 0
    now = _now()
    for transition in store.lookup("plan_transitions", "revision", revision_id):
        if transition["status"] not in {"stale", "withdrawn"}:
            tx.put("plan_transitions", transition["_id"],
                   _body(transition) | {"status": "stale", "stale_reason": reason,
                                        "invalidated_at": now},
                   expected_revision=transition["_rev"])
            count += 1
        for placement in store.lookup("production_placements", "transition", transition["_id"]):
            if placement["status"] not in {"stale", "withdrawn"}:
                tx.put("production_placements", placement["_id"],
                       _body(placement) | {"status": "stale", "stale_reason": reason,
                                            "invalidated_at": now},
                       expected_revision=placement["_rev"])
                count += 1
    return count


def reconcile_production(store: Store, principal: Principal,
                         station_id: str | UUID, revision_id: str | UUID) -> dict:
    """Persist stale flags after out-of-band plan edits or restored snapshots."""
    station, revision_id = _id(station_id, "station_id"), _id(revision_id, "revision_id")
    sequence = store.sequence
    require(store, principal, "edit", station)
    _revision_in_station(store, station, revision_id)
    stale = [row for row in store.lookup("plan_transitions", "revision", revision_id)
             if row["status"] not in {"stale", "withdrawn"} and
             not _transition_state(store, row)[0]]
    if not stale:
        return {"invalidated": 0, "revision_id": revision_id}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    count = 0
    for transition in stale:
        count += invalidate_production_for_transition(
            tx, store, transition, "plan_entry_context_changed")
    tx.commit()
    return {"invalidated": count, "revision_id": revision_id}


def invalidate_production_for_transition(tx: Transaction, store: Store,
                                         transition: dict, reason: str) -> int:
    now = _now()
    count = 0
    if transition["status"] not in {"stale", "withdrawn"}:
        tx.put("plan_transitions", transition["_id"],
               _body(transition) | {"status": "stale", "stale_reason": reason,
                                    "invalidated_at": now},
               expected_revision=transition["_rev"])
        count += 1
    for placement in store.lookup("production_placements", "transition", transition["_id"]):
        if placement["status"] not in {"stale", "withdrawn"}:
            tx.put("production_placements", placement["_id"],
                   _body(placement) | {"status": "stale", "stale_reason": reason,
                                        "invalidated_at": now},
                   expected_revision=placement["_rev"])
            count += 1
    return count


def rebind_plan_transition(store: Store, principal: Principal,
                           station_id: str | UUID, transition_id: str | UUID, *,
                           expected_revision: int, review_reason: str) -> dict:
    """Explicitly recheck a stale pair; all placements return to draft review."""
    station, transition_id = _id(station_id, "station_id"), _id(transition_id, "transition_id")
    sequence = store.sequence
    require(store, principal, "edit", station)
    transition = store.get("plan_transitions", transition_id)
    if transition is None or transition["station_id"] != station:
        raise ProductionError("transition is outside station")
    if transition["_rev"] != expected_revision or transition["status"] != "stale":
        raise Conflict("only the current stale transition can be rebound")
    if not isinstance(review_reason, str) or not review_reason.strip():
        raise ProductionError("rebind requires an explicit review reason")
    revision, _ = _revision_in_station(store, station, transition["plan_revision_id"])
    if revision["status"] != "draft":
        raise Conflict("approved plan context cannot be rebound")
    first, second, fingerprint = _transition_context(
        store, station, transition["plan_revision_id"],
        transition["from_entry_id"], transition["to_entry_id"])
    body = _body(transition) | {
        "from_version_id": first["payload"]["version_id"],
        "to_version_id": second["payload"]["version_id"],
        "context_fingerprint": fingerprint, "status": "draft",
        "rebound_at": _now(), "rebound_by": principal.user_id,
        "rebind_reason": review_reason.strip(),
    }
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.expect("plan_entries", first["_id"], first["_rev"])
    tx.expect("plan_entries", second["_id"], second["_rev"])
    tx.expect("plan_revisions", revision["_id"], revision["_rev"])
    tx.put("plan_transitions", transition_id, body,
           expected_revision=expected_revision)
    for placement in store.lookup("production_placements", "transition", transition_id):
        if placement["status"] == "withdrawn":
            continue
        placement_body = _body(placement) | {
            "context_fingerprint": fingerprint, "status": "draft",
            "reapproval_required": True,
        }
        for key in ("approved_at", "approved_by", "media_file_id", "media_sha256"):
            placement_body.pop(key, None)
        tx.put("production_placements", placement["_id"], placement_body,
               expected_revision=placement["_rev"])
    tx.commit()
    return store.get("plan_transitions", transition_id)


def withdraw_production_placement(
    store: Store, principal: Principal, station_id: str | UUID,
    placement_id: str | UUID, *, expected_revision: int, reason: str,
) -> dict:
    station, placement_id = _id(station_id, "station_id"), _id(placement_id, "placement_id")
    require(store, principal, "approve", station)
    row = store.get("production_placements", placement_id)
    if row is None or row["station_id"] != station:
        raise ProductionError("placement is outside station")
    revision, _ = _revision_in_station(store, station, row["plan_revision_id"])
    if revision["status"] != "draft":
        raise Conflict("withdraw placement through a new draft plan revision")
    if row["_rev"] != expected_revision or row["status"] == "withdrawn":
        raise Conflict("placement changed or was already withdrawn")
    if not isinstance(reason, str) or not reason.strip():
        raise ProductionError("withdrawal reason is required")
    return store.put("production_placements", placement_id,
                     _body(row) | {"status": "withdrawn", "withdrawn_at": _now(),
                                   "withdrawn_by": principal.user_id,
                                   "withdrawn_reason": reason.strip()},
                     expected_revision=expected_revision, actor=principal.user_id)


def withdraw_plan_transition(
    store: Store, principal: Principal, station_id: str | UUID,
    transition_id: str | UUID, *, expected_revision: int, reason: str,
) -> dict:
    station, transition_id = _id(station_id, "station_id"), _id(transition_id, "transition_id")
    require(store, principal, "approve", station)
    transition = store.get("plan_transitions", transition_id)
    if transition is None or transition["station_id"] != station:
        raise ProductionError("transition is outside station")
    revision, _ = _revision_in_station(store, station, transition["plan_revision_id"])
    if revision["status"] != "draft":
        raise Conflict("withdraw transition through a new draft plan revision")
    if transition["_rev"] != expected_revision or transition["status"] == "withdrawn":
        raise Conflict("transition changed or was already withdrawn")
    if not isinstance(reason, str) or not reason.strip():
        raise ProductionError("withdrawal reason is required")
    now = _now()
    tx = store.begin(actor=principal.user_id)
    tx.put("plan_transitions", transition_id,
           _body(transition) | {"status": "withdrawn", "withdrawn_at": now,
                                "withdrawn_by": principal.user_id,
                                "withdrawn_reason": reason.strip()},
           expected_revision=expected_revision)
    for placement in store.lookup("production_placements", "transition", transition_id):
        if placement["status"] != "withdrawn":
            tx.put("production_placements", placement["_id"],
                   _body(placement) | {"status": "withdrawn", "withdrawn_at": now,
                                       "withdrawn_by": principal.user_id,
                                       "withdrawn_reason": reason.strip()},
                   expected_revision=placement["_rev"])
    tx.commit()
    return store.get("plan_transitions", transition_id)


def require_current_production(store: Store, station_id: str | UUID,
                               revision_id: str | UUID) -> list[dict]:
    """Guard plan approval/distribution against stale or unreviewed placements."""
    sequence = store.sequence
    station, revision_id = _id(station_id, "station_id"), _id(revision_id, "revision_id")
    _revision_in_station(store, station, revision_id)
    output = []
    for transition in store.lookup("plan_transitions", "revision", revision_id):
        if transition["status"] == "withdrawn":
            continue
        current, reason = _transition_state(store, transition)
        if not current or transition["status"] == "stale":
            raise Conflict(f"transition {transition['_id']} is stale: {reason}")
        for placement in store.lookup("production_placements", "transition", transition["_id"]):
            if placement["status"] == "withdrawn":
                continue
            if placement["status"] != "approved" or \
                    placement["context_fingerprint"] != transition["context_fingerprint"]:
                raise Conflict(f"placement {placement['_id']} needs approval for current plan context")
            media = store.get("media_files", placement["media_file_id"])
            if media is None or media.get("status") != "verified" or \
                    media.get("sha256") != placement.get("media_sha256"):
                raise Conflict(f"placement {placement['_id']} media is no longer verified")
            version, item = _version_in_station(store, station, placement["content_version_id"])
            if version["status"] != "approved" or item["kind"] != placement["kind"]:
                raise Conflict(f"placement {placement['_id']} content approval changed")
            _placement_readiness(store, placement, transition, media)
            if placement.get("cue_id"):
                cue = store.get("radio_cues", placement["cue_id"])
                if cue is None or cue.get("status") != "approved" or \
                        cue.get("media_file_id") != media["_id"] or \
                        cue.get("media_sha256") != media["sha256"]:
                    raise Conflict(f"placement {placement['_id']} cue approval changed")
            output.append(placement)
    if store.sequence != sequence:
        raise Conflict("production changed while validating plan context")
    return output


def production_snapshot(store: Store, station_id: str | UUID,
                        revision_id: str | UUID) -> dict:
    """Return reviewed placement data and media needed by endpoint prefetch.

    This is a data manifest only. Its presence does not mean a playout engine
    knows how to mix, duck, trigger, or render the placement.
    """
    sequence = store.sequence
    station, revision_id = _id(station_id, "station_id"), _id(revision_id, "revision_id")
    placements = require_current_production(store, station, revision_id)
    media_rows: dict[str, dict] = {}
    output = []
    for row in placements:
        transition = store.get("plan_transitions", row["transition_id"])
        media = store.get("media_files", row["media_file_id"])
        media_rows[media["_id"]] = {"media_file_id": media["_id"],
                                    "sha256": media["sha256"],
                                    "size_bytes": media["size_bytes"]}
        cue = store.get("radio_cues", row["cue_id"]) if row.get("cue_id") else None
        output.append({
            "placement_id": row["_id"], "transition_id": transition["_id"],
            "plan_revision_id": revision_id,
            "from_entry_id": transition["from_entry_id"],
            "to_entry_id": transition["to_entry_id"],
            "anchor_entry_id": row["anchor_entry_id"],
            "kind": row["kind"], "content_version_id": row["content_version_id"],
            "media_file_id": media["_id"],
            "start_reference": row["start_reference"],
            "offset_ms": row["offset_ms"],
            "ducking_envelope": row["ducking_envelope"],
            "manual_lock": row["manual_lock"],
            "context_fingerprint": row["context_fingerprint"],
            "cue": ({"cue_id": cue["_id"], "cue_kind": cue["cue_kind"],
                     "position_ms": cue["position_ms"], "unit": cue["unit"],
                     "position_value": cue["position_value"]} if cue else None),
        })
    if store.sequence != sequence:
        raise Conflict("production changed while building its snapshot")
    return {"placements": output,
            "media": [media_rows[key] for key in sorted(media_rows)],
            "execution_capability": "data_only"}




# Stable compatibility imports for callers and schema validators.
from .radio_cues import (CUE_KINDS, CUE_UNITS, CUE_SOURCES, create_radio_cue,
                         propose_radio_cue_from_dj_mark, approve_radio_cue,
                         withdraw_radio_cue, approved_cues_for_version)
from .controller_profiles import (MIDI_EVENTS, TRIGGER_MODES, PAD_TARGETS,
                                  _device_spec, create_controller_profile,
                                  fork_controller_profile, put_controller_bank,
                                  put_controller_pad, publish_controller_profile,
                                  controller_profile_snapshot)
