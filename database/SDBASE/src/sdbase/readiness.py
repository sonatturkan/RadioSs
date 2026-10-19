"""Explain whether a version can be prepared or played for a specific slot."""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Mapping, Any
from uuid import UUID

from .storage import Store, StoreError


def execution_scope(payload: Mapping[str, Any]) -> tuple[str, str]:
    """Read an entry's rights scope; ``*`` output means unresolved execution.

    In a grant ``*`` permits every output. In a query or draft entry it does
    not prove which output will execute the entry; execution callers must
    resolve that value against a configured broadcast service first.
    """
    declared_territory = payload.get("territory")
    region_id = payload.get("region_id")
    if declared_territory is not None and region_id is not None and \
            declared_territory != region_id:
        raise StoreError("audio entry territory and region disagree")
    territory = declared_territory if declared_territory is not None else \
        region_id if region_id is not None else "*"
    output_id = payload.get("output_id", "*")
    if any(not isinstance(value, str) or not value.strip()
           for value in (territory, output_id)):
        raise StoreError("audio entry rights scope must contain nonempty strings")
    return territory, output_id


def _time(value: str | datetime) -> datetime:
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00")) if isinstance(value, str) else value
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError("readiness slot needs timezone-aware start/end")
    return parsed.astimezone(timezone.utc)


def _advertising_target_matches(source: Mapping[str, Any], field: str,
                                requested: str) -> bool:
    allowed = source.get(field)
    return allowed is None or (
        not isinstance(allowed, (str, bytes)) and
        isinstance(allowed, (list, tuple, set)) and requested in allowed)


def _advertising_scope_matches(source: Mapping[str, Any], territory: str,
                               output_id: str) -> bool:
    """Apply the same explicit campaign/spot targets as traffic selection."""
    return (_advertising_target_matches(source, "region_ids", territory) and
            _advertising_target_matches(source, "output_ids", output_id))


def _advertising_window_covers(campaign: Mapping[str, Any],
                               spot: Mapping[str, Any],
                               start: datetime, end: datetime) -> bool:
    """Malformed optional dates fail closed instead of approving the spot."""
    try:
        if _time(campaign["starts_at"]) > start or _time(campaign["ends_at"]) < end:
            return False
        if spot.get("starts_at") is not None and _time(spot["starts_at"]) > start:
            return False
        if spot.get("ends_at") is not None and _time(spot["ends_at"]) < end:
            return False
        if spot.get("approved_at") is not None and _time(spot["approved_at"]) > start:
            return False
    except (KeyError, TypeError, ValueError, AttributeError, StoreError):
        return False
    return True


