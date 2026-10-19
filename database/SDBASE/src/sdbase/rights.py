"""Version-specific rights for time, station, territory and output."""

from __future__ import annotations

from datetime import datetime
from uuid import UUID, uuid4

from .identity import Principal, require
from .storage import Store, StoreError


def grant(store: Store, principal: Principal, station_id: str, version_id: str, *,
          territory: str, output_id: str, starts_at: str, ends_at: str,
          decision: str = "allow", source_ref: str | None = None) -> dict:
    station_id, version_id = str(UUID(station_id)), str(UUID(version_id))
    require(store, principal, "approve", station_id)
    version = store.get("versions", version_id)
    if version is None or not store.lookup("station_items", "station_item", station_id,
                                            version["item_id"]):
        raise StoreError("version is not in station")
    if decision not in ("allow", "deny") or not territory or not output_id:
        raise StoreError("decision, territory and output are required")
    start, end = (datetime.fromisoformat(value.replace("Z", "+00:00"))
                  for value in (starts_at, ends_at))
    if start.tzinfo is None or end.tzinfo is None or end <= start:
        raise StoreError("rights interval must be timezone-aware and nonempty")
    return store.put("rights_grants", str(uuid4()), {
        "version_id": version_id, "station_id": station_id,
        "territory": territory, "output_id": output_id,
        "starts_at": start.isoformat(), "ends_at": end.isoformat(),
        "decision": decision, "source_ref": source_ref,
        "approved_by": principal.user_id,
    }, actor=principal.user_id)
