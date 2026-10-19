"""Transactional plan drafts, approval, cart resolution and distribution."""

from __future__ import annotations

from datetime import date, datetime, timedelta, timezone
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo

from .identity import Principal, require
from .planning import select_cart_cut, validate_entry, validate_plan
from .readiness import (execution_scope, readiness_at,
                        _advertising_target_matches)
from .snapshot_contract import CONTRACT_VERSION, freeze_entry
from .storage import Conflict, Store, StoreError
from .broadcast_services import (check_entry_scope, execution_entry,
                                 require_approved_execution_binding, require_service,
                                 resolve_execution_binding,
                                 service_key as checked_service_key)


def _body(row: dict) -> dict:
    return {key: value for key, value in row.items() if key not in ("_id", "_rev")}


def _plan(store: Store, station_id: str, plan_id: str) -> dict:
    plan = store.get("plans", str(UUID(plan_id)))
    if plan is None or plan["station_id"] != station_id:
        raise StoreError("plan is not in station")
    return plan


def create_plan(store: Store, principal: Principal, station_id: str,
                service_date: str, service_key: str = "default") -> dict:
    station_id = str(UUID(station_id))
    require(store, principal, "edit", station_id)
    service_key = checked_service_key(service_key)
    require_service(store, station_id, service_key)
    try:
        day = date.fromisoformat(service_date)
    except ValueError as exc:
        raise StoreError("service_date must be YYYY-MM-DD") from exc
    if store.lookup("plans", "station_date_service", station_id,
                    day.isoformat(), service_key):
        raise Conflict("broadcast service already has a plan for this date")
    plan_id, revision_id = str(uuid4()), str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.put("plans", plan_id, {"station_id": station_id, "service_date": day.isoformat(),
                              "service_key": service_key,
                              "state": "draft", "current_revision_id": None,
                              "draft_revision_id": revision_id})
    tx.put("plan_revisions", revision_id, {"plan_id": plan_id, "revision_no": 1,
                                           "status": "draft", "created_by": principal.user_id,
                                           "created_at": datetime.now(timezone.utc).isoformat()})
    tx.commit()
    return {"plan_id": plan_id, "service_key": service_key,
            "draft_revision_id": revision_id,
            "plan_revision": 1}


