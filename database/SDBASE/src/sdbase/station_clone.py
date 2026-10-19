"""Create a new station identity from a deliberately small configuration copy.

This is a station creation operation, not a restore.  The source station and
its broadcast evidence remain in place.  A preview hash binds the reviewed
copy plan to the exact database sequence used by the atomic commit.
"""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone
import hashlib
import json
from typing import Any
from uuid import UUID, NAMESPACE_URL, uuid5
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from .identity import Forbidden, Principal, require
from .storage import Conflict, Store, StoreError


# These collections have direct station scope.  Their dependent records are
# excluded as well; for example, excluding plans also excludes their revisions
# and entries.  No security credential or evidence is copied.
_EXCLUDED_STATION_COLLECTIONS = (
    "endpoints", "plans", "carts", "rotation_decisions", "plan_distributions",
    "active_acceptances", "engine_commands", "obligations", "playout_attempts",
    "output_observations", "import_jobs", "import_profiles", "view_profiles",
    "schedule_series", "radio_cues", "plan_transitions", "production_placements",
    "controller_profiles", "import_playlist_nodes", "rights_grants", "bulk_jobs",
    "diagnostic_events", "incidents", "listener_requests", "spot_changes",
    "clock_templates", "clock_assignments", "clock_materializations",
    "emergency_alerts", "emergency_alert_attempts",
    "emergency_alert_observation_links", "station_outputs", "recording_jobs",
    "recording_attempts", "now_playing_states", "now_playing_decisions",
    "asrun_report_runs", "live_overrides",
)
_EXCLUDED_DEPENDENTS = (
    "tokens", "permissions", "endpoint_copies", "plan_revisions", "plan_entries",
    "cart_cuts", "cart_states", "playout_events",
    "placements", "schedule_segments", "schedule_exceptions",
    "schedule_occurrences", "controller_profile_versions", "controller_banks",
    "controller_pads", "import_playlist_entries", "spot_change_targets",
    "clock_template_versions", "clock_slots", "clock_exceptions",
    "recording_events", "live_override_reports",
)
_SHARED_GLOBAL_COLLECTIONS = (
    "items", "versions", "media_files", "media_locations", "people",
    "person_memberships", "contributions", "advertisers", "campaigns", "spots",
    "field_definitions", "metadata_suggestions", "external_ids", "dj_analysis",
)
_MAX_COPIED_RECORDS = 2000


