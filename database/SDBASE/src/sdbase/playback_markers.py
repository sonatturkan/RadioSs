"""Immutable, reviewed source offsets carried by every approved audio event.

A set binds cue coordinates to one content version, one representation and
one SHA-256.  A plan approval captures that set's body and identity; later cue
withdrawal or a new set cannot change an already approved plan.
"""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime
from typing import Any, Mapping
from uuid import UUID, uuid4

from ._production_common import (_body, _id, _now, _sha, _verified_media,
                                 _version_in_station)
from .identity import Principal, require
from .radio_cues import CUE_KINDS, approved_cues_for_version
from .storage import Conflict, Store, StoreError


MARKER_CONTRACT_VERSION = 1
SEMANTIC_FIELDS = frozenset({
    "marker_contract_version", "station_id", "version_id", "media_file_id",
    "media_sha256", "media_duration_ms", "cue_in_ms", "cue_out_ms",
    "segue_in_ms", "segue_out_ms", "fade_in_ms", "fade_out_ms", "cues",
    "selection_source",
})
SET_FIELDS = SEMANTIC_FIELDS | {"status", "content_sha256", "created_by", "created_at"}
FROZEN_FIELDS = SEMANTIC_FIELDS | {"marker_set_id", "marker_set_revision", "content_sha256"}
_COORDINATES = {"audio_start": "cue_in_ms", "audio_end": "cue_out_ms",
                "segue_in": "segue_in_ms", "segue_out": "segue_out_ms"}
_SOURCES = {"manual_review", "approved_cues", "entry_payload", "media_default"}


def _integer(value: Any, field: str, *, positive: bool = False) -> int:
    if type(value) is not int or value < (1 if positive else 0):
        raise StoreError(f"{field} must be a {'positive' if positive else 'nonnegative'} integer")
    return value


def _fingerprint(body: Mapping[str, Any]) -> str:
    return _sha({field: body[field] for field in SEMANTIC_FIELDS})


def _validate_semantics(body: Mapping[str, Any]) -> None:
    if body.get("marker_contract_version") != MARKER_CONTRACT_VERSION or \
            type(body.get("marker_contract_version")) is not int:
        raise StoreError("unsupported playback marker contract")
    for field in ("station_id", "version_id", "media_file_id"):
        _id(body.get(field), field)
    digest = body.get("media_sha256")
    if not isinstance(digest, str) or len(digest) != 64 or any(
            char not in "0123456789abcdef" for char in digest):
        raise StoreError("playback markers need a lowercase SHA-256")
    duration = _integer(body.get("media_duration_ms"), "media_duration_ms", positive=True)
    cue_in = _integer(body.get("cue_in_ms"), "cue_in_ms")
    cue_out = _integer(body.get("cue_out_ms"), "cue_out_ms", positive=True)
    if not cue_in < cue_out <= duration:
        raise StoreError("playback cue span is outside the verified media")
    for field in ("segue_in_ms", "segue_out_ms"):
        if body.get(field) is not None and not cue_in <= _integer(body[field], field) <= cue_out:
            raise StoreError(f"{field} must lie inside the selected cue span")
    if body.get("segue_in_ms") is not None and body.get("segue_out_ms") is not None and \
            body["segue_in_ms"] > body["segue_out_ms"]:
        raise StoreError("segue-in must not follow segue-out")
    fade_in = _integer(body.get("fade_in_ms"), "fade_in_ms")
    fade_out = _integer(body.get("fade_out_ms"), "fade_out_ms")
    if fade_in + fade_out > cue_out - cue_in:
        raise StoreError("fade envelopes overlap inside the cue span")
    if not isinstance(body.get("selection_source"), str) or body["selection_source"] not in _SOURCES:
        raise StoreError("unsupported playback marker selection source")
    cues = body.get("cues")
    if not isinstance(cues, list) or len(cues) > len(CUE_KINDS):
        raise StoreError("playback cue provenance must be a bounded list")
    kinds: set[str] = set()
    identities: set[str] = set()
    for cue in cues:
        if not isinstance(cue, Mapping) or set(cue) != {
                "cue_id", "cue_revision", "cue_kind", "position_ms"}:
            raise StoreError("playback cue provenance fields differ")
        cue_id = _id(cue.get("cue_id"), "cue_id")
        _integer(cue.get("cue_revision"), "cue_revision", positive=True)
        kind = cue.get("cue_kind")
        if not isinstance(kind, str) or kind not in CUE_KINDS or \
                kind in kinds or cue_id in identities:
            raise StoreError("playback marker selection has duplicate or unknown cue kinds")
        kinds.add(kind)
        identities.add(cue_id)
        position = _integer(cue.get("position_ms"), "position_ms")
        if position > duration:
            raise StoreError("playback cue provenance exceeds media duration")
        field = _COORDINATES.get(kind)
        if field and body[field] != position:
            raise StoreError("playback coordinate differs from its approved source cue")
        if kind == "fade_start" and cue_out - fade_out != position:
            raise StoreError("fade-out differs from its approved fade-start cue")
    if body.get("content_sha256") != _fingerprint(body):
        raise StoreError("playback marker content fingerprint differs")