def fork_draft(store: Store, principal: Principal, station_id: str,
               plan_id: str, *, expected_plan_revision: int) -> dict:
    station_id = str(UUID(station_id))
    sequence = store.sequence
    require(store, principal, "edit", station_id)
    plan = _plan(store, station_id, plan_id)
    if plan["_rev"] != expected_plan_revision:
        raise Conflict("plan changed")
    if plan.get("draft_revision_id"):
        raise Conflict("plan already has a draft revision")
    current_id = plan.get("current_revision_id")
    if not current_id:
        raise Conflict("plan has no approved revision to fork")
    old_revision = store.get("plan_revisions", current_id)
    if old_revision is None or old_revision["status"] != "approved":
        raise Conflict("current plan revision is not approved")
    new_revision_id = str(uuid4())
    old_entries = store.lookup("plan_entries", "revision", current_id)
    remap = {row["_id"]: str(uuid4()) for row in old_entries}
    placement_copies: list[tuple[str, dict]] = []
    placement_ids_by_entry: dict[str, str] = {}
    copied_at = datetime.now(timezone.utc).isoformat()
    for row in old_entries:
        matches = store.lookup("placements", "plan_entry", row["_id"])
        payload = row.get("payload") or {}
        if not matches:
            if payload.get("traffic_placement_id") is not None:
                raise Conflict("fork source advert has a placement hint without a linked reservation")
            continue
        if len(matches) != 1:
            raise Conflict("fork source advert has multiple linked reservations")
        placement = matches[0]
        if row["event_kind"] != "audio" or row["state"] != "planned" or \
                placement["state"] != "planned" or \
                placement.get("makegood_request_id") is not None or \
                payload.get("makegood_request_id") is not None:
            raise Conflict("fork requires a planned ordinary advert; makegoods and final placements need explicit handling")
        if placement.get("plan_id") != plan["_id"] or \
                placement.get("plan_revision_id") != current_id or \
                placement.get("station_id") != station_id or \
                placement.get("break_entry_id") != row.get("ad_break_id") or \
                row.get("ad_break_id") not in remap or \
                payload.get("traffic_placement_id") != placement["_id"] or \
                payload.get("traffic_spot_id") != placement["spot_id"] or \
                payload.get("traffic_obligation_id") != placement["obligation_id"]:
            raise Conflict("fork source advert and reservation disagree")
        spot = store.get("spots", placement["spot_id"])
        if spot is None or payload.get("traffic_campaign_id") != spot["campaign_id"]:
            raise Conflict("fork source advert campaign and reservation disagree")
        new_placement_id = str(uuid4())
        copy = _body(placement)
        source_key = copy.pop("reservation_key", None)
        source_preview = copy.pop("preview_hash", copy.get("source_preview_hash"))
        copy.update({
            "plan_entry_id": remap[row["_id"]],
            "plan_revision_id": new_revision_id,
            "break_entry_id": remap[row["ad_break_id"]],
            "reservation_key": f"fork:{new_revision_id}:{placement['_id']}",
            "forked_from_placement_id": placement["_id"],
            "forked_at": copied_at,
            "forked_by": principal.user_id,
            "created_at": copied_at,
            "created_by": principal.user_id,
        })
        if source_key is not None:
            copy["source_reservation_key"] = source_key
        if source_preview is not None:
            copy["source_preview_hash"] = source_preview
        placement_copies.append((new_placement_id, copy))
        placement_ids_by_entry[row["_id"]] = new_placement_id
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("plan_revisions", new_revision_id, {"plan_id": plan["_id"],
        "revision_no": old_revision["revision_no"] + 1, "status": "draft",
        "based_on_id": current_id, "created_by": principal.user_id,
        "created_at": datetime.now(timezone.utc).isoformat()})
    for row in old_entries:
        body = _body(row)
        body["plan_revision_id"] = new_revision_id
        if body.get("ad_break_id") in remap:
            body["ad_break_id"] = remap[body["ad_break_id"]]
        if row["_id"] in placement_ids_by_entry:
            body["payload"] = dict(body["payload"])
            body["payload"]["traffic_placement_id"] = placement_ids_by_entry[row["_id"]]
        tx.put("plan_entries", remap[row["_id"]], body)
    for placement_id, placement in placement_copies:
        tx.put("placements", placement_id, placement)
    tx.put("plans", plan["_id"], _body(plan) | {"draft_revision_id": new_revision_id},
           expected_revision=plan["_rev"])
    tx.commit()
    return {"plan_id": plan["_id"], "draft_revision_id": new_revision_id,
            "revision_no": old_revision["revision_no"] + 1,
            "forked_placement_count": len(placement_copies)}


def put_entry(store: Store, principal: Principal, station_id: str, plan_id: str,
              revision_id: str, entry: dict, *, expected_plan_revision: int,
              entry_id: str | None = None, expected_entry_revision: int | None = None) -> dict:
    station_id, revision_id = str(UUID(station_id)), str(UUID(revision_id))
    require(store, principal, "edit", station_id)
    plan = _plan(store, station_id, plan_id)
    if plan["_rev"] != expected_plan_revision or plan.get("draft_revision_id") != revision_id:
        raise Conflict("draft plan changed or is not current")
    revision = store.get("plan_revisions", revision_id)
    if revision is None or revision["status"] != "draft":
        raise Conflict("only a draft revision is editable")
    station = store.get("stations", station_id)
    identifier = str(UUID(entry_id)) if entry_id else str(uuid4())
    validation = validate_entry(dict(entry) | {"id": identifier},
                                station["timezone"], plan["service_date"])
    if not validation["valid"]:
        raise StoreError("invalid plan entry: " + ", ".join(
            issue["code"] for issue in validation["errors"]))
    existing = store.get("plan_entries", identifier)
    if existing and (existing["plan_revision_id"] != revision_id or
                     expected_entry_revision != existing["_rev"]):
        raise Conflict("entry changed or belongs to another revision")
    if not existing and expected_entry_revision is not None:
        raise Conflict("entry does not exist at expected revision")
    normalized = validation["normalized"]
    body = {key: value for key, value in normalized.items()
            if key not in ("id", "nominal_end_at", "requires_resolution")}
    binding = require_service(store, station_id, plan["service_key"])
    check_entry_scope(body, binding)
    body["plan_revision_id"] = revision_id
    tx = store.begin(actor=principal.user_id)
    tx.put("plan_entries", identifier, body,
           expected_revision=existing["_rev"] if existing else None)
    from .production import invalidate_production_for_revision
    invalidate_production_for_revision(tx, store, revision_id, "plan_entry_changed")
    tx.put("plans", plan["_id"], _body(plan), expected_revision=plan["_rev"])
    tx.commit()
    return store.get("plan_entries", identifier)


