"""Station administration and explicit, evidence-preserving runtime recovery.

Recovery closes an operator session, never reconstructs unobserved audio or
pretends that a listener received it. It leaves accepted manifests and source
observations byte-for-byte intact and records a separate immutable decision.
"""
from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone
import hashlib
import json
from uuid import uuid4

from . import desktop_api as api
from .access_admin import _PERMISSIONS
from .catalog import NotFound
from .identity import Forbidden
from .integrity import inspect_integrity
from .storage import Conflict, StoreError
from .store_health import inspect_store_health

RUN_KINDS = frozenset({"broadcast_local_run", "advert_local_run"})
MAX_ROWS = 500


def _now():
    return datetime.now(timezone.utc).isoformat()


def _body(row):
    return {key: deepcopy(value) for key, value in row.items() if key not in {"_id", "_rev"}}


def _hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
        separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def _station(store, principal, station_id):
    sid, station = api._station(store, principal, station_id, "manage")
    user = store.get("users", principal.user_id)
    if principal.scope != "user" or not user or user["status"] != "active":
        raise Forbidden("active user management credential required")
    return sid, station


def _grants(store, user_id, *, include_revoked=False):
    return [row for row in store.lookup("permissions", "user", user_id)
        if include_revoked or not row.get("revoked_at")]


def _editable(store, sid, user_id):
    grants = _grants(store, user_id, include_revoked=True)
    return bool(grants) and all(row.get("station_id") == sid for row in grants)


def _user(store, sid, user_id):
    row = store.get("users", api._id(user_id, "user_id"))
    if row is None or not any(grant.get("station_id") in {None, sid} for grant in _grants(store, user_id)):
        raise NotFound("user is absent or outside station")
    return row


def _token_summary(row):
    return {"token_id": row["_id"], "revision": row["_rev"], "scope": row["scope"],
        "endpoint_id": row.get("endpoint_id"), "expires_at": row["expires_at"],
        "revoked_at": row.get("revoked_at")}


def _user_summary(store, sid, row, *, principal=None):
    grants = _grants(store, row["_id"])
    local = sorted({grant["permission"] for grant in grants if grant.get("station_id") == sid})
    global_permissions = sorted({grant["permission"] for grant in grants if grant.get("station_id") is None})
    editable = _editable(store, sid, row["_id"])
    tokens = []
    if editable or principal is not None and principal.user_id == row["_id"]:
        for token in store.scan("tokens"):
            if token["user_id"] != row["_id"]:
                continue
            if token["scope"] != "user":
                endpoint = store.get("endpoints", token.get("endpoint_id"))
                if not endpoint or endpoint["station_id"] != sid:
                    continue
            tokens.append(_token_summary(token))
    if len(tokens) > MAX_ROWS:
        raise StoreError("station user exceeds credential display budget")
    return {"user_id": row["_id"], "revision": row["_rev"], "username": row["username"],
        "display_name": row["display_name"], "status": row["status"],
        "local_permissions": local, "global_permissions": global_permissions,
        "permissions": sorted(set(local) | set(global_permissions)), "editable": editable,
        "tokens": sorted(tokens, key=lambda value: value["token_id"])}


def _usable_user_token(row, now):
    return row["scope"] == "user" and not row.get("revoked_at") and datetime.fromisoformat(row["expires_at"]) > now


def _ensure_manager(store, sid, *, change_user=None, status=None, permissions=None, revoke_token=None):
    """Count only active managers who still have a usable user credential."""
    now = datetime.now(timezone.utc)
    keys = {row["user_id"] for row in store.scan("tokens")
        if row["_id"] != revoke_token and _usable_user_token(row, now)}
    for user in store.scan("users"):
        current_status = status if user["_id"] == change_user else user["status"]
        grants = permissions if user["_id"] == change_user else {
            row["permission"] for row in _grants(store, user["_id"]) if row.get("station_id") in {None, sid}}
        if current_status == "active" and user["_id"] in keys and {"manage", "view"}.issubset(grants):
            return
    raise Conflict("operation would remove the last usable station manager")