def validate_marker_set(body: Mapping[str, Any], state: Mapping[str, Any] | None = None) -> None:
    """Schema hook: validate the set and historical source cue provenance."""
    if not isinstance(body, Mapping) or set(body) != SET_FIELDS or body.get("status") != "approved":
        raise StoreError("playback marker set must have the exact approved fields")
    _validate_semantics(body)
    _id(body.get("created_by"), "created_by")
    try:
        created = datetime.fromisoformat(body["created_at"].replace("Z", "+00:00"))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError("playback marker set needs ISO creation time") from exc
    if created.tzinfo is None or created.utcoffset() is None:
        raise StoreError("playback marker creation time needs a UTC offset")
    if state is None:
        return
    version_row = state.get("versions", {}).get(body["version_id"])
    media_row = state.get("media_files", {}).get(body["media_file_id"])
    if version_row is None or media_row is None:
        raise StoreError("playback marker version or media is absent")
    media = media_row["data"]
    if media.get("version_id") != body["version_id"] or \
            media.get("sha256") != body["media_sha256"] or \
            (media.get("technical") or {}).get("duration_ms") != body["media_duration_ms"]:
        raise StoreError("playback marker representation differs from verified source")
    item_id = version_row["data"]["item_id"]
    if not any(row["data"]["station_id"] == body["station_id"] and
               row["data"]["item_id"] == item_id
               for row in state.get("station_items", {}).values()):
        raise StoreError("playback marker version is outside station")
    for selected in body["cues"]:
        row = state.get("radio_cues", {}).get(selected["cue_id"])
        cue = row["data"] if row else None
        # An approved cue may later be withdrawn. Its approved coordinates are
        # immutable and remain valid historical provenance for this set.
        if cue is None or cue.get("status") not in {"approved", "withdrawn"} or \
                not cue.get("approved_at") or not cue.get("approved_by") or \
                selected["cue_revision"] != row["revision"] - (1 if cue.get("status") == "withdrawn" else 0) or \
                any(cue.get(field) != body[field] for field in
                    ("station_id", "version_id", "media_file_id", "media_sha256")) or \
                cue.get("cue_kind") != selected["cue_kind"] or \
                cue.get("position_ms") != selected["position_ms"]:
            raise StoreError("playback cue provenance differs from its approved source")


def freeze_marker_set(row: Mapping[str, Any]) -> dict:
    body = _body(row)
    validate_marker_set(body)
    return {field: deepcopy(body[field]) for field in SEMANTIC_FIELDS | {"content_sha256"}} | {
        "marker_set_id": _id(row.get("_id"), "marker_set_id"),
        "marker_set_revision": _integer(row.get("_rev"), "marker_set_revision", positive=True)}


def validate_frozen_markers(markers: Mapping[str, Any], *, station_id: str | None = None,
                            version_id: str | None = None, media_file_id: str | None = None,
                            media_sha256: str | None = None,
                            payload: Mapping[str, Any] | None = None,
                            planned_duration_ms: int | None = None,
                            duration_kind: str | None = None) -> None:
    if not isinstance(markers, Mapping) or set(markers) != FROZEN_FIELDS:
        raise StoreError("frozen playback marker fields differ")
    _id(markers.get("marker_set_id"), "marker_set_id")
    if _integer(markers.get("marker_set_revision"), "marker_set_revision", positive=True) != 1:
        raise StoreError("an immutable playback marker set must have revision one")
    _validate_semantics(markers)
    for field, expected in (("station_id", station_id), ("version_id", version_id),
                            ("media_file_id", media_file_id), ("media_sha256", media_sha256)):
        if expected is not None and markers[field] != expected:
            raise StoreError(f"frozen playback {field} differs from approved entry or media")
    if payload is not None:
        selected = payload.get("playback_marker_set_id")
        if selected is not None and selected != markers["marker_set_id"]:
            raise StoreError("selected playback marker set differs from frozen identity")
        for field in ("cue_in_ms", "cue_out_ms", "segue_in_ms", "segue_out_ms", "fade_in_ms", "fade_out_ms"):
            if payload.get(field) is not None and payload[field] != markers[field]:
                raise StoreError(f"entry {field} differs from frozen playback marker")
    if duration_kind == "fixed" and markers["cue_out_ms"] - markers["cue_in_ms"] != planned_duration_ms:
        raise StoreError("fixed planned duration differs from the approved cue span")


