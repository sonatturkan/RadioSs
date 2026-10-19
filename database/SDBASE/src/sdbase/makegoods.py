"""Human reviewed advertising makegoods, without claiming content delivery.

The pure suggestion engine supplies candidate future breaks. This service
subtracts existing future reservations, checks current media readiness, and
stores an immutable recommendation for a different approver. Only an approved
request can create a plan placement, in the same transaction that links it.
"""

from __future__ import annotations

from datetime import datetime, timezone
from uuid import uuid4

from . import ads_service
from .ads import suggest_makegoods
from .identity import Principal, require
from .storage import Conflict, Store, StoreError


def _request(store: Store, station_id: str, request_id: str) -> dict:
    row = store.get("makegood_requests", ads_service._id(request_id, "request_id"))
    if row is None or row["station_id"] != station_id:
        raise StoreError("makegood request is absent from station")
    return row


def propose_makegood(store: Store, principal: Principal, station_id: str,
                     campaign_id: str, plan_id: str, revision_id: str,
                     break_entry_id: str, *, output_id: str,
                     idempotency_key: str, region_id: str | None = None) -> dict:
    station_id, campaign_id, plan_id, revision_id, break_entry_id = (
        ads_service._id(station_id, "station_id"),
        ads_service._id(campaign_id, "campaign_id"),
        ads_service._id(plan_id, "plan_id"),
        ads_service._id(revision_id, "revision_id"),
        ads_service._id(break_entry_id, "break_entry_id"))
    require(store, principal, "edit", station_id)
    if not isinstance(idempotency_key, str) or not idempotency_key.strip() or \
            len(idempotency_key) > 160:
        raise StoreError("idempotency_key must be 1..160 characters")
    if not isinstance(output_id, str) or not output_id.strip():
        raise StoreError("output_id is required")
    if region_id is not None and (not isinstance(region_id, str) or not region_id.strip()):
        raise StoreError("region_id must be a nonempty code")
    prior = store.lookup("makegood_requests", "station_key", station_id, idempotency_key)
    if prior:
        row = prior[0]
        if any(row[key] != value for key, value in {
                "campaign_id": campaign_id, "plan_id": plan_id,
                "plan_revision_id": revision_id, "break_entry_id": break_entry_id,
                "output_id": output_id, "region_id": region_id}.items()):
            raise Conflict("makegood idempotency key was reused for another recommendation")
        return {"proposed": True, "request": row, "replayed": True}
    campaign = store.get("campaigns", campaign_id)
    if campaign is None or station_id not in (campaign.get("station_ids") or []) or \
            campaign["status"] not in {"approved", "active"}:
        raise StoreError("makegood campaign is absent, unapproved or outside station")
    _, _, break_entry, _, _ = ads_service._draft_break(
        store, station_id, plan_id, revision_id, break_entry_id)
    now = datetime.now(timezone.utc)
    if ads_service._time(break_entry["scheduled_at"], "scheduled_at") <= now:
        raise Conflict("makegood break must be in the future")
    reserved = ads_service._reserved_counts(store, now)
    obligations = [row for row in store.lookup("obligations", "campaign", campaign_id)
                   if row["station_id"] == station_id]
    remaining = {row["_id"]: max(0, row["required_plays"] - reserved.get(row["_id"], 0))
                 for row in obligations}
    adjusted = [ads_service._body(row) | {"id": row["_id"],
                "required_plays": remaining[row["_id"]]}
                for row in obligations if remaining[row["_id"]] > 0]
    offers, _ = ads_service._offers(store, station_id, reserved)
    offers = [row for row in offers if row["campaign"]["id"] == campaign_id and
              remaining.get(row["obligation"]["id"], 0) > 0]
    slot = {"id": break_entry_id, "station_id": station_id,
            "output_id": output_id, "region_id": region_id,
            "starts_at": break_entry["scheduled_at"],
            "capacity_ms": break_entry["planned_duration_ms"]}
    suggestion = suggest_makegoods(
        adjusted, offers, [slot],
        fulfillment={"obligations": [{"obligation_id": row["id"], "accepted_plays": 0}
                                     for row in adjusted]}, as_of=now)
    if not suggestion["proposals"]:
        return {"proposed": False, "unresolved": suggestion["unresolved"],
                "evidence_policy": "unverified output activity gives no delivery credit"}
    for candidate in suggestion["proposals"]:
        if store.lookup("makegood_requests", "obligation_break",
                        candidate["obligation_id"], break_entry_id):
            continue
        preview, _, _, _, _, by_offer = ads_service._proposal(
            store, principal, station_id, plan_id, revision_id, break_entry_id,
            output_id=output_id, region_id=region_id, max_spots=1,
            advertiser_gap_slots=1, category_gap_slots=1,
            offer_filter={candidate["offer_id"]})
        if not preview["selected"]:
            continue
        selected = preview["selected"][0]
        if selected["obligation_id"] != candidate["obligation_id"] or \
                selected["spot_id"] != candidate["spot_id"]:
            continue
        request_id = str(uuid4())
        proposed_at = datetime.now(timezone.utc).isoformat()
        chosen_offer = by_offer[selected["offer_id"]]
        media = store.get("media_files", chosen_offer["media_file_id"])
        recommendation = {"station_id": station_id, "campaign_id": campaign_id,
                          "obligation_id": candidate["obligation_id"],
                          "spot_id": candidate["spot_id"],
                          "version_id": chosen_offer["spot"]["version_id"],
                          "media_file_id": media["_id"], "media_sha256": media["sha256"],
                          "duration_ms": selected["duration_ms"],
                          "plan_id": plan_id, "plan_revision_id": revision_id,
                          "break_entry_id": break_entry_id, "output_id": output_id,
                          "region_id": region_id, "scheduled_at": selected["scheduled_at"],
                          "preview_hash": preview["preview_hash"],
                          "required_plays": next(row["required_plays"] for row in obligations
                                                 if row["_id"] == candidate["obligation_id"]),
                          "future_reserved_at_proposal": reserved.get(candidate["obligation_id"], 0),
                          "content_verified_plays_at_proposal": 0}
        body = recommendation | {
            "request_id": request_id, "status": "proposed",
            "idempotency_key": idempotency_key,
            "recommendation_hash": ads_service._sha(recommendation),
            "suggestion_reason": candidate["reason"],
            "proposed_by": principal.user_id, "proposed_at": proposed_at}
        tx = store.begin(actor=principal.user_id)
        tx.expect_sequence(preview["store_sequence"])
        row = tx.put("makegood_requests", request_id, body).commit()[0]
        return {"proposed": True, "request": row, "replayed": False,
                "unresolved": suggestion["unresolved"]}
    return {"proposed": False, "unresolved": suggestion["unresolved"],
            "reason": "suggested spots are no longer ready or already proposed"}


