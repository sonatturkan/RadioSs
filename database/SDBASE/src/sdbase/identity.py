"""Users, scoped permissions and endpoint-bound bearer credentials."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
import hashlib
import secrets
from uuid import UUID, uuid4

from .storage import Store, StoreError


class Forbidden(StoreError):
    pass


@dataclass(frozen=True)
class Principal:
    user_id: str
    scope: str
    endpoint_id: str | None = None
    monitor_id: str | None = None
    credential_id: str | None = None


def _id(value: str) -> str:
    return str(UUID(str(value)))


def bootstrap(store: Store, username: str, station_code: str, station_name: str,
              timezone_name: str = "UTC") -> dict:
    if store.scan("users"):
        raise StoreError("bootstrap only runs before the first user exists")
    user_id, station_id, endpoint_id = (str(uuid4()) for _ in range(3))
    token = secrets.token_urlsafe(32)
    now = datetime.now(timezone.utc)
    tx = store.begin(actor="bootstrap")
    tx.put("users", user_id, {"username": username, "display_name": username,
                               "status": "active", "created_at": now.isoformat()})
    tx.put("stations", station_id, {"code": station_code, "name": station_name,
                                     "timezone": timezone_name, "status": "active",
                                     "created_at": now.isoformat()})
    tx.put("endpoints", endpoint_id, {"station_id": station_id, "code": "main",
                                       "name": "Main playout", "status": "offline"})
    for permission in ("view", "edit", "bulk_edit", "import", "approve", "activate",
                       "operate", "repair", "manage"):
        tx.put("permissions", str(uuid4()), {"user_id": user_id, "permission": permission,
                                              "station_id": None, "station_scope": "*"})
    tx.put("tokens", str(uuid4()), {"user_id": user_id,
                                    "token_hash": hashlib.sha256(token.encode()).hexdigest(),
                                    "expires_at": (now + timedelta(days=30)).isoformat(),
                                    "scope": "user", "revoked_at": None})
    tx.commit()
    return {"user_id": user_id, "station_id": station_id,
            "endpoint_id": endpoint_id, "api_token": token}


def authenticate(store: Store, token: str) -> Principal:
    if not isinstance(token, str) or len(token) < 20:
        raise Forbidden("invalid bearer token")
    digest = hashlib.sha256(token.encode()).hexdigest()
    matches = store.lookup("tokens", "hash", digest)
    if not matches:
        raise Forbidden("invalid bearer token")
    record = matches[0]
    if record.get("revoked_at") or datetime.fromisoformat(record["expires_at"]) <= datetime.now(timezone.utc):
        raise Forbidden("expired or revoked bearer token")
    user = store.get("users", record["user_id"])
    if user is None or user["status"] != "active":
        raise Forbidden("user is disabled")
    return Principal(record["user_id"], record["scope"], record.get("endpoint_id"),
                     record["_id"] if record["scope"] == "monitor" else None,
                     record["_id"])


def require(store: Store, principal: Principal, permission: str, station_id: str,
            *, endpoint_id: str | None = None) -> None:
    station_id = _id(station_id)
    if principal.credential_id is not None:
        credential = store.get("tokens", principal.credential_id)
        if credential is None or credential["user_id"] != principal.user_id or credential["scope"] != principal.scope or \
                credential.get("revoked_at") or datetime.fromisoformat(credential["expires_at"]) <= datetime.now(timezone.utc):
            raise Forbidden("credential expired or revoked after authentication")
    user = store.get("users", principal.user_id)
    if user is None or user["status"] != "active":
        raise Forbidden("user is disabled")
    if principal.scope == "endpoint":
        if permission != "operate" or endpoint_id is None or principal.endpoint_id != _id(endpoint_id):
            raise Forbidden("endpoint credential is limited to its playout endpoint")
        endpoint = store.get("endpoints", principal.endpoint_id)
        if endpoint is None or endpoint["station_id"] != station_id:
            raise Forbidden("endpoint does not belong to station")
    elif principal.scope != "user":
        raise Forbidden("unsupported principal scope")
    allowed = any(not row.get("revoked_at") and row["permission"] == permission and row.get("station_id") in (None, station_id)
                  for row in store.lookup("permissions", "user", principal.user_id))
    if not allowed:
        raise Forbidden(f"permission {permission} required")


def issue_endpoint_token(store: Store, principal: Principal, station_id: str,
                         endpoint_id: str, *, days: int = 30) -> str:
    require(store, principal, "manage", station_id)
    endpoint = store.get("endpoints", _id(endpoint_id))
    if endpoint is None or endpoint["station_id"] != _id(station_id):
        raise Forbidden("endpoint is outside station")
    if type(days) is not int or not 1 <= days <= 365:
        raise StoreError("token lifetime must be 1..365 days")
    token = secrets.token_urlsafe(32)
    store.put("tokens", str(uuid4()), {
        "user_id": principal.user_id,
        "token_hash": hashlib.sha256(token.encode()).hexdigest(),
        "expires_at": (datetime.now(timezone.utc) + timedelta(days=days)).isoformat(),
        "scope": "endpoint", "endpoint_id": _id(endpoint_id), "revoked_at": None,
    }, actor=principal.user_id)
    return token


def issue_monitor_token(store: Store, principal: Principal, station_id: str,
                        endpoint_id: str, *, days: int = 30) -> dict:
    """Give an independent output observer a credential scoped to one endpoint."""
    station_id, endpoint_id = _id(station_id), _id(endpoint_id)
    require(store, principal, "manage", station_id)
    endpoint = store.get("endpoints", endpoint_id)
    if endpoint is None or endpoint["station_id"] != station_id or endpoint["status"] == "retired":
        raise Forbidden("endpoint is outside station or retired")
    if isinstance(days, bool) or not isinstance(days, int) or not 1 <= days <= 365:
        raise StoreError("token lifetime must be 1..365 days")
    token, monitor_id = secrets.token_urlsafe(32), str(uuid4())
    store.put("tokens", monitor_id, {
        "user_id": principal.user_id,
        "token_hash": hashlib.sha256(token.encode()).hexdigest(),
        "expires_at": (datetime.now(timezone.utc) + timedelta(days=days)).isoformat(),
        "scope": "monitor", "endpoint_id": endpoint_id, "revoked_at": None,
    }, actor=principal.user_id)
    return {"monitor_id": monitor_id, "endpoint_id": endpoint_id, "token": token}