def _make_set(store: Store, station: str, version_id: str, media: dict,
              cues: list[dict], *, actor: str, values: Mapping[str, Any],
              source: str, default_cue_out: int | None = None) -> dict:
    for field, value in values.items():
        _integer(value, field)
    coordinates: dict[str, int] = {}
    provenance = []
    for cue in cues:
        if cue.get("status") != "approved" or any(cue.get(field) != expected for field, expected in
                (("station_id", station), ("version_id", version_id),
                 ("media_file_id", media["_id"]), ("media_sha256", media["sha256"]))):
            raise Conflict("playback marker cue is not approved for this exact media")
        kind = cue["cue_kind"]
        if kind in coordinates:
            raise Conflict("multiple approved cues of one kind require an explicit selected cue set")
        coordinates[kind] = cue["position_ms"]
        provenance.append({"cue_id": cue["_id"], "cue_revision": cue["_rev"],
                           "cue_kind": kind, "position_ms": cue["position_ms"]})
    duration = _integer((media.get("technical") or {}).get("duration_ms"),
                        "media_duration_ms", positive=True)
    cue_in = coordinates.get("audio_start", values.get("cue_in_ms", 0))
    cue_out = coordinates.get("audio_end", values.get("cue_out_ms"))
    if cue_out is None:
        cue_out = default_cue_out if default_cue_out is not None else duration
    fade_out = cue_out - coordinates["fade_start"] if "fade_start" in coordinates else \
        values.get("fade_out_ms", 0)
    body = {"marker_contract_version": MARKER_CONTRACT_VERSION, "status": "approved",
            "station_id": station, "version_id": version_id, "media_file_id": media["_id"],
            "media_sha256": media["sha256"], "media_duration_ms": duration,
            "cue_in_ms": cue_in, "cue_out_ms": cue_out,
            "segue_in_ms": coordinates.get("segue_in", values.get("segue_in_ms")),
            "segue_out_ms": coordinates.get("segue_out", values.get("segue_out_ms")),
            "fade_in_ms": values.get("fade_in_ms", 0), "fade_out_ms": fade_out,
            "cues": sorted(provenance, key=lambda cue: (cue["cue_kind"], cue["cue_id"])),
            "selection_source": source, "created_by": actor, "created_at": _now()}
    body["content_sha256"] = _fingerprint(body)
    validate_marker_set(body)
    for field in ("cue_in_ms", "cue_out_ms", "segue_in_ms", "segue_out_ms", "fade_in_ms", "fade_out_ms"):
        if values.get(field) is not None and values[field] != body[field]:
            raise Conflict(f"explicit {field} conflicts with the selected approved cue")
    return body


def create_playback_marker_set(store: Store, principal: Principal,
                                station_id: str, version_id: str, *,
                                cue_ids: list[str], media_file_id: str | None = None,
                                cue_in_ms: int | None = None, cue_out_ms: int | None = None,
                                segue_in_ms: int | None = None, segue_out_ms: int | None = None,
                                fade_in_ms: int = 0, fade_out_ms: int | None = None) -> dict:
    """Approve one immutable cue selection; a subsequent selection gets a new ID."""
    station, version_id = _id(station_id, "station_id"), _id(version_id, "version_id")
    sequence = store.sequence
    require(store, principal, "approve", station)
    version, _ = _version_in_station(store, station, version_id)
    if version["status"] != "approved":
        raise Conflict("playback marker set requires an approved content version")
    if not isinstance(cue_ids, list) or len(cue_ids) > len(CUE_KINDS):
        raise StoreError("cue_ids must be a bounded list")
    ids = [_id(value, "cue_id") for value in cue_ids]
    if len(set(ids)) != len(ids):
        raise StoreError("cue_ids contains duplicates")
    media = _verified_media(store, version_id, media_file_id)
    cues = [store.get("radio_cues", cue_id) for cue_id in ids]
    if any(cue is None for cue in cues):
        raise StoreError("selected radio cue is absent")
    values = {key: value for key, value in {
        "cue_in_ms": cue_in_ms, "cue_out_ms": cue_out_ms,
        "segue_in_ms": segue_in_ms, "segue_out_ms": segue_out_ms,
        "fade_in_ms": fade_in_ms, "fade_out_ms": fade_out_ms}.items()
        if value is not None}
    body = _make_set(store, station, version_id, media, cues, actor=principal.user_id,
                     values=values, source="manual_review")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("playback_marker_sets", str(uuid4()), body)
    return tx.commit()[0]