def _approved_entries(store: Store, plan: dict, revision_id: str) -> tuple[list[dict], dict]:
    station = store.get("stations", plan["station_id"])
    entries = store.lookup("plan_entries", "revision", revision_id)
    entries.sort(key=lambda row: (row["ordinal"], row["scheduled_at"], row["_id"]))
    validation = validate_plan([_body(row) | {"id": row["_id"]} for row in entries],
                               station["timezone"], plan["service_date"])
    return entries, validation


def _traffic_claim_id(payload: dict, field: str) -> str | None:
    value = payload.get(field)
    if value is None:
        return None
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise Conflict(f"advertising {field} is not a valid identity") from exc


def _check_traffic_claim(store: Store, plan: dict, row: dict, ready: dict,
                         start: datetime, end: datetime,
                         territory: str, output_id: str) -> None:
    """Bind a declared traffic reservation to this exact playable plan entry."""
    payload = row["payload"]
    spot_id = _traffic_claim_id(payload, "traffic_spot_id")
    campaign_id = _traffic_claim_id(payload, "traffic_campaign_id")
    obligation_id = _traffic_claim_id(payload, "traffic_obligation_id")
    placement_id = _traffic_claim_id(payload, "traffic_placement_id")
    linked = store.lookup("placements", "plan_entry", row["_id"])
    if linked and (len(linked) != 1 or placement_id != linked[0]["_id"]):
        raise Conflict("advertising entry and reserved placement identity disagree")
    if not any((spot_id, campaign_id, obligation_id, placement_id)):
        return
    if spot_id is None or spot_id not in ready["valid_spot_ids"]:
        raise Conflict("advertising spot is not valid for this entry's station, time or output")
    spot = store.get("spots", spot_id)
    if spot is None:
        raise Conflict("advertising spot is absent")
    if campaign_id is not None and campaign_id != spot["campaign_id"]:
        raise Conflict("advertising campaign does not own the claimed spot")
    if obligation_id is not None:
        obligation = store.get("obligations", obligation_id)
        if obligation is None or obligation["campaign_id"] != spot["campaign_id"] or \
                obligation["station_id"] != plan["station_id"] or \
                not _advertising_target_matches(obligation, "region_ids", territory) or \
                not _advertising_target_matches(obligation, "output_ids", output_id):
            raise Conflict("advertising obligation is outside this entry's scope")
        try:
            window_start = datetime.fromisoformat(obligation["window_start"].replace("Z", "+00:00"))
            window_end = datetime.fromisoformat(obligation["window_end"].replace("Z", "+00:00"))
        except (KeyError, TypeError, ValueError, AttributeError) as exc:
            raise Conflict("advertising obligation window is invalid") from exc
        if window_start > start or window_end < end:
            raise Conflict("advertising obligation does not cover the entry slot")
    if placement_id is not None:
        if obligation_id is None:
            raise Conflict("advertising placement needs its obligation identity")
        placement = store.get("placements", placement_id)
        if placement is None or placement["spot_id"] != spot_id or \
                placement["obligation_id"] != obligation_id or \
                placement["plan_id"] != plan["_id"] or \
                placement["station_id"] != plan["station_id"] or \
                placement["plan_entry_id"] != row["_id"] or \
                placement["plan_revision_id"] != row["plan_revision_id"] or \
                placement["break_entry_id"] != row.get("ad_break_id") or \
                placement["output_id"] != output_id or \
                placement.get("region_id") != (None if territory == "*" else territory) or \
                placement["state"] not in {"planned", "completed"}:
            raise Conflict("advertising placement does not belong to this plan entry")