def readiness_at(store: Store, station_id: str, version_id: str, *,
                 slot_start: str | datetime, slot_end: str | datetime,
                 endpoint_id: str | None = None, require_endpoint_copy: bool = True,
                 territory: str = "*", output_id: str = "*") -> dict:
    station_id, version_id = str(UUID(station_id)), str(UUID(version_id))
    start, end = _time(slot_start), _time(slot_end)
    if end <= start:
        raise StoreError("readiness slot end must follow start")
    if any(not isinstance(scope, str) or not scope.strip()
           for scope in (territory, output_id)):
        raise StoreError("readiness territory and output must be nonempty strings")
    reasons: list[str] = []
    version = store.get("versions", version_id)
    if version is None:
        raise StoreError("version not found")
    item = store.get("items", version["item_id"])
    if item is None:
        raise StoreError("item not found")
    station_item = store.lookup("station_items", "station_item", station_id, item["_id"])
    if not station_item:
        reasons.append("item_not_in_station")
    elif not station_item[0]["eligible"]:
        reasons.append("station_not_eligible")
    if item["lifecycle"] != "active":
        reasons.append("item_not_active")
    if version["status"] != "approved":
        reasons.append("version_not_approved")
    from .metadata import missing_required
    if missing_required(store, station_id, item["_id"], version_id, stage="activate"):
        reasons.append("required_metadata_missing")
    if version.get("valid_from") and start < _time(version["valid_from"]):
        reasons.append("version_not_yet_valid")
    if version.get("valid_to") and end > _time(version["valid_to"]):
        reasons.append("version_expired_for_slot")

    grants = store.lookup("rights_grants", "version", version_id)
    station_grants = [grant for grant in grants
                      if grant["station_id"] == station_id]
    # A catalog query for every/unknown output must include a specific deny.
    # Wildcard allow grants remain permissions, never proof of the execution
    # destination. Likewise an unspecified territory cannot hide a deny for
    # one territory. Concrete queries continue to ignore unrelated scopes.
    denied_grants = [grant for grant in station_grants
                     if (territory == "*" or grant["territory"] in ("*", territory))
                     and (output_id == "*" or grant["output_id"] in ("*", output_id))]
    allowed_grants = [grant for grant in station_grants
                      if grant["territory"] in ("*", territory)
                      and grant["output_id"] in ("*", output_id)]
    # A permission must cover the entire planned slot. A prohibition blocks
    # any overlapping part of it; interval endpoints are half-open.
    if any(grant["decision"] == "deny"
           and _time(grant["starts_at"]) < end
           and _time(grant["ends_at"]) > start
           for grant in denied_grants):
        reasons.append("rights_denied_for_slot")
    elif not any(grant["decision"] == "allow"
                 and _time(grant["starts_at"]) <= start
                 and _time(grant["ends_at"]) >= end
                 for grant in allowed_grants):
        reasons.append("rights_unconfirmed_for_slot")

    if item["kind"] == "advertisement":
        valid_spots = []
        for spot in store.lookup("spots", "version", version_id):
            campaign = store.get("campaigns", spot["campaign_id"])
            if (spot["status"] == "approved" and campaign
                    and campaign["status"] in ("approved", "active")
                    and _advertising_target_matches(campaign, "station_ids", station_id)
                    and _advertising_target_matches(spot, "station_ids", station_id)
                    and _advertising_scope_matches(campaign, territory, output_id)
                    and _advertising_scope_matches(spot, territory, output_id)
                    and _advertising_window_covers(campaign, spot, start, end)):
                valid_spots.append(spot["_id"])
        if not valid_spots:
            reasons.append("no_valid_campaign_for_slot")
    else:
        valid_spots = []

    central_files = []
    for media in store.lookup("media_files", "version", version_id):
        if media["status"] != "verified" or not media.get("sha256"):
            continue
        if any(location["status"] in ("present", "verified") for location in
               store.lookup("media_locations", "file", media["_id"])):
            central_files.append(media)
    if not central_files:
        reasons.append("central_media_unverified")
    selected = central_files[0] if central_files else None

    if require_endpoint_copy:
        if endpoint_id is None:
            raise StoreError("endpoint_id is required for playable readiness")
        endpoint_id = str(UUID(endpoint_id))
        endpoint = store.get("endpoints", endpoint_id)
        if endpoint is None or endpoint["station_id"] != station_id or endpoint["status"] == "retired":
            reasons.append("endpoint_not_in_station")
        else:
            verified_ids = {copy["media_file_id"] for copy in
                            store.lookup("endpoint_copies", "endpoint", endpoint_id)
                            if copy["status"] == "verified"}
            selected = next((media for media in central_files
                             if media["_id"] in verified_ids and any(
                                 copy["sha256"] == media["sha256"]
                                 for copy in store.lookup("endpoint_copies", "endpoint_file",
                                                          endpoint_id, media["_id"]))), None)
            if selected is None:
                reasons.append("endpoint_media_unverified")
    return {"ready": not reasons, "blocked_reasons": reasons,
            "station_id": station_id, "version_id": version_id,
            "slot_start": start.isoformat(), "slot_end": end.isoformat(),
            "media_file_id": selected["_id"] if selected else None,
            "valid_spot_ids": valid_spots,
            "territory": territory, "output_id": output_id,
            "output_scope_resolved": output_id != "*",
            "stage": "playable" if require_endpoint_copy else "distributable"}


def readiness_now(store: Store, station_id: str, version_id: str, *,
                  endpoint_id: str, territory: str = "*", output_id: str = "*") -> dict:
    from datetime import timedelta
    now = datetime.now(timezone.utc)
    return readiness_at(store, station_id, version_id, slot_start=now,
                        slot_end=now + timedelta(seconds=1), endpoint_id=endpoint_id,
                        territory=territory, output_id=output_id)