def list_playback_marker_sets(store: Store, principal: Principal,
                              station_id: str, version_id: str) -> list[dict]:
    station, version = _id(station_id, "station_id"), _id(version_id, "version_id")
    require(store, principal, "view", station)
    _version_in_station(store, station, version)
    return [row for row in store.lookup("playback_marker_sets", "version", version)
            if row["station_id"] == station]


def get_playback_marker_set(store: Store, principal: Principal,
                            station_id: str, marker_set_id: str) -> dict:
    station, identifier = _id(station_id, "station_id"), _id(marker_set_id, "marker_set_id")
    require(store, principal, "view", station)
    row = store.get("playback_marker_sets", identifier)
    if row is None or row["station_id"] != station:
        raise StoreError("playback marker set is outside station")
    return row


def prepare_plan_playback(store: Store, station_id: str, entries: list[dict],
                           media_by_entry: Mapping[str, str], actor: str) -> tuple[dict, list[tuple[str, dict]]]:
    """Approval hook; return frozen bindings and new immutable rows for its transaction."""
    station = _id(station_id, "station_id")
    bindings: dict[str, dict] = {}
    new_rows: list[tuple[str, dict]] = []
    for entry in entries:
        if entry["event_kind"] != "audio" or entry["state"] != "planned":
            continue
        payload = entry["payload"]
        if any(payload.get(field) is not None for field in
               ("cue", "cue_id", "radio_cue_id", "marker_set_id")):
            raise Conflict("unresolved cue selectors need an explicit playback_marker_set_id")
        version_id = _id(payload["version_id"], "version_id")
        explicit = payload.get("playback_marker_set_id")
        if explicit is not None:
            row = store.get("playback_marker_sets", _id(explicit, "playback_marker_set_id"))
            if row is None:
                raise Conflict("selected playback marker set is absent")
            media = _verified_media(store, version_id, row["media_file_id"])
            if not any(location.get("status") in {"present", "verified"} for location in
                       store.lookup("media_locations", "file", media["_id"])):
                raise Conflict("selected playback marker media has no verified central copy")
            frozen = freeze_marker_set(row)
        else:
            media = _verified_media(store, version_id, media_by_entry[entry["_id"]])
            cues = approved_cues_for_version(store, station, version_id, media["_id"])
            values = {key: payload[key] for key in ("cue_in_ms", "cue_out_ms", "segue_in_ms",
                      "segue_out_ms", "fade_in_ms", "fade_out_ms")
                      if payload.get(key) is not None}
            for field, value in values.items():
                _integer(value, field)
            cue_in = next((cue["position_ms"] for cue in cues if cue["cue_kind"] == "audio_start"),
                           values.get("cue_in_ms", 0))
            default_out = cue_in + entry["planned_duration_ms"] if \
                entry["duration_kind"] == "fixed" else None
            body = _make_set(store, station, version_id, media, cues, actor=actor, values=values,
                             source="approved_cues" if cues else "entry_payload" if values else "media_default",
                             default_cue_out=default_out)
            identifier = str(uuid4())
            frozen = freeze_marker_set(body | {"_id": identifier, "_rev": 1})
            new_rows.append((identifier, body))
        validate_frozen_markers(frozen, station_id=station, version_id=version_id,
                                media_file_id=media["_id"], media_sha256=media["sha256"],
                                payload=payload, duration_kind=entry["duration_kind"],
                                planned_duration_ms=entry["planned_duration_ms"])
        bindings[entry["_id"]] = frozen
    return bindings, new_rows


def approved_playback_binding(store: Store, revision: Mapping[str, Any],
                               entry: Mapping[str, Any]) -> dict:
    """Distribution hook: use approval data only, never current cue selection."""
    bindings = revision.get("playback_bindings")
    frozen = bindings.get(entry["_id"]) if isinstance(bindings, Mapping) else None
    if frozen is None:
        raise Conflict("approved audio revision lacks frozen playback markers; fork and approve a new revision")
    validate_frozen_markers(frozen, version_id=entry["payload"]["version_id"],
                            payload=entry["payload"], duration_kind=entry["duration_kind"],
                            planned_duration_ms=entry["planned_duration_ms"])
    row = store.get("playback_marker_sets", frozen["marker_set_id"])
    if row is None or freeze_marker_set(row) != frozen:
        raise Conflict("frozen marker identity or revision differs from its immutable stored set")
    media = _verified_media(store, entry["payload"]["version_id"], frozen["media_file_id"])
    if media.get("sha256") != frozen["media_sha256"] or \
            (media.get("technical") or {}).get("duration_ms") != frozen["media_duration_ms"]:
        raise Conflict("approved playback media digest or technical duration changed")
    return deepcopy(frozen)