def _id(value: Any, name: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def _label(value: Any, name: str, *, max_length: int = 120) -> str:
    if not isinstance(value, str) or not value.strip() or len(value.strip()) > max_length:
        raise StoreError(f"{name} must be nonempty and at most {max_length} characters")
    return value.strip()


def _digest(value: Any) -> str:
    raw = json.dumps(value, sort_keys=True, ensure_ascii=False,
                     separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def _plain(row: dict) -> dict:
    return {key: deepcopy(value) for key, value in row.items()
            if key not in {"_id", "_rev"}}


def _input(store: Store, principal: Principal, source_station_id: str, *,
           new_code: str, new_name: str | None, timezone_name: str | None,
           copy_catalogue_links: bool, idempotency_key: str) -> dict:
    source_station_id = _id(source_station_id, "source_station_id")
    require(store, principal, "manage", source_station_id)
    source = store.get("stations", source_station_id)
    if source is None:
        raise StoreError("source station not found")
    if not isinstance(copy_catalogue_links, bool):
        raise StoreError("copy_catalogue_links must be Boolean")
    if copy_catalogue_links:
        require(store, principal, "view", source_station_id)
    code = _label(new_code, "new_code", max_length=64)
    name = _label(new_name if new_name is not None else source["name"], "new_name")
    tz = timezone_name if timezone_name is not None else source["timezone"]
    if not isinstance(tz, str):
        raise StoreError("timezone_name must be an IANA timezone")
    try:
        ZoneInfo(tz)
    except ZoneInfoNotFoundError as exc:
        raise StoreError("unknown IANA timezone") from exc
    key = _label(idempotency_key, "idempotency_key", max_length=128)
    key_hash = _digest(key)
    station_id = str(uuid5(NAMESPACE_URL,
                           f"sdbase/station-clone/{source_station_id}/{key_hash}"))
    params = {"source_station_id": source_station_id, "new_code": code,
              "new_name": name, "timezone": tz,
              "copy_catalogue_links": copy_catalogue_links}
    return {"source": source, "source_station_id": source_station_id,
            "new_station_id": station_id, "new_code": code, "new_name": name,
            "timezone": tz, "copy_catalogue_links": copy_catalogue_links,
            "clone_key_hash": key_hash, "clone_params_hash": _digest(params)}


def _collect(store: Store, principal: Principal, opts: dict) -> dict:
    """Collect a stable source snapshot and an explicit exclusion report."""
    sequence = store.sequence
    source_id = opts["source_station_id"]
    source = store.get("stations", source_id)
    if source is None:
        raise Conflict("source station disappeared")
    categories = sorted((r for r in store.scan("categories")
                         if r["station_id"] == source_id), key=lambda r: r["_id"])
    category_ids = {r["_id"] for r in categories}
    rules = sorted((r for r in store.scan("rotation_rules")
                    if r["station_id"] == source_id), key=lambda r: r["_id"])
    accepted_rules, skipped_rules = [], []
    for rule in rules:
        missing = sorted(set(rule["config"].get("allowed_categories", [])) - category_ids)
        if missing:
            skipped_rules.append({"id": rule["_id"], "reason": "unmapped_category_filter",
                                  "category_ids": missing})
        else:
            accepted_rules.append(rule)

    all_links = sorted(store.lookup("station_items", "station", source_id),
                       key=lambda r: r["_id"])
    links, skipped_links = [], []
    if opts["copy_catalogue_links"]:
        viewable: dict[str, bool] = {}
        for link in all_links:
            category_id = link.get("category_id")
            if category_id is not None and category_id not in category_ids:
                skipped_links.append({"id": link["_id"], "reason": "unmapped_category"})
                continue
            item_id = link["item_id"]
            if item_id not in viewable:
                stations = {r["station_id"] for r in
                            store.lookup("station_items", "item", item_id)}
                try:
                    for station_id in stations:
                        require(store, principal, "view", station_id)
                except Forbidden:
                    viewable[item_id] = False
                else:
                    viewable[item_id] = True
            if viewable[item_id]:
                links.append(link)
            else:
                skipped_links.append({"id": link["_id"],
                                      "reason": "shared_item_view_permission"})

    copied_count = len(categories) + len(accepted_rules) + len(links)
    if copied_count > _MAX_COPIED_RECORDS:
        raise StoreError("station clone exceeds 2000 copied configuration records")
    excluded_counts = {
        collection: sum(r.get("station_id") == source_id for r in store.scan(collection))
        for collection in _EXCLUDED_STATION_COLLECTIONS
    }
    excluded_counts["station_items"] = len(all_links) - len(links)
    excluded_counts["rotation_rules"] = len(skipped_rules)
    excluded_counts["permissions"] = sum(
        r.get("station_id") == source_id for r in store.scan("permissions"))
    endpoint_ids = {r["_id"] for r in store.scan("endpoints")
                    if r["station_id"] == source_id}
    excluded_counts["tokens"] = sum(
        r.get("endpoint_id") in endpoint_ids for r in store.scan("tokens"))
    excluded_counts["metadata_values"] = sum(
        r.get("station_scope") == source_id for r in store.scan("metadata_values"))
    if store.sequence != sequence:
        raise Conflict("database changed during clone preview; retry")
    snapshot = {"sequence": sequence, "source": source,
                "categories": categories, "rotation_rules": accepted_rules,
                "station_items": links, "skipped_rules": skipped_rules,
                "skipped_links": skipped_links, "excluded_counts": excluded_counts,
                "parameters": {key: opts[key] for key in
                               ("new_station_id", "new_code", "new_name", "timezone",
                                "copy_catalogue_links", "clone_key_hash",
                                "clone_params_hash")}}
    return {"sequence": sequence, "snapshot_hash": _digest(snapshot),
            "categories": categories, "rotation_rules": accepted_rules,
            "station_items": links, "skipped_rules": skipped_rules,
            "skipped_links": skipped_links, "excluded_counts": excluded_counts}


def preview_station_clone(store: Store, principal: Principal, source_station_id: str, *,
                          new_code: str, idempotency_key: str,
                          new_name: str | None = None,
                          timezone_name: str | None = None,
                          copy_catalogue_links: bool = False) -> dict:
    """Return the exact proposed copy and loss report without writing data."""
    opts = _input(store, principal, source_station_id, new_code=new_code,
                  new_name=new_name, timezone_name=timezone_name,
                  copy_catalogue_links=copy_catalogue_links,
                  idempotency_key=idempotency_key)
    existing = store.get("stations", opts["new_station_id"])
    if existing is not None:
        if (existing.get("clone_source_station_id") == opts["source_station_id"] and
                existing.get("clone_key_hash") == opts["clone_key_hash"] and
                existing.get("clone_params_hash") == opts["clone_params_hash"]):
            return {"already_applied": True, "source_station_id": opts["source_station_id"],
                    "new_station_id": opts["new_station_id"],
                    "snapshot_hash": existing.get("clone_snapshot_hash"),
                    "copied_counts": existing.get("clone_copied_counts", {})}
        raise Conflict("clone key already belongs to another station configuration")
    if store.lookup("stations", "code", opts["new_code"]):
        raise Conflict("new station code already exists")
    plan = _collect(store, principal, opts)
    return {"already_applied": False, "source_station_id": opts["source_station_id"],
            "new_station_id": opts["new_station_id"], "new_code": opts["new_code"],
            "new_name": opts["new_name"], "timezone": opts["timezone"],
            "new_status": "disabled", "source_sequence": plan["sequence"],
            "snapshot_hash": plan["snapshot_hash"],
            "copied_counts": {"categories": len(plan["categories"]),
                              "rotation_rules": len(plan["rotation_rules"]),
                              "station_items": len(plan["station_items"])},
            "skipped_rotation_rules": plan["skipped_rules"],
            "skipped_station_items": plan["skipped_links"],
            "excluded_counts": plan["excluded_counts"],
            "excluded_dependent_collections": list(_EXCLUDED_DEPENDENTS),
            "shared_global_collections_not_duplicated": list(_SHARED_GLOBAL_COLLECTIONS),
            "copied_fields": {"categories": ["code", "name"],
                              "rotation_rules": ["name", "config"],
                              "station_items": ["item_id", "category_id", "rotation_code"]},
            "catalogue_links_eligible": False,
            "rotation_rule_status": "draft", "activation_required": True}


def apply_station_clone(store: Store, principal: Principal, source_station_id: str, *,
                        new_code: str, idempotency_key: str, preview_hash: str,
                        new_name: str | None = None,
                        timezone_name: str | None = None,
                        copy_catalogue_links: bool = False) -> dict:
    """Apply a reviewed preview atomically; replay with the same key is safe."""
    opts = _input(store, principal, source_station_id, new_code=new_code,
                  new_name=new_name, timezone_name=timezone_name,
                  copy_catalogue_links=copy_catalogue_links,
                  idempotency_key=idempotency_key)
    if not isinstance(preview_hash, str) or len(preview_hash) != 64 or any(
            char not in "0123456789abcdef" for char in preview_hash):
        raise StoreError("preview_hash must be the SHA-256 from a clone preview")
    existing = store.get("stations", opts["new_station_id"])
    if existing is not None:
        if (existing.get("clone_source_station_id") != opts["source_station_id"] or
                existing.get("clone_key_hash") != opts["clone_key_hash"] or
                existing.get("clone_params_hash") != opts["clone_params_hash"] or
                existing.get("clone_snapshot_hash") != preview_hash):
            raise Conflict("clone key or preview disagrees with existing station")
        return {"already_applied": True, "station": existing,
                "copied_counts": existing.get("clone_copied_counts", {})}
    if store.lookup("stations", "code", opts["new_code"]):
        raise Conflict("new station code already exists")
    plan = _collect(store, principal, opts)
    if plan["snapshot_hash"] != preview_hash:
        raise Conflict("source changed since clone preview; preview again")
    now = datetime.now(timezone.utc).isoformat()
    new_station_id = opts["new_station_id"]
    category_map = {row["_id"]: str(uuid5(UUID(new_station_id),
                                           f"categories/{row['_id']}"))
                    for row in plan["categories"]}
    counts = {"categories": len(plan["categories"]),
              "rotation_rules": len(plan["rotation_rules"]),
              "station_items": len(plan["station_items"])}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(plan["sequence"])
    tx.put("stations", new_station_id, {
        "code": opts["new_code"], "name": opts["new_name"],
        "timezone": opts["timezone"], "status": "disabled", "created_at": now,
        "created_by": principal.user_id,
        "clone_source_station_id": opts["source_station_id"],
        "clone_key_hash": opts["clone_key_hash"],
        "clone_params_hash": opts["clone_params_hash"],
        "clone_snapshot_hash": preview_hash, "clone_copied_counts": counts,
    })
    for row in plan["categories"]:
        tx.put("categories", category_map[row["_id"]], {
            "station_id": new_station_id, "code": row["code"], "name": row["name"]})
    for row in plan["rotation_rules"]:
        config = deepcopy(row["config"])
        config["allowed_categories"] = [category_map[value]
                                         for value in config.get("allowed_categories", [])]
        tx.put("rotation_rules", str(uuid5(UUID(new_station_id),
                                           f"rotation_rules/{row['_id']}")), {
            "station_id": new_station_id, "name": row["name"],
            "status": "draft", "config": config,
            "created_by": principal.user_id, "created_at": now,
            "updated_by": principal.user_id, "updated_at": now})
    for row in plan["station_items"]:
        tx.put("station_items", str(uuid5(UUID(new_station_id),
                                          f"station_items/{row['_id']}")), {
            "station_id": new_station_id, "item_id": row["item_id"],
            "category_id": category_map.get(row.get("category_id")),
            "eligible": False, "rotation_code": row.get("rotation_code")})
    for permission in ("view", "edit", "manage"):
        tx.put("permissions", str(uuid5(UUID(new_station_id),
                                        f"creator_permission/{permission}")), {
            "user_id": principal.user_id, "station_id": new_station_id,
            "permission": permission, "station_scope": new_station_id})
    tx.commit()
    return {"already_applied": False, "station": store.get("stations", new_station_id),
            "copied_counts": counts, "excluded_counts": plan["excluded_counts"],
            "skipped_rotation_rules": plan["skipped_rules"],
            "skipped_station_items": plan["skipped_links"]}