def _start(store, principal, station_id, operation, payload, key):
    sequence = store.sequence
    sid, _ = _station(store, principal, station_id)
    receipt, digest, replay = api.operation_replay(store, principal, sid, key,
        "desktop-admin/" + operation, payload)
    return sid, sequence, receipt, digest, replay


def _finish(tx, principal, sid, receipt, key, operation, digest, result):
    api.add_operation(tx, principal, sid, receipt, key, "desktop-admin/" + operation, digest, result)
    tx.commit()
    return result


def recovery_for_run(store, sid, run_id):
    for kind in ("admin_recovery", "admin_restore_fence"):
        for row in store.lookup("desktop_configurations", "station_kind", sid, kind):
            if row["config"]["run_id"] == run_id:
                return row
    return None


def assert_run_open(store, sid, run_id):
    if recovery_for_run(store, sid, run_id) is not None:
        raise Conflict("operator closed this runtime session; lost observations remain unknown")


def assert_start_replay(store, sid, run_id):
    assert_run_open(store, sid, run_id)
    row = store.get("desktop_configurations", run_id)
    if row is None or row["station_id"] != sid or row["kind"] not in RUN_KINDS or row["config"]["state"] != "prepared":
        raise Conflict("an already observed runtime cannot be started again with an old request")


def _run_terminal(row):
    terminal = {"completed", "failed", "interrupted"} if row["kind"] == "broadcast_local_run" else {
        "local_complete", "failed", "interrupted", "notexecuted"}
    return row["config"]["state"] in terminal


def _run_summary(row):
    config = row["config"]
    return {"run_id": row["_id"], "revision": row["_rev"], "kind": row["kind"],
        "name": row["name"], "state": config["state"], "updated_at": row["updated_at"],
        "owner_user_id": row["created_by"], "proof_class": config["proof_class"],
        "delivery_proven": False, "last_observation": deepcopy(config["events"][-1]) if config["events"] else None,
        "recovery": None}


def _audit_tail(store, sid):
    """Only the metadata of explicitly station-owned changes leaves this API."""
    result = []
    for row in store.scan("_audit"):
        collections = set()
        for change in row["changes"]:
            for body in (change.get("before"), change.get("after")):
                if isinstance(body, dict) and (body.get("station_id") == sid or
                        change["collection"] == "stations" and change["key"] == sid):
                    collections.add(change["collection"])
        if collections:
            result.append({"sequence": row["sequence"], "at": row["committed_at"],
                "actor": row["actor"], "collections": sorted(collections)})
    return sorted(result, key=lambda row: row["sequence"], reverse=True)[:25]


@api._consistent_read
def overview(store, principal, station_id):
    sid, _ = _station(store, principal, station_id)
    ids = {row["user_id"] for row in store.scan("permissions") if not row.get("revoked_at") and row.get("station_id") in {None, sid}}
    users = [_user_summary(store, sid, row, principal=principal) for row in store.scan("users") if row["_id"] in ids]
    if len(users) > MAX_ROWS:
        raise StoreError("station exceeds user display budget")
    current = next(row for row in users if row["user_id"] == principal.user_id)
    runs = []
    recovered = []
    for kind in RUN_KINDS:
        for row in store.lookup("desktop_configurations", "station_kind", sid, kind):
            receipt = recovery_for_run(store, sid, row["_id"])
            if receipt:
                recovered.append(_run_summary(row) | {"recovery": deepcopy(receipt["config"])})
            elif not _run_terminal(row):
                runs.append(_run_summary(row))
    if len(runs) > MAX_ROWS:
        raise StoreError("station exceeds pending runtime display budget")
    try:
        health = inspect_store_health(store)
        integrity = inspect_integrity(store, max_records=50000, max_bytes=64 * 1024 * 1024,
            max_issues=20, verify_media=True, verify_cold=True)
        health["integrity"] = {key: deepcopy(integrity[key]) for key in
            ("status", "complete", "sequence", "checks", "work", "limits", "issue_count")}
        if integrity["status"] != "ok":
            health["status"] = "critical" if integrity["status"] == "invalid" else "unknown"
        health["metrics"] = store.metrics.snapshot()
    except (StoreError, OSError, ValueError):
        health = {"status": "unavailable", "integrity": {"status": "unavailable"}, "sequence": store.sequence}
    return {"station_id": sid, "current_user": current, "permissions": current["permissions"],
        "users": sorted(users, key=lambda row: (row["username"].casefold(), row["user_id"])),
        "health": health, "recoverable_runs": sorted(runs, key=lambda row: row["updated_at"], reverse=True),
        "recovered_runs": sorted(recovered, key=lambda row: row["updated_at"], reverse=True)[:50],
        "audit_tail": _audit_tail(store, sid), "sequence": store.sequence}