def review_makegood(store: Store, principal: Principal, station_id: str,
                    request_id: str, *, expected_revision: int,
                    expected_recommendation_hash: str, decision: str,
                    reason: str | None = None) -> dict:
    station_id = ads_service._id(station_id, "station_id")
    require(store, principal, "approve", station_id)
    if not isinstance(decision, str) or decision not in {"approved", "rejected"}:
        raise StoreError("makegood decision must be approved or rejected")
    if type(expected_revision) is not int or expected_revision < 1:
        raise StoreError("expected_revision must be positive")
    if reason is not None and not isinstance(reason, str):
        raise StoreError("reason must be text")
    if not isinstance(expected_recommendation_hash, str) or \
            len(expected_recommendation_hash) != 64:
        raise StoreError("expected_recommendation_hash must be SHA-256")
    sequence = store.sequence
    row = _request(store, station_id, request_id)
    if row["proposed_by"] == principal.user_id:
        raise StoreError("makegood proposal needs a different human approver")
    if row["recommendation_hash"] != expected_recommendation_hash:
        raise Conflict("makegood recommendation changed")
    if row["status"] == decision and row.get("reviewed_by") == principal.user_id:
        return row | {"replayed": True}
    if row["_rev"] != expected_revision or row["status"] != "proposed":
        raise Conflict("makegood proposal is no longer awaiting review")
    if decision == "rejected" and (not isinstance(reason, str) or not reason.strip()):
        raise StoreError("rejected makegood needs a reason")
    offer_id = f"{row['obligation_id']}:{row['spot_id']}"
    preview, _, _, _, _, by_offer = ads_service._proposal(
        store, principal, station_id, row["plan_id"], row["plan_revision_id"],
        row["break_entry_id"], output_id=row["output_id"],
        region_id=row.get("region_id"), max_spots=1,
        advertiser_gap_slots=1, category_gap_slots=1,
        offer_filter={offer_id}, permission="approve")
    if not _matches_recommendation(store, row, preview, by_offer):
        raise Conflict("makegood recommendation is stale; propose again")
    stamp = datetime.now(timezone.utc).isoformat()
    updates = {"status": decision, "reviewed_by": principal.user_id,
               "reviewed_at": stamp, "review_reason": reason.strip() if reason else None}
    if decision == "approved":
        updates.update({"approved_by": principal.user_id, "approved_at": stamp})
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    return tx.put("makegood_requests", row["_id"],
                  ads_service._body(row) | updates,
                  expected_revision=expected_revision).commit()[0]


