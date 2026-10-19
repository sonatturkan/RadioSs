"""Central plan delivery, fenced commands, and append-only playout evidence.

The SDBASE store records claims made by an engine and an output observer. A
command acknowledgement does not prove sound was produced; a completed engine
event does not prove audio reached listeners. A separate monitor must supply
independent output observations when that evidence is required.
"""

from __future__ import annotations

from datetime import date, datetime, time, timedelta, timezone
import hashlib
import json
from typing import Any, Mapping
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo

from .identity import Forbidden, Principal, require
from .readiness import execution_scope, readiness_at
from .snapshot_contract import (SnapshotContractError, freeze_entry,
                                validate_snapshot_contract)
from .storage import Conflict, Store, StoreError
from .broadcast_services import (check_entry_scope, execution_entry,
                                 require_approved_execution_binding, require_service)


_COMMAND_KINDS = {"start", "stop", "skip", "load_plan", "takeover", "other"}
_REVISION_COMMANDS = {"start", "skip", "load_plan"}
_PHASES = {"attempt", "start", "complete", "fail", "interrupt", "skip",
           "manual_start", "manual_complete"}
_OBSERVATIONS = {"audio_present", "silence", "off_air", "monitor_error"}
_OFFLINE_HEARTBEAT_MAX_AGE = timedelta(minutes=5)


