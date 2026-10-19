"""Transactional traffic planning and evidence-labelled campaign reconciliation.

Selection uses the pure rules in ``ads.py``. A reservation writes its audio
entries and placements in one SDBASE transaction, guarded by the complete
store sequence so another planner cannot consume the same obligation between
selection and commit. Output activity is never called proof of a specific ad.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import json
from typing import Any
from uuid import UUID, uuid4

from .ads import calculate_fulfillment, select_break
from .broadcast_services import require_service
from .identity import Principal, require
from .planning import validate_entry, validate_plan
from .readiness import readiness_at
from .storage import Conflict, Store, StoreError


def _id(value: Any, name: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def _time(value: str, name: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (AttributeError, ValueError) as exc:
        raise StoreError(f"{name} must be an ISO instant") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError(f"{name} must include a timezone")
    return parsed.astimezone(timezone.utc)


def _body(row: dict) -> dict:
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _sha(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                     separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def _targets(values: list[str] | None, name: str) -> list[str] | None:
    if values is None:
        return None
    if not isinstance(values, list) or not values or any(
            not isinstance(value, str) or not value.strip() for value in values):
        raise StoreError(f"{name} must be a nonempty list of codes")
    return sorted(set(values))


def create_advertiser(store: Store, principal: Principal, station_id: str,
                      name: str) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "manage", station_id)
    if not isinstance(name, str) or not name.strip():
        raise StoreError("advertiser name is required")
    return store.put("advertisers", str(uuid4()), {
        "name": name.strip(), "created_by": principal.user_id,
        "created_at": datetime.now(timezone.utc).isoformat()}, actor=principal.user_id)


def create_campaign(store: Store, principal: Principal, station_id: str,
                    advertiser_id: str, name: str, starts_at: str,
                    ends_at: str, *, region_ids: list[str] | None = None,
                    output_ids: list[str] | None = None) -> dict:
    sequence = store.sequence
    station_id, advertiser_id = _id(station_id, "station_id"), _id(advertiser_id, "advertiser_id")
    require(store, principal, "edit", station_id)
    if store.get("advertisers", advertiser_id) is None:
        raise StoreError("advertiser is absent")
    if not isinstance(name, str) or not name.strip():
        raise StoreError("campaign name is required")
    start, end = _time(starts_at, "starts_at"), _time(ends_at, "ends_at")
    if end <= start:
        raise StoreError("campaign interval is empty")
    body = {"advertiser_id": advertiser_id, "name": name.strip(),
            "starts_at": start.isoformat(), "ends_at": end.isoformat(),
            "station_ids": [station_id], "region_ids": _targets(region_ids, "region_ids"),
            "output_ids": _targets(output_ids, "output_ids"),
            "status": "draft", "created_by": principal.user_id,
            "created_at": datetime.now(timezone.utc).isoformat()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    return tx.put("campaigns", str(uuid4()), body).commit()[0]


def approve_campaign(store: Store, principal: Principal, station_id: str,
                     campaign_id: str, *, expected_revision: int) -> dict:
    sequence = store.sequence
    station_id, campaign_id = _id(station_id, "station_id"), _id(campaign_id, "campaign_id")
    require(store, principal, "approve", station_id)
    campaign = store.get("campaigns", campaign_id)
    if campaign is None or station_id not in (campaign.get("station_ids") or []):
        raise StoreError("campaign is outside station")
    if campaign["_rev"] != expected_revision or campaign["status"] != "draft":
        raise Conflict("only the current draft campaign can be approved")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    return tx.put("campaigns", campaign_id, _body(campaign) | {
        "status": "approved", "approved_by": principal.user_id,
        "approved_at": datetime.now(timezone.utc).isoformat()},
        expected_revision=expected_revision).commit()[0]


def approve_spot(store: Store, principal: Principal, station_id: str,
                 campaign_id: str, version_id: str, *, position: str = "any",
                 region_ids: list[str] | None = None,
                 output_ids: list[str] | None = None,
                 competitive_categories: list[str] | None = None) -> dict:
    sequence = store.sequence
    station_id, campaign_id, version_id = (_id(station_id, "station_id"),
                                           _id(campaign_id, "campaign_id"),
                                           _id(version_id, "version_id"))
    require(store, principal, "approve", station_id)
    campaign = store.get("campaigns", campaign_id)
    version = store.get("versions", version_id)
    item = store.get("items", version["item_id"]) if version else None
    if campaign is None or station_id not in (campaign.get("station_ids") or []) or \
            version is None or version["status"] != "approved" or \
            item is None or item["kind"] != "advertisement" or \
            not store.lookup("station_items", "station_item", station_id, item["_id"]):
        raise StoreError("approved spot needs an approved advertisement version in station")
    if position not in {"any", "first", "last"}:
        raise StoreError("spot position must be any, first or last")
    if store.lookup("spots", "campaign_version", campaign_id, version_id):
        raise Conflict("campaign already has this spot version")
    body = {"campaign_id": campaign_id, "version_id": version_id,
            "status": "approved", "position": position,
            "region_ids": _targets(region_ids, "region_ids"),
            "output_ids": _targets(output_ids, "output_ids"),
            "competitive_categories": _targets(competitive_categories, "competitive_categories"),
            "approved_by": principal.user_id,
            "approved_at": datetime.now(timezone.utc).isoformat()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    return tx.put("spots", str(uuid4()), body).commit()[0]


def create_obligation(store: Store, principal: Principal, station_id: str,
                      campaign_id: str, required_plays: int, window_start: str,
                      window_end: str, *, priority: int = 0,
                      region_ids: list[str] | None = None,
                      output_ids: list[str] | None = None) -> dict:
    sequence = store.sequence
    station_id, campaign_id = _id(station_id, "station_id"), _id(campaign_id, "campaign_id")
    require(store, principal, "edit", station_id)
    campaign = store.get("campaigns", campaign_id)
    if campaign is None or station_id not in (campaign.get("station_ids") or []):
        raise StoreError("campaign is outside station")
    if (isinstance(required_plays, bool) or not isinstance(required_plays, int) or
            required_plays < 1 or isinstance(priority, bool) or
            not isinstance(priority, int) or priority < 0):
        raise StoreError("required_plays and priority must be valid positive counts")
    start, end = _time(window_start, "window_start"), _time(window_end, "window_end")
    if end <= start or start < _time(campaign["starts_at"], "campaign.starts_at") or \
            end > _time(campaign["ends_at"], "campaign.ends_at"):
        raise StoreError("obligation window must be inside campaign")
    body = {"campaign_id": campaign_id, "station_id": station_id,
            "required_plays": required_plays, "window_start": start.isoformat(),
            "window_end": end.isoformat(), "priority": priority,
            "region_ids": _targets(region_ids, "region_ids"),
            "output_ids": _targets(output_ids, "output_ids"),
            "created_by": principal.user_id,
            "created_at": datetime.now(timezone.utc).isoformat()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    return tx.put("obligations", str(uuid4()), body).commit()[0]


def _draft_break(store: Store, station_id: str, plan_id: str,
                 revision_id: str, break_entry_id: str) -> tuple[dict, dict, dict, dict, list[dict]]:
    plan = store.get("plans", plan_id)
    revision = store.get("plan_revisions", revision_id)
    entry = store.get("plan_entries", break_entry_id)
    station = store.get("stations", station_id)
    if station is None or plan is None or plan.get("station_id") != station_id or \
            revision is None or revision.get("plan_id") != plan_id or \
            revision.get("status") != "draft" or plan.get("draft_revision_id") != revision_id:
        raise Conflict("break needs the current draft plan revision")
    if entry is None or entry.get("plan_revision_id") != revision_id or \
            entry.get("event_kind") != "ad_break" or entry.get("state") != "planned":
        raise Conflict("break entry is absent or not planned in this draft")
    capacity = entry.get("planned_duration_ms")
    if isinstance(capacity, bool) or not isinstance(capacity, int) or capacity < 1:
        raise StoreError("advertising break needs positive fixed capacity")
    if entry.get("duration_kind") != "fixed":
        raise StoreError("advertising break capacity must be fixed")
    rows = store.lookup("plan_entries", "revision", revision_id)
    if any(row.get("ad_break_id") == break_entry_id and row.get("state") == "planned"
           for row in rows):
        raise Conflict("planned audio already occupies this break")
    return plan, revision, entry, station, rows


def _reserved_counts(store: Store, as_of: datetime) -> dict[str, int]:
    """Count future current/draft reservations once per plan.

    A past unverified attempt remains in the reconciliation report, but no
    longer consumes a future scheduling slot. It can therefore be made good.
    """
    grouped: dict[tuple[str, str, str], int] = {}
    for placement in store.scan("placements"):
        if placement.get("state") != "planned":
            continue
        entry = store.get("plan_entries", placement["plan_entry_id"])
        revision = store.get("plan_revisions", entry["plan_revision_id"]) if entry else None
        plan = store.get("plans", revision["plan_id"]) if revision else None
        if entry is None or entry.get("state") != "planned" or \
                _time(entry["scheduled_at"], "scheduled_at") < as_of or plan is None or \
                revision["status"] not in {"draft", "approved"} or \
                revision["_id"] not in {plan.get("draft_revision_id"),
                                        plan.get("current_revision_id")}:
            continue
        key = (placement["obligation_id"], plan["_id"], revision["_id"])
        grouped[key] = grouped.get(key, 0) + 1
    per_plan: dict[tuple[str, str], int] = {}
    for (obligation_id, plan_id, _revision_id), count in grouped.items():
        key = (obligation_id, plan_id)
        per_plan[key] = max(per_plan.get(key, 0), count)
    totals: dict[str, int] = {}
    for (obligation_id, _plan_id), count in per_plan.items():
        totals[obligation_id] = totals.get(obligation_id, 0) + count
    return totals


def _offers(store: Store, station_id: str, reserved: dict[str, int]) -> tuple[list[dict], list[dict]]:
    offers: list[dict] = []
    rejected: list[dict] = []
    spots_by_campaign: dict[str, list[dict]] = {}
    for spot in store.scan("spots"):
        spots_by_campaign.setdefault(spot["campaign_id"], []).append(spot)
    for obligation in store.lookup("obligations", "station", station_id):
        campaign = store.get("campaigns", obligation["campaign_id"])
        if campaign is None:
            continue
        for spot in spots_by_campaign.get(campaign["_id"], []):
            offer_id = f"{obligation['_id']}:{spot['_id']}"
            version = store.get("versions", spot["version_id"])
            item = store.get("items", version["item_id"]) if version else None
            if version is None or item is None or item.get("kind") != "advertisement":
                rejected.append({"offer_id": offer_id, "reasons": ["spot_version_invalid"]})
                continue
            media = [row for row in store.lookup("media_files", "version", version["_id"])
                     if row.get("status") == "verified" and
                     isinstance((row.get("technical") or {}).get("duration_ms"), int) and
                     not isinstance((row.get("technical") or {}).get("duration_ms"), bool) and
                     (row.get("technical") or {})["duration_ms"] > 0 and
                     any(location.get("status") in {"present", "verified"}
                         for location in store.lookup("media_locations", "file", row["_id"]))]
            if not media:
                rejected.append({"offer_id": offer_id, "reasons": ["verified_duration_missing"]})
                continue
            media.sort(key=lambda row: row["_id"])
            chosen = media[0]
            offers.append({"id": offer_id,
                           "duration_ms": chosen["technical"]["duration_ms"],
                           "media_file_id": chosen["_id"],
                           "priority": obligation.get("priority", 0),
                           "competitive_categories": (spot.get("competitive_categories")
                                                      if spot.get("competitive_categories") is not None
                                                      else campaign.get("competitive_categories") or []),
                           "campaign": _body(campaign) | {"id": campaign["_id"]},
                           "spot": _body(spot) | {"id": spot["_id"]},
                           "obligation": _body(obligation) | {
                               "id": obligation["_id"], "accepted_plays": 0,
                               "reserved_plays": reserved.get(obligation["_id"], 0)}})
    return offers, rejected


def _proposal(store: Store, principal: Principal, station_id: str,
              plan_id: str, revision_id: str, break_entry_id: str,
              *, output_id: str, region_id: str | None, max_spots: int | None,
              advertiser_gap_slots: int, category_gap_slots: int,
              offer_filter: set[str] | None = None,
              permission: str = "edit") -> tuple[dict, dict, dict, dict, list[dict], dict[str, dict]]:
    sequence = store.sequence
    require(store, principal, permission, station_id)
    plan, revision, break_entry, station, entries = _draft_break(
        store, station_id, plan_id, revision_id, break_entry_id)
    if not isinstance(output_id, str) or not output_id.strip():
        raise StoreError("output_id is required")
    binding = require_service(store, station_id, plan.get("service_key", "default"))
    if binding is not None and output_id != binding["output_id"]:
        raise Conflict("traffic output differs from broadcast service output")
    outputs = [row for row in store.lookup("station_outputs", "station", station_id)
               if row.get("output_id") == output_id and row.get("status") == "active" and
               (binding is None or row.get("endpoint_id") == binding["endpoint_id"])]
    if not outputs:
        raise Conflict("advertising output is not configured and active")
    if region_id is not None and (not isinstance(region_id, str) or not region_id or
                                  not any(region_id in row.get("geo_codes", []) for row in outputs)):
        raise StoreError("region_id is outside configured output coverage")
    if break_entry.get("payload", {}).get("output_id") not in {None, output_id}:
        raise Conflict("break output differs from traffic output")
    slot = {"id": break_entry_id, "station_id": station_id,
            "output_id": output_id, "region_id": region_id,
            "starts_at": break_entry["scheduled_at"],
            "capacity_ms": break_entry["planned_duration_ms"],
            "advertiser_gap_slots": advertiser_gap_slots,
            "category_gap_slots": category_gap_slots}
    if max_spots is not None:
        slot["max_spots"] = max_spots
    offers, pre_rejected = _offers(store, station_id,
                                   _reserved_counts(store, datetime.now(timezone.utc)))
    if offer_filter is not None:
        offers = [offer for offer in offers if offer["id"] in offer_filter]
        pre_rejected = [row for row in pre_rejected if row["offer_id"] in offer_filter]
    by_offer = {offer["id"]: offer for offer in offers}
    excluded: dict[str, list[str]] = {}
    while True:
        decision = select_break(slot, [offer for offer in offers if offer["id"] not in excluded])
        newly_excluded = {}
        for placement in decision["selected"]:
            offer = by_offer[placement["offer_id"]]
            start = _time(placement["scheduled_at"], "scheduled_at")
            end = start + timedelta(milliseconds=placement["duration_ms"])
            ready = readiness_at(store, station_id, offer["spot"]["version_id"],
                                 slot_start=start, slot_end=end,
                                 require_endpoint_copy=False,
                                 territory=region_id or "*", output_id=output_id)
            if not ready["ready"]:
                newly_excluded[offer["id"]] = [f"not_ready:{reason}" for reason in ready["blocked_reasons"]]
            elif offer["spot"]["id"] not in ready["valid_spot_ids"]:
                newly_excluded[offer["id"]] = ["selected_spot_not_valid_for_slot"]
            elif (_time(offer["obligation"]["window_start"], "window_start") > start or
                  _time(offer["obligation"]["window_end"], "window_end") < end):
                newly_excluded[offer["id"]] = ["obligation_does_not_cover_slot"]
            elif ready["media_file_id"] != offer["media_file_id"]:
                newly_excluded[offer["id"]] = ["selected_media_differs_from_verified_copy"]
        if not newly_excluded:
            break
        excluded.update(newly_excluded)
    decision["rejected"].extend(pre_rejected)
    decision["rejected"].extend({"offer_id": key, "reasons": reasons}
                                for key, reasons in sorted(excluded.items()))
    if store.sequence != sequence:
        raise Conflict("traffic inputs changed during break selection")
    preview = {"station_id": station_id, "plan_id": plan_id,
               "plan_revision_id": revision_id, "plan_revision": plan["_rev"],
               "break_entry_id": break_entry_id, "output_id": output_id,
               "region_id": region_id, "store_sequence": sequence,
               "offer_filter": sorted(offer_filter) if offer_filter is not None else None,
               "selected": decision["selected"], "rejected": decision["rejected"],
               "used_ms": decision["used_ms"],
               "remaining_capacity_ms": decision["remaining_capacity_ms"],
               "unfilled_obligations": decision["unfilled_obligations"]}
    preview["preview_hash"] = _sha({key: value for key, value in preview.items()
                                     if key not in {"preview_hash", "store_sequence"}})
    return preview, plan, revision, station, entries, by_offer


def preview_break(store: Store, principal: Principal, station_id: str,
                  plan_id: str, revision_id: str, break_entry_id: str, *,
                  output_id: str, region_id: str | None = None,
                  max_spots: int | None = None, advertiser_gap_slots: int = 1,
                  category_gap_slots: int = 1) -> dict:
    station_id, plan_id, revision_id, break_entry_id = (
        _id(station_id, "station_id"), _id(plan_id, "plan_id"),
        _id(revision_id, "revision_id"), _id(break_entry_id, "break_entry_id"))
    return _proposal(store, principal, station_id, plan_id, revision_id,
                     break_entry_id, output_id=output_id, region_id=region_id,
                     max_spots=max_spots, advertiser_gap_slots=advertiser_gap_slots,
                     category_gap_slots=category_gap_slots)[0]


def reserve_break(store: Store, principal: Principal, station_id: str,
                  plan_id: str, revision_id: str, break_entry_id: str, *,
                  output_id: str, expected_plan_revision: int,
                  expected_preview_hash: str, idempotency_key: str,
                  region_id: str | None = None, max_spots: int | None = None,
                  advertiser_gap_slots: int = 1,
                  category_gap_slots: int = 1,
                  _offer_filter: set[str] | None = None,
                  _makegood_request_id: str | None = None,
                  _expected_makegood_revision: int | None = None) -> dict:
    station_id, plan_id, revision_id, break_entry_id = (
        _id(station_id, "station_id"), _id(plan_id, "plan_id"),
        _id(revision_id, "revision_id"), _id(break_entry_id, "break_entry_id"))
    require(store, principal, "edit", station_id)
    if not isinstance(idempotency_key, str) or not idempotency_key.strip() or len(idempotency_key) > 160:
        raise StoreError("idempotency_key must be 1..160 characters")
    if not isinstance(expected_preview_hash, str) or len(expected_preview_hash) != 64:
        raise StoreError("expected_preview_hash must be SHA-256")
    previous = [row for row in store.scan("placements")
                if row.get("reservation_key") == idempotency_key]
    if previous:
        if any(row.get("station_id") != station_id or row.get("plan_id") != plan_id or
               row.get("plan_revision_id") != revision_id or
               row.get("break_entry_id") != break_entry_id or
               row.get("output_id") != output_id or
               row.get("preview_hash") != expected_preview_hash or
               row.get("makegood_request_id") != _makegood_request_id
               for row in previous):
            raise Conflict("traffic idempotency key was reused for another break")
        return {"plan_id": plan_id, "plan_revision_id": revision_id,
                "placement_ids": sorted(row["_id"] for row in previous),
                "replayed": True}
    request = None
    if _makegood_request_id is not None:
        request = store.get("makegood_requests", _makegood_request_id)
        if request is None or request["status"] != "approved" or \
                request["_rev"] != _expected_makegood_revision or \
                request["station_id"] != station_id or request["plan_id"] != plan_id or \
                request["plan_revision_id"] != revision_id or \
                request["break_entry_id"] != break_entry_id or \
                request["output_id"] != output_id or \
                request.get("region_id") != region_id or \
                _offer_filter != {f"{request['obligation_id']}:{request['spot_id']}"}:
            raise Conflict("makegood request is stale, unapproved or outside this break")
    preview, plan, revision, station, entries, by_offer = _proposal(
        store, principal, station_id, plan_id, revision_id, break_entry_id,
        output_id=output_id, region_id=region_id, max_spots=max_spots,
        advertiser_gap_slots=advertiser_gap_slots,
        category_gap_slots=category_gap_slots, offer_filter=_offer_filter)
    if plan["_rev"] != expected_plan_revision or preview["preview_hash"] != expected_preview_hash:
        raise Conflict("traffic preview or plan revision changed")
    if not preview["selected"]:
        if request is not None:
            raise Conflict("approved makegood is no longer eligible for reservation")
        return {"plan_id": plan_id, "plan_revision_id": revision_id,
                "placement_ids": [], "rejected": preview["rejected"], "replayed": False}
    if request is not None and (len(preview["selected"]) != 1 or
                                preview["selected"][0]["obligation_id"] != request["obligation_id"] or
                                preview["selected"][0]["spot_id"] != request["spot_id"] or
                                preview["selected"][0]["scheduled_at"] != request["scheduled_at"]):
        raise Conflict("makegood recommendation no longer matches selected placement")
    if request is not None:
        selected = preview["selected"][0]
        offer = by_offer[selected["offer_id"]]
        media = store.get("media_files", offer["media_file_id"])
        if (selected["duration_ms"] != request["duration_ms"] or
                offer["spot"]["version_id"] != request["version_id"] or
                media is None or media["_id"] != request["media_file_id"] or
                media["sha256"] != request["media_sha256"]):
            raise Conflict("approved makegood creative or duration changed")
    created: list[tuple[str, dict, str, dict]] = []
    for placement in preview["selected"]:
        offer = by_offer[placement["offer_id"]]
        entry_id, placement_id = str(uuid4()), str(uuid4())
        payload = {"version_id": offer["spot"]["version_id"],
                   "output_id": output_id,
                   "region_id": region_id,
                   "traffic_campaign_id": placement["campaign_id"],
                   "traffic_spot_id": placement["spot_id"],
                   "traffic_obligation_id": placement["obligation_id"],
                   "traffic_placement_id": placement_id}
        if request is not None:
            payload["makegood_request_id"] = request["_id"]
        raw = {"ordinal": 0, "event_kind": "audio", "state": "planned",
               "scheduled_at": placement["scheduled_at"],
               "planned_duration_ms": placement["duration_ms"],
               "duration_kind": "fixed", "ad_break_id": break_entry_id,
               "payload": payload}
        checked = validate_entry(raw | {"id": entry_id}, station["timezone"],
                                 plan["service_date"])
        if not checked["valid"]:
            raise StoreError("selected advert is not a valid plan entry")
        body = {key: value for key, value in checked["normalized"].items()
                if key not in {"id", "nominal_end_at", "requires_resolution"}}
        body["plan_revision_id"] = revision_id
        created.append((entry_id, body, placement_id, placement))
    all_entries = [(_row["_id"], _body(_row), _row) for _row in entries]
    all_entries.extend((entry_id, body, None) for entry_id, body, _, _ in created)
    all_entries.sort(key=lambda part: (
        _time(part[1]["scheduled_at"], "scheduled_at"),
        0 if part[1]["event_kind"] == "ad_break" else 1,
        part[1].get("ordinal", 0), part[0]))
    for ordinal, (_entry_id, body, _prior) in enumerate(all_entries):
        body["ordinal"] = ordinal
    validation = validate_plan([body | {"id": entry_id} for entry_id, body, _ in all_entries],
                               station["timezone"], plan["service_date"])
    if not validation["valid"] or not validation["approvable"]:
        raise Conflict("traffic selection conflicts with plan: " + ", ".join(
            issue["code"] for issue in validation.get("conflicts", [])))
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(preview["store_sequence"])
    for entry_id, body, prior in all_entries:
        if prior is None or body != _body(prior):
            tx.put("plan_entries", entry_id, body,
                   expected_revision=prior["_rev"] if prior else None)
    timestamp = datetime.now(timezone.utc).isoformat()
    for entry_id, _body_entry, placement_id, selected in created:
        placement_body = {
            "obligation_id": selected["obligation_id"],
            "spot_id": selected["spot_id"], "plan_entry_id": entry_id,
            "station_id": station_id, "plan_id": plan_id,
            "plan_revision_id": revision_id, "break_entry_id": break_entry_id,
            "output_id": output_id, "region_id": region_id,
            "state": "planned", "reservation_key": idempotency_key,
            "preview_hash": expected_preview_hash,
            "created_by": principal.user_id, "created_at": timestamp}
        if request is not None:
            placement_body["makegood_request_id"] = request["_id"]
        tx.put("placements", placement_id, placement_body)
    if request is not None:
        tx.put("makegood_requests", request["_id"], _body(request) | {
            "status": "reserved", "placement_id": created[0][2],
            "reserved_by": principal.user_id, "reserved_at": timestamp,
            "reserved_preview_hash": expected_preview_hash},
            expected_revision=request["_rev"])
    from .production import invalidate_production_for_revision
    invalidate_production_for_revision(tx, store, revision_id, "traffic_placement_changed")
    tx.put("plans", plan_id, _body(plan), expected_revision=plan["_rev"])
    tx.commit()
    return {"plan_id": plan_id, "plan_revision_id": revision_id,
            "placement_ids": [placement_id for _, _, placement_id, _ in created],
            "plan_revision": plan["_rev"] + 1, "selected": preview["selected"],
            "replayed": False}


def reconcile_campaign(store: Store, principal: Principal, station_id: str,
                       campaign_id: str) -> dict:
    """Separate plan, engine and output-activity evidence; certify no advert."""
    station_id, campaign_id = _id(station_id, "station_id"), _id(campaign_id, "campaign_id")
    require(store, principal, "view", station_id)
    campaign = store.get("campaigns", campaign_id)
    if campaign is None:
        raise StoreError("campaign is absent")
    if campaign.get("station_ids") is not None and station_id not in campaign["station_ids"]:
        raise StoreError("campaign does not target station")
    obligations = [row for row in store.lookup("obligations", "campaign", campaign_id)
                   if row["station_id"] == station_id]
    obligation_ids = {row["_id"] for row in obligations}
    placements = [row for row in store.scan("placements")
                  if row["obligation_id"] in obligation_ids]
    by_entry = {row["plan_entry_id"]: row for row in placements
                if row["state"] in {"planned", "completed"}}
    attempts = [row for row in store.lookup("playout_attempts", "station", station_id)
                if row.get("plan_entry_id") in by_entry]
    attempt_map = {row["_id"]: row for row in attempts}
    events: list[dict] = []
    anomalies: list[dict] = []
    for event in store.scan("playout_events"):
        attempt = attempt_map.get(event["attempt_id"])
        if attempt is None:
            continue
        placement = by_entry[attempt["plan_entry_id"]]
        spot = store.get("spots", placement["spot_id"])
        if spot is None or attempt["version_id"] != spot["version_id"]:
            anomalies.append({"attempt_id": attempt["_id"], "reason": "spot_version_mismatch"})
            continue
        details = event.get("details") or {}
        events.append({"id": event["_id"], "attempt_id": attempt["_id"],
                       "obligation_id": placement["obligation_id"],
                       "placement_id": placement["_id"],
                       "campaign_id": campaign_id, "spot_id": placement["spot_id"],
                       "station_id": station_id,
                       "region_id": details.get("region_id"),
                       "output_id": details.get("output_id"),
                       "endpoint_id": attempt["endpoint_id"],
                       "phase": event["phase"], "occurred_at": event["occurred_at"]})
    observations = []
    for row in store.scan("output_observations"):
        if row.get("attempt_id") not in attempt_map or row.get("observer_kind") != "external_monitor":
            continue
        details = row.get("details") or {}
        observations.append({"id": row["_id"], "attempt_id": row["attempt_id"],
                             "station_id": row["station_id"],
                             "endpoint_id": row["endpoint_id"],
                             "region_id": details.get("region_id"),
                             "output_id": details.get("output_id"),
                             "observation_type": row["kind"],
                             "observed_at": row["occurred_at"]})
    normalized_placements = [{"id": row["_id"], "obligation_id": row["obligation_id"],
                              "spot_id": row["spot_id"], "campaign_id": campaign_id,
                              "station_id": station_id, "region_id": row.get("region_id"),
                              "output_id": row.get("output_id")}
                             for row in placements if row["state"] in {"planned", "completed"}]
    result = calculate_fulfillment(
        [{"id": row["_id"], "required_plays": row["required_plays"],
          "station_id": station_id, "window_start": row["window_start"],
          "window_end": row["window_end"]} for row in obligations],
        events, observations, placements=normalized_placements, require_output=True)
    summaries = []
    makegoods = [row for row in store.lookup("makegood_requests", "campaign", campaign_id)
                 if row["station_id"] == station_id]
    for row in result["obligations"]:
        oid = row["obligation_id"]
        summaries.append({"obligation_id": oid,
                          "required_plays": row["required_plays"],
                          "planned_placements": sum(p["obligation_id"] == oid for p in placements),
                          "makegood_requests": sum(m["obligation_id"] == oid for m in makegoods),
                          "approved_makegoods": sum(m["obligation_id"] == oid and m["status"] in
                                                    {"approved", "reserved"} for m in makegoods),
                          "reserved_makegoods": sum(m["obligation_id"] == oid and m["status"] ==
                                                    "reserved" for m in makegoods),
                          "external_output_activity_candidates": row["accepted_plays"],
                          "content_verified_plays": 0,
                          "unverified_shortfall": row["required_plays"]})
    return {"station_id": station_id, "campaign_id": campaign_id,
            "obligations": summaries,
            "makegoods": [{"request_id": row["_id"], "obligation_id": row["obligation_id"],
                           "spot_id": row["spot_id"], "status": row["status"],
                           "recommendation_hash": row["recommendation_hash"],
                           "proposed_by": row["proposed_by"],
                           "approved_by": row.get("approved_by"),
                           "placement_id": row.get("placement_id"),
                           "delivery_proven": False} for row in makegoods],
            "attempts": [{**row, "delivery_proven": False,
                          "state": ("output_activity_candidate" if row["state"] == "accepted"
                                    else row["state"])} for row in result["attempts"]],
            "invalid_records": result["invalid_records"],
            "anomalies": anomalies,
            "evidence_policy": "engine completion and audio presence do not identify the advert"}
