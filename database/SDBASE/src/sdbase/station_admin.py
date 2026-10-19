"""Small station provisioning operations for a newly cloned RadioS station."""

from __future__ import annotations

from datetime import datetime, timezone
import re
from typing import Any
from uuid import UUID, uuid4

from .identity import Principal, require
from .storage import Conflict, Store, StoreError


_CODE = re.compile(r"[A-Za-z0-9_.-]{1,64}\Z")
_PERMISSIONS = frozenset({"view", "edit", "bulk_edit", "import", "approve",
                          "activate", "operate", "repair", "manage"})


def _id(value: Any, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _station(store: Store, principal: Principal, station_id: str) -> dict:
    require(store, principal, "manage", station_id)
    station = store.get("stations", station_id)
    if station is None:
        raise StoreError("station is absent")
    return station


def create_station_endpoint(store: Store, principal: Principal, station_id: str,
                            code: str, name: str) -> dict:
    """Create an offline endpoint with a fresh identity and no inherited token."""
    station_id = _id(station_id, "station_id")
    _station(store, principal, station_id)
    if not isinstance(code, str) or not _CODE.fullmatch(code) or \
            not isinstance(name, str) or not name.strip() or len(name) > 120:
        raise StoreError("endpoint needs a valid code and nonempty name")
    if store.lookup("endpoints", "station_code", station_id, code):
        raise Conflict("endpoint code already exists in station")
    sequence = store.sequence
    endpoint_id = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("endpoints", endpoint_id, {
        "station_id": station_id, "code": code, "name": name.strip(),
        "status": "offline", "created_at": datetime.now(timezone.utc).isoformat(),
        "created_by": principal.user_id,
    })
    tx.commit()
    return store.get("endpoints", endpoint_id)


def activate_cloned_station(store: Store, principal: Principal, station_id: str,
                            *, expected_revision: int) -> dict:
    """Enable only a provisioned clone; this does not start its audio engine."""
    station_id = _id(station_id, "station_id")
    station = _station(store, principal, station_id)
    if type(expected_revision) is not int or expected_revision != station["_rev"]:
        raise Conflict("station changed")
    if not station.get("clone_source_station_id"):
        raise StoreError("station is not a clone")
    if station["status"] != "disabled":
        raise Conflict("cloned station is already active")
    if not store.lookup("endpoints", "station", station_id):
        raise Conflict("cloned station needs a new endpoint before activation")
    data = {key: value for key, value in station.items() if key not in {"_id", "_rev"}}
    data.update({"status": "active", "status_changed_at": datetime.now(timezone.utc).isoformat(),
                 "status_changed_by": principal.user_id})
    return store.put("stations", station_id, data,
                     expected_revision=expected_revision, actor=principal.user_id)


def grant_station_permission(store: Store, principal: Principal, station_id: str,
                             user_id: str, permission: str) -> dict:
    """Grant one named permission on this station, never a global wildcard."""
    sequence = store.sequence
    station_id, user_id = _id(station_id, "station_id"), _id(user_id, "user_id")
    _station(store, principal, station_id)
    user = store.get("users", user_id)
    if user is None or user["status"] != "active":
        raise StoreError("permission target must be an active user")
    if permission not in _PERMISSIONS:
        raise StoreError("unknown station permission")
    for row in store.lookup("permissions", "user", user_id):
        if row["permission"] == permission and row.get("station_id") == station_id:
            if row.get("revoked_at"):
                body = {key: value for key, value in row.items() if key not in {"_id", "_rev"}}
                body.update(revoked_at=None, revoked_by=None, granted_by=principal.user_id,
                    granted_at=datetime.now(timezone.utc).isoformat())
                tx = store.begin(actor=principal.user_id)
                tx.expect_sequence(sequence)
                tx.put("permissions", row["_id"], body, expected_revision=row["_rev"])
                tx.commit()
                return store.get("permissions", row["_id"]) | {"replayed": False}
            return row | {"replayed": True}
    permission_id = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("permissions", permission_id, {
        "user_id": user_id, "station_id": station_id,
        "permission": permission, "station_scope": station_id,
        "granted_by": principal.user_id,
        "granted_at": datetime.now(timezone.utc).isoformat(),
    })
    tx.commit()
    return store.get("permissions", permission_id) | {"replayed": False}
