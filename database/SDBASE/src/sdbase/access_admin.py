"""Station-scoped user provisioning and explicit credential revocation."""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import re
import secrets
from uuid import UUID, uuid4

from .identity import Forbidden, Principal, require
from .storage import Conflict, Store, StoreError


_USERNAME = re.compile(r"[A-Za-z0-9_.@-]{3,80}\Z")
_PERMISSIONS = frozenset({"view", "edit", "bulk_edit", "import", "approve",
                          "activate", "operate", "repair", "manage"})


def _id(value: str, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _lifetime(days: int) -> datetime:
    if type(days) is not int or not 1 <= days <= 365:
        raise StoreError("token lifetime must be 1..365 days")
    return datetime.now(timezone.utc) + timedelta(days=days)


def _can_manage_user(store: Store, principal: Principal, user_id: str) -> None:
    """A local manager cannot mint or revoke a more privileged user's key."""
    if principal.scope != "user":
        raise Forbidden("user management needs a user credential")
    permissions = [row for row in store.lookup("permissions", "user", user_id) if not row.get("revoked_at")]
    if not permissions:
        raise Conflict("user has no station permission")
    global_manager = any(not row.get("revoked_at") and row["permission"] == "manage" and
                         row.get("station_id") is None for row in
                         store.lookup("permissions", "user", principal.user_id))
    if global_manager:
        stations = store.scan("stations")
        if not stations:
            raise Conflict("global management needs a real station")
        require(store, principal, "manage", stations[0]["_id"])
    for row in permissions:
        station_id = row.get("station_id")
        if station_id is None:
            if not global_manager:
                raise Forbidden("global user credential needs global management")
        else:
            require(store, principal, "manage", station_id)


def create_station_user(store: Store, principal: Principal, station_id: str,
                        username: str, display_name: str,
                        permissions: list[str], *, days: int = 30) -> dict:
    """Create one user and station-only grants atomically; show the key once."""
    station_id = _id(station_id, "station_id")
    require(store, principal, "manage", station_id)
    if store.get("stations", station_id) is None:
        raise StoreError("station is absent")
    if not isinstance(username, str) or not _USERNAME.fullmatch(username) or \
            not isinstance(display_name, str) or not display_name.strip() or \
            len(display_name) > 120:
        raise StoreError("username or display name is invalid")
    if not isinstance(permissions, list) or not permissions or \
            any(permission not in _PERMISSIONS for permission in permissions) or \
            len(set(permissions)) != len(permissions):
        raise StoreError("permissions must be distinct named station permissions")
    expiry = _lifetime(days)
    if store.lookup("users", "username", username):
        raise Conflict("username already exists")
    now = datetime.now(timezone.utc).isoformat()
    user_id, token_id, token = str(uuid4()), str(uuid4()), secrets.token_urlsafe(32)
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(store.sequence)
    tx.put("users", user_id, {"username": username, "display_name": display_name.strip(),
                              "status": "active", "created_at": now,
                              "created_by": principal.user_id})
    for permission in permissions:
        tx.put("permissions", str(uuid4()), {
            "user_id": user_id, "permission": permission,
            "station_id": station_id, "station_scope": station_id,
            "granted_by": principal.user_id, "granted_at": now})
    tx.put("tokens", token_id, {
        "user_id": user_id, "scope": "user",
        "token_hash": hashlib.sha256(token.encode()).hexdigest(),
        "expires_at": expiry.isoformat(), "revoked_at": None})
    tx.commit()
    return {"user_id": user_id, "token_id": token_id, "token": token,
            "station_id": station_id, "expires_at": expiry.isoformat()}


def issue_user_token(store: Store, principal: Principal, user_id: str,
                     *, days: int = 30) -> dict:
    user_id = _id(user_id, "user_id")
    user = store.get("users", user_id)
    if user is None or user["status"] != "active":
        raise Conflict("user is absent or disabled")
    _can_manage_user(store, principal, user_id)
    expiry = _lifetime(days)
    token_id, token = str(uuid4()), secrets.token_urlsafe(32)
    store.put("tokens", token_id, {
        "user_id": user_id, "scope": "user",
        "token_hash": hashlib.sha256(token.encode()).hexdigest(),
        "expires_at": expiry.isoformat(), "revoked_at": None,
    }, actor=principal.user_id)
    return {"user_id": user_id, "token_id": token_id,
            "token": token, "expires_at": expiry.isoformat()}


def _authorize_token(store: Store, principal: Principal, row: dict) -> None:
    if row["scope"] == "user":
        _can_manage_user(store, principal, row["user_id"])
        return
    endpoint = store.get("endpoints", row["endpoint_id"])
    if endpoint is None:
        raise Conflict("credential endpoint is absent")
    require(store, principal, "manage", endpoint["station_id"])


def get_token(store: Store, principal: Principal, token_id: str) -> dict:
    token_id = _id(token_id, "token_id")
    row = store.get("tokens", token_id)
    if row is None:
        raise Conflict("credential is absent")
    _authorize_token(store, principal, row)
    return {"token_id": token_id, "scope": row["scope"],
            "user_id": row["user_id"], "endpoint_id": row.get("endpoint_id"),
            "expires_at": row["expires_at"], "revoked_at": row.get("revoked_at"),
            "revision": row["_rev"]}


def revoke_token(store: Store, principal: Principal, token_id: str,
                 *, expected_revision: int) -> dict:
    sequence = store.sequence
    token_id = _id(token_id, "token_id")
    row = store.get("tokens", token_id)
    if row is None:
        raise Conflict("credential is absent")
    _authorize_token(store, principal, row)
    if type(expected_revision) is not int or row["_rev"] != expected_revision:
        raise Conflict("credential changed")
    if row.get("revoked_at"):
        return {"token_id": token_id, "revoked": True, "replayed": True}
    if row["scope"] == "user":
        if row["user_id"] == principal.user_id:
            raise Conflict("current operator cannot revoke its own user credentials")
        from .desktop_admin import _ensure_manager
        stations = {grant.get("station_id") for grant in store.lookup("permissions", "user", row["user_id"])
            if not grant.get("revoked_at") and grant["permission"] == "manage"}
        if None in stations:
            stations = {station["_id"] for station in store.scan("stations")}
        for station_id in stations:
            _ensure_manager(store, station_id, revoke_token=token_id)
    body = {key: value for key, value in row.items() if key not in {"_id", "_rev"}}
    body.update(revoked_at=datetime.now(timezone.utc).isoformat(),
                revoked_by=principal.user_id)
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("tokens", token_id, body, expected_revision=expected_revision)
    tx.commit()
    return {"token_id": token_id, "revoked": True, "replayed": False}
