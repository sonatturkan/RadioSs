"""Fenced, short-lived live playout changes with separate execution evidence.

An override addresses one entry in an endpoint's accepted approved snapshot.
The approved plan remains immutable. Acknowledgement and execution are endpoint
claims; the final evidence link checks recorded playout attempts but does not
prove that sound reached the audience. An endpoint adapter must implement the
poll/ack/result wire flow before these records can drive a playout process.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
from hashlib import sha256
import json
from typing import Any, Mapping
from uuid import UUID, uuid4

from .identity import Forbidden, Principal, require
from .readiness import readiness_at
from .storage import Conflict, Store, StoreError


def _id(value: Any, name: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def _instant(value: Any, name: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00")) if isinstance(value, str) else value
    except ValueError as exc:
        raise StoreError(f"{name} must be an ISO instant") from exc
    if not isinstance(parsed, datetime) or parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError(f"{name} needs a timezone offset")
    return parsed.astimezone(timezone.utc)


def _now(value: datetime | str | None) -> datetime:
    return datetime.now(timezone.utc) if value is None else _instant(value, "now")


def _body(row: dict) -> dict:
    return {key: value for key, value in row.items() if key not in ("_id", "_rev")}


def _hash(value: Any) -> str:
    try:
        data = json.dumps(value, ensure_ascii=False, sort_keys=True,
                          separators=(",", ":"), allow_nan=False).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise StoreError("live override intent must be finite JSON") from exc
    return sha256(data).hexdigest()


def _text(value: Any, name: str, limit: int = 200) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > limit:
        raise StoreError(f"{name} must be nonempty text up to {limit} characters")
    return value.strip()


def _engine_identity(engine_instance: Any, engine_epoch: Any) -> tuple[str, int]:
    instance = _text(engine_instance, "engine_instance")
    if type(engine_epoch) is not int or engine_epoch < 0:
        raise StoreError("engine_epoch must be a nonnegative integer")
    return instance, engine_epoch


def _endpoint_user(store: Store, principal: Principal, station_id: str,
                   endpoint_id: str) -> None:
    require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    if principal.scope != "user":
        raise Forbidden("endpoint credentials cannot propose live overrides")


def _endpoint_agent(store: Store, principal: Principal, station_id: str,
                    endpoint_id: str) -> None:
    require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    if principal.scope != "endpoint" or principal.endpoint_id != endpoint_id:
        raise Forbidden("live override report requires its bound endpoint credential")


def _active(store: Store, station_id: str, endpoint_id: str) -> tuple[dict, dict, dict]:
    endpoint = store.get("endpoints", endpoint_id)
    if endpoint is None or endpoint["station_id"] != station_id or endpoint["status"] == "retired":
        raise StoreError("endpoint is outside station or retired")
    acceptance = store.get("active_acceptances", endpoint_id)
    if acceptance is None or acceptance["station_id"] != station_id or \
            acceptance.get("state", "active") != "active":
        raise Conflict("endpoint has no active accepted plan")
    revision = store.get("plan_revisions", acceptance["revision_id"])
    plan = store.get("plans", revision["plan_id"]) if revision else None
    if revision is None or revision["status"] != "approved" or \
            plan is None or plan["station_id"] != station_id or \
            plan.get("current_revision_id") != revision["_id"] or \
            plan.get("state") in {"cancelled", "completed"}:
        raise Conflict("accepted plan is no longer the current approved plan")
    deliveries = store.lookup("plan_distributions", "endpoint_revision",
                              endpoint_id, revision["_id"])
    if len(deliveries) != 1 or deliveries[0]["status"] != "accepted":
        raise Conflict("endpoint has no accepted snapshot for active plan")
    if _hash(deliveries[0]["snapshot"]) != deliveries[0]["payload_hash"]:
        raise Conflict("accepted snapshot digest changed")
    return acceptance, plan, deliveries[0]


def _fence(override: dict, active: dict, delivery: dict) -> None:
    if override["plan_revision_id"] != active["revision_id"] or \
            override["engine_instance"] != active["engine_instance"] or \
            override["engine_epoch"] != active["engine_epoch"] or \
            override["snapshot_hash"] != delivery["payload_hash"]:
        raise Conflict("live override was fenced by another plan, engine or snapshot")


def _override(store: Store, station_id: str, endpoint_id: str,
              override_id: str) -> dict:
    row = store.get("live_overrides", _id(override_id, "override_id"))
    if row is None or row["station_id"] != station_id or row["endpoint_id"] != endpoint_id:
        raise StoreError("live override is outside endpoint")
    return row


def _intent(operation: str, anchor_entry_id: str, position: str | None,
            version_id: str | None, ttl_seconds: int) -> dict:
    if operation not in {"skip", "replace", "insert"}:
        raise StoreError("operation must be skip, replace or insert")
    if type(ttl_seconds) is not int or not 1 <= ttl_seconds <= 300:
        raise StoreError("ttl_seconds must be 1..300")
    if operation == "insert":
        if position not in {"before", "after"}:
            raise StoreError("insert requires before or after position")
    elif position is not None:
        raise StoreError("skip or replace cannot have insert position")
    if operation == "skip":
        if version_id is not None:
            raise StoreError("skip cannot select new audio")
    elif version_id is None:
        raise StoreError("insert or replace needs a content version")
    return {"operation": operation, "plan_entry_id": anchor_entry_id,
            "position": position, "version_id": version_id,
            "ttl_seconds": ttl_seconds}


def _selected_asset_ready(store: Store, row: dict, at: datetime) -> None:
    if row["version_id"] is None:
        return
    end = _instant(row["expires_at"], "expires_at")
    if end <= at:
        raise Conflict("live override asset window has expired")
    ready = readiness_at(store, row["station_id"], row["version_id"],
                         slot_start=at, slot_end=end, endpoint_id=row["endpoint_id"],
                         require_endpoint_copy=True)
    if not ready["ready"] or ready["media_file_id"] != row["media_file_id"]:
        raise Conflict("live override selected audio is no longer ready")
    media = store.get("media_files", row["media_file_id"])
    copies = store.lookup("endpoint_copies", "endpoint_file",
                          row["endpoint_id"], row["media_file_id"])
    if media is None or media.get("sha256") != row["media_sha256"] or \
            len(copies) != 1 or copies[0]["status"] != "verified" or \
            copies[0].get("sha256") != media["sha256"] or not copies[0].get("verified_at"):
        raise Conflict("live override endpoint media copy is no longer verified")


def propose_live_override(store: Store, principal: Principal, station_id: str,
                          endpoint_id: str, *, command_key: str,
                          operation: str, anchor_entry_id: str,
                          version_id: str | None = None,
                          position: str | None = None,
                          ttl_seconds: int = 30,
                          expected_acceptance_revision: int,
                          now: datetime | str | None = None) -> dict:
    station_id, endpoint_id = _id(station_id, "station_id"), _id(endpoint_id, "endpoint_id")
    anchor_entry_id = _id(anchor_entry_id, "anchor_entry_id")
    version_id = _id(version_id, "version_id") if version_id is not None else None
    _endpoint_user(store, principal, station_id, endpoint_id)
    key = _text(command_key, "command_key")
    if type(expected_acceptance_revision) is not int or expected_acceptance_revision < 1:
        raise StoreError("expected_acceptance_revision must be positive")
    request = _intent(operation, anchor_entry_id, position, version_id, ttl_seconds)
    request_hash = _hash(request)
    previous = store.lookup("live_overrides", "endpoint_key", endpoint_id, key)
    if previous:
        row = previous[0]
        if row["station_id"] != station_id or row["request_hash"] != request_hash:
            raise Conflict("live override command_key was reused with different intent")
        return {"override_id": row["_id"], "state": row["state"], "replayed": True}
    sequence = store.sequence
    active, plan, delivery = _active(store, station_id, endpoint_id)
    if active["_rev"] != expected_acceptance_revision:
        raise Conflict("endpoint acceptance changed")
    snapshot = delivery["snapshot"]
    anchor = next((entry for entry in snapshot["entries"]
                   if entry.get("plan_entry_id") == anchor_entry_id), None)
    if anchor is None:
        raise Conflict("anchor entry is absent from accepted snapshot")
    if operation in {"skip", "replace"} and anchor.get("entry_kind") != "audio":
        raise Conflict("skip and replace require an audio entry")
    stored_anchor = store.get("plan_entries", anchor_entry_id)
    if stored_anchor is None or stored_anchor["plan_revision_id"] != active["revision_id"] or \
            stored_anchor.get("state") != "planned":
        raise Conflict("anchor plan entry no longer matches approved revision")
    instant = _now(now)
    end = instant + timedelta(seconds=ttl_seconds)
    selected_media_id = None
    selected_digest = None
    if version_id is not None:
        ready = readiness_at(store, station_id, version_id,
                             slot_start=instant, slot_end=end,
                             endpoint_id=endpoint_id, require_endpoint_copy=True)
        if not ready["ready"]:
            raise Conflict("override audio is not ready: " + ", ".join(ready["blocked_reasons"]))
        selected_media_id = ready["media_file_id"]
        media = store.get("media_files", selected_media_id)
        copies = store.lookup("endpoint_copies", "endpoint_file", endpoint_id, selected_media_id)
        if media is None or len(copies) != 1 or copies[0]["status"] != "verified" or \
                copies[0].get("sha256") != media.get("sha256") or \
                not copies[0].get("verified_at"):
            raise Conflict("override audio lacks a verified endpoint copy")
        selected_digest = media["sha256"]
    override_id = str(uuid4())
    record = {"station_id": station_id, "endpoint_id": endpoint_id,
              "plan_id": plan["_id"], "plan_revision_id": active["revision_id"],
              "plan_entry_id": anchor_entry_id, "anchor_version_id": anchor.get("version_id"),
              "anchor_media_file_id": anchor.get("media_file_id"),
              "operation": operation, "position": position,
              "version_id": version_id, "media_file_id": selected_media_id,
              "media_sha256": selected_digest,
              "command_key": key, "request_hash": request_hash,
              "snapshot_hash": delivery["payload_hash"],
              "engine_instance": active["engine_instance"],
              "engine_epoch": active["engine_epoch"],
              "state": "pending", "issued_at": instant.isoformat(),
              "expires_at": end.isoformat(), "created_by": principal.user_id,
              "acknowledged_at": None, "reported_at": None,
              "playout_attempt_id": None, "skip_attempt_id": None}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.expect("active_acceptances", endpoint_id, active["_rev"])
    tx.put("live_overrides", override_id, record)
    tx.commit()
    return {"override_id": override_id, "state": "pending",
            "expires_at": end.isoformat(), "replayed": False}


def poll_live_overrides(store: Store, principal: Principal, station_id: str,
                        endpoint_id: str, engine_instance: str,
                        engine_epoch: int, *, limit: int = 100,
                        now: datetime | str | None = None) -> list[dict]:
    station_id, endpoint_id = _id(station_id, "station_id"), _id(endpoint_id, "endpoint_id")
    _endpoint_agent(store, principal, station_id, endpoint_id)
    engine_instance, engine_epoch = _engine_identity(engine_instance, engine_epoch)
    if type(limit) is not int or not 1 <= limit <= 1000:
        raise StoreError("limit must be 1..1000")
    active, _plan, delivery = _active(store, station_id, endpoint_id)
    if active["engine_instance"] != engine_instance or active["engine_epoch"] != engine_epoch:
        raise Conflict("engine is fenced")
    instant = _now(now)
    pending = store.lookup("live_overrides", "endpoint_state", endpoint_id, "pending")
    deliverable = []
    for row in pending:
        stale = (row["plan_revision_id"] != active["revision_id"] or
                 row["engine_instance"] != active["engine_instance"] or
                 row["engine_epoch"] != active["engine_epoch"] or
                 row["snapshot_hash"] != delivery["payload_hash"] or
                 _instant(row["expires_at"], "expires_at") <= instant)
        reason = "ttl_or_epoch"
        if not stale:
            try:
                _selected_asset_ready(store, row, instant)
            except (Conflict, StoreError):
                stale = True
                reason = "asset_not_ready"
        if stale:
            try:
                store.put("live_overrides", row["_id"],
                          _body(row) | {"state": "expired", "error_code": reason},
                          expected_revision=row["_rev"], actor=principal.user_id)
            except Conflict:
                pass
            continue
        deliverable.append(row)
    deliverable.sort(key=lambda row: (row["issued_at"], row["_id"]))
    return [{"override_id": row["_id"], "command_key": row["command_key"],
             "operation": row["operation"], "position": row["position"],
             "plan_revision_id": row["plan_revision_id"],
             "plan_entry_id": row["plan_entry_id"],
             "version_id": row["version_id"], "media_file_id": row["media_file_id"],
             "media_sha256": row["media_sha256"],
             "snapshot_hash": row["snapshot_hash"],
             "engine_epoch": row["engine_epoch"],
             "issued_at": row["issued_at"], "expires_at": row["expires_at"]}
            for row in deliverable[:limit]]


def _report_replay(store: Store, endpoint_id: str, event_key: str,
                   claim_fingerprint: str) -> dict | None:
    rows = store.lookup("live_override_reports", "endpoint_key", endpoint_id, event_key)
    if not rows:
        return None
    report = rows[0]
    if report["claim_fingerprint"] != claim_fingerprint:
        raise Conflict("live override event_key was reused with different content")
    override = store.get("live_overrides", report["override_id"])
    if override is None:
        raise Conflict("live override report has no command")
    return {"override_id": override["_id"], "state": override["state"],
            "report_id": report["_id"], "replayed": True}


def acknowledge_live_override(store: Store, principal: Principal,
                              station_id: str, endpoint_id: str, override_id: str,
                              engine_instance: str, engine_epoch: int, *,
                              event_key: str, accepted: bool,
                              occurred_at: datetime | str,
                              reason_code: str | None = None,
                              now: datetime | str | None = None) -> dict:
    station_id, endpoint_id = _id(station_id, "station_id"), _id(endpoint_id, "endpoint_id")
    _endpoint_agent(store, principal, station_id, endpoint_id)
    engine_instance, engine_epoch = _engine_identity(engine_instance, engine_epoch)
    row = _override(store, station_id, endpoint_id, override_id)
    key = _text(event_key, "event_key")
    if type(accepted) is not bool:
        raise StoreError("accepted must be Boolean")
    reason = _text(reason_code, "reason_code", 80) if reason_code is not None else None
    if accepted and reason is not None or not accepted and reason is None:
        raise StoreError("rejection requires a reason; acceptance cannot have one")
    instant = _instant(occurred_at, "occurred_at")
    received = _now(now)
    claim = {"override_id": row["_id"], "report_kind": "ack",
             "event_key": key, "accepted": accepted, "reason_code": reason,
             "occurred_at": instant.isoformat(), "engine_instance": engine_instance,
             "engine_epoch": engine_epoch}
    digest = _hash(claim)
    replay = _report_replay(store, endpoint_id, key, digest)
    if replay:
        return replay
    active, _plan, delivery = _active(store, station_id, endpoint_id)
    _fence(row, active, delivery)
    if engine_instance != row["engine_instance"] or engine_epoch != row["engine_epoch"]:
        raise Conflict("acknowledgement came from a fenced engine")
    if row["state"] != "pending":
        raise Conflict("live override is no longer pending")
    if instant < _instant(row["issued_at"], "issued_at") or \
            instant >= _instant(row["expires_at"], "expires_at") or \
            received >= _instant(row["expires_at"], "expires_at"):
        raise Conflict("live override acceptance window expired")
    if accepted:
        _selected_asset_ready(store, row, received)
    target = "accepted" if accepted else "rejected"
    report_id = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect("active_acceptances", endpoint_id, active["_rev"])
    tx.put("live_overrides", row["_id"],
           _body(row) | {"state": target, "acknowledged_at": instant.isoformat(),
                         "error_code": reason}, expected_revision=row["_rev"])
    tx.put("live_override_reports", report_id, {
        "override_id": row["_id"], "endpoint_id": endpoint_id,
        "event_key": key, "report_kind": "ack", "occurred_at": instant.isoformat(),
        "received_at": received.isoformat(),
        "claim_fingerprint": digest, "accepted": accepted,
        "reason_code": reason, "engine_instance": engine_instance,
        "engine_epoch": engine_epoch})
    tx.commit()
    return {"override_id": row["_id"], "state": target,
            "report_id": report_id, "replayed": False}


def report_live_override_result(store: Store, principal: Principal,
                                station_id: str, endpoint_id: str,
                                override_id: str, engine_instance: str,
                                engine_epoch: int, *, event_key: str,
                                outcome: str, occurred_at: datetime | str,
                                reason_code: str | None = None,
                                now: datetime | str | None = None) -> dict:
    station_id, endpoint_id = _id(station_id, "station_id"), _id(endpoint_id, "endpoint_id")
    _endpoint_agent(store, principal, station_id, endpoint_id)
    engine_instance, engine_epoch = _engine_identity(engine_instance, engine_epoch)
    row = _override(store, station_id, endpoint_id, override_id)
    key = _text(event_key, "event_key")
    if outcome not in {"applied", "failed"}:
        raise StoreError("outcome must be applied or failed")
    reason = _text(reason_code, "reason_code", 80) if reason_code is not None else None
    if outcome == "failed" and reason is None or outcome == "applied" and reason is not None:
        raise StoreError("failed needs a reason; applied cannot have one")
    instant = _instant(occurred_at, "occurred_at")
    received = _now(now)
    claim = {"override_id": row["_id"], "report_kind": "result",
             "event_key": key, "outcome": outcome, "reason_code": reason,
             "occurred_at": instant.isoformat(), "engine_instance": engine_instance,
             "engine_epoch": engine_epoch}
    digest = _hash(claim)
    replay = _report_replay(store, endpoint_id, key, digest)
    if replay:
        return replay
    active, _plan, delivery = _active(store, station_id, endpoint_id)
    _fence(row, active, delivery)
    if engine_instance != row["engine_instance"] or engine_epoch != row["engine_epoch"]:
        raise Conflict("result came from a fenced engine")
    if row["state"] != "accepted":
        raise Conflict("execution result requires an accepted live override")
    if instant < _instant(row["acknowledged_at"], "acknowledged_at"):
        raise Conflict("execution result predates acceptance")
    target = "execution_claimed" if outcome == "applied" else "failed"
    report_id = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect("active_acceptances", endpoint_id, active["_rev"])
    tx.put("live_overrides", row["_id"],
           _body(row) | {"state": target, "reported_at": instant.isoformat(),
                         "error_code": reason}, expected_revision=row["_rev"])
    tx.put("live_override_reports", report_id, {
        "override_id": row["_id"], "endpoint_id": endpoint_id,
        "event_key": key, "report_kind": "result", "occurred_at": instant.isoformat(),
        "received_at": received.isoformat(),
        "claim_fingerprint": digest, "outcome": outcome,
        "reason_code": reason, "engine_instance": engine_instance,
        "engine_epoch": engine_epoch})
    tx.commit()
    return {"override_id": row["_id"], "state": target,
            "report_id": report_id, "replayed": False}


def _attempt_evidence(store: Store, override: dict, attempt_id: str, *,
                      role: str) -> dict:
    attempt = store.get("playout_attempts", _id(attempt_id, f"{role}_attempt_id"))
    if attempt is None or attempt["station_id"] != override["station_id"] or \
            attempt["endpoint_id"] != override["endpoint_id"]:
        raise Conflict("playout attempt belongs to another endpoint")
    if role == "skip":
        if attempt.get("plan_entry_id") != override["plan_entry_id"] or \
                attempt["version_id"] != override["anchor_version_id"] or \
                attempt["state"] != "skipped" or attempt["last_phase"] != "skip":
            raise Conflict("skip attempt does not match the anchor entry")
    else:
        if attempt.get("plan_entry_id") is not None or \
                attempt.get("attempt_key") != "live-override:" + override["_id"] or \
                attempt["version_id"] != override["version_id"] or \
                attempt.get("media_file_id") != override["media_file_id"] or \
                attempt["state"] != "completed" or attempt["last_phase"] != "manual_complete":
            raise Conflict("manual attempt does not match selected override audio")
    terminal = store.get("playout_events", attempt["last_event_id"])
    details = terminal.get("details", {}) if terminal else {}
    if terminal is None or terminal["attempt_id"] != attempt["_id"] or \
            details.get("live_override_id") != override["_id"] or \
            details.get("plan_revision_id") != override["plan_revision_id"] or \
            details.get("engine_instance") != override["engine_instance"] or \
            details.get("engine_epoch") != override["engine_epoch"] or \
            _instant(terminal["occurred_at"], "occurred_at") < \
                _instant(override["acknowledged_at"], "acknowledged_at"):
        raise Conflict("playout event does not identify this accepted override")
    return attempt


def link_live_override_evidence(store: Store, principal: Principal,
                                station_id: str, endpoint_id: str,
                                override_id: str, *,
                                playout_attempt_id: str | None = None,
                                skip_attempt_id: str | None = None,
                                expected_override_revision: int) -> dict:
    station_id, endpoint_id = _id(station_id, "station_id"), _id(endpoint_id, "endpoint_id")
    _endpoint_user(store, principal, station_id, endpoint_id)
    row = _override(store, station_id, endpoint_id, override_id)
    if row["state"] == "evidenced" and \
            row.get("playout_attempt_id") == (_id(playout_attempt_id, "playout_attempt_id")
                if playout_attempt_id is not None else None) and \
            row.get("skip_attempt_id") == (_id(skip_attempt_id, "skip_attempt_id")
                if skip_attempt_id is not None else None):
        return {"override_id": row["_id"], "state": "evidenced",
                "playout_attempt_id": row.get("playout_attempt_id"),
                "skip_attempt_id": row.get("skip_attempt_id"),
                "evidence_level": "engine_claim", "replayed": True}
    if row["_rev"] != expected_override_revision:
        raise Conflict("live override changed")
    if row["state"] != "execution_claimed":
        raise Conflict("only an execution claim can receive playout evidence")
    sequence = store.sequence
    skip = None
    play = None
    if row["operation"] in {"skip", "replace"}:
        if skip_attempt_id is None:
            raise StoreError("skip or replace needs a skipped anchor attempt")
        skip = _attempt_evidence(store, row, skip_attempt_id, role="skip")
    elif skip_attempt_id is not None:
        raise StoreError("insert cannot name a skipped anchor attempt")
    if row["operation"] in {"insert", "replace"}:
        if playout_attempt_id is None:
            raise StoreError("insert or replace needs a completed manual attempt")
        play = _attempt_evidence(store, row, playout_attempt_id, role="play")
    elif playout_attempt_id is not None:
        raise StoreError("skip cannot name a manual playout attempt")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("live_overrides", row["_id"],
           _body(row) | {"state": "evidenced",
                         "playout_attempt_id": play["_id"] if play else None,
                         "skip_attempt_id": skip["_id"] if skip else None,
                         "evidenced_by": principal.user_id,
                         "evidenced_at": datetime.now(timezone.utc).isoformat()},
           expected_revision=row["_rev"])
    tx.commit()
    return {"override_id": row["_id"], "state": "evidenced",
            "playout_attempt_id": play["_id"] if play else None,
            "skip_attempt_id": skip["_id"] if skip else None,
            "evidence_level": "engine_claim"}