def user_save(store, principal, station_id, user_id, expected_revision, display_name,
              status, permissions, idempotency_key):
    user_id = api._id(user_id, "user_id")
    api._text(display_name, "display_name", 120, True)
    if not isinstance(status, str) or status not in {"active", "disabled"}:
        raise StoreError("user status must be active or disabled")
    api._list(permissions, "permissions", 9)
    if any(not isinstance(value, str) or value not in _PERMISSIONS for value in permissions) or len(set(permissions)) != len(permissions):
        raise StoreError("permissions must be distinct named station grants")
    if not permissions or "view" not in permissions:
        raise StoreError("station user must retain view permission")
    payload = {"user_id": user_id, "expected_revision": expected_revision,
        "display_name": display_name, "status": status, "permissions": permissions}
    sid, sequence, receipt, digest, replay = _start(store, principal, station_id, "user-save", payload, idempotency_key)
    if replay is not None:
        return replay
    row = _user(store, sid, user_id)
    if not _editable(store, sid, user_id):
        raise Forbidden("global or multi-station users need administration in their complete scope")
    if type(expected_revision) is not int or expected_revision != row["_rev"]:
        raise Conflict("user changed; reload before applying")
    if user_id == principal.user_id and (status != "active" or "manage" not in permissions):
        raise Conflict("current manager cannot disable or demote its own session")
    _ensure_manager(store, sid, change_user=user_id, status=status, permissions=set(permissions))
    now = _now()
    body = _body(row) | {"display_name": display_name.strip(), "status": status,
        "updated_by": principal.user_id, "updated_at": now}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("users", user_id, body, expected_revision=expected_revision)
    existing = {grant["permission"]: grant for grant in _grants(store, user_id, include_revoked=True)}
    for permission, grant in existing.items():
        if permission not in permissions and not grant.get("revoked_at"):
            tx.put("permissions", grant["_id"], _body(grant) | {"revoked_at": now,
                "revoked_by": principal.user_id}, expected_revision=grant["_rev"])
        elif permission in permissions and grant.get("revoked_at"):
            tx.put("permissions", grant["_id"], _body(grant) | {"revoked_at": None,
                "revoked_by": None, "granted_at": now, "granted_by": principal.user_id}, expected_revision=grant["_rev"])
    for permission in permissions:
        if permission not in existing:
            tx.put("permissions", str(uuid4()), {"user_id": user_id, "permission": permission,
                "station_id": sid, "station_scope": sid, "granted_by": principal.user_id, "granted_at": now})
    result = {"user_id": user_id, "revision": row["_rev"] + 1, "username": row["username"],
        "display_name": display_name.strip(), "status": status, "permissions": sorted(permissions),
        "local_permissions": sorted(permissions), "global_permissions": [], "editable": True}
    return _finish(tx, principal, sid, receipt, idempotency_key, "user-save", digest, result)


def user_create(store, principal, station_id, username, display_name, role,
                additional_permissions, idempotency_key, custom_role="",
                shift_start="", shift_end="", temporary=False):
    # Existing private-key preparation is fsynced before the transaction.
    # The admin panel displays a private file identity, never a bearer key.
    _station(store, principal, station_id)
    result = api.create_user(store, principal, station_id, username, display_name,
        role, additional_permissions, idempotency_key, custom_role=custom_role,
        shift_start=shift_start, shift_end=shift_end, temporary=temporary)
    return {key: value for key, value in result.items() if key != "token"}


