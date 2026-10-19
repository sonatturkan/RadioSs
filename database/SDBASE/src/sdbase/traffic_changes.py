"""Track an advertising sound replacement through plans and playout endpoints.

This module records impact and evidence. It deliberately never edits an approved
plan, its distribution, or the spot that was used by an earlier broadcast.
Traffic staff create a new approved spot/version, propose the replacement, then
approve a new plan revision and distribute it through the normal playout flow.
Each affected placement and endpoint remains pending until that exact replacement
can be proven from current plan and endpoint records.
"""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Any, Mapping
from uuid import UUID, uuid4

from .identity import Principal, require
from .storage import Conflict, Store, StoreError


_MAX_TARGETS = 4000
_FINAL = frozenset({"applied", "superseded"})


def _id(value: Any, name: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def _instant(value: Any, name: str) -> datetime:
    try:
        result = (datetime.fromisoformat(value.replace("Z", "+00:00"))
                  if isinstance(value, str) else value)
    except ValueError as exc:
        raise StoreError(f"{name} must be an ISO timestamp") from exc
    if not isinstance(result, datetime) or result.tzinfo is None or result.utcoffset() is None:
        raise StoreError(f"{name} needs a timezone")
    return result.astimezone(timezone.utc)


def _body(row: Mapping[str, Any]) -> dict:
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _spot(store: Store, spot_id: str, station_id: str) -> tuple[dict, dict, dict]:
    spot = store.get("spots", spot_id)
    if spot is None:
        raise StoreError("spot is absent")
    campaign = store.get("campaigns", spot["campaign_id"])
    version = store.get("versions", spot["version_id"])
    if campaign is None or version is None:
        raise StoreError("spot relation is incomplete")
    item = store.get("items", version["item_id"])
    if item is None or item["kind"] != "advertisement":
        raise StoreError("spot must reference an advertisement version")
    if not store.lookup("station_items", "station_item", station_id, item["_id"]):
        raise StoreError("spot version is outside station")
    station_ids = campaign.get("station_ids")
    if station_ids is not None and station_id not in station_ids:
        raise StoreError("campaign does not target station")
    return spot, campaign, version


def _source_placements(store: Store, station_id: str, campaign_id: str,
                       old_spot_id: str, old_version_id: str,
                       first_at: datetime) -> list[dict]:
    result: list[dict] = []
    for placement in store.scan("placements"):
        if placement["spot_id"] != old_spot_id or placement["state"] != "planned":
            continue
        obligation = store.get("obligations", placement["obligation_id"])
        if obligation is None or obligation["station_id"] != station_id or \
                obligation["campaign_id"] != campaign_id:
            continue
        entry = store.get("plan_entries", placement["plan_entry_id"])
        if entry is None or _instant(entry["scheduled_at"], "scheduled_at") < first_at:
            continue
        revision = store.get("plan_revisions", entry["plan_revision_id"])
        plan = store.get("plans", revision["plan_id"]) if revision else None
        if plan is None or plan["station_id"] != station_id:
            continue
        if revision["status"] == "draft":
            if plan.get("draft_revision_id") != revision["_id"]:
                continue
        elif revision["status"] == "approved":
            if plan.get("current_revision_id") != revision["_id"]:
                continue
        else:
            continue
        result.append({"placement": placement, "entry": entry,
                       "revision": revision, "plan": plan,
                       "source_consistency": ("matching_version" if
                                              entry.get("event_kind") == "audio" and
                                              entry.get("payload", {}).get("version_id") == old_version_id
                                              else "plan_entry_version_mismatch")})
    result.sort(key=lambda value: (value["entry"]["scheduled_at"],
                                   value["placement"]["_id"]))
    return result


def _snapshot_version(distribution: dict | None, entry_id: str) -> str | None:
    if distribution is None:
        return None
    snapshot = distribution.get("snapshot") or {}
    for entry in snapshot.get("entries", []):
        if entry.get("plan_entry_id") == entry_id:
            return entry.get("version_id")
    return None


def _target_rows(store: Store, station_id: str, candidates: list[dict],
                 change_id: str, old_version_id: str,
                 new_version_id: str, created_at: str) -> list[dict]:
    endpoints = [row for row in store.lookup("endpoints", "station", station_id)
                 if row["status"] != "retired"]
    rows: list[dict] = []
    for candidate in candidates:
        placement = candidate["placement"]
        entry = candidate["entry"]
        revision = candidate["revision"]
        plan = candidate["plan"]
        common = {"change_id": change_id, "station_id": station_id,
                  "placement_id": placement["_id"], "plan_id": plan["_id"],
                  "plan_revision_id": revision["_id"],
                  "plan_entry_id": entry["_id"],
                  "scheduled_at": entry["scheduled_at"],
                  "old_version_id": old_version_id,
                  "new_version_id": new_version_id,
                  "source_plan_revision": plan["_rev"],
                  "source_revision_status": revision["status"],
                  "source_placement_revision": placement["_rev"],
                  "source_entry_revision": entry["_rev"],
                  "source_consistency": candidate["source_consistency"],
                  "status": "pending", "created_at": created_at,
                  "updated_at": created_at}
        rows.append(common | {"target_kind": "placement",
                              "target_key": f"placement:{placement['_id']}",
                              "endpoint_id": None, "distribution_id": None,
                              "source_distribution_revision": None,
                              "source_payload_hash": None,
                              "source_distribution_status": None,
                              "source_snapshot_version_id": None})
        if revision["status"] != "approved":
            continue
        for endpoint in endpoints:
            distributions = store.lookup("plan_distributions", "endpoint_revision",
                                         endpoint["_id"], revision["_id"])
            distribution = distributions[0] if distributions else None
            rows.append(common | {"target_kind": "endpoint",
                                  "target_key": f"endpoint:{placement['_id']}:{endpoint['_id']}",
                                  "endpoint_id": endpoint["_id"],
                                  "distribution_id": distribution["_id"] if distribution else None,
                                  "source_distribution_revision": (distribution["_rev"]
                                                                   if distribution else None),
                                  "source_payload_hash": (distribution.get("payload_hash")
                                                          if distribution else None),
                                  "source_distribution_status": (distribution["status"]
                                                                if distribution else "absent"),
                                  "source_snapshot_version_id": _snapshot_version(distribution,
                                                                                  entry["_id"])})
        if len(rows) > _MAX_TARGETS:
            raise StoreError("spot change has too many impacts for one atomic proposal")
    return rows


def propose_spot_change(store: Store, principal: Principal, station_id: str,
                        old_spot_id: str, new_spot_id: str, *,
                        effective_at: str | datetime,
                        idempotency_key: str,
                        expected_old_spot_revision: int,
                        expected_new_spot_revision: int,
                        now: str | datetime | None = None) -> dict:
    """Record all current future impacts without changing any approved plan.

    The proposal is tied to the exact store sequence it inspected. A concurrent
    plan, placement, endpoint, or distribution change makes it fail and retry.
    """
    station_id = _id(station_id, "station_id")
    old_spot_id, new_spot_id = (_id(old_spot_id, "old_spot_id"),
                                _id(new_spot_id, "new_spot_id"))
    require(store, principal, "edit", station_id)
    if old_spot_id == new_spot_id:
        raise StoreError("replacement spot must differ")
    if not isinstance(idempotency_key, str) or not idempotency_key.strip() or \
            len(idempotency_key) > 200:
        raise StoreError("idempotency_key must be 1..200 characters")
    effective = _instant(effective_at, "effective_at")
    at = _instant(now or datetime.now(timezone.utc), "now")
    first_at = max(effective, at)
    sequence = store.sequence
    previous = store.lookup("spot_changes", "station_key", station_id, idempotency_key)
    if previous:
        row = previous[0]
        if (row["old_spot_id"], row["new_spot_id"], row["effective_at"]) != \
                (old_spot_id, new_spot_id, effective.isoformat()):
            raise Conflict("idempotency_key was used for another spot change")
        return spot_change_status(store, principal, station_id, row["_id"]) | {"replayed": True}
    old_spot, campaign, old_version = _spot(store, old_spot_id, station_id)
    new_spot, new_campaign, new_version = _spot(store, new_spot_id, station_id)
    if campaign["_id"] != new_campaign["_id"] or \
            old_version["_id"] == new_version["_id"]:
        raise StoreError("replacement must be a new version in the same campaign")
    if new_spot["status"] != "approved" or new_version["status"] != "approved":
        raise Conflict("replacement spot and version need approval")
    if old_spot["_rev"] != expected_old_spot_revision or \
            new_spot["_rev"] != expected_new_spot_revision:
        raise Conflict("spot changed since replacement request")
    candidates = _source_placements(store, station_id, campaign["_id"],
                                    old_spot_id, old_version["_id"], first_at)
    change_id = str(uuid4())
    timestamp = at.isoformat()
    targets = _target_rows(store, station_id, candidates, change_id,
                           old_version["_id"], new_version["_id"], timestamp)
    change = {"station_id": station_id, "campaign_id": campaign["_id"],
              "old_spot_id": old_spot_id, "new_spot_id": new_spot_id,
              "old_version_id": old_version["_id"],
              "new_version_id": new_version["_id"],
              "effective_at": effective.isoformat(),
              "idempotency_key": idempotency_key,
              "status": "pending" if targets else "applied",
              "created_at": timestamp, "updated_at": timestamp,
              "created_by": principal.user_id,
              "impact_count": len(targets)}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("spot_changes", change_id, change)
    for target in targets:
        tx.put("spot_change_targets", str(uuid4()), target)
    tx.commit()
    return spot_change_status(store, principal, station_id, change_id) | {"replayed": False}


def spot_change_status(store: Store, principal: Principal,
                       station_id: str, change_id: str) -> dict:
    """Show affected plans, endpoints, and the current rollout state."""
    station_id, change_id = _id(station_id, "station_id"), _id(change_id, "change_id")
    require(store, principal, "view", station_id)
    change = store.get("spot_changes", change_id)
    if change is None or change["station_id"] != station_id:
        raise StoreError("spot change is outside station")
    targets = store.lookup("spot_change_targets", "change", change_id)
    targets.sort(key=lambda row: (row["scheduled_at"], row["target_kind"],
                                  row.get("endpoint_id") or ""))
    counts = {state: sum(row["status"] == state for row in targets)
              for state in ("pending", "applied", "superseded")}
    return {"change": change, "targets": targets, "counts": counts}


def _replacement(store: Store, change: dict, target: dict,
                 replacement_placement_id: str) -> tuple[dict, dict, dict, dict]:
    replacement = store.get("placements", replacement_placement_id)
    source = store.get("placements", target["placement_id"])
    if replacement is None or source is None or \
            replacement["spot_id"] != change["new_spot_id"] or \
            replacement["obligation_id"] != source["obligation_id"] or \
            replacement["state"] not in {"planned", "completed"}:
        raise Conflict("replacement placement does not match original obligation and new spot")
    entry = store.get("plan_entries", replacement["plan_entry_id"])
    revision = store.get("plan_revisions", entry["plan_revision_id"]) if entry else None
    plan = store.get("plans", revision["plan_id"]) if revision else None
    if plan is None or plan["_id"] != target["plan_id"] or \
            plan["station_id"] != change["station_id"] or \
            plan.get("current_revision_id") != revision["_id"] or \
            revision["status"] != "approved" or \
            entry["event_kind"] != "audio" or \
            entry.get("payload", {}).get("version_id") != change["new_version_id"]:
        raise Conflict("replacement is not in the current approved plan with new version")
    if target["plan_revision_id"] == revision["_id"] and \
            target["source_revision_status"] == "approved":
        raise Conflict("approved source revision cannot be rewritten in place")
    if target["plan_revision_id"] == revision["_id"] and source["state"] == "planned":
        raise Conflict("old spot placement remains planned in replacement revision")
    if _instant(entry["scheduled_at"], "scheduled_at") < \
            _instant(change["effective_at"], "effective_at"):
        raise Conflict("replacement precedes change effective time")
    return replacement, entry, revision, plan


def _update_change_status(tx: Any, store: Store, change: dict,
                          target: dict, new_state: str, timestamp: str) -> str:
    targets = store.lookup("spot_change_targets", "change", change["_id"])
    states = [new_state if row["_id"] == target["_id"] else row["status"]
              for row in targets]
    if any(state == "pending" for state in states):
        status = "pending"
    elif any(state == "applied" for state in states):
        status = "applied"
    else:
        status = "superseded"
    if status != change["status"]:
        tx.put("spot_changes", change["_id"],
               _body(change) | {"status": status, "updated_at": timestamp},
               expected_revision=change["_rev"])
    return status


def apply_spot_change_target(store: Store, principal: Principal,
                             station_id: str, target_id: str, *,
                             replacement_placement_id: str,
                             replacement_distribution_id: str | None = None,
                             expected_target_revision: int,
                             now: str | datetime | None = None) -> dict:
    """Confirm exact new plan and, for an endpoint, actual accepted distribution."""
    station_id, target_id = _id(station_id, "station_id"), _id(target_id, "target_id")
    replacement_placement_id = _id(replacement_placement_id, "replacement_placement_id")
    require(store, principal, "approve", station_id)
    sequence = store.sequence
    target = store.get("spot_change_targets", target_id)
    if target is None or target["station_id"] != station_id:
        raise StoreError("spot change target is outside station")
    if target["_rev"] != expected_target_revision:
        raise Conflict("spot change target changed")
    if target["status"] in _FINAL:
        if target["status"] == "applied" and \
                target.get("replacement_placement_id") == replacement_placement_id and \
                (target["target_kind"] != "endpoint" or
                 target.get("replacement_distribution_id") == replacement_distribution_id):
            return {"target_id": target_id, "status": "applied", "replayed": True}
        raise Conflict("spot change target is already final")
    change = store.get("spot_changes", target["change_id"])
    if change is None or change["station_id"] != station_id:
        raise StoreError("spot change is absent")
    replacement, entry, revision, _ = _replacement(
        store, change, target, replacement_placement_id)
    distribution_id = None
    if target["target_kind"] == "endpoint":
        if replacement_distribution_id is None:
            raise StoreError("endpoint target needs replacement_distribution_id")
        distribution_id = _id(replacement_distribution_id, "replacement_distribution_id")
        distribution = store.get("plan_distributions", distribution_id)
        acceptance = store.get("active_acceptances", target["endpoint_id"])
        if distribution is None or distribution["endpoint_id"] != target["endpoint_id"] or \
                distribution["revision_id"] != revision["_id"] or \
                distribution["status"] != "accepted" or \
                distribution["plan_id"] != target["plan_id"] or \
                _snapshot_version(distribution, entry["_id"]) != change["new_version_id"] or \
                acceptance is None or acceptance.get("state") != "active" or \
                acceptance["revision_id"] != revision["_id"] or \
                acceptance["payload_hash"] != distribution["payload_hash"]:
            raise Conflict("new version has not been accepted by this endpoint")
    elif replacement_distribution_id is not None:
        raise StoreError("placement target does not take replacement_distribution_id")
    timestamp = _instant(now or datetime.now(timezone.utc), "now").isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("spot_change_targets", target_id,
           _body(target) | {"status": "applied", "updated_at": timestamp,
                            "resolved_at": timestamp,
                            "replacement_placement_id": replacement["_id"],
                            "replacement_plan_revision_id": revision["_id"],
                            "replacement_plan_entry_id": entry["_id"],
                            "replacement_distribution_id": distribution_id,
                            "resolution_reason": "verified_new_plan_and_endpoint" if distribution_id
                                                 else "verified_new_approved_plan"},
           expected_revision=target["_rev"])
    status = _update_change_status(tx, store, change, target, "applied", timestamp)
    tx.commit()
    return {"target_id": target_id, "status": "applied",
            "change_status": status, "replayed": False}


def supersede_spot_change_target(store: Store, principal: Principal,
                                 station_id: str, target_id: str, *,
                                 reason: str, expected_target_revision: int,
                                 now: str | datetime | None = None) -> dict:
    """Resolve an obsolete impact explicitly, preserving the original evidence."""
    station_id, target_id = _id(station_id, "station_id"), _id(target_id, "target_id")
    require(store, principal, "approve", station_id)
    if not isinstance(reason, str) or not reason.strip() or len(reason) > 500:
        raise StoreError("supersession reason must be 1..500 characters")
    sequence = store.sequence
    target = store.get("spot_change_targets", target_id)
    if target is None or target["station_id"] != station_id:
        raise StoreError("spot change target is outside station")
    if target["_rev"] != expected_target_revision or target["status"] != "pending":
        raise Conflict("target changed or is already final")
    change = store.get("spot_changes", target["change_id"])
    if change is None or change["station_id"] != station_id:
        raise StoreError("spot change is absent")
    instant = _instant(now or datetime.now(timezone.utc), "now")
    source = store.get("placements", target["placement_id"])
    plan = store.get("plans", target["plan_id"])
    revision_is_current = (plan is not None and
                           target["plan_revision_id"] in {
                               plan.get("current_revision_id"),
                               plan.get("draft_revision_id")})
    still_future = _instant(target["scheduled_at"], "scheduled_at") >= instant
    if source is not None and source["state"] == "planned" and \
            revision_is_current and still_future:
        raise Conflict("source placement remains planned in a current future revision")
    if target["target_kind"] == "endpoint" and still_future:
        acceptance = store.get("active_acceptances", target["endpoint_id"])
        if acceptance is not None and acceptance.get("state") == "active" and \
                acceptance["revision_id"] == target["plan_revision_id"]:
            raise Conflict("endpoint still accepts the old plan revision")
    timestamp = instant.isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("spot_change_targets", target_id,
           _body(target) | {"status": "superseded", "updated_at": timestamp,
                            "resolved_at": timestamp, "resolution_reason": reason},
           expected_revision=target["_rev"])
    status = _update_change_status(tx, store, change, target, "superseded", timestamp)
    tx.commit()
    return {"target_id": target_id, "status": "superseded",
            "change_status": status}
