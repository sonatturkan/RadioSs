"""Persistent, station-scoped fault evidence and operator incident workflow.

An endpoint or operator reports an observation. Reporting never claims that
sound actually reached listeners. Incidents group observations by a stable
correlation key and require an explicit operator resolution.
"""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from typing import Any, Mapping
from uuid import UUID, uuid4

from .identity import Principal, require
from .storage import Conflict, Store, StoreError


COMPONENTS = frozenset({"database", "network", "media", "playout", "output",
                        "scheduler", "advertising", "backup", "integration"})
CONDITIONS = frozenset({"degraded", "failed", "recovered"})
SEVERITIES = frozenset({"warning", "critical"})
IMPACTS = frozenset({"none", "risk", "on_air", "unknown"})
RESOLUTIONS = frozenset({"recovered", "false_alarm", "handed_off"})


def _id(value: Any, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _instant(value: Any, field: str) -> str:
    if not isinstance(value, str):
        raise StoreError(f"{field} must be an ISO timestamp")
    try:
        result = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise StoreError(f"{field} must be an ISO timestamp") from exc
    if result.tzinfo is None or result.utcoffset() is None:
        raise StoreError(f"{field} needs a timezone offset")
    return result.astimezone(timezone.utc).isoformat()


def _short(value: Any, field: str, limit: int) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > limit:
        raise StoreError(f"{field} must be nonempty text of at most {limit} characters")
    return value.strip()


def _details(value: Mapping[str, Any] | None) -> dict:
    if value is None:
        return {}
    if not isinstance(value, Mapping):
        raise StoreError("diagnostic details must be an object")
    details = dict(value)
    if len(details) > 24 or any(
            not isinstance(key, str) or len(key) > 64 or
            any(secret in key.lower() for secret in ("token", "password", "secret", "authorization"))
            for key in details):
        raise StoreError("diagnostic details contain too many or sensitive keys")
    try:
        encoded = json.dumps(details, ensure_ascii=False, sort_keys=True,
                             separators=(",", ":"), allow_nan=False)
    except (TypeError, ValueError) as exc:
        raise StoreError("diagnostic details must be finite JSON") from exc
    if len(encoded.encode("utf-8")) > 8192:
        raise StoreError("diagnostic details exceed 8 KiB")
    return json.loads(encoded)


def _fingerprint(body: dict) -> str:
    payload = json.dumps(body, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def record_diagnostic(store: Store, principal: Principal, station_id: str, *,
                      event_key: str, correlation_key: str, component: str,
                      condition: str, severity: str, impact: str, code: str,
                      message: str, occurred_at: str,
                      endpoint_id: str | None = None,
                      details: Mapping[str, Any] | None = None) -> dict:
    """Record one idempotent observation and update its incident atomically."""
    station_id = _id(station_id, "station_id")
    endpoint_id = _id(endpoint_id, "endpoint_id") if endpoint_id else None
    if principal.scope == "endpoint":
        if endpoint_id is None:
            raise StoreError("endpoint diagnostic needs endpoint_id")
        require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    else:
        require(store, principal, "repair", station_id)
    if store.get("stations", station_id) is None:
        raise StoreError("station is absent")
    if endpoint_id:
        endpoint = store.get("endpoints", endpoint_id)
        if endpoint is None or endpoint["station_id"] != station_id:
            raise StoreError("diagnostic endpoint belongs to another station")
    if component not in COMPONENTS or condition not in CONDITIONS or \
            severity not in SEVERITIES or impact not in IMPACTS:
        raise StoreError("invalid diagnostic component, condition, severity or impact")
    body = {"station_id": station_id,
            "event_key": _short(event_key, "event_key", 160),
            "correlation_key": _short(correlation_key, "correlation_key", 160),
            "endpoint_id": endpoint_id, "component": component,
            "condition": condition, "severity": severity, "impact": impact,
            "code": _short(code, "code", 80),
            "message": _short(message, "message", 500),
            "occurred_at": _instant(occurred_at, "occurred_at"),
            "details": _details(details)}
    fingerprint = _fingerprint(body)
    previous = store.lookup("diagnostic_events", "station_event", station_id, body["event_key"])
    if previous:
        if previous[0]["fingerprint"] != fingerprint:
            raise Conflict("diagnostic event key was reused with different data")
        return {"event": previous[0],
                "incident": store.get("incidents", previous[0]["incident_id"]),
                "idempotent": True}
    current = store.lookup("incidents", "station_correlation", station_id,
                           body["correlation_key"])
    incident = current[0] if current else None
    if incident and incident["component"] != component:
        raise Conflict("correlation key already belongs to another component")
    if condition == "recovered" and incident is None:
        raise Conflict("recovery has no incident to update")
    event_id = str(uuid4())
    incident_id = incident["_id"] if incident else str(uuid4())
    now = datetime.now(timezone.utc).isoformat()
    event = body | {"incident_id": incident_id,
                    "fingerprint": fingerprint, "recorded_at": now,
                    "reporter_id": principal.user_id}
    if incident:
        revised = {key: value for key, value in incident.items()
                   if key not in {"_id", "_rev"}}
        revised["occurrence_count"] = incident["occurrence_count"] + 1
        # Offline endpoints can deliver an older observation after a newer one.
        # Preserve evidence without letting it rewind or reopen incident state.
        if _instant(body["occurred_at"], "occurred_at") > \
                _instant(incident["last_seen_at"], "last_seen_at"):
            revised.update({"last_event_id": event_id,
                            "last_seen_at": body["occurred_at"],
                            "last_condition": condition, "severity": severity,
                            "impact": impact})
            if condition == "recovered":
                revised["recovery_observed_at"] = body["occurred_at"]
            elif incident["state"] == "resolved":
                revised.update({"state": "open", "reopened_at": now,
                                "resolution": None, "resolved_at": None,
                                "resolved_by": None})
    else:
        revised = {"station_id": station_id,
                   "correlation_key": body["correlation_key"],
                   "component": component, "endpoint_id": endpoint_id,
                   "state": "open", "severity": severity, "impact": impact,
                   "first_event_id": event_id, "last_event_id": event_id,
                   "first_seen_at": body["occurred_at"],
                   "last_seen_at": body["occurred_at"],
                   "last_condition": condition, "occurrence_count": 1,
                   "assigned_user_id": None}
    tx = store.begin(actor=principal.user_id)
    tx.put("diagnostic_events", event_id, event)
    tx.put("incidents", incident_id, revised,
           expected_revision=incident["_rev"] if incident else None)
    records = tx.commit()
    return {"event": records[0], "incident": records[1], "idempotent": False}


def list_incidents(store: Store, principal: Principal, station_id: str, *,
                   state: str | None = None) -> list[dict]:
    station_id = _id(station_id, "station_id")
    require(store, principal, "view", station_id)
    if state is not None and state not in {"open", "acknowledged", "resolved"}:
        raise StoreError("invalid incident state")
    rows = store.lookup("incidents", "station", station_id)
    return sorted((row for row in rows if state is None or row["state"] == state),
                  key=lambda row: (row["last_seen_at"], row["_id"]), reverse=True)


def update_incident(store: Store, principal: Principal, station_id: str,
                    incident_id: str, *, expected_revision: int,
                    action: str, assigned_user_id: str | None = None,
                    resolution: str | None = None, note: str | None = None) -> dict:
    """Assign, acknowledge, or explicitly resolve an incident with CAS."""
    station_id = _id(station_id, "station_id")
    incident_id = _id(incident_id, "incident_id")
    require(store, principal, "repair", station_id)
    row = store.get("incidents", incident_id)
    if row is None or row["station_id"] != station_id:
        raise StoreError("incident is absent from station")
    if isinstance(expected_revision, bool) or not isinstance(expected_revision, int) or \
            expected_revision != row["_rev"]:
        raise Conflict("incident changed")
    if action not in {"assign", "acknowledge", "resolve"}:
        raise StoreError("invalid incident action")
    if row["state"] == "resolved":
        raise Conflict("resolved incident cannot be changed until a new fault reopens it")
    revised = {key: value for key, value in row.items() if key not in {"_id", "_rev"}}
    now = datetime.now(timezone.utc).isoformat()
    if action == "assign":
        assignee = _id(assigned_user_id, "assigned_user_id")
        if store.get("users", assignee) is None:
            raise StoreError("assigned user is absent")
        revised["assigned_user_id"] = assignee
        revised["assigned_at"] = now
    elif action == "acknowledge":
        revised.update({"state": "acknowledged", "acknowledged_by": principal.user_id,
                        "acknowledged_at": now})
    else:
        if resolution not in RESOLUTIONS:
            raise StoreError("resolution must be recovered, false_alarm or handed_off")
        revised.update({"state": "resolved", "resolution": resolution,
                        "resolved_by": principal.user_id, "resolved_at": now})
    if note is not None:
        revised["operator_note"] = _short(note, "note", 1000)
    return store.put("incidents", incident_id, revised,
                     expected_revision=expected_revision, actor=principal.user_id)
