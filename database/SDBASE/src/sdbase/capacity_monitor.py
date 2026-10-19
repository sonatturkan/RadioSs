"""Bounded, station-scoped growth and storage checks for RadioS.

The LibreTime empty-playlist incident in the local research report (PDF p. 9)
motivates explicit plan-generation and catalogue limits. Thresholds are supplied
per installation: the report itself (PDF p. 13) rejects a universal load profile.
This module observes records and filesystem capacity; it does not stop a writer,
probe audio output, or prove that a station can remain on air.

Recent-record checks use a finite scan budget. An exhausted budget yields an
``unknown`` result and its own operator alarm, never a false healthy result.
The exact total-catalogue count uses the existing station index. No SQL or new
collection is required; persistent alarms use diagnostic_events/incidents.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import date, datetime, timedelta, timezone
from hashlib import sha256
import os
import re
from typing import Any, Mapping
from uuid import UUID
from zoneinfo import ZoneInfo

from .diagnostics import record_diagnostic
from .identity import Principal, require
from .media import object_root
from .storage import Conflict, Store, StoreError


_POLICY_ID = re.compile(r"[A-Za-z0-9_.:-]{1,64}\Z")
_OBSERVATION_KEY = re.compile(r"[A-Za-z0-9_.:-]{1,64}\Z")


@dataclass(frozen=True)
class CapacityPolicy:
    """Installation-set limits; counts include historical station associations.

    ``window_hours`` applies to new items and plan revisions. One newly made
    empty draft can be normal; the operator chooses the allowed count. Byte
    limits cover the database journal/checkpoint/metadata, not media objects.
    ``max_scan_rows`` bounds each history pass, not the engine's resident state.
    """

    policy_id: str
    window_hours: int
    max_catalog_items: int
    max_recent_items: int
    max_recent_plan_revisions: int
    max_recent_empty_plan_revisions: int
    max_entries_per_recent_revision: int
    max_journal_bytes: int
    max_database_bytes: int
    min_free_bytes: int
    critical_free_bytes: int
    max_scan_rows: int = 100_000


def _policy(value: CapacityPolicy | Mapping[str, Any]) -> CapacityPolicy:
    if isinstance(value, CapacityPolicy):
        result = value
    elif isinstance(value, Mapping):
        try:
            result = CapacityPolicy(**value)
        except (TypeError, ValueError) as exc:
            raise StoreError("invalid capacity policy fields") from exc
    else:
        raise StoreError("capacity policy must be an object")
    if not isinstance(result.policy_id, str) or not _POLICY_ID.fullmatch(result.policy_id):
        raise StoreError("policy_id must be 1..64 ASCII letters, digits, _, ., : or -")
    for name, value in asdict(result).items():
        if name == "policy_id":
            continue
        if type(value) is not int or value < (0 if name in {
                "max_recent_empty_plan_revisions", "critical_free_bytes"} else 1):
            raise StoreError(f"{name} must be a nonnegative whole number")
    if result.window_hours > 24 * 30 or result.max_scan_rows > 1_000_000:
        raise StoreError("window_hours or max_scan_rows exceeds the monitor bound")
    if result.critical_free_bytes >= result.min_free_bytes:
        raise StoreError("critical_free_bytes must be below min_free_bytes")
    return result


def _instant(value: str | None) -> datetime:
    if value is None:
        return datetime.now(timezone.utc)
    if not isinstance(value, str):
        raise StoreError("observed_at must be an ISO timestamp")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise StoreError("observed_at must be an ISO timestamp") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError("observed_at needs a timezone offset")
    return parsed.astimezone(timezone.utc)


def _created_at(value: Any) -> datetime | None:
    if not isinstance(value, str):
        return None
    try:
        result = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return result.astimezone(timezone.utc) if result.tzinfo else None


def _file_size(path: os.PathLike[str], *, optional: bool = False) -> int | None:
    try:
        return os.stat(path).st_size
    except FileNotFoundError:
        return 0 if optional else None
    except OSError:
        return None


def _snapshot(store: Store, station_id: str, policy: CapacityPolicy,
              observed_at: datetime) -> dict:
    cutoff = observed_at - timedelta(hours=policy.window_hours)
    if "station" not in store.index_specs.get("station_items", {}) or \
            "revision" not in store.index_specs.get("plan_entries", {}):
        raise StoreError("capacity monitor requires the RadioS station and revision indexes")
    # Store is an in-memory document engine. Inspect its indexes and resident
    # records under the same shared lock to avoid materializing an unbounded
    # scan() result; keep each historical pass below max_scan_rows.
    with store._locked(exclusive=False):
        store._reload_locked()
        indices = store._indices
        state = store._state
        assoc_ids = indices.get(("station_items", "station"), {}).get((station_id,), set())
        catalog_items = len(assoc_ids)
        items = state.get("items", {})
        associations = state.get("station_items", {})
        recent_items = 0
        undated_items = 0
        item_scanned = 0
        for assoc_id in assoc_ids:
            if item_scanned >= policy.max_scan_rows:
                break
            item_scanned += 1
            assoc = associations.get(assoc_id)
            item = items.get(assoc["data"]["item_id"]) if assoc else None
            created = _created_at(item["data"].get("created_at")) if item else None
            if created is None:
                undated_items += 1
            elif cutoff <= created <= observed_at:
                recent_items += 1
        catalog_complete = item_scanned == catalog_items and undated_items == 0

        plan_rows = state.get("plans", {})
        revision_rows = state.get("plan_revisions", {})
        entry_index = indices.get(("plan_entries", "revision"), {})
        recent_revisions = recent_empty = max_entries = undated_revisions = 0
        revision_scanned = 0
        for revision_id, envelope in revision_rows.items():
            if revision_scanned >= policy.max_scan_rows:
                break
            revision_scanned += 1
            revision = envelope["data"]
            plan = plan_rows.get(revision["plan_id"])
            if plan is None or plan["data"].get("station_id") != station_id:
                continue
            created = _created_at(revision.get("created_at"))
            if created is None:
                undated_revisions += 1
                continue
            if cutoff <= created <= observed_at:
                recent_revisions += 1
                entry_count = len(entry_index.get((revision_id,), set()))
                recent_empty += int(entry_count == 0)
                max_entries = max(max_entries, entry_count)
        revisions_complete = revision_scanned == len(revision_rows) and undated_revisions == 0
        sequence = store._sequence

    journal_bytes = _file_size(store.journal)
    checkpoint_bytes = _file_size(store.checkpoint_file, optional=True)
    metadata_bytes = _file_size(store.meta_file)
    database_bytes = (journal_bytes + checkpoint_bytes + metadata_bytes
                      if None not in (journal_bytes, checkpoint_bytes, metadata_bytes) else None)
    try:
        filesystem = os.statvfs(store.root)
        database_free_bytes = filesystem.f_bavail * filesystem.f_frsize
    except OSError:
        database_free_bytes = None
    media_root = object_root(store)
    try:
        media_filesystem = os.statvfs(media_root if media_root.exists() else store.root)
        media_free_bytes = media_filesystem.f_bavail * media_filesystem.f_frsize
    except OSError:
        media_free_bytes = None
    free_bytes = (min(database_free_bytes, media_free_bytes)
                  if database_free_bytes is not None and media_free_bytes is not None else None)
    return {
        "sequence": sequence,
        "catalog_items": catalog_items,
        "recent_items": recent_items,
        "recent_plan_revisions": recent_revisions,
        "recent_empty_plan_revisions": recent_empty,
        "max_entries_in_recent_revision": max_entries,
        "catalog_history_complete": catalog_complete,
        "revision_history_complete": revisions_complete,
        "catalog_rows_inspected": item_scanned,
        "revision_rows_inspected": revision_scanned,
        "undated_items": undated_items,
        "undated_revisions": undated_revisions,
        "journal_bytes": journal_bytes,
        "checkpoint_bytes": checkpoint_bytes,
        "metadata_bytes": metadata_bytes,
        "database_bytes": database_bytes,
        "database_free_bytes": database_free_bytes,
        "media_free_bytes": media_free_bytes,
        "free_bytes": free_bytes,
    }


_ACTIONS = {
    "catalog_limit": "Review retained catalogue records and the import job that created them.",
    "catalog_growth": "Inspect recent imports and stop a repeating import before retrying.",
    "plan_growth": "Inspect the scheduler or clock materializer for a repeated generation loop.",
    "empty_plans": "Inspect newly created empty drafts and their generator; cancel unwanted drafts.",
    "plan_entries": "Inspect the largest recent plan revision and its generation rule.",
    "journal_size": "Check write rate and take a verified backup before checkpoint maintenance.",
    "database_size": "Review retention, checkpoint size and available filesystem capacity.",
    "free_space": "Free or expand the database filesystem; verify backup and media locations.",
    "scan_incomplete": "Increase the bounded scan budget or add a scoped time index/archive.",
    "storage_probe": "Restore access to the database filesystem and rerun the capacity probe.",
}

_COMPONENT = {
    "catalog_limit": "database", "catalog_growth": "integration",
    "plan_growth": "scheduler", "empty_plans": "scheduler",
    "plan_entries": "scheduler", "journal_size": "database",
    "database_size": "database", "free_space": "database",
    "scan_incomplete": "database", "storage_probe": "database",
}


def _check(code: str, measured: int | None, limit: int | None, *,
           known: bool = True, critical: bool = False, low: bool = False) -> dict:
    if measured is None:
        state = "unknown"
    elif not known and not (measured < limit if low else measured > limit):
        # A partial count is a lower bound: it can prove a high-water breach,
        # but cannot prove that a threshold was respected.
        state = "unknown"
    elif (measured < limit if low else measured > limit):
        state = "critical" if critical else "warning"
    else:
        state = "ok"
    return {"code": code, "state": state, "measured": measured,
            "limit": limit, "action": _ACTIONS[code]}


def _station_id(station_id: str) -> str:
    try:
        return str(UUID(str(station_id)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError("station_id must be a UUID") from exc


def _inspect_authorized(store: Store, station_id: str,
                        policy: CapacityPolicy | Mapping[str, Any],
                        observed_at: str | None) -> dict:
    if store.get("stations", station_id) is None:
        raise StoreError("station is absent")
    selected = _policy(policy)
    at = _instant(observed_at)
    metrics = _snapshot(store, station_id, selected, at)
    checks = [
        _check("catalog_limit", metrics["catalog_items"], selected.max_catalog_items),
        _check("catalog_growth", metrics["recent_items"], selected.max_recent_items,
               known=metrics["catalog_history_complete"]),
        _check("plan_growth", metrics["recent_plan_revisions"],
               selected.max_recent_plan_revisions,
               known=metrics["revision_history_complete"]),
        _check("empty_plans", metrics["recent_empty_plan_revisions"],
               selected.max_recent_empty_plan_revisions,
               known=metrics["revision_history_complete"]),
        _check("plan_entries", metrics["max_entries_in_recent_revision"],
               selected.max_entries_per_recent_revision,
               known=metrics["revision_history_complete"]),
        _check("journal_size", metrics["journal_bytes"], selected.max_journal_bytes),
        _check("database_size", metrics["database_bytes"], selected.max_database_bytes),
        _check("free_space", metrics["free_bytes"], selected.min_free_bytes,
               low=True, critical=(metrics["free_bytes"] is not None and
                                   metrics["free_bytes"] < selected.critical_free_bytes)),
    ]
    incomplete_scan = not metrics["catalog_history_complete"] or not metrics["revision_history_complete"]
    checks.append(_check("scan_incomplete", int(incomplete_scan), 0))
    failed_storage_probe = any(metrics[key] is None for key in
                               ("journal_bytes", "database_bytes", "free_bytes"))
    checks.append(_check("storage_probe", int(failed_storage_probe), 0))
    return {"station_id": station_id, "observed_at": at.isoformat(),
            "window_starts_at": (at - timedelta(hours=selected.window_hours)).isoformat(),
            "policy": asdict(selected), "metrics": metrics, "checks": checks}


def inspect_capacity(store: Store, principal: Principal, station_id: str,
                     policy: CapacityPolicy | Mapping[str, Any], *,
                     observed_at: str | None = None) -> dict:
    """Return bounded measurements and explicit unknown results; no writes."""
    station_id = _station_id(station_id)
    require(store, principal, "view", station_id)
    return _inspect_authorized(store, station_id, policy, observed_at)


def evaluate_capacity(store: Store, principal: Principal, station_id: str,
                      policy: CapacityPolicy | Mapping[str, Any], *,
                      observation_key: str, observed_at: str | None = None) -> dict:
    """Persist active/recovered checks as station-scoped diagnostic evidence.

    Reuse ``observation_key`` only when retrying one sample. A fresh key is
    needed for each later scheduled evaluation; retries return the first event
    for that key and signal without producing duplicate incidents.
    """
    if not isinstance(observation_key, str) or not _OBSERVATION_KEY.fullmatch(observation_key):
        raise StoreError("observation_key must be 1..64 ASCII letters, digits, _, ., : or -")
    station_id = _station_id(station_id)
    require(store, principal, "repair", station_id)
    result = _inspect_authorized(store, station_id, policy, observed_at)
    policy_id = result["policy"]["policy_id"]
    events = []
    for check in result["checks"]:
        code = check["code"]
        correlation_key = f"capacity:{policy_id}:{code}"
        previous = store.lookup("incidents", "station_correlation", station_id,
                                correlation_key)
        prior = previous[0] if previous else None
        state = check["state"]
        if state == "ok":
            if prior is None or prior["last_condition"] == "recovered":
                continue
            condition = "recovered"
        elif state == "unknown" and code != "storage_probe":
            # The scan-incomplete alarm carries the uncertainty; never recover
            # a growth incident from an incomplete sample.
            continue
        else:
            condition = "degraded"
        event_key = "capacity:" + sha256(
            f"{policy_id}:{code}:{condition}:{observation_key}".encode("utf-8")
        ).hexdigest()
        existing = store.lookup("diagnostic_events", "station_event", station_id, event_key)
        if existing:
            events.append({"code": code, "event": existing[0],
                           "incident": store.get("incidents", existing[0]["incident_id"]),
                           "idempotent": True})
            continue
        details = {"measured": check["measured"], "limit": check["limit"],
                   "action": check["action"], "window_hours": result["policy"]["window_hours"],
                   "snapshot_sequence": result["metrics"]["sequence"],
                   "observation_key": observation_key}
        if code == "scan_incomplete":
            details.update({"catalog_rows_inspected": result["metrics"]["catalog_rows_inspected"],
                            "revision_rows_inspected": result["metrics"]["revision_rows_inspected"],
                            "max_scan_rows": result["policy"]["max_scan_rows"],
                            "undated_items": result["metrics"]["undated_items"],
                            "undated_revisions": result["metrics"]["undated_revisions"]})
        if code in {"free_space", "storage_probe"}:
            details.update({"database_free_bytes": result["metrics"]["database_free_bytes"],
                            "media_free_bytes": result["metrics"]["media_free_bytes"]})
        severity = "critical" if state == "critical" else "warning"
        recorded = record_diagnostic(
            store, principal, station_id, event_key=event_key,
            correlation_key=correlation_key, component=_COMPONENT[code],
            condition=condition, severity=severity,
            impact="unknown" if code in {"scan_incomplete", "storage_probe"} else "risk",
            code=f"capacity_{code}",
            message=(f"Capacity check {code}: {state}. {check['action']}"),
            occurred_at=result["observed_at"], details=details)
        events.append({"code": code, **recorded})
    return result | {"diagnostics": events}


@dataclass(frozen=True)
class DeliveryPolicy:
    """Operator-set deadlines for current plan delivery and endpoint media."""

    policy_id: str
    max_pending_minutes: int
    max_acceptance_minutes: int
    copy_lead_minutes: int
    max_scan_rows: int = 100_000
    max_sample_ids: int = 20


def _delivery_policy(value: DeliveryPolicy | Mapping[str, Any]) -> DeliveryPolicy:
    if isinstance(value, DeliveryPolicy):
        result = value
    elif isinstance(value, Mapping):
        try:
            result = DeliveryPolicy(**value)
        except (TypeError, ValueError) as exc:
            raise StoreError("invalid delivery policy fields") from exc
    else:
        raise StoreError("delivery policy must be an object")
    if not isinstance(result.policy_id, str) or not _POLICY_ID.fullmatch(result.policy_id):
        raise StoreError("policy_id must be 1..64 ASCII letters, digits, _, ., : or -")
    for field in ("max_pending_minutes", "max_acceptance_minutes", "max_scan_rows",
                  "max_sample_ids"):
        if type(getattr(result, field)) is not int or getattr(result, field) < 1:
            raise StoreError(f"{field} must be a positive whole number")
    if type(result.copy_lead_minutes) is not int or result.copy_lead_minutes < 0:
        raise StoreError("copy_lead_minutes must be a nonnegative whole number")
    if result.max_scan_rows > 1_000_000 or result.max_sample_ids > 50 or \
            max(result.max_pending_minutes, result.max_acceptance_minutes,
                result.copy_lead_minutes) > 24 * 60 * 30:
        raise StoreError("delivery policy exceeds the monitor bound")
    return result


def _delivery_snapshot(store: Store, station_id: str, policy: DeliveryPolicy,
                       observed_at: datetime) -> dict:
    if "station" not in store.index_specs.get("endpoints", {}) or \
            "endpoint" not in store.index_specs.get("plan_distributions", {}) or \
            "endpoint_revision" not in store.index_specs.get("plan_distributions", {}) or \
            "endpoint_file" not in store.index_specs.get("endpoint_copies", {}) or \
            "station_service" not in store.index_specs.get("broadcast_services", {}):
        raise StoreError("delivery monitor requires RadioS endpoint and media indexes")
    with store._locked(exclusive=False):
        store._reload_locked()
        state, indices = store._state, store._indices
        endpoint_ids = indices.get(("endpoints", "station"), {}).get((station_id,), set())
        endpoints = state.get("endpoints", {})
        distributions = state.get("plan_distributions", {})
        plans = state.get("plans", {})
        revisions = state.get("plan_revisions", {})
        copies = state.get("endpoint_copies", {})
        copy_index = indices.get(("endpoint_copies", "endpoint_file"), {})
        distribution_index = indices.get(("plan_distributions", "endpoint"), {})
        active_endpoints = [endpoint_id for endpoint_id in sorted(endpoint_ids)
                            if endpoints[endpoint_id]["data"]["status"] != "retired"]
        total_rows = sum(len(distribution_index.get((endpoint_id,), set()))
                         for endpoint_id in active_endpoints)
        metrics = {endpoint_id: {
            "endpoint_id": endpoint_id, "missing_copies": 0,
            "critical_missing_copies": 0, "delayed_distributions": 0,
            "critical_delayed_distributions": 0,
            "missing_media_file_ids": set(), "delayed_distribution_ids": set(),
            "missing_distribution_plan_ids": set(),
        } for endpoint_id in active_endpoints}
        # Value is whether a counted missing copy was already critical.
        seen_copies: dict[tuple[str, str], bool | None] = {}
        row_scanned = asset_scanned = plan_scanned = malformed = unbound_plans = 0
        exhausted = False
        for endpoint_id in active_endpoints:
            current = metrics[endpoint_id]
            for distribution_id in distribution_index.get((endpoint_id,), set()):
                if row_scanned >= policy.max_scan_rows:
                    exhausted = True
                    break
                row_scanned += 1
                envelope = distributions.get(distribution_id)
                if envelope is None:
                    malformed += 1
                    continue
                delivery = envelope["data"]
                if delivery.get("station_id") != station_id:
                    malformed += 1
                    continue
                plan = plans.get(delivery.get("plan_id"))
                revision = revisions.get(delivery.get("revision_id"))
                if plan is None or revision is None:
                    malformed += 1
                    continue
                plan_data, revision_data = plan["data"], revision["data"]
                if (plan_data.get("station_id") != station_id or
                        revision_data.get("plan_id") != delivery.get("plan_id")):
                    malformed += 1
                    continue
                if (plan_data.get("current_revision_id") != delivery.get("revision_id") or
                        plan_data.get("state") not in {"approved", "distributed", "active"} or
                        revision_data.get("status") != "approved"):
                    continue

                status = delivery.get("status")
                activation = _created_at(delivery.get("activate_after"))
                delayed = False
                critical_delay = False
                if status == "pending":
                    queued = _created_at(delivery.get("queued_at"))
                    if queued is None:
                        malformed += 1
                    else:
                        delayed = observed_at > queued + timedelta(
                            minutes=policy.max_pending_minutes)
                elif status == "sent":
                    if activation is None:
                        malformed += 1
                    else:
                        delayed = observed_at > activation + timedelta(
                            minutes=policy.max_acceptance_minutes)
                        critical_delay = delayed
                elif status in {"rejected", "expired"}:
                    delayed = critical_delay = True
                if delayed:
                    current["delayed_distributions"] += 1
                    current["critical_delayed_distributions"] += int(critical_delay)
                    current["delayed_distribution_ids"].add(distribution_id)

                copy_due = status == "accepted" or (
                    status in {"pending", "sent"} and activation is not None and
                    observed_at >= activation - timedelta(minutes=policy.copy_lead_minutes))
                if status in {"pending", "sent"} and activation is None:
                    malformed += 1
                if not copy_due:
                    continue
                snapshot = delivery.get("snapshot")
                manifest = snapshot.get("media") if isinstance(snapshot, dict) else None
                if not isinstance(manifest, list):
                    malformed += 1
                    continue
                for asset in manifest:
                    if asset_scanned >= policy.max_scan_rows:
                        exhausted = True
                        break
                    asset_scanned += 1
                    if not isinstance(asset, dict) or not isinstance(
                            asset.get("media_file_id"), str) or not isinstance(
                            asset.get("sha256"), str) or not re.fullmatch(
                            r"[0-9a-f]{64}", asset["sha256"]):
                        malformed += 1
                        continue
                    try:
                        media_id = str(UUID(asset["media_file_id"]))
                    except (ValueError, TypeError, AttributeError):
                        malformed += 1
                        continue
                    if media_id != asset["media_file_id"]:
                        malformed += 1
                        continue
                    identity = (endpoint_id, media_id)
                    is_critical = status == "accepted" or observed_at >= activation
                    if identity in seen_copies:
                        if seen_copies[identity] is False and is_critical:
                            current["critical_missing_copies"] += 1
                            seen_copies[identity] = True
                        continue
                    copy_ids = copy_index.get(identity, set())
                    copy = copies[next(iter(copy_ids))]["data"] if copy_ids else None
                    if copy is None or copy.get("status") != "verified" or \
                            copy.get("sha256") != asset["sha256"] or not copy.get("verified_at"):
                        current["missing_copies"] += 1
                        current["critical_missing_copies"] += int(is_critical)
                        current["missing_media_file_ids"].add(media_id)
                        seen_copies[identity] = is_critical
                    else:
                        seen_copies[identity] = None
                if exhausted:
                    break
            if exhausted:
                break

        # A current approved plan may have no distribution row at all. Once
        # its preparation window opens, that absence is also a delivery fault.
        # An unbound legacy default with multiple endpoints has no safe target
        # to infer; report uncertainty instead of choosing an arbitrary one.
        if not exhausted:
            from .playout import _local_day_start
            station = state["stations"][station_id]["data"]
            zone = ZoneInfo(station["timezone"])
            service_index = indices.get(("broadcast_services", "station_service"), {})
            service_rows = state.get("broadcast_services", {})
            revision_index = indices.get(("plan_distributions", "endpoint_revision"), {})
            active_endpoint_set = set(active_endpoints)
            for plan_id, envelope in plans.items():
                if plan_scanned >= policy.max_scan_rows:
                    exhausted = True
                    break
                plan_scanned += 1
                plan = envelope["data"]
                if (plan.get("station_id") != station_id or
                        plan.get("state") not in {"approved", "distributed", "active"} or
                        not plan.get("current_revision_id")):
                    continue
                revision_id = plan["current_revision_id"]
                revision = revisions.get(revision_id)
                if revision is None or revision["data"].get("status") != "approved":
                    malformed += 1
                    continue
                try:
                    activation = _local_day_start(date.fromisoformat(plan["service_date"]), zone)
                except (KeyError, TypeError, ValueError, Conflict):
                    malformed += 1
                    continue
                if observed_at < activation - timedelta(minutes=policy.copy_lead_minutes):
                    continue
                service_ids = service_index.get((station_id, plan.get("service_key")), set())
                if service_ids:
                    service = service_rows[next(iter(service_ids))]["data"]
                    if service["status"] != "active":
                        continue
                    endpoint_id = service["endpoint_id"]
                elif plan.get("service_key") == "default":
                    delivered_to = [candidate for candidate in active_endpoints
                                    if revision_index.get((candidate, revision_id))]
                    if len(delivered_to) == 1:
                        endpoint_id = delivered_to[0]
                    elif not delivered_to and len(active_endpoints) == 1:
                        endpoint_id = active_endpoints[0]
                    else:
                        unbound_plans += 1
                        continue
                else:
                    unbound_plans += 1
                    continue
                if endpoint_id not in active_endpoint_set:
                    unbound_plans += 1
                    continue
                if revision_index.get((endpoint_id, revision_id)):
                    continue
                current = metrics[endpoint_id]
                current["delayed_distributions"] += 1
                current["critical_delayed_distributions"] += int(observed_at >= activation)
                current["missing_distribution_plan_ids"].add(plan_id)

        complete = (not exhausted and row_scanned == total_rows and
                    plan_scanned == len(plans) and malformed == 0 and unbound_plans == 0)
        for current in metrics.values():
            current["missing_media_file_ids"] = sorted(
                current["missing_media_file_ids"])[:policy.max_sample_ids]
            current["delayed_distribution_ids"] = sorted(
                current["delayed_distribution_ids"])[:policy.max_sample_ids]
            current["missing_distribution_plan_ids"] = sorted(
                current["missing_distribution_plan_ids"])[:policy.max_sample_ids]
        return {"sequence": store._sequence, "endpoints": metrics,
                "distribution_rows_inspected": row_scanned,
                "distribution_rows_total": total_rows,
                "plan_rows_inspected": plan_scanned, "plan_rows_total": len(plans),
                "media_assets_inspected": asset_scanned,
                "malformed_rows": malformed, "unbound_plans": unbound_plans,
                "scan_complete": complete}


def _delivery_check(code: str, endpoint_id: str | None, measured: int,
                    critical_count: int, complete: bool, action: str) -> dict:
    state = ("critical" if critical_count else "warning" if measured else
             "ok" if complete else "unknown")
    return {"code": code, "endpoint_id": endpoint_id, "state": state,
            "measured": measured, "action": action}


def _inspect_delivery_authorized(store: Store, station_id: str,
                                 policy: DeliveryPolicy | Mapping[str, Any],
                                 observed_at: str | None) -> dict:
    if store.get("stations", station_id) is None:
        raise StoreError("station is absent")
    selected = _delivery_policy(policy)
    at = _instant(observed_at)
    metrics = _delivery_snapshot(store, station_id, selected, at)
    complete = metrics["scan_complete"]
    checks = []
    for endpoint_id, endpoint in metrics["endpoints"].items():
        checks.append(_delivery_check(
            "endpoint_media_missing", endpoint_id, endpoint["missing_copies"],
            endpoint["critical_missing_copies"], complete,
            "Verify the endpoint's local media against the current plan manifest and restage missing copies."))
        checks.append(_delivery_check(
            "plan_distribution_delay", endpoint_id, endpoint["delayed_distributions"],
            endpoint["critical_delayed_distributions"], complete,
            "Inspect endpoint connectivity and the current plan delivery/acceptance state."))
    checks.append(_delivery_check(
        "delivery_scan_incomplete", None, int(not complete), 0, True,
        "Inspect the scan budget, malformed records and ambiguous service bindings."))
    return {"station_id": station_id, "observed_at": at.isoformat(),
            "policy": asdict(selected), "metrics": metrics, "checks": checks}


def inspect_delivery_health(store: Store, principal: Principal, station_id: str,
                            policy: DeliveryPolicy | Mapping[str, Any], *,
                            observed_at: str | None = None) -> dict:
    """Read current endpoint copy and plan delivery risks without writing."""
    station_id = _station_id(station_id)
    require(store, principal, "view", station_id)
    return _inspect_delivery_authorized(store, station_id, policy, observed_at)


def evaluate_delivery_health(store: Store, principal: Principal, station_id: str,
                             policy: DeliveryPolicy | Mapping[str, Any], *,
                             observation_key: str,
                             observed_at: str | None = None) -> dict:
    """Persist one diagnostic per condition transition, scoped to a station."""
    if not isinstance(observation_key, str) or not _OBSERVATION_KEY.fullmatch(observation_key):
        raise StoreError("observation_key must be 1..64 ASCII letters, digits, _, ., : or -")
    station_id = _station_id(station_id)
    require(store, principal, "repair", station_id)
    result = _inspect_delivery_authorized(store, station_id, policy, observed_at)
    events = []
    for check in result["checks"]:
        if check["state"] == "unknown":
            continue  # Uncertainty may not recover an earlier fault.
        endpoint_id = check["endpoint_id"]
        code = check["code"]
        correlation_key = (f"delivery:{result['policy']['policy_id']}:"
                           f"{endpoint_id or 'station'}:{code}")
        condition = "recovered" if check["state"] == "ok" else "degraded"
        for _ in range(3):
            prior_rows = store.lookup("incidents", "station_correlation",
                                      station_id, correlation_key)
            prior = prior_rows[0] if prior_rows else None
            if condition == "recovered" and prior is None:
                break
            desired_severity = "critical" if check["state"] == "critical" else "warning"
            if prior and prior["last_condition"] == condition and (
                    condition == "recovered" or
                    (prior["state"] != "resolved" and prior["severity"] == desired_severity)):
                break
            if prior is not None and _created_at(prior["last_seen_at"]) >= _created_at(
                    result["observed_at"]):
                # Old/equal-time samples are evidence of the past, not a new
                # condition transition for the live incident.
                break
            predecessor = prior["last_event_id"] if prior else "first"
            event_key = "delivery:" + sha256(
                f"{correlation_key}:{condition}:{predecessor}".encode("utf-8")
            ).hexdigest()
            endpoint_metrics = result["metrics"]["endpoints"].get(endpoint_id, {})
            details = {"measured": check["measured"],
                       "snapshot_sequence": result["metrics"]["sequence"],
                       "observation_key": observation_key,
                       "max_pending_minutes": result["policy"]["max_pending_minutes"],
                       "max_acceptance_minutes": result["policy"]["max_acceptance_minutes"],
                       "copy_lead_minutes": result["policy"]["copy_lead_minutes"],
                       "scan_complete": result["metrics"]["scan_complete"],
                       "malformed_rows": result["metrics"]["malformed_rows"],
                       "unbound_plans": result["metrics"]["unbound_plans"],
                       "distribution_rows_inspected": result["metrics"]["distribution_rows_inspected"],
                       "plan_rows_inspected": result["metrics"]["plan_rows_inspected"],
                       "media_assets_inspected": result["metrics"]["media_assets_inspected"],
                       "sample_missing_media_file_ids": endpoint_metrics.get("missing_media_file_ids", []),
                       "sample_delayed_distribution_ids": endpoint_metrics.get("delayed_distribution_ids", []),
                       "sample_missing_distribution_plan_ids": endpoint_metrics.get(
                           "missing_distribution_plan_ids", []),
                       "action": check["action"]}
            try:
                recorded = record_diagnostic(
                    store, principal, station_id, event_key=event_key,
                    correlation_key=correlation_key,
                    component=("media" if code == "endpoint_media_missing" else
                               "scheduler" if code == "plan_distribution_delay" else "database"),
                    condition=condition,
                    severity=desired_severity,
                    impact=("none" if condition == "recovered" else
                            "unknown" if code == "delivery_scan_incomplete" else "risk"),
                    code=code, message=f"Delivery check {code}: {check['state']}. {check['action']}",
                    occurred_at=result["observed_at"], endpoint_id=endpoint_id,
                    details=details)
            except Conflict:
                # Another evaluator may have written the same transition.
                continue
            if not recorded["idempotent"]:
                events.append({"code": code, "endpoint_id": endpoint_id, **recorded})
            break
        else:
            raise Conflict("delivery diagnostic changed repeatedly during evaluation")
    return result | {"diagnostics": events}