def approve_plan(store: Store, principal: Principal, station_id: str,
                 plan_id: str, revision_id: str, *, expected_plan_revision: int) -> dict:
    station_id, revision_id = str(UUID(station_id)), str(UUID(revision_id))
    require(store, principal, "approve", station_id)
    plan = _plan(store, station_id, plan_id)
    if plan["_rev"] != expected_plan_revision or plan.get("draft_revision_id") != revision_id:
        raise Conflict("draft plan changed")
    revision = store.get("plan_revisions", revision_id)
    if revision is None or revision["status"] != "draft":
        raise Conflict("only a draft revision can be approved")
    sequence = store.sequence
    entries, validation = _approved_entries(store, plan, revision_id)
    binding = resolve_execution_binding(store, station_id, plan["service_key"], entries)
    if not validation["valid"] or not validation["approvable"]:
        raise Conflict("plan is not approvable: " + ", ".join(
            issue["code"] for issue in validation.get("conflicts", [])))
    media_by_entry = {}
    resolved_entries = [execution_entry(row, binding) for row in entries]
    for row in resolved_entries:
        if row["state"] != "planned" or row["event_kind"] != "audio":
            continue
        version_id = row["payload"]["version_id"]
        territory, output_id = execution_scope(row["payload"])
        start = datetime.fromisoformat(row["scheduled_at"].replace("Z", "+00:00"))
        end = start + timedelta(milliseconds=row.get("planned_duration_ms") or 1)
        ready = readiness_at(store, station_id, version_id, slot_start=start,
                             slot_end=end, require_endpoint_copy=False,
                             territory=territory, output_id=output_id)
        if not ready["ready"]:
            raise Conflict(f"plan entry {row['_id']} is not distributable: " +
                           ", ".join(ready["blocked_reasons"]))
        _check_traffic_claim(store, plan, row, ready, start, end, territory, output_id)
        media_by_entry[row["_id"]] = ready["media_file_id"]
    from .playback_markers import prepare_plan_playback
    playback_bindings, marker_rows = prepare_plan_playback(
        store, station_id, resolved_entries, media_by_entry, principal.user_id)
    from .production import require_current_production
    require_current_production(store, station_id, revision_id)
    old_id = plan.get("current_revision_id")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    for marker_id, marker_body in marker_rows:
        tx.put("playback_marker_sets", marker_id, marker_body)
    tx.put("plan_revisions", revision_id,
           _body(revision) | {"status": "approved", "approved_by": principal.user_id,
                              "approved_at": datetime.now(timezone.utc).isoformat(),
                              "execution_binding": binding,
                              "playback_bindings": playback_bindings,
                              "execution_contract_version": CONTRACT_VERSION},
           expected_revision=revision["_rev"])
    if old_id:
        old = store.get("plan_revisions", old_id)
        tx.put("plan_revisions", old_id, _body(old) | {"status": "superseded"},
               expected_revision=old["_rev"])
        # A queued old revision may be downloaded after this approval unless
        # it is retired in the same transaction.  An already accepted copy is
        # retained until the endpoint explicitly accepts its replacement.
        for delivery in store.scan("plan_distributions"):
            if delivery["plan_id"] == plan["_id"] and \
                    delivery["revision_id"] != revision_id and \
                    delivery["status"] in {"pending", "sent"}:
                tx.put("plan_distributions", delivery["_id"],
                       _body(delivery) | {"status": "expired",
                                          "error_code": "revision_superseded"},
                       expected_revision=delivery["_rev"])
    tx.put("plans", plan["_id"], _body(plan) | {
        "current_revision_id": revision_id, "draft_revision_id": None,
        "state": "approved"}, expected_revision=plan["_rev"])
    tx.commit()
    return {"plan_id": plan["_id"], "approved_revision_id": revision_id,
            "entry_count": len(entries), "warnings": validation["warnings"]}


