"""Configured station outputs and exact routing area codes.

The codes are opaque local identifiers; this module does not infer geographic
containment or implement any country's emergency-alert geocode standard.
"""

from __future__ import annotations

import re
from uuid import UUID, uuid4

from .identity import Principal, require
from .storage import Conflict, Store, StoreError


OUTPUT_STATUSES = frozenset({"active", "disabled"})
_CODE = re.compile(r"[A-Za-z0-9_.:-]{1,100}\Z")


def _uuid(value: str, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _code(value: str, field: str) -> str:
    if not isinstance(value, str) or not _CODE.fullmatch(value):
        raise StoreError(f"{field} must be a short opaque code")
    return value


def _geos(values: list[str] | tuple[str, ...]) -> list[str]:
    if not isinstance(values, (list, tuple)):
        raise StoreError("geo_codes must be a list")
    result = sorted({_code(code, "geo_code") for code in values})
    if not result:
        raise StoreError("station output needs at least one geo code")
    return result


def _endpoint(store: Store, station_id: str, endpoint_id: str) -> dict:
    endpoint = store.get("endpoints", endpoint_id)
    if endpoint is None or endpoint["station_id"] != station_id or endpoint["status"] == "retired":
        raise Conflict("endpoint is not available in station")
    return endpoint


def lookup_output(store: Store, station_id: str, endpoint_id: str,
                  output_id: str) -> dict | None:
    """Internal lookup; caller must enforce station permission."""
    rows = store.lookup("station_outputs", "station_endpoint_output",
                        station_id, endpoint_id, output_id)
    return rows[0] if rows else None


def get_station_output(store: Store, principal: Principal, station_id: str,
                       endpoint_id: str, output_id: str) -> dict:
    station_id, endpoint_id = (_uuid(station_id, "station_id"),
                               _uuid(endpoint_id, "endpoint_id"))
    require(store, principal, "view", station_id)
    _endpoint(store, station_id, endpoint_id)
    row = lookup_output(store, station_id, endpoint_id, _code(output_id, "output_id"))
    if row is None:
        raise Conflict("station output is absent")
    return row


def create_station_output(store: Store, principal: Principal, station_id: str,
                          endpoint_id: str, output_id: str, *,
                          geo_codes: list[str] | tuple[str, ...]) -> dict:
    station_id, endpoint_id = (_uuid(station_id, "station_id"),
                               _uuid(endpoint_id, "endpoint_id"))
    require(store, principal, "manage", station_id)
    _endpoint(store, station_id, endpoint_id)
    output_id = _code(output_id, "output_id")
    geos = _geos(geo_codes)
    if lookup_output(store, station_id, endpoint_id, output_id):
        raise Conflict("station output already exists")
    sequence = store.sequence
    body = {"station_id": station_id, "endpoint_id": endpoint_id,
            "output_id": output_id, "geo_codes": geos,
            "status": "active", "created_by": principal.user_id}
    identifier = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("station_outputs", identifier, body)
    tx.commit()
    return store.get("station_outputs", identifier)


def update_station_output(store: Store, principal: Principal, station_id: str,
                          output_record_id: str, *, expected_revision: int,
                          geo_codes: list[str] | tuple[str, ...] | None = None,
                          status: str | None = None) -> dict:
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "manage", station_id)
    row = store.get("station_outputs", _uuid(output_record_id, "output_record_id"))
    if row is None or row["station_id"] != station_id:
        raise Conflict("station output is outside station")
    if row["_rev"] != expected_revision:
        raise Conflict("station output changed")
    if geo_codes is None and status is None:
        raise StoreError("geo_codes or status change is required")
    body = {key: value for key, value in row.items() if not key.startswith("_")}
    if geo_codes is not None:
        body["geo_codes"] = _geos(geo_codes)
    if status is not None:
        if status not in OUTPUT_STATUSES:
            raise StoreError("invalid station output status")
        body["status"] = status
    if body == {key: value for key, value in row.items() if not key.startswith("_")}:
        return row
    return store.put("station_outputs", row["_id"], body,
                     expected_revision=expected_revision, actor=principal.user_id)


def list_station_outputs(store: Store, principal: Principal, station_id: str,
                         endpoint_id: str | None = None) -> list[dict]:
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "view", station_id)
    if endpoint_id is not None:
        endpoint_id = _uuid(endpoint_id, "endpoint_id")
        _endpoint(store, station_id, endpoint_id)
        rows = store.lookup("station_outputs", "endpoint", endpoint_id)
    else:
        rows = store.lookup("station_outputs", "station", station_id)
    return sorted((row for row in rows if row["station_id"] == station_id),
                  key=lambda row: (row["endpoint_id"], row["output_id"]))