def token_revoke(store, principal, station_id, token_id, expected_revision, idempotency_key):
    token_id = api._id(token_id, "token_id")
    payload = {"token_id": token_id, "expected_revision": expected_revision}
    sid, sequence, receipt, digest, replay = _start(store, principal, station_id, "token-revoke", payload, idempotency_key)
    if replay is not None:
        return replay
    row = store.get("tokens", api._id(token_id, "token_id"))
    if row is None:
        raise NotFound("credential is absent")
    if row["scope"] == "user":
        _user(store, sid, row["user_id"])
        if row["user_id"] == principal.user_id:
            raise Conflict("current operator cannot revoke its own user credentials")
        if not _editable(store, sid, row["user_id"]):
            raise Forbidden("global or multi-station credentials cannot be revoked in one station")
    else:
        endpoint = store.get("endpoints", row.get("endpoint_id"))
        if not endpoint or endpoint["station_id"] != sid:
            raise Forbidden("credential belongs to another station")
    if type(expected_revision) is not int or expected_revision != row["_rev"]:
        raise Conflict("credential changed; reload before applying")
    _ensure_manager(store, sid, revoke_token=token_id)
    body = _body(row) | {"revoked_at": row.get("revoked_at") or _now(), "revoked_by": principal.user_id}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("tokens", token_id, body, expected_revision=expected_revision)
    result = {"token_id": token_id, "revision": row["_rev"] + 1, "revoked": True}
    return _finish(tx, principal, sid, receipt, idempotency_key, "token-revoke", digest, result)


def validate_recovery(config, sid, state):
    fields = {"schema_version", "run_id", "run_revision", "run_kind", "run_snapshot_hash", "original_state",
        "last_observations", "observation_count", "owner_user_id", "closed_by", "closed_at", "reason", "state", "unknown_tail",
        "proof_class", "delivery_proven"}
    api._object(config, fields, fields)
    restored_terminal = config["proof_class"] == "local_runtime_restore_fence"
    if type(config["schema_version"]) is not int or config["schema_version"] != 1 or \
            not isinstance(config["run_kind"], str) or config["run_kind"] not in RUN_KINDS or \
            config["state"] != ("closed" if restored_terminal else "interrupted") or \
            config["unknown_tail"] is not (False if restored_terminal else True) or \
            config["proof_class"] not in {"local_runtime_recovery", "local_runtime_restore_fence"} or config["delivery_proven"] is not False:
        raise StoreError("recovery cannot assert playback completion or remote delivery")
    run = state.get("desktop_configurations", {}).get(api._id(config["run_id"]), {})
    body = run.get("data")
    if not body or body["station_id"] != sid or body["kind"] != config["run_kind"] or \
            run.get("revision") != config["run_revision"] or config["run_snapshot_hash"] != _hash(body["config"]) or \
            config["original_state"] != body["config"]["state"] or config["owner_user_id"] != body["created_by"]:
        raise StoreError("recovery must preserve its exact accepted runtime snapshot")
    if config["last_observations"] != body["config"]["events"][-64:] or config["observation_count"] != len(body["config"]["events"]):
        raise StoreError("recovery cannot invent or lose source observations")
    if restored_terminal and not _run_terminal(body):
        raise StoreError("terminal restore fences cannot hide an unknown incomplete tail")
    api._int(config["run_revision"], "run_revision", 1, 2 ** 63 - 1)
    if api._id(config["closed_by"]) not in state.get("users", {}):
        raise StoreError("recovery operator is absent")
    api._stamp(config["closed_at"], "closed_at")
    api._text(config["reason"], "reason", 1000, True)


def guard_recovery(before, after):
    if before is not None and before != after:
        raise StoreError("runtime recovery decisions are immutable")


def _recovery_body(row, actor_user_id, reason):
    config = {"schema_version": 1, "run_id": row["_id"], "run_revision": row["_rev"], "run_kind": row["kind"],
        "run_snapshot_hash": _hash(row["config"]), "original_state": row["config"]["state"],
        "last_observations": deepcopy(row["config"]["events"][-64:]), "observation_count": len(row["config"]["events"]),
        "owner_user_id": row["created_by"], "closed_by": actor_user_id, "closed_at": _now(),
        "reason": reason.strip(), "state": "interrupted", "unknown_tail": True,
        "proof_class": "local_runtime_recovery", "delivery_proven": False}
    return {"station_id": row["station_id"], "kind": "admin_recovery", "name": row["name"], "config": config,
        "status": "active", "created_by": actor_user_id, "created_at": config["closed_at"],
        "updated_by": actor_user_id, "updated_at": config["closed_at"]}


