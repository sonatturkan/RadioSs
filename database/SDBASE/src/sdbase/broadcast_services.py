"""Bind one independent broadcast service to one endpoint and output.

An unbound historic ``default`` may be drafted. Its execution destination must
be resolved from one configured active output and frozen at approval. Explicit
service bindings are immutable.
"""

from __future__ import annotations

import re
from copy import deepcopy
from typing import Mapping
from uuid import UUID, uuid4

from .identity import Principal, require
from .station_outputs import lookup_output
from .storage import Conflict, Store, StoreError


_CODE = re.compile(r"[A-Za-z0-9_.:-]{1,100}\Z")


def validate_service_key(value: str) -> str:
    if not isinstance(value, str) or not _CODE.fullmatch(value):
        raise StoreError("service_key must be a short opaque code")
    return value


# Keep the small internal validator import stable for planning modules.
service_key = validate_service_key


def _uuid(value: str, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def service_for_key(store: Store, station_id: str, key: str) -> dict | None:
    key = validate_service_key(key)
    rows = store.lookup("broadcast_services", "station_service", station_id, key)
    return rows[0] if rows else None


def require_service(store: Store, station_id: str, key: str,
                    endpoint_id: str | None = None) -> dict | None:
    """Return a configured binding; unbound default is permitted for drafts.

    Approval and execution must use ``resolve_execution_binding`` or
    ``require_approved_execution_binding`` instead.
    """
    row = service_for_key(store, station_id, key)
    if row is None:
        if key == "default":
            if endpoint_id is not None and store.lookup(
                    "broadcast_services", "endpoint", endpoint_id):
                raise Conflict("endpoint is bound to another broadcast service")
            return None
        raise Conflict("broadcast service is not configured")
    if row["status"] != "active":
        raise Conflict("broadcast service is disabled")
    endpoint = store.get("endpoints", row["endpoint_id"])
    output = lookup_output(store, station_id, row["endpoint_id"], row["output_id"])
    if endpoint is None or endpoint.get("station_id") != station_id or \
            endpoint.get("status") == "retired" or output is None or \
            output.get("status") != "active":
        raise Conflict("broadcast service output or endpoint is unavailable")
    if endpoint_id is not None and row["endpoint_id"] != endpoint_id:
        raise Conflict("plan belongs to another broadcast service endpoint")
    return row


def resolve_execution_binding(store: Store, station_id: str, key: str,
                              entries: list[dict],
                              endpoint_id: str | None = None, *,
                              require_active_station: bool = True) -> dict:
    """Resolve one actual configured destination, never a wildcard output.

    For legacy default an explicitly declared sound output can select one
    destination. Otherwise all available unbound station outputs must identify
    exactly one endpoint/output pair. Merely supplying a delivery endpoint is
    not sufficient to turn an ambiguous station plan into an approved plan.
    """
    station_id = _uuid(station_id, "station_id")
    station = store.get("stations", station_id)
    if station is None or require_active_station and station.get("status") != "active":
        raise Conflict("execution requires an active station")
    if endpoint_id is not None:
        endpoint_id = _uuid(endpoint_id, "endpoint_id")
    key = validate_service_key(key)
    service = require_service(store, station_id, key, endpoint_id)
    if service is not None:
        output = lookup_output(store, station_id, service["endpoint_id"],
                               service["output_id"])
        return {"source": "broadcast_service", "service_key": key,
                "endpoint_id": service["endpoint_id"],
                "output_id": service["output_id"],
                "station_output_id": output["_id"],
                "broadcast_service_id": service["_id"]}
    declared: set[str] = set()
    for row in entries:
        if row.get("state", "planned") != "planned" or \
                row.get("event_kind", row.get("kind")) not in {"audio", "live", "stream"}:
            continue
        value = (row.get("payload") or {}).get("output_id")
        if value in (None, "*"):
            continue
        if not isinstance(value, str) or not _CODE.fullmatch(value):
            raise StoreError("execution output_id must be a short concrete output code")
        declared.add(value)
    if len(declared) > 1:
        raise Conflict("default plan sound entries address different outputs")
    requested = next(iter(declared), None)
    candidates = []
    for output in store.lookup("station_outputs", "station", station_id):
        if output["status"] != "active" or requested is not None and \
                output["output_id"] != requested:
            continue
        endpoint = store.get("endpoints", output["endpoint_id"])
        if endpoint is None or endpoint["station_id"] != station_id or \
                endpoint["status"] == "retired" or store.lookup(
                    "broadcast_services", "endpoint", output["endpoint_id"]):
            continue
        candidates.append(output)
    if len(candidates) != 1:
        raise Conflict("default execution needs one unambiguous active configured output")
    output = candidates[0]
    if endpoint_id is not None and output["endpoint_id"] != endpoint_id:
        raise Conflict("default execution output belongs to another endpoint")
    return {"source": "unique_default", "service_key": key,
            "endpoint_id": output["endpoint_id"], "output_id": output["output_id"],
            "station_output_id": output["_id"], "broadcast_service_id": None}


def require_approved_execution_binding(store: Store, station_id: str, key: str,
                                       revision: Mapping,
                                       endpoint_id: str | None = None, *,
                                       allow_disabled_station: bool = False) -> dict:
    """Check the frozen approval destination against current configuration."""
    approved = revision.get("execution_binding")
    if not isinstance(approved, dict):
        raise Conflict("approved plan lacks frozen execution binding; fork and approve again")
    required = {"source", "service_key", "endpoint_id", "output_id",
                "station_output_id", "broadcast_service_id"}
    if set(approved) != required or approved.get("service_key") != key or \
            approved.get("output_id") in (None, "*"):
        raise Conflict("approved execution binding is invalid")
    actual = resolve_execution_binding(store, station_id, key,
        [{"event_kind": "audio", "payload": {"output_id": approved["output_id"]}}],
        endpoint_id=endpoint_id, require_active_station=not allow_disabled_station)
    if actual != approved:
        raise Conflict("execution destination differs from approved binding")
    return deepcopy(approved)


def execution_entry(entry: Mapping, binding: Mapping) -> dict:
    """Resolve a default draft's output using its frozen approval binding."""
    result = deepcopy(dict(entry))
    payload = result.get("payload") or {}
    kind = result.get("event_kind", result.get("kind"))
    if result.get("state", "planned") == "planned" and \
            kind in {"audio", "live", "stream"} and \
            binding.get("source") == "unique_default" and \
            payload.get("output_id") in (None, "*"):
        result["payload"] = payload | {"output_id": binding["output_id"]}
    check_entry_scope(result, dict(binding))
    return result


def check_entry_scope(entry: dict, binding: dict | None) -> None:
    """Sound in a bound service must name its exact output for rights checks."""
    if binding is None or entry.get("state", "planned") != "planned":
        return
    kind = entry.get("event_kind", entry.get("kind"))
    payload = entry.get("payload") or {}
    if kind in {"audio", "live", "stream"} and \
            payload.get("output_id") != binding["output_id"]:
        raise Conflict("broadcast service sound entry needs its bound output_id")
    if payload.get("output_id") not in (None, binding["output_id"]):
        raise Conflict("plan entry addresses another broadcast service output")


def create_service(store: Store, principal: Principal, station_id: str,
                   service_key: str, name: str, endpoint_id: str,
                   output_id: str) -> dict:
    """Create a fixed service/output binding on an unused endpoint."""
    station_id = _uuid(station_id, "station_id")
    endpoint_id = _uuid(endpoint_id, "endpoint_id")
    key = validate_service_key(service_key)
    require(store, principal, "manage", station_id)
    if not isinstance(name, str) or not name.strip() or len(name) > 200:
        raise StoreError("service name must be 1..200 characters")
    if not isinstance(output_id, str) or not _CODE.fullmatch(output_id):
        raise StoreError("output_id must be a short opaque code")
    endpoint = store.get("endpoints", endpoint_id)
    if endpoint is None or endpoint.get("station_id") != station_id or \
            endpoint.get("status") == "retired":
        raise Conflict("service endpoint is unavailable in station")
    output = lookup_output(store, station_id, endpoint_id, output_id)
    if output is None or output["status"] != "active":
        raise Conflict("active station output is required")
    if service_for_key(store, station_id, key):
        raise Conflict("broadcast service key already exists")
    if store.lookup("broadcast_services", "endpoint", endpoint_id):
        raise Conflict("endpoint is already bound to a broadcast service")
    if store.get("active_acceptances", endpoint_id) or any(
            row["status"] in {"pending", "sent", "acknowledged", "active"}
            for row in store.lookup("plan_distributions", "endpoint", endpoint_id)):
        raise Conflict("endpoint has playout ownership or an outstanding delivery")
    identifier = str(uuid4())
    sequence = store.sequence
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("broadcast_services", identifier, {
        "station_id": station_id, "service_key": key, "name": name.strip(),
        "endpoint_id": endpoint_id, "output_id": output_id, "status": "active",
        "created_by": principal.user_id})
    tx.commit()
    return store.get("broadcast_services", identifier)


def list_services(store: Store, principal: Principal, station_id: str) -> list[dict]:
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "view", station_id)
    return sorted(store.lookup("broadcast_services", "station", station_id),
                  key=lambda row: row["service_key"])