def _id(value: Any, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _instant(value: Any, field: str) -> datetime:
    try:
        parsed = value if isinstance(value, datetime) else datetime.fromisoformat(
            str(value).replace("Z", "+00:00"))
    except ValueError as exc:
        raise StoreError(f"{field} must be an ISO instant") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError(f"{field} must include a timezone offset")
    return parsed.astimezone(timezone.utc)


def _now(value: datetime | str | None = None) -> datetime:
    return datetime.now(timezone.utc) if value is None else _instant(value, "now")


def _iso(value: datetime) -> str:
    return value.astimezone(timezone.utc).isoformat()


def _data(record: dict) -> dict:
    return {key: value for key, value in record.items() if key not in {"_id", "_rev"}}


def _sha(snapshot: Mapping[str, Any]) -> str:
    try:
        body = json.dumps(snapshot, ensure_ascii=False, sort_keys=True,
                          separators=(",", ":"), allow_nan=False).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise StoreError("snapshot must be a finite JSON object") from exc
    return hashlib.sha256(body).hexdigest()


def _endpoint(store: Store, station_id: str, endpoint_id: str,
              *, allow_retired: bool = False) -> dict:
    endpoint = store.get("endpoints", endpoint_id)
    if endpoint is None or endpoint.get("station_id") != station_id:
        raise StoreError("endpoint does not belong to station")
    if endpoint.get("status") == "retired" and not allow_retired:
        raise Conflict("retired endpoint cannot receive playout control")
    return endpoint


def _require_endpoint(store: Store, principal: Principal,
                      station_id: str, endpoint_id: str) -> None:
    require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    if principal.scope != "endpoint" or principal.endpoint_id != endpoint_id:
        raise Forbidden("endpoint operation requires its bound endpoint credential")


def _approved_revision(store: Store, station_id: str, revision_id: str) -> tuple[dict, dict]:
    revision = store.get("plan_revisions", revision_id)
    if revision is None or revision.get("status") != "approved":
        raise Conflict("plan revision is not approved")
    plan = store.get("plans", revision["plan_id"])
    if plan is None or plan.get("station_id") != station_id:
        raise StoreError("plan revision belongs to another station")
    if plan.get("current_revision_id") != revision_id or plan.get("state") in {"cancelled", "completed"}:
        raise Conflict("revision is not the current station plan")
    return plan, revision


def _version_at(store: Store, station_id: str, version_id: str,
                start: datetime, end: datetime) -> None:
    version = store.get("versions", version_id)
    if version is None or version.get("status") != "approved":
        raise Conflict("planned version is absent or unapproved")
    item = store.get("items", version["item_id"])
    if item is None or item.get("lifecycle") != "active":
        raise Conflict("planned item is inactive")
    station_rows = store.lookup("station_items", "station_item", station_id, version["item_id"])
    if not station_rows or station_rows[0].get("eligible") is not True:
        raise Conflict("planned item is not station eligible")
    if version.get("valid_from") and _instant(version["valid_from"], "valid_from") > start:
        raise Conflict("version is not yet valid at scheduled slot")
    if version.get("valid_to") and _instant(version["valid_to"], "valid_to") < end:
        raise Conflict("version expires during scheduled slot")


def date_from_iso(value: Any) -> date:
    try:
        return date.fromisoformat(str(value))
    except ValueError as exc:
        raise StoreError("service_date must be an ISO date") from exc


def _local_day_start(day: date, zone: ZoneInfo) -> datetime:
    """First real instant of a station-local day, including DST midnight gaps."""
    naive_midnight = datetime.combine(day, time.min)
    for minute in range(24 * 60):
        wall = naive_midnight + timedelta(minutes=minute)
        instants = []
        for fold in (0, 1):
            candidate = wall.replace(tzinfo=zone, fold=fold).astimezone(timezone.utc)
            if candidate.astimezone(zone).replace(tzinfo=None) == wall:
                instants.append(candidate)
        if instants:
            return min(instants)
    raise Conflict("station has no local civil time on service_date")


def _snapshot_matches(store: Store, station_id: str, endpoint_id: str,
                      revision_id: str, snapshot: Mapping[str, Any]) -> tuple[dict, dict, dict]:
    if not isinstance(snapshot, Mapping):
        raise StoreError("snapshot must be an object")
    try:
        validate_snapshot_contract(snapshot)
    except SnapshotContractError as exc:
        raise StoreError(str(exc)) from exc
    plan, revision = _approved_revision(store, station_id, revision_id)
    binding = require_approved_execution_binding(store, station_id,
                plan["service_key"], revision, endpoint_id)
    if snapshot.get("execution_binding") != binding:
        raise StoreError("snapshot execution binding differs from approved plan")
    if snapshot.get("service_key", "default") != plan["service_key"]:
        raise StoreError("snapshot broadcast service differs from approved plan")
    for field, expected in (("station_id", station_id), ("endpoint_id", endpoint_id),
                            ("plan_revision_id", revision_id), ("plan_id", plan["_id"])):
        if _id(snapshot.get(field), field) != expected:
            raise StoreError(f"snapshot {field} differs from approved plan")
    if snapshot.get("revision_no") != revision.get("revision_no"):
        raise StoreError("snapshot revision number differs from approved revision")
    station = store.get("stations", station_id)
    if snapshot.get("service_date") != plan.get("service_date") or \
            snapshot.get("station_timezone") != station.get("timezone"):
        raise StoreError("snapshot service date or station timezone differs from plan")
    activation = _local_day_start(date_from_iso(plan["service_date"]),
                                  ZoneInfo(station["timezone"]))
    if _instant(snapshot.get("activate_after"), "activate_after") != activation:
        raise StoreError("snapshot activation instant differs from station service day start")
    entries = snapshot.get("entries")
    media = snapshot.get("media")
    if not isinstance(entries, list) or not isinstance(media, list):
        raise StoreError("snapshot entries and media must be lists")
    stored = {row["_id"]: row for row in store.lookup("plan_entries", "revision", revision_id)
              if row.get("state", "planned") == "planned"}
    supplied: dict[str, dict] = {}
    seen_ordinals: set[int] = set()
    for raw in entries:
        if not isinstance(raw, Mapping):
            raise StoreError("snapshot entry must be an object")
        entry_id = _id(raw.get("plan_entry_id"), "plan_entry_id")
        if entry_id in supplied:
            raise StoreError("snapshot has duplicate plan entry")
        stored_row = stored.get(entry_id)
        if stored_row is None:
            raise StoreError("snapshot has an entry outside the approved revision")
        row = execution_entry(stored_row, binding)
        markers = None
        if row["event_kind"] == "audio":
            from .playback_markers import approved_playback_binding
            markers = approved_playback_binding(store, revision, row)
        try:
            frozen = freeze_entry(row, markers["media_file_id"] if markers else None, markers)
        except SnapshotContractError as exc:
            raise StoreError(str(exc)) from exc
        if dict(raw) != frozen:
            raise StoreError("snapshot execution fields differ from approved entry")
        ordinal = raw.get("ordinal")
        if isinstance(ordinal, bool) or not isinstance(ordinal, int) or ordinal in seen_ordinals or ordinal != row.get("ordinal"):
            raise StoreError("snapshot entry ordinal differs from approved plan")
        seen_ordinals.add(ordinal)
        kind = raw.get("entry_kind", raw.get("event_kind"))
        if kind != row.get("event_kind") or _instant(raw.get("scheduled_at"), "scheduled_at") != \
                _instant(row.get("scheduled_at"), "scheduled_at"):
            raise StoreError("snapshot entry kind or scheduled instant changed")
        payload = row.get("payload") or {}
        planned_version = payload.get("version_id") if kind == "audio" else None
        if kind == "audio" and not planned_version:
            raise Conflict("cart cut must be resolved before distribution")
        if (str(raw.get("version_id")) if raw.get("version_id") else None) != planned_version:
            raise StoreError("snapshot entry version differs from approved plan")
        if planned_version:
            start = _instant(raw["scheduled_at"], "scheduled_at")
            duration = row.get("planned_duration_ms")
            end = start + timedelta(milliseconds=duration if isinstance(duration, int) and duration > 0 else 1)
            _version_at(store, station_id, planned_version, start, end)
        supplied[entry_id] = dict(raw)
    if set(stored) != set(supplied):
        raise StoreError("snapshot omits planned entries")
    manifest: dict[str, dict] = {}
    for raw in media:
        if not isinstance(raw, Mapping):
            raise StoreError("media manifest entry must be an object")
        media_id = _id(raw.get("media_file_id"), "media_file_id")
        if media_id in manifest:
            raise StoreError("media manifest has a duplicate file")
        file = store.get("media_files", media_id)
        if file is None or file.get("status") != "verified" or \
                file.get("sha256") != raw.get("sha256") or \
                file.get("size_bytes") != raw.get("size_bytes"):
            raise Conflict("central media is absent, unverified, or changed")
        if not any(location.get("status") in {"present", "verified"} for location in
                   store.lookup("media_locations", "file", media_id)):
            raise Conflict("media representation has no verified central location")
        manifest[media_id] = dict(raw)
    for entry in supplied.values():
        if entry.get("version_id") is None:
            if entry.get("media_file_id") is not None:
                raise StoreError("non-audio entry cannot name a media file")
            continue
        media_id = _id(entry.get("media_file_id"), "media_file_id")
        if media_id not in manifest:
            raise StoreError("audio entry media is absent from manifest")
        if store.get("media_files", media_id).get("version_id") != entry["version_id"]:
            raise StoreError("audio entry media belongs to a different version")
        stored_entry = execution_entry(
            stored[_id(entry["plan_entry_id"], "plan_entry_id")], binding)
        territory, output_id = execution_scope(stored_entry["payload"])
        start = _instant(entry["scheduled_at"], "scheduled_at")
        duration = stored_entry.get("planned_duration_ms")
        end = start + timedelta(milliseconds=duration if isinstance(duration, int) and duration > 0 else 1)
        readiness = readiness_at(store, station_id, entry["version_id"],
                                 slot_start=start, slot_end=end,
                                 endpoint_id=endpoint_id,
                                 require_endpoint_copy=False,
                                 territory=territory, output_id=output_id)
        if not readiness["ready"]:
            raise Conflict("planned slot cannot be distributed: " +
                           ", ".join(readiness["blocked_reasons"]))
    from .production import production_snapshot
    production = production_snapshot(store, station_id, revision_id)
    expected_production = {"placements": production["placements"],
                           "execution_capability": production["execution_capability"]}
    supplied_production = snapshot.get("production")
    if supplied_production != expected_production and not (
            not production["placements"] and supplied_production is None):
        raise StoreError("snapshot production data differs from approved plan")
    for asset in production["media"]:
        if manifest.get(asset["media_file_id"]) != asset:
            raise StoreError("snapshot omits or changes production media")
    return plan, revision, manifest


def _copies_ready(store: Store, endpoint_id: str, snapshot: Mapping[str, Any]) -> None:
    for raw in snapshot["media"]:
        media_id = _id(raw["media_file_id"], "media_file_id")
        rows = store.lookup("endpoint_copies", "endpoint_file", endpoint_id, media_id)
        if not rows or rows[0].get("status") != "verified" or \
                rows[0].get("sha256") != raw["sha256"] or not rows[0].get("verified_at"):
            raise Conflict(f"endpoint has no verified copy of media {media_id}")


def prepare_distribution(store: Store, principal: Principal, station_id: str,
                         endpoint_id: str, revision_id: str,
                         snapshot: Mapping[str, Any], *, now: datetime | str | None = None) -> dict:
    """Queue an immutable snapshot; central verified media is enough here."""
    station_id, endpoint_id, revision_id = (_id(station_id, "station_id"),
                                            _id(endpoint_id, "endpoint_id"),
                                            _id(revision_id, "revision_id"))
    require(store, principal, "activate", station_id)
    _endpoint(store, station_id, endpoint_id)
    sequence = store.sequence
    _snapshot_matches(store, station_id, endpoint_id, revision_id, snapshot)
    digest = _sha(snapshot)
    existing = store.lookup("plan_distributions", "endpoint_revision", endpoint_id, revision_id)
    if existing:
        row = existing[0]
        if row.get("payload_hash") != digest:
            raise Conflict("distribution identity was reused with changed snapshot")
        return {"distribution_id": row["_id"], "status": row["status"],
                "payload_hash": digest, "replayed": True}
    plan = store.get("plans", _id(snapshot["plan_id"], "plan_id"))
    distribution_id = str(uuid4())
    row = {"station_id": station_id, "endpoint_id": endpoint_id,
           "revision_id": revision_id, "plan_id": plan["_id"],
           "service_date": plan["service_date"], "status": "pending",
           "activate_after": snapshot["activate_after"],
           "snapshot": dict(snapshot), "payload_hash": digest,
           "queued_at": _iso(_now(now)), "sent_at": None,
           "acknowledged_at": None, "error_code": None}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("plan_distributions", distribution_id, row)
    tx.commit()
    return {"distribution_id": distribution_id, "status": "pending",
            "payload_hash": digest, "replayed": False}


def plan_state(store: Store, principal: Principal, station_id: str,
               endpoint_id: str, *, now: datetime | str | None = None) -> dict:
    """Return endpoint-scoped active ownership and its next deliverable plan."""
    station_id, endpoint_id = _id(station_id, "station_id"), _id(endpoint_id, "endpoint_id")
    _require_endpoint(store, principal, station_id, endpoint_id)
    _endpoint(store, station_id, endpoint_id)
    acceptance = store.get("active_acceptances", endpoint_id)
    pending = []
    for row in store.lookup("plan_distributions", "endpoint", endpoint_id):
        if row.get("endpoint_id") != endpoint_id or row.get("status") not in {"pending", "sent"}:
            continue
        plan = store.get("plans", row["plan_id"])
        revision = store.get("plan_revisions", row["revision_id"])
        if (plan is not None and revision is not None and
                plan.get("station_id") == station_id and
                plan.get("current_revision_id") == row["revision_id"] and
                revision.get("status") == "approved"):
            pending.append(row)
    # The endpoint prefetches future media but waits for activate_after before
    # accepting and switching the active manifest.
    pending.sort(key=lambda row: (row.get("activate_after", ""), row.get("queued_at", "")))
    selected = pending[0] if pending else None
    return {"station_id": station_id, "endpoint_id": endpoint_id,
            "service_key": (store.get("plans", selected["plan_id"])["service_key"]
                            if selected else None),
            "accepted_revision_id": acceptance.get("revision_id") if acceptance and acceptance.get("state", "active") == "active" else None,
            "engine_instance": acceptance.get("engine_instance") if acceptance and acceptance.get("state", "active") == "active" else None,
            "engine_epoch": acceptance.get("engine_epoch") if acceptance else None,
            "pending_revision_id": selected.get("revision_id") if selected else None,
            "pending_payload_hash": selected.get("payload_hash") if selected else None,
            "pending_activate_after": selected.get("activate_after") if selected else None}


def fetch_snapshot(store: Store, principal: Principal, station_id: str,
                   endpoint_id: str, revision_id: str) -> dict:
    station_id, endpoint_id, revision_id = (_id(station_id, "station_id"),
                                            _id(endpoint_id, "endpoint_id"),
                                            _id(revision_id, "revision_id"))
    _require_endpoint(store, principal, station_id, endpoint_id)
    _endpoint(store, station_id, endpoint_id)
    rows = store.lookup("plan_distributions", "endpoint_revision", endpoint_id, revision_id)
    if not rows or rows[0]["status"] not in {"pending", "sent", "accepted"}:
        raise StoreError("distribution is absent or unavailable")
    _snapshot_matches(store, station_id, endpoint_id, revision_id, rows[0]["snapshot"])
    return rows[0]["snapshot"]


def _next_day_offline_grant(store: Store, station_id: str, endpoint_id: str,
                            delivery: Mapping[str, Any], now: datetime) -> dict | None:
    """Authorize only the next local day for the current, recently seen owner."""
    acceptance = store.get("active_acceptances", endpoint_id)
    if acceptance is None or acceptance.get("state", "active") != "active" or \
            acceptance.get("station_id") != station_id or \
            not isinstance(acceptance.get("engine_instance"), str) or \
            not acceptance["engine_instance"].strip() or \
            not acceptance.get("heartbeat_at"):
        return None
    seen_at = _instant(acceptance.get("heartbeat_at"), "heartbeat_at")
    if not timedelta(0) <= now - seen_at <= _OFFLINE_HEARTBEAT_MAX_AGE:
        return None
    active_rows = store.lookup("plan_distributions", "endpoint_revision",
                               endpoint_id, acceptance["revision_id"])
    if not active_rows or active_rows[0].get("status") != "accepted" or \
            active_rows[0].get("payload_hash") != acceptance.get("payload_hash"):
        return None
    active_day = date.fromisoformat(active_rows[0]["service_date"])
    target_day = date.fromisoformat(delivery["service_date"])
    if target_day != active_day + timedelta(days=1):
        return None
    station = store.get("stations", station_id)
    if station is None:
        return None
    zone = ZoneInfo(station["timezone"])
    if now.astimezone(zone).date() != active_day:
        return None
    activate_after = _instant(delivery["activate_after"], "activate_after")
    expires_at = _local_day_start(target_day + timedelta(days=1), zone)
    if now >= activate_after or expires_at <= activate_after:
        return None
    return {"grant_version": 1, "grant_id": str(uuid4()), "station_id": station_id,
            "endpoint_id": endpoint_id, "plan_revision_id": delivery["revision_id"],
            "snapshot_sha256": delivery["payload_hash"],
            "base_revision_id": acceptance["revision_id"],
            "base_engine_epoch": acceptance["engine_epoch"],
            "engine_instance": acceptance["engine_instance"],
            "engine_epoch": acceptance["engine_epoch"] + 1,
            "activate_after": _iso(activate_after),
            "expires_at": _iso(expires_at), "issued_at": _iso(now)}


def acknowledge_delivery(store: Store, principal: Principal, station_id: str,
                         endpoint_id: str, revision_id: str, snapshot_sha256: str,
                         *, now: datetime | str | None = None) -> dict:
    """Mark delivery only after endpoint copy records match the exact snapshot."""
    station_id, endpoint_id, revision_id = (_id(station_id, "station_id"),
                                            _id(endpoint_id, "endpoint_id"),
                                            _id(revision_id, "revision_id"))
    _require_endpoint(store, principal, station_id, endpoint_id)
    _endpoint(store, station_id, endpoint_id)
    rows = store.lookup("plan_distributions", "endpoint_revision", endpoint_id, revision_id)
    if not rows:
        raise StoreError("distribution not found")
    row = rows[0]
    sequence = store.sequence
    if row["payload_hash"] != snapshot_sha256 or _sha(row["snapshot"]) != snapshot_sha256:
        raise Conflict("snapshot digest differs from queued distribution")
    _snapshot_matches(store, station_id, endpoint_id, revision_id, row["snapshot"])
    _copies_ready(store, endpoint_id, row["snapshot"])
    if row["status"] not in {"pending", "sent", "accepted"}:
        raise Conflict("distribution cannot be acknowledged")
    moment = _now(now)
    grant = row.get("offline_grant")
    if grant is None and row["status"] != "accepted":
        grant = _next_day_offline_grant(store, station_id, endpoint_id, row, moment)
    if row["status"] == "pending" or grant is not None and row.get("offline_grant") is None:
        updated = _data(row) | {"status": "sent" if row["status"] == "pending" else row["status"]}
        if row["status"] == "pending":
            updated.update(sent_at=_iso(moment), acknowledged_at=_iso(moment))
        if grant is not None:
            updated["offline_grant"] = grant
        tx = store.begin(actor=principal.user_id)
        tx.expect_sequence(sequence)
        tx.put("plan_distributions", row["_id"], updated,
               expected_revision=row["_rev"])
        tx.commit()
    return {"distribution_id": row["_id"],
            "status": "sent" if row["status"] == "pending" else row["status"],
            "replayed": row["status"] != "pending", "offline_grant": grant}


def accept_plan(store: Store, principal: Principal, station_id: str,
                endpoint_id: str, revision_id: str, engine_instance: str,
                engine_epoch: int, *, now: datetime | str | None = None) -> dict:
    """Fence the old engine and activate one delivered revision at service start."""
    station_id, endpoint_id, revision_id = (_id(station_id, "station_id"),
                                            _id(endpoint_id, "endpoint_id"),
                                            _id(revision_id, "revision_id"))
    _require_endpoint(store, principal, station_id, endpoint_id)
    _endpoint(store, station_id, endpoint_id)
    sequence = store.sequence
    if not isinstance(engine_instance, str) or not engine_instance.strip() or \
            isinstance(engine_epoch, bool) or not isinstance(engine_epoch, int) or engine_epoch < 1:
        raise StoreError("engine instance and positive epoch are required")
    rows = store.lookup("plan_distributions", "endpoint_revision", endpoint_id, revision_id)
    if not rows or rows[0]["status"] not in {"sent", "accepted"}:
        raise Conflict("plan has not been delivered to this endpoint")
    distribution = rows[0]
    _snapshot_matches(store, station_id, endpoint_id, revision_id, distribution["snapshot"])
    if _now(now) < _instant(distribution["activate_after"], "activate_after"):
        raise Conflict("service day has not reached its activation instant")
    if _sha(distribution["snapshot"]) != distribution["payload_hash"]:
        raise Conflict("distributed snapshot changed")
    _copies_ready(store, endpoint_id, distribution["snapshot"])
    current = store.get("active_acceptances", endpoint_id)
    if current is not None:
        if engine_epoch < current["engine_epoch"] or \
                (current.get("state", "active") != "active" and engine_epoch <= current["engine_epoch"]):
            raise Conflict("engine epoch moved backwards")
        if engine_epoch == current["engine_epoch"]:
            if current["engine_instance"] != engine_instance or current["revision_id"] != revision_id:
                raise Conflict("same epoch is already owned by another engine or revision")
            return {"accepted_revision_id": revision_id, "engine_epoch": engine_epoch,
                    "engine_instance": engine_instance, "replayed": True}
    timestamp = _iso(_now(now))
    acceptance = {"station_id": station_id, "endpoint_id": endpoint_id,
                  "revision_id": revision_id, "engine_instance": engine_instance,
                  "engine_epoch": engine_epoch, "accepted_at": timestamp,
                  "heartbeat_at": timestamp, "payload_hash": distribution["payload_hash"],
                  "state": "active"}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("active_acceptances", endpoint_id, acceptance,
           expected_revision=current["_rev"] if current else None)
    if distribution["status"] != "accepted":
        tx.put("plan_distributions", distribution["_id"],
               _data(distribution) | {"status": "accepted", "acknowledged_at": timestamp},
               expected_revision=distribution["_rev"])
    tx.commit()
    return {"accepted_revision_id": revision_id, "engine_epoch": engine_epoch,
            "engine_instance": engine_instance, "replayed": False}


def heartbeat_acceptance(store: Store, principal: Principal, station_id: str,
                         endpoint_id: str, revision_id: str, engine_instance: str,
                         engine_epoch: int, *, now: datetime | str | None = None) -> dict:
    station_id, endpoint_id, revision_id = (_id(station_id, "station_id"),
                                            _id(endpoint_id, "endpoint_id"),
                                            _id(revision_id, "revision_id"))
    _require_endpoint(store, principal, station_id, endpoint_id)
    current = _active_acceptance(store, station_id, endpoint_id,
                                 engine_instance, engine_epoch)
    if current["revision_id"] != revision_id:
        raise Conflict("heartbeat belongs to an older plan revision")
    updated = _data(current) | {"heartbeat_at": _iso(_now(now))}
    store.put("active_acceptances", endpoint_id, updated,
              expected_revision=current["_rev"], actor=principal.user_id)
    return {"accepted_revision_id": revision_id, "engine_epoch": engine_epoch,
            "heartbeat_at": updated["heartbeat_at"]}


def _active_acceptance(store: Store, station_id: str, endpoint_id: str,
                       engine_instance: str | None = None,
                       engine_epoch: int | None = None) -> dict:
    current = store.get("active_acceptances", endpoint_id)
    if current is None or current.get("station_id") != station_id or \
            current.get("state", "active") != "active":
        raise Conflict("endpoint has no active plan acceptance")
    if engine_instance is not None and (current["engine_instance"] != engine_instance or
                                       current["engine_epoch"] != engine_epoch):
        raise Conflict("engine instance or epoch is fenced")
    return current


def _command_payload(store: Store, acceptance: dict, command_kind: str,
                     payload: Mapping[str, Any]) -> dict:
    body = dict(payload)
    revision_id = acceptance["revision_id"]
    if command_kind in _REVISION_COMMANDS:
        if body.get("plan_revision_id") not in (None, revision_id):
            raise Conflict("command addresses an inactive plan revision")
        body["plan_revision_id"] = revision_id
    if command_kind in {"start", "skip"}:
        entry_id = _id(body.get("plan_entry_id"), "plan_entry_id")
        distribution = store.lookup("plan_distributions", "endpoint_revision",
                                    acceptance["endpoint_id"], revision_id)
        if not distribution or distribution[0]["status"] != "accepted":
            raise Conflict("active plan snapshot is unavailable")
        entry = next((row for row in distribution[0]["snapshot"]["entries"]
                      if row.get("plan_entry_id") == entry_id), None)
        if entry is None:
            raise Conflict("command plan entry is not in the accepted snapshot")
        for key in ("version_id", "media_file_id"):
            if body.get(key) is not None and body[key] != entry.get(key):
                raise Conflict(f"command {key} differs from accepted snapshot")
            if entry.get(key) is not None:
                body[key] = entry[key]
        body["plan_entry_id"] = entry_id
    return body


def queue_command(store: Store, principal: Principal, station_id: str,
                  endpoint_id: str, command_kind: str, payload: Mapping[str, Any],
                  command_key: str, *, ttl_seconds: int = 30,
                  now: datetime | str | None = None) -> dict:
    """Queue an expiring command; start always identifies its plan entry."""
    station_id, endpoint_id = _id(station_id, "station_id"), _id(endpoint_id, "endpoint_id")
    require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    if principal.scope != "user":
        raise Conflict("endpoint credentials cannot issue their own commands")
    _endpoint(store, station_id, endpoint_id)
    if not isinstance(command_kind, str) or command_kind not in _COMMAND_KINDS or not isinstance(payload, Mapping):
        raise StoreError("unsupported command kind or payload")
    if not isinstance(command_key, str) or not command_key.strip() or len(command_key) > 200:
        raise StoreError("stable command_key is required")
    if isinstance(ttl_seconds, bool) or not isinstance(ttl_seconds, int) or \
            not 1 <= ttl_seconds <= 300:
        raise StoreError("command TTL must be 1..300 seconds")
    active = _active_acceptance(store, station_id, endpoint_id)
    body = _command_payload(store, active, command_kind, payload)
    prior = store.lookup("engine_commands", "endpoint_key", endpoint_id, command_key)
    if prior:
        row = prior[0]
        if (row["command_kind"] != command_kind or row["payload"] != body or
                row["plan_revision_id"] != active["revision_id"]):
            raise Conflict("command idempotency key was reused with different intent")
        return {"command_id": row["_id"], "state": row["state"], "replayed": True}
    issued = _now(now)
    command_id = str(uuid4())
    record = {"station_id": station_id, "endpoint_id": endpoint_id,
              "plan_revision_id": active["revision_id"],
              "plan_entry_id": body.get("plan_entry_id"),
              "command_key": command_key, "command_kind": command_kind,
              "payload": body, "engine_instance": active["engine_instance"],
              "engine_epoch": active["engine_epoch"], "state": "pending",
              "issued_at": _iso(issued),
              "expires_at": _iso(issued + timedelta(seconds=ttl_seconds)),
              "acknowledged_at": None, "executed_at": None,
              "error_code": None}
    store.put("engine_commands", command_id, record, actor=principal.user_id)
    return {"command_id": command_id, "state": "pending", "replayed": False}


def poll_commands(store: Store, principal: Principal, station_id: str,
                  endpoint_id: str, engine_instance: str, engine_epoch: int,
                  *, limit: int = 100, now: datetime | str | None = None) -> list[dict]:
    station_id, endpoint_id = _id(station_id, "station_id"), _id(endpoint_id, "endpoint_id")
    _require_endpoint(store, principal, station_id, endpoint_id)
    _endpoint(store, station_id, endpoint_id)
    if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= 1000:
        raise StoreError("limit must be 1..1000")
    active = _active_acceptance(store, station_id, endpoint_id,
                                engine_instance, engine_epoch)
    instant = _now(now)
    commands = [row for row in store.scan("engine_commands")
                if row.get("endpoint_id") == endpoint_id and row.get("state") == "pending"]
    deliverable = []
    for row in commands:
        stale = (row["engine_instance"] != active["engine_instance"] or
                 row["engine_epoch"] != active["engine_epoch"] or
                 row["plan_revision_id"] != active["revision_id"] or
                 _instant(row["expires_at"], "expires_at") <= instant)
        if stale:
            try:
                store.put("engine_commands", row["_id"],
                          _data(row) | {"state": "expired", "error_code": "ttl_or_epoch"},
                          expected_revision=row["_rev"], actor=principal.user_id)
            except Conflict:
                pass
            continue
        deliverable.append(row)
    deliverable.sort(key=lambda row: (row["issued_at"], row["_id"]))
    return [{"id": row["_id"], "command_kind": row["command_kind"],
             "payload": row["payload"], "engine_epoch": row["engine_epoch"],
             "issued_at": row["issued_at"], "expires_at": row["expires_at"]}
            for row in deliverable[:limit]]


def acknowledge_command(store: Store, principal: Principal, station_id: str,
                        endpoint_id: str, command_id: str, engine_instance: str,
                        engine_epoch: int, executed: bool,
                        *, now: datetime | str | None = None) -> dict:
    """Only pending→accepted→executed; repeated matching acks are idempotent."""
    station_id, endpoint_id, command_id = (_id(station_id, "station_id"),
                                           _id(endpoint_id, "endpoint_id"),
                                           _id(command_id, "command_id"))
    _require_endpoint(store, principal, station_id, endpoint_id)
    _endpoint(store, station_id, endpoint_id)
    if not isinstance(executed, bool):
        raise StoreError("executed must be boolean")
    active = _active_acceptance(store, station_id, endpoint_id,
                                engine_instance, engine_epoch)
    row = store.get("engine_commands", command_id)
    if row is None or row["endpoint_id"] != endpoint_id or row["station_id"] != station_id:
        raise StoreError("command not found in endpoint")
    if (row["engine_instance"] != engine_instance or row["engine_epoch"] != engine_epoch or
            row["plan_revision_id"] != active["revision_id"]):
        raise Conflict("command was fenced by a new engine or plan")
    if row["state"] == "executed" and not executed:
        return {"command_id": command_id, "state": "executed", "replayed": True}
    if row["state"] == ("executed" if executed else "accepted"):
        return {"command_id": command_id, "state": row["state"], "replayed": True}
    if executed and row["state"] != "accepted":
        raise Conflict("execution requires a preceding accepted acknowledgement")
    if not executed and row["state"] != "pending":
        raise Conflict("command cannot be accepted from this state")
    instant = _now(now)
    if row["state"] == "pending" and _instant(row["expires_at"], "expires_at") <= instant:
        tx = store.begin(actor=principal.user_id)
        tx.expect("active_acceptances", endpoint_id, active["_rev"])
        tx.put("engine_commands", command_id,
               _data(row) | {"state": "expired", "error_code": "ttl"},
               expected_revision=row["_rev"])
        tx.commit()
        raise Conflict("command expired before acceptance")
    target = "executed" if executed else "accepted"
    changes = ({"state": target, "executed_at": _iso(instant)} if executed else
               {"state": target, "acknowledged_at": _iso(instant)})
    tx = store.begin(actor=principal.user_id)
    tx.expect("active_acceptances", endpoint_id, active["_rev"])
    tx.put("engine_commands", command_id, _data(row) | changes,
           expected_revision=row["_rev"])
    tx.commit()
    return {"command_id": command_id, "state": target, "replayed": False}


def reject_command(store: Store, principal: Principal, station_id: str,
                   endpoint_id: str, command_id: str, engine_instance: str,
                   engine_epoch: int, reason: str,
                   *, now: datetime | str | None = None) -> dict:
    station_id, endpoint_id, command_id = (_id(station_id, "station_id"),
                                           _id(endpoint_id, "endpoint_id"),
                                           _id(command_id, "command_id"))
    _require_endpoint(store, principal, station_id, endpoint_id)
    active = _active_acceptance(store, station_id, endpoint_id,
                                engine_instance, engine_epoch)
    row = store.get("engine_commands", command_id)
    if row is None or row["endpoint_id"] != endpoint_id or \
            row["engine_instance"] != engine_instance or row["engine_epoch"] != engine_epoch:
        raise StoreError("command not found in current engine epoch")
    if not isinstance(reason, str) or not reason.strip():
        raise StoreError("rejection reason is required")
    if row["state"] == "rejected" and row.get("error_code") == reason:
        return {"command_id": command_id, "state": "rejected", "replayed": True}
    if row["state"] not in {"pending", "accepted"}:
        raise Conflict("terminal command cannot be rejected")
    tx = store.begin(actor=principal.user_id)
    tx.expect("active_acceptances", endpoint_id, active["_rev"])
    tx.put("engine_commands", command_id,
           _data(row) | {"state": "rejected", "error_code": reason,
                         "acknowledged_at": _iso(_now(now))},
           expected_revision=row["_rev"])
    tx.commit()
    return {"command_id": command_id, "state": "rejected", "replayed": False}


def _next_attempt_state(prior: dict | None, phase: str) -> str:
    if prior is None:
        if phase == "attempt":
            return "pending"
        if phase in {"start", "manual_start"}:
            return "started"
        if phase == "fail":
            return "failed"
        if phase == "skip":
            return "skipped"
        raise Conflict("completion or interruption requires a start event")
    state = prior["state"]
    if state == "pending":
        if phase == "attempt" and prior.get("last_phase") != "attempt":
            return "pending"
        if phase in {"start", "manual_start"}:
            return "started"
        if phase == "fail":
            return "failed"
        if phase == "skip":
            return "skipped"
    elif state == "started":
        if phase == "manual_complete" and prior.get("last_phase") != "manual_start":
            raise Conflict("manual completion requires a manual start")
        if phase == "complete" and prior.get("last_phase") != "start":
            raise Conflict("completion requires a normal start")
        if phase in {"complete", "manual_complete"}:
            return "completed"
        if phase == "fail":
            return "failed"
        if phase == "interrupt":
            return "interrupted"
    raise Conflict("attempt phase cannot follow its current state")


def record_playout_event(store: Store, principal: Principal,
                         payload: Mapping[str, Any], *,
                         now: datetime | str | None = None) -> dict:
    """Append one engine event and advance its exact attempt atomically.

    Planned retries use the same plan_entry_id with incremented attempt_no.
    Off-plan manual phases must carry stable ``details.attempt_key`` so their
    start and completion can be grouped without guessing by time or version.
    """
    if not isinstance(payload, Mapping):
        raise StoreError("playout event payload must be an object")
    sequence = store.sequence
    station_id = _id(payload.get("station_id"), "station_id")
    endpoint_id = _id(payload.get("endpoint_id"), "endpoint_id")
    version_id = _id(payload.get("version_id"), "version_id")
    _require_endpoint(store, principal, station_id, endpoint_id)
    _endpoint(store, station_id, endpoint_id, allow_retired=True)
    if store.get("versions", version_id) is None:
        raise StoreError("event version is absent")
    phase = payload.get("event_type", payload.get("phase"))
    if not isinstance(phase, str) or phase not in _PHASES:
        raise StoreError("unsupported playout phase")
    event_id = payload.get("engine_event_id")
    if not isinstance(event_id, str) or not event_id.strip() or len(event_id) > 200:
        raise StoreError("stable engine_event_id is required")
    event_key = f"{endpoint_id}:{event_id}"
    occurred = _instant(payload.get("occurred_at"), "occurred_at")
    attempt_no = payload.get("attempt_no", 1)
    if isinstance(attempt_no, bool) or not isinstance(attempt_no, int) or attempt_no < 1:
        raise StoreError("attempt_no must be positive")
    details = payload.get("details", {})
    if not isinstance(details, Mapping):
        raise StoreError("event details must be an object")
    details = dict(details)
    plan_entry_id = (_id(payload["plan_entry_id"], "plan_entry_id")
                     if payload.get("plan_entry_id") else None)
    media_file_id = (_id(payload["media_file_id"], "media_file_id")
                     if payload.get("media_file_id") else None)
    # A duplicate is evidence already committed by this endpoint. Allow its
    # exact replay after a restore or ownership change without creating a new
    # event or requiring its historical delivery to remain active.
    earlier = store.lookup("playout_events", "event_key", event_key)
    if earlier:
        old = earlier[0]
        old_details = old.get("details")
        details_match = old_details == details or (
            "output_id" not in details and isinstance(old_details, dict) and
            {key: value for key, value in old_details.items()
             if key != "output_id"} == details)
        identity = {"station_id": station_id, "endpoint_id": endpoint_id,
                    "version_id": version_id, "plan_entry_id": plan_entry_id,
                    "media_file_id": media_file_id, "attempt_no": attempt_no,
                    "event_key": event_key, "engine_event_id": event_id,
                    "phase": phase, "occurred_at": _iso(occurred)}
        if not details_match or any(old.get(key) != value for key, value in identity.items()):
            raise Conflict("engine_event_id was reused with changed evidence")
        return {"event_id": old["_id"], "attempt_id": old["attempt_id"],
                "phase": old["phase"], "replayed": True}
    accepted = None
    binding = None
    if plan_entry_id is not None:
        entry = store.get("plan_entries", plan_entry_id)
        if entry is None or entry.get("payload", {}).get("version_id") != version_id:
            raise Conflict("event version does not match planned entry")
        revision = store.get("plan_revisions", entry["plan_revision_id"])
        plan = store.get("plans", revision["plan_id"]) if revision else None
        if plan is None or plan.get("station_id") != station_id:
            raise Conflict("event plan entry belongs to another station")
        binding = require_approved_execution_binding(store, station_id,
                    plan["service_key"], revision, endpoint_id,
                    allow_disabled_station=True)
        execution_entry(entry, binding)
        deliveries = store.lookup("plan_distributions", "endpoint_revision",
                                  endpoint_id, revision["_id"])
        accepted = next((row for row in deliveries if row.get("status") == "accepted"), None)
        if accepted is None or accepted.get("payload_hash") != _sha(accepted["snapshot"]):
            raise Conflict("planned event has no accepted endpoint snapshot")
        frozen = next((row for row in accepted["snapshot"].get("entries", [])
                       if row.get("plan_entry_id") == plan_entry_id), None)
        if frozen is None or frozen.get("version_id") != version_id or \
                media_file_id is not None and frozen.get("media_file_id") != media_file_id:
            raise Conflict("planned event differs from accepted endpoint snapshot")
        planned_output = (frozen.get("payload") or {}).get("output_id")
        claimed_output = details.get("output_id")
        if planned_output and claimed_output not in (None, planned_output):
            raise Conflict("event output differs from accepted plan output")
        if planned_output:
            details["output_id"] = planned_output
        accepted_snapshot_sha256 = accepted["payload_hash"]
        attempt_key = f"{plan_entry_id}:{attempt_no}"
    else:
        accepted_snapshot_sha256 = None
        attempt_key = details.get("attempt_key")
        if not isinstance(attempt_key, str) or not attempt_key.strip() or len(attempt_key) > 200:
            raise StoreError("off-plan event needs stable details.attempt_key")
        service_rows = store.lookup("broadcast_services", "endpoint", endpoint_id)
        if service_rows:
            binding = require_service(store, station_id,
                                      service_rows[0]["service_key"], endpoint_id)
            if details.get("output_id") not in (None, binding["output_id"]):
                raise Conflict("manual event output differs from bound service")
            details["output_id"] = binding["output_id"]
    if details.get("output_id") is not None:
        from .station_outputs import lookup_output
        output_id = details["output_id"]
        if not isinstance(output_id, str) or not output_id:
            raise StoreError("event output_id must be a nonempty string")
        station_output = lookup_output(store, station_id, endpoint_id, output_id)
        if station_output is None or station_output["status"] != "active":
            raise Conflict("event output is not active on its endpoint")
    if media_file_id is not None:
        file = store.get("media_files", media_file_id)
        if file is None or file.get("version_id") != version_id:
            raise Conflict("event media representation differs from version")
    core = {"station_id": station_id, "endpoint_id": endpoint_id,
            "version_id": version_id, "plan_entry_id": plan_entry_id,
            "media_file_id": media_file_id, "attempt_no": attempt_no,
            "event_key": event_key, "engine_event_id": event_id,
            "phase": phase, "occurred_at": _iso(occurred), "details": details,
            "accepted_snapshot_sha256": accepted_snapshot_sha256}
    attempts = store.lookup("playout_attempts", "endpoint_key", endpoint_id, attempt_key)
    prior = attempts[0] if attempts else None
    if prior is not None and (prior["station_id"] != station_id or
                              prior["version_id"] != version_id or
                              prior.get("plan_entry_id") != plan_entry_id or
                              prior.get("media_file_id") != media_file_id or
                              prior["attempt_no"] != attempt_no):
        raise Conflict("attempt key was reused for different playout identity")
    state = _next_attempt_state(prior, phase)
    if prior is not None and occurred < _instant(prior["last_event_at"], "last_event_at"):
        raise Conflict("event phase time precedes prior phase time")
    attempt_id = prior["_id"] if prior else str(uuid4())
    event_record_id = str(uuid4())
    timestamp = _iso(_now(now))
    attempt = {"station_id": station_id, "endpoint_id": endpoint_id,
               "attempt_key": attempt_key, "version_id": version_id,
               "plan_entry_id": plan_entry_id, "media_file_id": media_file_id,
               "attempt_no": attempt_no, "state": state, "last_phase": phase,
               "last_event_at": _iso(occurred),
               "created_at": prior["created_at"] if prior else timestamp,
               "updated_at": timestamp, "last_event_id": event_record_id}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    if accepted is not None:
        tx.expect("plan_distributions", accepted["_id"], accepted["_rev"])
    if binding is not None:
        service_id = binding.get("broadcast_service_id", binding.get("_id"))
        if service_id is not None:
            service = store.get("broadcast_services", service_id)
            tx.expect("broadcast_services", service_id, service["_rev"])
        if binding.get("station_output_id") is not None:
            output = store.get("station_outputs", binding["station_output_id"])
            tx.expect("station_outputs", output["_id"], output["_rev"])
    tx.put("playout_attempts", attempt_id, attempt,
           expected_revision=prior["_rev"] if prior else None)
    tx.put("playout_events", event_record_id,
           core | {"attempt_id": attempt_id, "received_at": timestamp})
    tx.commit()
    return {"event_id": event_record_id, "attempt_id": attempt_id,
            "phase": phase, "attempt_state": state, "replayed": False}


def record_output_observation(store: Store, principal: Principal,
                              station_id: str, endpoint_id: str,
                              observation_key: str, kind: str,
                              occurred_at: datetime | str, *,
                              event_id: str | None = None,
                              observer_kind: str = "endpoint_self_report",
                              evidence_ref: str | None = None,
                              details: Mapping[str, Any] | None = None,
                              now: datetime | str | None = None) -> dict:
    """Append a separately labelled output claim; no acoustic proof is inferred."""
    station_id, endpoint_id = _id(station_id, "station_id"), _id(endpoint_id, "endpoint_id")
    if observer_kind == "endpoint_self_report":
        _require_endpoint(store, principal, station_id, endpoint_id)
    elif observer_kind == "external_monitor":
        if (principal.scope != "monitor" or principal.endpoint_id != endpoint_id or
                not principal.monitor_id):
            raise Forbidden("external observation needs its bound monitor credential")
    else:
        raise StoreError("unsupported observer_kind")
    _endpoint(store, station_id, endpoint_id, allow_retired=True)
    if not isinstance(observation_key, str) or not observation_key.strip() or len(observation_key) > 200:
        raise StoreError("stable observation_key is required")
    if not isinstance(kind, str) or kind not in _OBSERVATIONS:
        raise StoreError("unsupported output observation kind")
    if observer_kind == "external_monitor" and \
            (not isinstance(evidence_ref, str) or not evidence_ref.strip() or
             len(evidence_ref) > 1000):
        raise StoreError("external monitor claim needs an evidence_ref of at most 1000 characters")
    if details is not None and not isinstance(details, Mapping):
        raise StoreError("observation details must be an object")
    if observer_kind == "external_monitor":
        from .station_outputs import lookup_output
        output_id = (details or {}).get("output_id")
        if not isinstance(output_id, str) or not output_id:
            raise StoreError("external observation needs an output_id")
        output = lookup_output(store, station_id, endpoint_id, output_id)
        if output is None or output["status"] != "active":
            raise Conflict("external observation output is absent or inactive")
    event = store.get("playout_events", _id(event_id, "event_id")) if event_id else None
    if event_id and (event is None or event["endpoint_id"] != endpoint_id or
                     event["station_id"] != station_id):
        raise Conflict("observation event belongs to another endpoint")
    key = (f"{endpoint_id}:{principal.monitor_id}:{observation_key}"
           if observer_kind == "external_monitor" else
           f"{endpoint_id}:{observation_key}")
    core = {"station_id": station_id, "endpoint_id": endpoint_id,
            "observation_key": key, "kind": kind,
            "occurred_at": _iso(_instant(occurred_at, "occurred_at")),
            "event_id": event["_id"] if event else None,
            "attempt_id": event["attempt_id"] if event else None,
            "observer_kind": observer_kind, "evidence_ref": evidence_ref,
            "details": dict(details or {})}
    if observer_kind == "external_monitor":
        core["observer_id"] = principal.monitor_id
    previous = store.lookup("output_observations", "observation_key", key)
    if previous:
        old = previous[0]
        if any(old.get(field) != value for field, value in core.items()):
            raise Conflict("observation_key was reused with changed evidence")
        return {"observation_id": old["_id"], "replayed": True,
                "evidence_level": observer_kind}
    observation_id = str(uuid4())
    store.put("output_observations", observation_id,
              core | {"received_at": _iso(_now(now))},
              actor=principal.monitor_id or principal.user_id)
    return {"observation_id": observation_id, "replayed": False,
            "evidence_level": observer_kind}