def recovery_close(store, principal, station_id, run_id, expected_revision, idempotency_key, reason):
    run_id = api._id(run_id, "run_id")
    api._text(reason, "reason", 1000, True)
    payload = {"run_id": run_id, "expected_revision": expected_revision, "reason": reason}
    sid, sequence, receipt, digest, replay = _start(store, principal, station_id, "recovery-close", payload, idempotency_key)
    if replay is not None:
        return replay
    row = store.get("desktop_configurations", api._id(run_id, "run_id"))
    if row is None or row["station_id"] != sid or row["kind"] not in RUN_KINDS:
        raise NotFound("local runtime is absent or outside station")
    if type(expected_revision) is not int or expected_revision != row["_rev"]:
        raise Conflict("runtime changed; refresh before declaring its session interrupted")
    assert_run_open(store, sid, run_id)
    if _run_terminal(row):
        raise Conflict("runtime already has a terminal source observation")
    body = _recovery_body(row, principal.user_id, reason)
    validate_recovery(body["config"], sid, store._state)
    identifier = str(uuid4())
    result = {"run_id": run_id, "revision": row["_rev"], "recovery_id": identifier, "state": "interrupted",
        "unknown_tail": True, "proof_class": "local_runtime_recovery", "delivery_proven": False}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("desktop_configurations", identifier, body)
    return _finish(tx, principal, sid, receipt, idempotency_key, "recovery-close", digest, result)


def fence_restored_runtime(store, actor_user_id=None):
    """Fence pending sessions on a private, offline restored database.

    This is an offline deployment helper, deliberately absent from HTTP routes.
    It must never run against a live production store. The surrounding restore
    transaction owns its staged directory and publication/rollback protocol.
    """
    sequence = store.sequence
    if actor_user_id is None:
        candidates = [user for user in store.scan("users") if user["status"] == "active" and
            any(grant["permission"] == "manage" and grant.get("station_id") is None
                for grant in _grants(store, user["_id"]))]
        if not candidates:
            raise StoreError("restored database needs an active global restore manager")
        actor_user_id = sorted(candidates, key=lambda row: row["_id"])[0]["_id"]
    actor_user_id = api._id(actor_user_id, "actor_user_id")
    user = store.get("users", actor_user_id)
    if user is None or user["status"] != "active":
        raise StoreError("restore manager is absent or disabled")
    pending = [row for row in store.scan("desktop_configurations") if row["kind"] in RUN_KINDS
        and recovery_for_run(store, row["station_id"], row["_id"]) is None]
    if len(pending) > MAX_ROWS:
        raise StoreError("restored runtime fence exceeds 500 session budget")
    if not pending:
        return {"closed_runs": 0, "fenced_runs": 0, "recovery_ids": [], "sequence": sequence}
    tx = store.begin(actor="offline-restore:" + actor_user_id)
    tx.expect_sequence(sequence)
    identifiers = []
    closed_runs = 0
    for row in pending:
        from .identity import Principal, require
        require(store, Principal(actor_user_id, "user"), "manage", row["station_id"])
        body = _recovery_body(row, actor_user_id, "offline_backup_restore: original process is stopped; unobserved tail remains unknown")
        if _run_terminal(row):
            body["kind"] = "admin_restore_fence"
            body["config"].update(state="closed", unknown_tail=False, proof_class="local_runtime_restore_fence",
                reason="offline_backup_restore: original terminal observations are preserved; historical session cannot execute again")
        else:
            closed_runs += 1
        validate_recovery(body["config"], row["station_id"], store._state)
        identifier = str(uuid4())
        tx.put("desktop_configurations", identifier, body)
        identifiers.append(identifier)
    tx.commit()
    return {"closed_runs": closed_runs, "fenced_runs": len(identifiers), "recovery_ids": identifiers, "sequence": sequence + 1}