def build_snapshot(store: Store, principal: Principal, station_id: str,
                   endpoint_id: str, revision_id: str) -> dict:
    from .playout import _local_day_start
    station_id, endpoint_id, revision_id = (str(UUID(station_id)), str(UUID(endpoint_id)),
                                            str(UUID(revision_id)))
    require(store, principal, "activate", station_id)
    endpoint = store.get("endpoints", endpoint_id)
    if endpoint is None or endpoint["station_id"] != station_id:
        raise StoreError("endpoint is outside station")
    revision = store.get("plan_revisions", revision_id)
    if revision is None or revision["status"] != "approved":
        raise Conflict("revision is not approved")
    plan = _plan(store, station_id, revision["plan_id"])
    if plan.get("current_revision_id") != revision_id:
        raise Conflict("revision is no longer current")
    binding = require_approved_execution_binding(store, station_id,
                plan["service_key"], revision, endpoint_id)
    from .production import production_snapshot
    production = production_snapshot(store, station_id, revision_id)
    station = store.get("stations", station_id)
    entries, validation = _approved_entries(store, plan, revision_id)
    if not validation["valid"] or not validation["approvable"]:
        raise Conflict("approved plan no longer passes validation")
    manifest: dict[str, dict] = {}
    output_entries: list[dict] = []
    for stored_row in entries:
        row = execution_entry(stored_row, binding)
        if row["state"] != "planned":
            continue
        check_entry_scope(row, binding)
        media_id = None
        markers = None
        if row["event_kind"] == "audio":
            start = datetime.fromisoformat(row["scheduled_at"].replace("Z", "+00:00"))
            end = start + timedelta(milliseconds=row.get("planned_duration_ms") or 1)
            version_id = row["payload"]["version_id"]
            territory, output_id = execution_scope(row["payload"])
            ready = readiness_at(store, station_id, version_id, slot_start=start,
                                 slot_end=end, endpoint_id=endpoint_id,
                                 require_endpoint_copy=False,
                                 territory=territory, output_id=output_id)
            if not ready["ready"]:
                raise Conflict(f"entry {row['_id']} not distributable: " +
                               ", ".join(ready["blocked_reasons"]))
            _check_traffic_claim(store, plan, row, ready, start, end,
                                 territory, output_id)
            from .playback_markers import approved_playback_binding
            markers = approved_playback_binding(store, revision, row)
            media_id = markers["media_file_id"]
            media = store.get("media_files", media_id)
            manifest[media_id] = {"media_file_id": media_id,
                                  "sha256": media["sha256"],
                                  "size_bytes": media["size_bytes"]}
        output_entries.append(freeze_entry(row, media_id, markers))
    for asset in production["media"]:
        prior = manifest.get(asset["media_file_id"])
        if prior is not None and prior != asset:
            raise Conflict("production media manifest conflicts with plan media")
        manifest[asset["media_file_id"]] = asset
    activation = _local_day_start(date.fromisoformat(plan["service_date"]),
                                  ZoneInfo(station["timezone"]))
    return {"contract_version": CONTRACT_VERSION,
            "station_id": station_id, "endpoint_id": endpoint_id,
            "plan_id": plan["_id"], "plan_revision_id": revision_id,
            "revision_no": revision["revision_no"],
            "service_date": plan["service_date"],
            "service_key": plan["service_key"],
            "execution_binding": binding,
            "station_timezone": station["timezone"],
            "activate_after": activation.isoformat(),
            "entries": output_entries,
            "production": {"placements": production["placements"],
                           "execution_capability": production["execution_capability"]},
            "media": [manifest[key] for key in sorted(manifest)]}