def preview_makegood(store: Store, principal: Principal, station_id: str,
                     request_id: str) -> dict:
    station_id = ads_service._id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    row = _request(store, station_id, request_id)
    if row["status"] != "approved":
        raise Conflict("makegood request needs separate approval")
    offer_id = f"{row['obligation_id']}:{row['spot_id']}"
    preview, _, _, _, _, by_offer = ads_service._proposal(
        store, principal, station_id, row["plan_id"], row["plan_revision_id"],
        row["break_entry_id"], output_id=row["output_id"],
        region_id=row.get("region_id"), max_spots=1,
        advertiser_gap_slots=1, category_gap_slots=1,
        offer_filter={offer_id})
    if not _matches_recommendation(store, row, preview, by_offer):
        raise Conflict("approved makegood no longer has a ready spot and free capacity")
    return preview | {"makegood_request_id": row["_id"],
                      "makegood_request_revision": row["_rev"],
                      "recommendation_hash": row["recommendation_hash"]}


def _matches_recommendation(store: Store, row: dict, preview: dict,
                            by_offer: dict[str, dict]) -> bool:
    if len(preview["selected"]) != 1:
        return False
    selected = preview["selected"][0]
    offer = by_offer.get(selected["offer_id"])
    media = store.get("media_files", offer["media_file_id"]) if offer else None
    return bool(offer and media and
                selected["spot_id"] == row["spot_id"] and
                selected["obligation_id"] == row["obligation_id"] and
                selected["scheduled_at"] == row["scheduled_at"] and
                selected["duration_ms"] == row["duration_ms"] and
                offer["spot"]["version_id"] == row["version_id"] and
                media["_id"] == row["media_file_id"] and
                media["sha256"] == row["media_sha256"])


def reserve_makegood(store: Store, principal: Principal, station_id: str,
                     request_id: str, *, expected_request_revision: int,
                     expected_plan_revision: int, expected_preview_hash: str,
                     idempotency_key: str) -> dict:
    station_id = ads_service._id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    if type(expected_request_revision) is not int or expected_request_revision < 1:
        raise StoreError("expected_request_revision must be positive")
    row = _request(store, station_id, request_id)
    offer_id = f"{row['obligation_id']}:{row['spot_id']}"
    return ads_service.reserve_break(
        store, principal, station_id, row["plan_id"], row["plan_revision_id"],
        row["break_entry_id"], output_id=row["output_id"],
        region_id=row.get("region_id"), max_spots=1,
        expected_plan_revision=expected_plan_revision,
        expected_preview_hash=expected_preview_hash,
        idempotency_key=idempotency_key,
        _offer_filter={offer_id}, _makegood_request_id=row["_id"],
        _expected_makegood_revision=expected_request_revision)
