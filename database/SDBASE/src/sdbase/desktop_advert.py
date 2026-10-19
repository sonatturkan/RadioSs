"""Native traffic preparation and separately classified local PCM receipts.

Catalog approval and reservations use canonical traffic/plan records. Native
source consumption is not an endpoint delivery or external on-air observation.
All writes fence the source sequence and publish their replay receipt atomically.
"""
from __future__ import annotations

from copy import deepcopy
import csv
from datetime import datetime, timedelta, timezone
import hashlib
import io
import json
import re
from uuid import uuid4
from zoneinfo import ZoneInfo

from . import ads_service, asrun_reports, desktop_api as api, desktop_media, desktop_playout
from .broadcast_services import require_service
from .catalog import NotFound, require_shared_item_permission
from .identity import require
from .planning import validate_entry, validate_plan
from .station_outputs import _code
from .storage import Conflict, StoreError

OWNED_KINDS = frozenset({"advert_local_run", "advert_report", "advert_notifications"})
MAX_ROWS = 500
MAX_BYTES = 1_800_000
TERMINAL = frozenset({"local_complete", "interrupted", "failed", "notexecuted"})


def _now():
    return datetime.now(timezone.utc).isoformat()


def _body(row):
    return {key: deepcopy(value) for key, value in row.items() if key not in {"_id", "_rev"}}


def _hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
        separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def _bounded(value):
    if len(json.dumps(value, ensure_ascii=False, allow_nan=False).encode()) > MAX_BYTES:
        raise StoreError("advertising response exceeds the bounded snapshot budget")
    return value


def _revision(row, expected):
    if type(expected) is not int or expected < 1 or expected != row["_rev"]:
        raise Conflict("advertising record changed; reload before applying")


def _begin(store, principal, station_id, key, operation, payload, permission="edit"):
    sequence = store.sequence
    sid, station = api._station(store, principal, station_id, permission)
    receipt, digest, replay = api.operation_replay(store, principal, sid, key,
        "desktop-advert/" + operation, payload)
    return sid, station, sequence, receipt, digest, replay


def _commit(store, tx, principal, sid, key, operation, receipt, digest, result):
    _bounded(result)
    api.add_operation(tx, principal, sid, receipt, key, "desktop-advert/" + operation,
        digest, result)
    tx.commit()
    return result


def _put_doc(tx, principal, sid, kind, name, config, old=None):
    now = _now()
    body = {"station_id": sid, "kind": kind, "name": name, "config": deepcopy(config),
            "status": "active", "created_by": old["created_by"] if old else principal.user_id,
            "created_at": old["created_at"] if old else now,
            "updated_by": principal.user_id, "updated_at": now}
    identifier = old["_id"] if old else str(uuid4())
    tx.put("desktop_configurations", identifier, body,
           expected_revision=old["_rev"] if old else None)
    return body | {"_id": identifier, "_rev": old["_rev"] + 1 if old else 1}


def _document(store, sid, kind, identifier):
    row = store.get("desktop_configurations", api._id(identifier))
    if row is None or row["station_id"] != sid or row["kind"] != kind or row["status"] != "active":
        raise NotFound("advertising document not found in station")
    return row


def _fresh(store, sid, binding, *, advertisement=False):
    api._media_binding(binding, sid, store._state)
    detail = desktop_media._snapshot(store, *desktop_media._library(store, sid, binding["media_id"]))
    if advertisement and detail["media"]["kind"] != "advertisement":
        raise StoreError("campaign spot requires an imported advertisement")
    if not detail["version"]["object_available"] or not detail.get("analysis") or \
            detail["analysis"]["sha256"] != binding["media_sha256"]:
        raise Conflict("verified managed media bytes are unavailable or changed")
    detail["analysis"].pop("waveform", None)
    return detail


def validate_campaign_binding(data, state):
    if "desktop_binding" not in data:
        return
    binding = data["desktop_binding"]
    stations = data.get("station_ids") or []
    if not stations:
        raise StoreError("native campaign binding requires station scope")
    for sid in stations:
        api._media_binding(binding, sid, state, persisted=True)
    library = state["desktop_media_library"][binding["media_id"]]["data"]
    item = state["items"][library["item_id"]]["data"]
    if item["kind"] != "advertisement" or data.get("desktop_item_id") != library["item_id"] or \
            data.get("desktop_version_id") != binding["version_id"]:
        raise StoreError("campaign binding differs from its advertisement/version")


def _enriched(store, sid, campaign):
    result = deepcopy(campaign)
    advertiser = store.get("advertisers", campaign["advertiser_id"])
    result["advertiser_name"] = advertiser["name"] if advertiser else ""
    result["binding"] = deepcopy(campaign.get("desktop_binding"))
    result["versions"] = store.lookup("versions", "item", campaign["desktop_item_id"]) if campaign.get("desktop_item_id") else []
    result["versions"].sort(key=lambda row: row["version_no"])
    for version in result["versions"]:
        files = [row for row in store.lookup("media_files", "version", version["_id"]) if row["status"] == "verified"]
        media = files[0] if len(files) == 1 else None
        version.update(media_file_id=media["_id"] if media else None, sha256=media.get("sha256") if media else None,
                       duration_ms=media.get("technical", {}).get("duration_ms") if media else None)
    result["spots"] = [row for row in store.scan("spots") if row["campaign_id"] == campaign["_id"]]
    result["obligations"] = [row for row in store.lookup("obligations", "station", sid) if row["campaign_id"] == campaign["_id"]]
    return result


def _outputs(store, sid):
    return sorted([row for row in store.lookup("station_outputs", "station", sid) if row["status"] == "active"],
                  key=lambda row: (row["output_id"], row["_id"]))


def _output(store, sid, output_id, region_id=None):
    code = _code(output_id, "output_id")
    matches = [row for row in _outputs(store, sid) if row["output_id"] == code]
    if len(matches) != 1:
        raise Conflict("select one uniquely configured active station output")
    if region_id is not None and _code(region_id, "region_id") not in matches[0]["geo_codes"]:
        raise StoreError("region is outside configured output coverage")
    endpoint = store.get("endpoints", matches[0]["endpoint_id"])
    if not endpoint or endpoint["status"] == "retired":
        raise Conflict("station output endpoint is retired or absent")
    return matches[0]


def _break_snapshot(store, row):
    result = deepcopy(row)
    bridge = row["config"].get("bridge")
    if bridge:
        result.update({key: bridge[key] for key in ("plan_id", "plan_revision_id", "break_entry_id")})
        plan = store.get("plans", bridge["plan_id"])
        result["plan_revision"] = plan["_rev"] if plan else None
        result["reservation_state"] = bridge["state"]
    return result


def _notifications(store, sid, campaigns, breaks, principal):
    rows = []
    for campaign in campaigns:
        if campaign["status"] not in {"completed", "cancelled"}:
            reason = "spot_unbound" if not campaign.get("binding") else \
                "spot_unapproved" if not any(spot["status"] == "approved" for spot in campaign["spots"]) else None
            if reason:
                rows.append({"id": _hash([campaign["_id"], campaign["_rev"], reason]),
                    "campaign_id": campaign["_id"], "reason": reason, "message": campaign["name"] + ": " + reason})
    for row in breaks:
        bridge = row["config"].get("bridge")
        if not bridge or not bridge["overhead_ready"]:
            rows.append({"id": _hash([row["_id"], row["_rev"], "break_unready"]),
                "break_document_id": row["_id"], "reason": "break_unready", "message": row["name"] + ": break_unready"})
    read = set()
    for row in store.lookup("desktop_configurations", "station_kind", sid, "advert_notifications"):
        if row["config"]["user_id"] == principal.user_id:
            read.update(row["config"]["notification_ids"])
    for row in rows:
        row["read"] = row["id"] in read
    return rows[:MAX_ROWS]


@api._consistent_read
def overview(store, principal, station_id):
    sid, _ = api._station(store, principal, station_id)
    campaigns = [_enriched(store, sid, row) for row in store.scan("campaigns") if sid in (row.get("station_ids") or [])]
    breaks = [_break_snapshot(store, row) for row in store.lookup("desktop_configurations", "station_kind", sid, "advert_break") if row["status"] == "active"]
    if len(campaigns) > MAX_ROWS or len(breaks) > MAX_ROWS:
        raise StoreError("advertising workspace exceeds 500 records; narrow station scope")
    clocks = {"templates": [row for row in store.scan("clock_templates") if row["station_id"] == sid],
              "assignments": [row for row in store.scan("clock_assignments") if row["station_id"] == sid]}
    return _bounded({"campaigns": campaigns, "outputs": _outputs(store, sid), "breaks": breaks,
        "clocks": clocks, "notifications": _notifications(store, sid, campaigns, breaks, principal),
        "sequence": store.sequence})


@api._consistent_read
def campaign_get(store, principal, station_id, campaign_id):
    sid, _ = api._station(store, principal, station_id)
    return _bounded({"campaign": _enriched(store, sid, api._campaign(store, sid, campaign_id)),
                     "outputs": _outputs(store, sid), "sequence": store.sequence})


def output_create(store, principal, station_id, output_id, idempotency_key, display_name="Yerel MAIN"):
    output_id = _code(output_id, "output_id")
    display_name = api._text(display_name, "display_name", 160, True)
    payload = {"output_id": output_id, "display_name": display_name}
    sid, _, sequence, receipt, digest, replay = _begin(store, principal, station_id, idempotency_key, "output-create", payload, "manage")
    if replay is not None:
        return replay
    if any(row["output_id"] == output_id for row in store.lookup("station_outputs", "station", sid)):
        raise Conflict("output code already configured; select the existing output")
    endpoints = [row for row in store.lookup("endpoints", "station", sid) if row["status"] != "retired"]
    if len(endpoints) != 1:
        raise Conflict("explicit local output requires exactly one managed endpoint")
    identifier = str(uuid4())
    body = {"station_id": sid, "endpoint_id": endpoints[0]["_id"], "output_id": output_id,
        "geo_codes": ["LOCAL"], "status": "active", "display_name": display_name,
        "desktop_local": True, "created_by": principal.user_id}
    result = body | {"_id": identifier, "_rev": 1}
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    tx.put("station_outputs", identifier, body)
    return _commit(store, tx, principal, sid, idempotency_key, "output-create", receipt, digest, result)


def campaign_state(store, principal, station_id, campaign_id, expected_revision, state, idempotency_key):
    transitions = {"approve": ({"draft"}, "approved"), "pause": ({"approved", "active"}, "paused"),
                   "resume": ({"paused"}, "approved"), "cancel": ({"draft", "approved", "active", "paused"}, "cancelled")}
    if state not in transitions:
        raise StoreError("campaign state action must be approve, pause, resume or cancel")
    payload = {"campaign_id": campaign_id, "expected_revision": expected_revision, "state": state}
    sid, _, sequence, receipt, digest, replay = _begin(store, principal, station_id, idempotency_key, "campaign-state", payload, "approve")
    if replay is not None:
        return replay
    campaign = api._campaign(store, sid, campaign_id); _revision(campaign, expected_revision)
    if campaign["status"] not in transitions[state][0]:
        raise Conflict("campaign cannot make the requested transition")
    body = _body(campaign) | {"status": transitions[state][1], "updated_at": _now(), "updated_by": principal.user_id}
    if state == "approve":
        body.update(approved_at=_now(), approved_by=principal.user_id)
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    tx.put("campaigns", campaign["_id"], body, expected_revision=expected_revision)
    result = body | {"_id": campaign["_id"], "_rev": expected_revision + 1}
    return _commit(store, tx, principal, sid, idempotency_key, "campaign-state", receipt, digest, result)


def bind(store, principal, station_id, campaign_id, expected_revision, media_id,
         version_id, media_file_id, media_sha256, idempotency_key):
    binding = {"media_id": api._id(media_id, "media_id"), "version_id": api._id(version_id, "version_id"),
               "media_file_id": api._id(media_file_id, "media_file_id"), "media_sha256": media_sha256}
    payload = {"campaign_id": campaign_id, "expected_revision": expected_revision, **binding}
    sid, _, sequence, receipt, digest, replay = _begin(store, principal, station_id, idempotency_key, "bind", payload)
    if replay is not None:
        return replay
    campaign = api._campaign(store, sid, campaign_id); _revision(campaign, expected_revision)
    if campaign["status"] in {"completed", "cancelled"}:
        raise Conflict("a final campaign cannot acquire a new spot")
    detail = _fresh(store, sid, binding, advertisement=True)
    body = _body(campaign) | {"desktop_binding": binding, "desktop_item_id": detail["media"]["item_id"],
        "desktop_version_id": binding["version_id"], "updated_at": _now(), "updated_by": principal.user_id}
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    tx.put("campaigns", campaign["_id"], body, expected_revision=expected_revision)
    result = body | {"_id": campaign["_id"], "_rev": expected_revision + 1}
    return _commit(store, tx, principal, sid, idempotency_key, "bind", receipt, digest, result)


def spot_approve(store, principal, station_id, campaign_id, expected_revision, idempotency_key,
                 position="any", competitive_categories=None, rights=None):
    if position not in {"any", "first", "last"}:
        raise StoreError("spot position must be any, first or last")
    categories = [api._text(value, "competitive category", 100, True) for value in api._list(competitive_categories or [], "competitive_categories", 32)]
    if len(categories) != len(set(categories)):
        raise StoreError("competitive categories must be unique")
    if rights is not None:
        api._object(rights, {"territory", "output_id", "source_ref"}, {"territory", "output_id", "source_ref"})
        rights = {"territory": _code(rights["territory"], "rights territory"), "output_id": _code(rights["output_id"], "rights output_id"),
                  "source_ref": api._text(rights["source_ref"], "rights source_ref", 1000, True).strip()}
        if not rights["source_ref"]:
            raise StoreError("explicit rights authorization needs an operator source reference")
    payload = {"campaign_id": campaign_id, "expected_revision": expected_revision,
               "position": position, "competitive_categories": categories, "rights": rights}
    sid, station, sequence, receipt, digest, replay = _begin(store, principal, station_id, idempotency_key, "spot-approve", payload, "approve")
    if replay is not None:
        return replay
    require(store, principal, "activate", sid); require(store, principal, "edit", sid)
    campaign = api._campaign(store, sid, campaign_id); _revision(campaign, expected_revision)
    if rights is not None:
        _output(store, sid, rights["output_id"], rights["territory"])
    if campaign["status"] not in {"draft", "approved", "active", "paused"} or not campaign.get("desktop_binding"):
        raise Conflict("campaign needs a current imported spot binding")
    detail = _fresh(store, sid, campaign["desktop_binding"], advertisement=True)
    version = store.get("versions", detail["version"]["_id"])
    item = store.get("items", detail["media"]["item_id"])
    require_shared_item_permission(store, principal, item["_id"], "approve")
    require_shared_item_permission(store, principal, item["_id"], "edit")
    if item["lifecycle"] != "active":
        require_shared_item_permission(store, principal, item["_id"], "activate")
    if version["status"] not in {"draft", "review", "approved"} or item["lifecycle"] not in {"draft", "active"}:
        raise Conflict("withdrawn/rejected spot cannot be approved")
    existing_spots = store.lookup("spots", "campaign_version", campaign["_id"], version["_id"])
    existing_spot = existing_spots[0] if existing_spots else None
    if existing_spot and (rights is None or existing_spot["status"] != "approved" or existing_spot["position"] != position or
                         (existing_spot.get("competitive_categories") or []) != categories):
        raise Conflict("spot already approved; explicit rights can be added with unchanged spot constraints")
    fields = campaign.get("desktop_fields")
    if not fields or fields["planned_count"] < 1:
        raise StoreError("spot approval requires a positive actual planned count")
    now = _now(); spot_id = existing_spot["_id"] if existing_spot else str(uuid4()); obligation_ids = []
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    if version["status"] != "approved":
        tx.put("versions", version["_id"], _body(version) | {"status": "approved", "approved_by": principal.user_id, "approved_at": now}, expected_revision=version["_rev"])
    if item["lifecycle"] != "active":
        tx.put("items", item["_id"], _body(item) | {"lifecycle": "active"}, expected_revision=item["_rev"])
    association = store.lookup("station_items", "station_item", sid, item["_id"])[0]
    if not association["eligible"]:
        tx.put("station_items", association["_id"], _body(association) | {"eligible": True}, expected_revision=association["_rev"])
    spot = {"campaign_id": campaign["_id"], "version_id": version["_id"], "status": "approved", "position": position,
            "region_ids": campaign.get("region_ids"), "output_ids": campaign.get("output_ids"),
            "competitive_categories": categories or None, "approved_by": principal.user_id, "approved_at": now}
    if existing_spot is None:
        tx.put("spots", spot_id, spot)
    if rights is not None:
        tx.put("rights_grants", str(uuid4()), {"version_id": version["_id"], "station_id": sid,
            "territory": rights["territory"], "output_id": rights["output_id"], "source_ref": rights["source_ref"],
            "starts_at": campaign["starts_at"], "ends_at": campaign["ends_at"], "decision": "allow", "approved_by": principal.user_id})
    existing = [row for row in store.lookup("obligations", "station", sid) if row["campaign_id"] == campaign["_id"]]
    if not existing:
        # A slot constrains each local day's real obligation window, rather
        # than silently turning the displayed slot into an all-day contract.
        zone = ZoneInfo(station["timezone"])
        first = datetime.fromisoformat(campaign["starts_at"]).astimezone(zone)
        last = datetime.fromisoformat(campaign["ends_at"]).astimezone(zone)
        windows = [(first, last)]
        if fields["slot"]:
            days = (last.date() - first.date()).days
            if not 1 <= days <= 367:
                raise StoreError("slot-based obligation is bounded to 367 days")
            hour, minute = map(int, fields["slot"].split(":"))
            windows = []
            for day in range(days):
                begin = (first + timedelta(days=day)).replace(hour=hour, minute=minute, second=0, microsecond=0)
                end = min(begin + timedelta(seconds=max(fields["capacity_seconds"], detail["version"]["duration_ms"] / 1000)), last)
                if begin < first or end <= begin or begin.astimezone(timezone.utc).astimezone(zone) != begin:
                    raise StoreError("campaign slot is invalid in the station calendar")
                windows.append((begin, end))
            if fields["planned_count"] < len(windows):
                raise StoreError("planned count must cover every selected daily slot")
        quotient, remainder = divmod(fields["planned_count"], len(windows))
        for index, (start, end) in enumerate(windows):
            identifier = str(uuid4()); obligation_ids.append(identifier)
            tx.put("obligations", identifier, {"campaign_id": campaign["_id"], "station_id": sid,
                "required_plays": quotient + (1 if index < remainder else 0), "window_start": start.isoformat(),
                "window_end": end.isoformat(), "priority": 0, "region_ids": campaign.get("region_ids"),
                "output_ids": campaign.get("output_ids"), "created_by": principal.user_id, "created_at": now})
    else:
        obligation_ids = [row["_id"] for row in existing]
    campaign_body = _body(campaign) | {"updated_at": now, "updated_by": principal.user_id}
    tx.put("campaigns", campaign["_id"], campaign_body, expected_revision=expected_revision)
    result = {"campaign_id": campaign["_id"], "campaign_revision": expected_revision + 1,
        "spot": deepcopy(existing_spot) if existing_spot else spot | {"_id": spot_id, "_rev": 1}, "obligation_ids": obligation_ids,
        "version_id": version["_id"], "media_file_id": detail["version"]["media_file_id"]}
    return _commit(store, tx, principal, sid, idempotency_key, "spot-approve", receipt, digest, result)


BREAK_KEYS = frozenset({"time", "capacity_seconds", "tolerance_seconds", "fixed_seconds", "opener_seconds",
    "closer_seconds", "empty_mode", "start_mode", "wait_seconds"})


def validate_break(config, sid, state, *, persisted=False):
    api._object(config, BREAK_KEYS | {"date", "output_id", "region_id", "opener", "closer", "fixed", "bridge"}, BREAK_KEYS)
    api._time(config["time"], "time")
    for key, low, high in (("capacity_seconds",10,1800), ("tolerance_seconds",0,120), ("fixed_seconds",0,300),
            ("opener_seconds",0,120), ("closer_seconds",0,120), ("wait_seconds",5,600), ("empty_mode",0,2), ("start_mode",0,2)):
        api._int(config[key], key, low, high)
    if "date" in config:
        api._date(config["date"], "date")
    if "output_id" in config:
        _code(config["output_id"], "output_id")
    if "region_id" in config and config["region_id"] is not None:
        _code(config["region_id"], "region_id")
    for key in ("opener", "closer", "fixed"):
        if key in config:
            api._media_binding(config[key], sid, state, persisted=persisted)
    if "bridge" in config:
        bridge = config["bridge"]
        api._object(bridge, {"plan_id", "plan_revision_id", "break_entry_id", "entry_ids", "spot_budget_ms", "overhead_ms", "overhead_ready", "state"},
            {"plan_id", "plan_revision_id", "break_entry_id", "entry_ids", "spot_budget_ms", "overhead_ms", "overhead_ready", "state"})
        plan = state.get("plans", {}).get(api._id(bridge["plan_id"]), {}).get("data")
        revision = state.get("plan_revisions", {}).get(api._id(bridge["plan_revision_id"]), {}).get("data")
        entry = state.get("plan_entries", {}).get(api._id(bridge["break_entry_id"]), {}).get("data")
        if not plan or plan["station_id"] != sid or not revision or revision["plan_id"] != bridge["plan_id"] or \
                not entry or entry["plan_revision_id"] != bridge["plan_revision_id"] or entry["event_kind"] != "ad_break":
            raise StoreError("native break bridge crosses station, plan revision or break")
        ids = [api._id(value) for value in api._list(bridge["entry_ids"], "entry_ids", 4)]
        if not ids or len(ids) != len(set(ids)) or bridge["break_entry_id"] not in ids or any(
                state.get("plan_entries", {}).get(identifier, {}).get("data", {}).get("plan_revision_id") != bridge["plan_revision_id"] for identifier in ids):
            raise StoreError("native overhead entries differ from their plan revision")
        api._int(bridge["spot_budget_ms"], "spot_budget_ms", 1, 1_920_000)
        api._int(bridge["overhead_ms"], "overhead_ms", 0, 1_919_999)
        api._bool(bridge["overhead_ready"], "overhead_ready")
        if bridge["state"] not in {"draft", "reserved", "cancelled"}:
            raise StoreError("invalid native break reservation state")


def _local_time(station, config):
    zone = ZoneInfo(station["timezone"])
    stamp = datetime.fromisoformat(config["date"] + "T" + config["time"]).replace(tzinfo=zone, fold=0)
    if stamp.astimezone(timezone.utc).astimezone(zone).replace(tzinfo=None) != stamp.replace(tzinfo=None):
        raise StoreError("break local time does not exist in the station calendar")
    other = stamp.replace(fold=1)
    if other.utcoffset() != stamp.utcoffset() and other.astimezone(timezone.utc).astimezone(zone).replace(tzinfo=None) == stamp.replace(tzinfo=None):
        raise StoreError("ambiguous station break time requires an explicit calendar clock")
    return stamp.astimezone(timezone.utc)


def _plan_entry(raw, identifier, revision_id, station, service_date):
    checked = validate_entry(raw | {"id": identifier}, station["timezone"], service_date)
    if not checked["valid"]:
        raise StoreError("invalid native advertising plan entry: " + ",".join(row["code"] for row in checked["errors"]))
    return {key: value for key, value in checked["normalized"].items() if key not in {"id", "nominal_end_at", "requires_resolution"}} | {"plan_revision_id": revision_id}


def _has_consumption(store, sid, entry_id):
    if any(row.get("plan_entry_id") == entry_id for row in store.lookup("playout_attempts", "station", sid)):
        return True
    for row in store.lookup("desktop_configurations", "station_kind", sid, "advert_local_run"):
        if any(event["entry_id"] == entry_id and event["consumed_frames"] > 0 for event in row["config"]["events"]):
            return True
    return False


def break_save(store, principal, station_id, name, config, idempotency_key, document_id=None, expected_revision=None):
    name = api._text(name, "name", 160, True).strip()
    if not isinstance(config, dict) or "bridge" in config or not {"date", "output_id"} <= set(config):
        raise StoreError("dated break requires a configured output; bridge is server managed")
    payload = {"name": name, "config": config, "document_id": document_id, "expected_revision": expected_revision}
    sid, station, sequence, receipt, digest, replay = _begin(store, principal, station_id, idempotency_key, "break-save", payload)
    if replay is not None:
        return replay
    validate_break(config, sid, store._state)
    _output(store, sid, config["output_id"], config.get("region_id"))
    start = _local_time(station, config)
    old = _document(store, sid, "advert_break", document_id) if document_id else None
    if old:
        _revision(old, expected_revision)
        bridge = old["config"].get("bridge")
        if bridge and bridge["state"] == "reserved":
            raise Conflict("cancel the existing reservation before editing this break")
        if bridge and any(_has_consumption(store, sid, identifier) for identifier in bridge["entry_ids"] + [
                row["_id"] for row in store.lookup("plan_entries", "revision", bridge["plan_revision_id"]) if row.get("ad_break_id") == bridge["break_entry_id"]]):
            raise Conflict("consumed break history is retained; create a new dated break instead")
    elif expected_revision is not None:
        raise Conflict("new break cannot carry an existing revision")
    details = {}; overhead = 0; overhead_ready = True
    for role in ("opener", "fixed", "closer"):
        if role in config:
            details[role] = _fresh(store, sid, config[role])
            overhead += details[role]["version"]["duration_ms"]
        else:
            overhead += config[role + "_seconds"] * 1000
            if config[role + "_seconds"]:
                overhead_ready = False
    capacity = (config["capacity_seconds"] + config["tolerance_seconds"]) * 1000
    if overhead >= capacity:
        raise StoreError("actual opener/fixed/closer durations exhaust break capacity")
    plan_matches = store.lookup("plans", "station_date_service", sid, config["date"], "default")
    require_service(store, sid, "default")
    plan = plan_matches[0] if plan_matches else None
    if plan and not plan.get("draft_revision_id"):
        raise Conflict("date has no editable draft plan; fork its approved revision explicitly")
    plan_id = plan["_id"] if plan else str(uuid4())
    revision_id = plan["draft_revision_id"] if plan else str(uuid4())
    if old and old["config"].get("bridge") and (old["config"]["bridge"]["plan_id"] != plan_id or old["config"]["bridge"]["plan_revision_id"] != revision_id):
        raise Conflict("moving an existing break across plans requires a new document")
    revision = store.get("plan_revisions", revision_id) if plan else None
    if revision and revision["status"] != "draft":
        raise Conflict("break requires an editable draft revision")
    entries = store.lookup("plan_entries", "revision", revision_id) if plan else []
    old_ids = set(old["config"].get("bridge", {}).get("entry_ids", [])) if old else set()
    old_break = old["config"].get("bridge", {}).get("break_entry_id") if old else None
    owned = [row for row in entries if row["_id"] in old_ids or row.get("ad_break_id") == old_break and old_break is not None]
    if any(row["plan_revision_id"] != revision_id for row in owned):
        raise Conflict("moving an existing break across plans requires a new document")
    parent_id = str(uuid4()); additions = []; cursor = start
    for role in ("opener", "fixed"):
        if role in details:
            identifier = str(uuid4()); duration = details[role]["version"]["duration_ms"]
            additions.append((identifier, _plan_entry({"ordinal": 0, "event_kind": "audio", "state": "planned",
                "scheduled_at": cursor.isoformat(), "planned_duration_ms": duration, "duration_kind": "fixed",
                "payload": {"version_id": config[role]["version_id"], "output_id": config["output_id"], "region_id": config.get("region_id"), "native_break_role": role}}, identifier, revision_id, station, config["date"])))
            cursor += timedelta(milliseconds=duration)
        else:
            cursor += timedelta(seconds=config[role + "_seconds"])
    additions.append((parent_id, _plan_entry({"ordinal": 0, "event_kind": "ad_break", "state": "planned",
        "scheduled_at": cursor.isoformat(), "planned_duration_ms": capacity - overhead, "duration_kind": "fixed",
        "payload": {"break_id": parent_id, "output_id": config["output_id"], "region_id": config.get("region_id")}}, parent_id, revision_id, station, config["date"])))
    if "closer" in details:
        identifier = str(uuid4()); duration = details["closer"]["version"]["duration_ms"]
        additions.append((identifier, _plan_entry({"ordinal": 0, "event_kind": "audio", "state": "planned",
            "scheduled_at": (start + timedelta(milliseconds=capacity - duration)).isoformat(),
            "planned_duration_ms": duration, "duration_kind": "fixed", "payload": {"version_id": config["closer"]["version_id"],
            "output_id": config["output_id"], "region_id": config.get("region_id"), "native_break_role": "closer"}}, identifier, revision_id, station, config["date"])))
    combined = [(row["_id"], _body(row) | ({"state": "cancelled"} if row in owned else {}), row) for row in entries]
    combined += [(identifier, body, None) for identifier, body in additions]
    combined.sort(key=lambda part: (datetime.fromisoformat(part[1]["scheduled_at"]), 0 if part[1]["event_kind"] == "ad_break" else 1, part[0]))
    for ordinal, (_, body, _) in enumerate(combined):
        body["ordinal"] = ordinal
    validation = validate_plan([body | {"id": identifier} for identifier, body, _ in combined], station["timezone"], config["date"])
    if not validation["valid"] or not validation["approvable"]:
        raise Conflict("native break conflicts with the existing draft plan")
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    if not plan:
        tx.put("plans", plan_id, {"station_id": sid, "service_date": config["date"], "service_key": "default", "state": "draft", "current_revision_id": None, "draft_revision_id": revision_id})
        tx.put("plan_revisions", revision_id, {"plan_id": plan_id, "revision_no": 1, "status": "draft", "created_by": principal.user_id, "created_at": _now()})
    else:
        tx.put("plans", plan_id, _body(plan), expected_revision=plan["_rev"])
    # Preserve cancelled entries and placements as audit history. They do not
    # occupy the replacement draft's audio timeline.
    for identifier, body, prior in combined:
        if prior is None or body != _body(prior):
            tx.put("plan_entries", identifier, body, expected_revision=prior["_rev"] if prior else None)
    saved = deepcopy(config) | {"bridge": {"plan_id": plan_id, "plan_revision_id": revision_id,
        "break_entry_id": parent_id, "entry_ids": [identifier for identifier, _ in additions], "spot_budget_ms": capacity - overhead,
        "overhead_ms": overhead, "overhead_ready": overhead_ready, "state": "draft"}}
    result = _put_doc(tx, principal, sid, "advert_break", name, saved, old)
    result.update(plan_id=plan_id, plan_revision_id=revision_id, break_entry_id=parent_id, plan_revision=plan["_rev"] + 1 if plan else 1)
    return _commit(store, tx, principal, sid, idempotency_key, "break-save", receipt, digest, result)


def _bound_break(store, sid, identifier, expected_revision, *, reserved=False, verify_overhead=True):
    row = _document(store, sid, "advert_break", identifier); _revision(row, expected_revision)
    bridge = row["config"].get("bridge")
    if not bridge or bridge["state"] != ("reserved" if reserved else "draft"):
        raise Conflict("break is not in the required draft/reserved state")
    plan = store.get("plans", bridge["plan_id"])
    revision = store.get("plan_revisions", bridge["plan_revision_id"])
    if not plan or plan.get("draft_revision_id") != bridge["plan_revision_id"] or not revision or revision["status"] != "draft":
        raise Conflict("break draft revision is no longer current")
    _output(store, sid, row["config"]["output_id"], row["config"].get("region_id"))
    if verify_overhead:
        _verify_overhead(store, sid, row, ("opener", "fixed", "closer"))
    return row, plan


def _verify_overhead(store, sid, row, roles):
    for role in roles:
        if role in row["config"]:
            _fresh(store, sid, row["config"][role])
        elif row["config"][role + "_seconds"]:
            raise Conflict("bind each nonzero required overhead duration to verified media")


def _empty_policy(store, sid, row):
    mode = row["config"]["empty_mode"]
    if mode == 1:
        return "skip", []
    roles = ["fixed"] if mode == 0 else ["opener", "fixed", "closer"]
    _verify_overhead(store, sid, row, roles)
    present = [role for role in roles if role in row["config"]]
    return ("fixed_only" if mode == 0 else "overhead", present) if present else ("skip", [])


def _binding_version(store, sid, version_id, media_file_id=None):
    matches = [row for row in store.lookup("desktop_media_library", "station", sid)
               if row["version_id"] == version_id and row["status"] == "active" and
               (media_file_id is None or row["media_file_id"] == media_file_id)]
    if len(matches) != 1:
        raise Conflict("selected spot has no unique imported station media binding")
    media = store.get("media_files", matches[0]["media_file_id"])
    binding = {"media_id": matches[0]["_id"], "version_id": version_id,
               "media_file_id": matches[0]["media_file_id"], "media_sha256": media["sha256"]}
    _fresh(store, sid, binding)
    return binding


def _proposal(store, principal, sid, row, max_spots, advertiser_gap_slots, category_gap_slots):
    max_spots = api._int(max_spots, "max_spots", 1, 500) if max_spots is not None else 500
    advertiser_gap_slots = api._int(advertiser_gap_slots, "advertiser_gap_slots", 0, 500)
    category_gap_slots = api._int(category_gap_slots, "category_gap_slots", 0, 500)
    bridge, config = row["config"]["bridge"], row["config"]
    proposal = ads_service._proposal(store, principal, sid, bridge["plan_id"], bridge["plan_revision_id"], bridge["break_entry_id"],
        output_id=config["output_id"], region_id=config.get("region_id"), max_spots=max_spots,
        advertiser_gap_slots=advertiser_gap_slots, category_gap_slots=category_gap_slots)
    preview, _, _, _, _, offers = proposal
    for selected in preview["selected"]:
        offer = offers[selected["offer_id"]]
        binding = _binding_version(store, sid, offer["spot"]["version_id"], offer["media_file_id"])
        _fresh(store, sid, binding, advertisement=True)
    if preview["selected"]:
        _verify_overhead(store, sid, row, ("opener", "fixed", "closer"))
        action = "spots"
    else:
        action, _ = _empty_policy(store, sid, row)
    preview.update(break_document_id=row["_id"], break_revision=row["_rev"],
        total_capacity_ms=config["capacity_seconds"] * 1000, overhead_ms=bridge["overhead_ms"],
        nominal_capacity_ms=config["capacity_seconds"] * 1000, tolerance_ms=config["tolerance_seconds"] * 1000,
        effective_capacity_ms=(config["capacity_seconds"] + config["tolerance_seconds"]) * 1000,
        empty_action=action, reason="empty_break_policy_" + action if not preview["selected"] else "",
        proof_class="traffic_plan", delivery_proven=False)
    return proposal


@api._consistent_read
def preview(store, principal, station_id, break_document_id, expected_revision,
            max_spots=None, advertiser_gap_slots=1, category_gap_slots=1):
    sid, _ = api._station(store, principal, station_id)
    row, _ = _bound_break(store, sid, break_document_id, expected_revision, verify_overhead=False)
    return _bounded(_proposal(store, principal, sid, row, max_spots, advertiser_gap_slots, category_gap_slots)[0])


def reserve(store, principal, station_id, break_document_id, expected_revision, expected_preview_hash,
            expected_plan_revision, idempotency_key, max_spots=None, advertiser_gap_slots=1, category_gap_slots=1):
    payload = {"break_document_id": break_document_id, "expected_revision": expected_revision,
        "expected_preview_hash": expected_preview_hash, "expected_plan_revision": expected_plan_revision,
        "max_spots": max_spots, "advertiser_gap_slots": advertiser_gap_slots, "category_gap_slots": category_gap_slots}
    sid, _, sequence, receipt, digest, replay = _begin(store, principal, station_id, idempotency_key, "reserve", payload)
    if replay is not None:
        return replay
    row, plan = _bound_break(store, sid, break_document_id, expected_revision, verify_overhead=False)
    _revision(plan, expected_plan_revision)
    if not isinstance(expected_preview_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", expected_preview_hash):
        raise StoreError("expected_preview_hash must be lowercase SHA-256")
    decision, plan, _, station, entries, offers = _proposal(store, principal, sid, row, max_spots, advertiser_gap_slots, category_gap_slots)
    if decision["preview_hash"] != expected_preview_hash:
        raise Conflict("traffic preview changed; preview this break again")
    revision_id, parent_id = row["config"]["bridge"]["plan_revision_id"], row["config"]["bridge"]["break_entry_id"]
    created = []
    for selected in decision["selected"]:
        offer = offers[selected["offer_id"]]
        identifier, placement_id = str(uuid4()), str(uuid4())
        body = _plan_entry({"ordinal": 0, "event_kind": "audio", "state": "planned",
            "scheduled_at": selected["scheduled_at"], "planned_duration_ms": selected["duration_ms"],
            "duration_kind": "fixed", "ad_break_id": parent_id, "payload": {"version_id": offer["spot"]["version_id"],
                "output_id": row["config"]["output_id"], "region_id": row["config"].get("region_id"),
                "traffic_campaign_id": selected["campaign_id"], "traffic_spot_id": selected["spot_id"],
                "traffic_obligation_id": selected["obligation_id"], "traffic_placement_id": placement_id}},
            identifier, revision_id, station, plan["service_date"])
        created.append((identifier, body, placement_id, selected))
    combined = [(entry["_id"], _body(entry), entry) for entry in entries]
    combined.extend((identifier, body, None) for identifier, body, _, _ in created)
    combined.sort(key=lambda part: (datetime.fromisoformat(part[1]["scheduled_at"]), 0 if part[1]["event_kind"] == "ad_break" else 1, part[0]))
    for ordinal, (_, body, _) in enumerate(combined):
        body["ordinal"] = ordinal
    checked = validate_plan([body | {"id": identifier} for identifier, body, _ in combined], station["timezone"], plan["service_date"])
    if not checked["valid"] or not checked["approvable"]:
        raise Conflict("reserved spots conflict with the actual draft timeline")
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    for identifier, body, prior in combined:
        if prior is None or body != _body(prior):
            tx.put("plan_entries", identifier, body, expected_revision=prior["_rev"] if prior else None)
    for identifier, _, placement_id, selected in created:
        tx.put("placements", placement_id, {"station_id": sid, "plan_id": plan["_id"], "plan_revision_id": revision_id,
            "break_entry_id": parent_id, "plan_entry_id": identifier, "obligation_id": selected["obligation_id"], "spot_id": selected["spot_id"],
            "output_id": row["config"]["output_id"], "region_id": row["config"].get("region_id"), "state": "planned",
            "reservation_key": idempotency_key, "preview_hash": expected_preview_hash, "created_by": principal.user_id, "created_at": _now()})
    tx.put("plans", plan["_id"], _body(plan), expected_revision=plan["_rev"])
    from .production import invalidate_production_for_revision
    invalidate_production_for_revision(tx, store, revision_id, "native_traffic_reservation")
    config = deepcopy(row["config"]); config["bridge"]["state"] = "reserved"
    document = _put_doc(tx, principal, sid, "advert_break", row["name"], config, row)
    result = {"break_document_id": row["_id"], "break_revision": document["_rev"], "document": document,
        "plan_id": plan["_id"], "plan_revision_id": revision_id, "plan_revision": plan["_rev"] + 1,
        "placement_ids": [value[2] for value in created], "selected": decision["selected"], "proof_class": "traffic_plan", "delivery_proven": False}
    result["empty_action"] = decision["empty_action"]
    return _commit(store, tx, principal, sid, idempotency_key, "reserve", receipt, digest, result)


def cancel(store, principal, station_id, break_document_id, expected_revision, idempotency_key):
    payload = {"break_document_id": break_document_id, "expected_revision": expected_revision}
    sid, _, sequence, receipt, digest, replay = _begin(store, principal, station_id, idempotency_key, "cancel", payload)
    if replay is not None:
        return replay
    row, plan = _bound_break(store, sid, break_document_id, expected_revision, reserved=True, verify_overhead=False)
    bridge = row["config"]["bridge"]
    entries = store.lookup("plan_entries", "revision", bridge["plan_revision_id"])
    targets = [entry for entry in entries if entry["_id"] in bridge["entry_ids"] or entry.get("ad_break_id") == bridge["break_entry_id"]]
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    kept, cancelled = [], []
    for entry in targets:
        if _has_consumption(store, sid, entry["_id"]):
            kept.append(entry["_id"]); continue
        for placement in store.lookup("placements", "plan_entry", entry["_id"]):
            if placement["state"] == "planned":
                tx.put("placements", placement["_id"], _body(placement) | {"state": "cancelled", "cancelled_by": principal.user_id, "cancelled_at": _now()}, expected_revision=placement["_rev"])
        tx.put("plan_entries", entry["_id"], _body(entry) | {"state": "cancelled"}, expected_revision=entry["_rev"])
        cancelled.append(entry["_id"])
    config = deepcopy(row["config"]); config["bridge"]["state"] = "cancelled"
    document = _put_doc(tx, principal, sid, "advert_break", row["name"], config, row)
    tx.put("plans", plan["_id"], _body(plan), expected_revision=plan["_rev"])
    from .production import invalidate_production_for_revision
    invalidate_production_for_revision(tx, store, bridge["plan_revision_id"], "native_traffic_cancelled")
    result = {"document": document, "break_document_id": row["_id"], "break_revision": document["_rev"],
              "cancelled_entry_ids": cancelled, "retained_consumed_entry_ids": kept}
    return _commit(store, tx, principal, sid, idempotency_key, "cancel", receipt, digest, result)


def _manifest_hash(manifest):
    return _hash({key: value for key, value in manifest.items() if key != "source_sequence"})


def _prepare(store, principal, sid, row, plan):
    bridge, config = row["config"]["bridge"], row["config"]
    selected = [entry for entry in store.lookup("plan_entries", "revision", bridge["plan_revision_id"])
        if entry["state"] == "planned" and entry["event_kind"] == "audio" and
        (entry["_id"] in bridge["entry_ids"] or entry.get("ad_break_id") == bridge["break_entry_id"])]
    if any(entry["payload"].get("traffic_placement_id") for entry in selected):
        _verify_overhead(store, sid, row, ("opener", "fixed", "closer"))
        empty_action = "spots"
    else:
        empty_action, roles = _empty_policy(store, sid, row)
        if empty_action == "skip":
            return {"skipped": True, "empty_action": "skip", "reason": "empty_break_policy_skip", "break_document_id": row["_id"],
                "break_revision": row["_rev"], "playback": None, "prepared": None, "manifest": None, "manifest_hash": None}
        selected = [entry for entry in selected if entry["payload"].get("native_break_role") in roles]
    selected.sort(key=lambda entry: (datetime.fromisoformat(entry["scheduled_at"]), entry["ordinal"], entry["_id"]))
    if not selected or len(selected) > MAX_ROWS:
        raise Conflict("reserved break has no bounded playable entries")
    events, manifest_entries = [], []
    for entry in selected:
        payload = entry["payload"]
        binding = _binding_version(store, sid, payload["version_id"])
        detail = _fresh(store, sid, binding)
        campaign_id, spot_id = payload.get("traffic_campaign_id"), payload.get("traffic_spot_id")
        if campaign_id:
            campaign = api._campaign(store, sid, campaign_id)
            spot = store.get("spots", spot_id)
            if campaign["status"] not in {"approved", "active"} or not spot or spot["status"] != "approved" or spot["campaign_id"] != campaign_id or spot["version_id"] != binding["version_id"]:
                raise Conflict("reserved campaign or spot is paused, cancelled, withdrawn or changed")
            from .readiness import readiness_at
            start = datetime.fromisoformat(entry["scheduled_at"])
            ready = readiness_at(store, sid, binding["version_id"], slot_start=start,
                slot_end=start + timedelta(milliseconds=entry["planned_duration_ms"]),
                require_endpoint_copy=False, territory=config.get("region_id") or "*", output_id=config["output_id"])
            if not ready["ready"] or spot_id not in ready["valid_spot_ids"] or ready["media_file_id"] != binding["media_file_id"]:
                raise Conflict("reserved advertising media is no longer eligible for its actual slot")
        duration = detail["version"]["duration_ms"]
        if duration != entry["planned_duration_ms"]:
            raise Conflict("reserved duration differs from measured media duration")
        frames = (detail["analysis"]["sample_frames"] * 48000 + detail["analysis"]["sample_rate"] - 1) // detail["analysis"]["sample_rate"]
        role = payload.get("native_break_role", "spot")
        manifest_entries.append({"entry_id": entry["_id"], "placement_id": payload.get("traffic_placement_id"),
            "spot_id": spot_id, "campaign_id": campaign_id, "obligation_id": payload.get("traffic_obligation_id"),
            "role": role, "kind": detail["media"]["kind"], "title": detail["media"]["title"],
            "cue_in_frame": 0, "cue_out_frame": frames, "expected_frames": frames, "duration_ms": duration, **binding})
        events.append({"id": entry["_id"], "title": detail["media"]["title"], "artist": detail["media"]["artist"],
            "album": detail["media"]["album"], "kind": detail["media"]["kind"], "start_time": "", "duration_ms": duration,
            "version_label": detail["version"]["label"], "primary_file": "", "backup_file": "", "note": "Yerel reklam kaynak tüketimi", **binding})
    programme = {"title": row["name"], "date": config["date"], "note": "Rezerve reklam kuşağı; yerel PCM tüketimi", "events": events}
    playback = desktop_playout.prepare(store, principal, sid, "programme", config=programme)
    if any(not track["eligible"] for track in playback["tracks"]):
        raise Conflict("reserved break includes unavailable media")
    for track, span in zip(playback["tracks"], manifest_entries):
        # A contractual spot consumes its complete measured file, irrespective
        # of unrelated draft music markers stored against the same version.
        track["detail"]["markers"] = None
        track["cue_in_frame"] = span["cue_in_frame"]
        track["cue_out_frame"] = span["cue_out_frame"]
    manifest = {"schema_version": 1, "station_id": sid, "break_document_id": row["_id"], "break_revision": row["_rev"],
        "plan_id": plan["_id"], "plan_revision_id": bridge["plan_revision_id"], "plan_revision": plan["_rev"],
        "break_entry_id": bridge["break_entry_id"], "output_id": config["output_id"], "region_id": config.get("region_id"),
        "scheduled_at": _local_time(store.get("stations", sid), config).isoformat(), "capacity_ms": config["capacity_seconds"] * 1000,
        "start_mode": config["start_mode"], "wait_seconds": config["wait_seconds"],
        "tolerance_ms": config["tolerance_seconds"] * 1000,
        "effective_capacity_ms": (config["capacity_seconds"] + config["tolerance_seconds"]) * 1000,
        "source_sequence": store.sequence, "entries": manifest_entries, "proof_class": "local_source_consumption", "delivery_proven": False}
    manifest["scheduled_utc_us"] = int(datetime.fromisoformat(manifest["scheduled_at"]).timestamp() * 1_000_000)
    return _bounded({"prepared": playback, "playback": playback, "manifest": manifest, "manifest_hash": _manifest_hash(manifest),
        "break_document_id": row["_id"], "break_revision": row["_rev"], "skipped": False, "empty_action": empty_action})


@api._consistent_read
def prepare(store, principal, station_id, break_document_id, expected_revision):
    sid, _ = api._station(store, principal, station_id)
    row, plan = _bound_break(store, sid, break_document_id, expected_revision, reserved=True, verify_overhead=False)
    return _prepare(store, principal, sid, row, plan)


def _validate_manifest(manifest, sid, state):
    keys = {"schema_version", "station_id", "break_document_id", "break_revision", "plan_id", "plan_revision_id",
        "plan_revision", "break_entry_id", "output_id", "region_id", "scheduled_at", "scheduled_utc_us", "capacity_ms",
        "source_sequence", "entries", "proof_class", "delivery_proven", "start_mode", "wait_seconds", "tolerance_ms", "effective_capacity_ms"}
    api._object(manifest, keys, keys)
    if type(manifest["schema_version"]) is not int or manifest["schema_version"] != 1 or manifest["station_id"] != sid or \
            manifest["proof_class"] != "local_source_consumption" or manifest["delivery_proven"] is not False:
        raise StoreError("native advert manifest cannot claim broadcast delivery")
    _code(manifest["output_id"], "output_id")
    if manifest["region_id"] is not None:
        _code(manifest["region_id"], "region_id")
    api._stamp(manifest["scheduled_at"], "scheduled_at")
    api._int(manifest["scheduled_utc_us"], "scheduled_utc_us", 0, 253402300799999999)
    for key in ("break_revision", "plan_revision", "source_sequence"):
        api._int(manifest[key], key, 1, 2 ** 63 - 1)
    api._int(manifest["capacity_ms"], "capacity_ms", 10000, 1800000)
    api._int(manifest["tolerance_ms"], "tolerance_ms", 0, 120000)
    api._int(manifest["effective_capacity_ms"], "effective_capacity_ms", 10000, 1920000)
    if manifest["capacity_ms"] + manifest["tolerance_ms"] != manifest["effective_capacity_ms"]:
        raise StoreError("nominal advertising capacity and permitted overrun are inconsistent")
    api._int(manifest["start_mode"], "start_mode", 0, 2)
    api._int(manifest["wait_seconds"], "wait_seconds", 5, 600)
    document = state.get("desktop_configurations", {}).get(api._id(manifest["break_document_id"]), {}).get("data")
    plan = state.get("plans", {}).get(api._id(manifest["plan_id"]), {}).get("data")
    revision = state.get("plan_revisions", {}).get(api._id(manifest["plan_revision_id"]), {}).get("data")
    parent = state.get("plan_entries", {}).get(api._id(manifest["break_entry_id"]), {}).get("data")
    if not document or document["station_id"] != sid or document["kind"] != "advert_break" or not plan or plan["station_id"] != sid or \
            not revision or revision["plan_id"] != manifest["plan_id"] or not parent or parent["plan_revision_id"] != manifest["plan_revision_id"]:
        raise StoreError("manifest plan/break identities cross station scope")
    entries = api._list(manifest["entries"], "manifest entries", 500)
    if not entries:
        raise StoreError("native advert manifest requires measured entries")
    ids = set()
    for row in entries:
        fields = api.MEDIA_BINDING_KEYS | {"entry_id", "placement_id", "spot_id", "campaign_id", "obligation_id", "role",
            "kind", "title", "cue_in_frame", "cue_out_frame", "expected_frames", "duration_ms"}
        api._object(row, fields, fields)
        identifier = api._id(row["entry_id"])
        if identifier in ids:
            raise StoreError("manifest entry identities must be unique")
        ids.add(identifier)
        entry = state.get("plan_entries", {}).get(identifier, {}).get("data")
        if not entry or entry["plan_revision_id"] != manifest["plan_revision_id"] or entry["event_kind"] != "audio":
            raise StoreError("manifest audio identity differs from reserved plan revision")
        api._media_binding({key: row[key] for key in api.MEDIA_BINDING_KEYS}, sid, state, persisted=True)
        api._text(row["title"], "title", 200, True)
        if row["role"] not in {"spot", "opener", "closer", "fixed"}:
            raise StoreError("unsupported advertising manifest role")
        if row["kind"] not in desktop_media.KINDS:
            raise StoreError("invalid frozen manifest catalog kind")
        begin = api._int(row["cue_in_frame"], "cue_in_frame", 0, 4147200000)
        end = api._int(row["cue_out_frame"], "cue_out_frame", 1, 4147200000)
        expected = api._int(row["expected_frames"], "expected_frames", 1, 4147200000)
        api._int(row["duration_ms"], "duration_ms", 1, 86400000)
        if end <= begin or end - begin != expected:
            raise StoreError("manifest source frame span is inconsistent")
        analysis = next((record["data"]["analysis"] for record in state.get("desktop_media_analysis", {}).values()
            if record["data"]["media_file_id"] == row["media_file_id"] and record["data"]["station_id"] == sid), None)
        if not analysis or expected != (analysis["sample_frames"] * 48000 + analysis["sample_rate"] - 1) // analysis["sample_rate"] or begin != 0 or row["duration_ms"] != analysis["duration_ms"]:
            raise StoreError("manifest span differs from immutable decoded media")
        # Creation verifies the live payload in _prepare. Later traffic edits
        # may replace an unconsumed planned creative; an accepted immutable
        # local run still reports its original source, never the replacement.
        for key in ("placement_id", "spot_id", "campaign_id", "obligation_id"):
            if row[key] is not None:
                api._id(row[key], key)
        if row["role"] == "spot":
            placement = state.get("placements", {}).get(row["placement_id"], {}).get("data")
            spot = state.get("spots", {}).get(row["spot_id"], {}).get("data")
            campaign = state.get("campaigns", {}).get(row["campaign_id"], {}).get("data")
            obligation = state.get("obligations", {}).get(row["obligation_id"], {}).get("data")
            if not placement or placement["plan_entry_id"] != identifier or placement.get("station_id") != sid or \
                    not spot or spot["campaign_id"] != row["campaign_id"] or spot["version_id"] != row["version_id"] or \
                    not campaign or sid not in (campaign.get("station_ids") or []) or not obligation or obligation["station_id"] != sid or obligation["campaign_id"] != row["campaign_id"]:
                raise StoreError("frozen local traffic references cross station or creative identity")
        elif any(row[key] is not None for key in ("placement_id", "spot_id", "campaign_id", "obligation_id")):
            raise StoreError("overhead source cannot claim a traffic placement")


def _event(value, spans):
    fields = {"event_key", "entry_id", "occurrence", "state", "consumed_frames", "occurred_at", "reason", "main_sink"}
    required = fields - {"reason", "main_sink"}
    api._object(value, fields, required)
    result = deepcopy(value)
    result["event_key"] = api._id(value["event_key"], "event_key")
    result["entry_id"] = api._id(value["entry_id"], "entry_id")
    if result["entry_id"] not in spans:
        raise StoreError("local receipt entry is absent from its frozen manifest")
    api._int(value["occurrence"], "occurrence", 1, 2 ** 63 - 1)
    frames = api._int(value["consumed_frames"], "consumed_frames", 0, spans[result["entry_id"]]["expected_frames"])
    if value["state"] not in TERMINAL | {"started"}:
        raise StoreError("invalid native source-consumption state")
    if value["state"] == "started" and frames < 1 or value["state"] == "local_complete" and frames != spans[result["entry_id"]]["expected_frames"] or value["state"] == "notexecuted" and frames:
        raise StoreError("receipt state does not match actual consumed source frames")
    api._stamp(value["occurred_at"], "occurred_at")
    result["reason"] = api._text(value.get("reason", ""), "reason", 1000)
    sink = api._text(value.get("main_sink", ""), "main_sink", 512)
    if any(ord(char) < 32 or ord(char) == 127 for char in sink):
        raise StoreError("invalid actual MAIN device identity")
    result["main_sink"] = sink
    return result


def _validate_events(events, spans):
    known, pairs = {}, {}
    for value in api._list(events, "local events", 4000):
        event = _event(value, spans)
        if event["event_key"] in known:
            raise StoreError("local event identities must be unique")
        known[event["event_key"]] = event
        pair = (event["entry_id"], event["occurrence"])
        previous = pairs.get(pair)
        if previous and previous["state"] in TERMINAL:
            raise StoreError("terminal local occurrence is immutable")
        if previous and (event["state"] == "started" or event["consumed_frames"] < previous["consumed_frames"] or
                datetime.fromisoformat(event["occurred_at"]) < datetime.fromisoformat(previous["occurred_at"])):
            raise StoreError("local occurrence frames and time must progress monotonically")
        if not previous and event["state"] == "local_complete":
            raise StoreError("completed local occurrence requires its observed start")
        pairs[pair] = event


def _run_state(events, spans):
    if not events:
        return "prepared"
    latest = {}
    for event in events:
        previous = latest.get(event["entry_id"])
        if previous is None or event["occurrence"] >= previous["occurrence"]:
            latest[event["entry_id"]] = event
    if len(latest) == len(spans) and all(event["state"] == "local_complete" for event in latest.values()):
        return "local_complete"
    if any(event["state"] == "failed" for event in latest.values()):
        return "failed"
    if any(event["state"] == "interrupted" for event in latest.values()):
        return "interrupted"
    if len(latest) == len(spans) and all(event["state"] == "notexecuted" for event in latest.values()):
        return "notexecuted"
    return "running"


def validate_owned_configuration(kind, config, sid, state):
    if kind == "advert_local_run":
        keys = {"schema_version", "manifest", "manifest_hash", "events", "state", "proof_class", "delivery_proven"}
        api._object(config, keys, keys)
        _validate_manifest(config["manifest"], sid, state)
        if config["manifest_hash"] != _manifest_hash(config["manifest"]) or config["proof_class"] != "local_source_consumption" or config["delivery_proven"] is not False:
            raise StoreError("local run snapshot/classification changed")
        if type(config["schema_version"]) is not int or config["schema_version"] != 1 or config["state"] not in {"prepared", "running"} | TERMINAL:
            raise StoreError("invalid local advert run state")
        _validate_events(config["events"], {row["entry_id"]: row for row in config["manifest"]["entries"]})
        if config["state"] != _run_state(config["events"], config["manifest"]["entries"]):
            raise StoreError("local run state differs from its appended source receipts")
    elif kind == "advert_report":
        keys = {"schema_version", "filters", "snapshot", "snapshot_hash", "source_sequence", "created_at"}
        api._object(config, keys, keys)
        if type(config["schema_version"]) is not int or config["schema_version"] != 1 or config["snapshot_hash"] != _hash(config["snapshot"]):
            raise StoreError("immutable advertising report snapshot hash mismatch")
        api._int(config["source_sequence"], "source_sequence", 1, 2 ** 63 - 1)
        api._stamp(config["created_at"], "created_at")
        _report_filters(config["filters"], sid)
        if not isinstance(config["snapshot"], dict) or config["snapshot"].get("station_id") != sid or config["snapshot"].get("delivery_proven") is not False or config["snapshot"].get("proof_class") != "separate_traffic_engine_observation_and_local_source":
            raise StoreError("advert report station or evidence classification invalid")
    elif kind == "advert_notifications":
        api._object(config, {"user_id", "notification_ids"}, {"user_id", "notification_ids"})
        if api._id(config["user_id"], "user_id") not in state.get("users", {}):
            raise StoreError("notification reader user is absent")
        values = api._list(config["notification_ids"], "notification_ids", 500)
        if len(values) != len(set(values)) or any(not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value) for value in values):
            raise StoreError("notification IDs must be distinct SHA-256 identities")


def guard_owned_document(before, after):
    if after is None or after.get("status") != "active":
        raise StoreError("managed advertising audit records cannot be removed or hidden")
    if before["kind"] == "advert_report" and after != before:
        raise StoreError("advertising report runs are immutable")
    if before["kind"] == "advert_notifications" and before["config"]["user_id"] != after["config"]["user_id"]:
        raise StoreError("notification reader identity is immutable")
    if before["kind"] == "advert_local_run":
        for key in ("schema_version", "manifest", "manifest_hash", "proof_class", "delivery_proven"):
            if before["config"][key] != after["config"][key]:
                raise StoreError("native run manifest and evidence classification are immutable")
        old_events, new_events = before["config"]["events"], after["config"]["events"]
        if new_events[:len(old_events)] != old_events or len(new_events) < len(old_events):
            raise StoreError("local source receipts are append-only")


def run_start(store, principal, station_id, break_document_id, expected_revision, manifest_hash, idempotency_key):
    payload = {"break_document_id": break_document_id, "expected_revision": expected_revision, "manifest_hash": manifest_hash}
    sid, _, sequence, receipt, digest, replay = _begin(store, principal, station_id, idempotency_key, "run-start", payload, "operate")
    if replay is not None:
        # Keep the durable receipt small and never cache a managed path. Every
        # replay must recheck both traffic state and immutable media bytes.
        from .desktop_admin import assert_start_replay
        assert_start_replay(store, sid, replay["run_id"])
        row, plan = _bound_break(store, sid, break_document_id, expected_revision, reserved=True, verify_overhead=False)
        prepared = _prepare(store, principal, sid, row, plan)
        if prepared["manifest_hash"] != manifest_hash:
            raise Conflict("run input changed after its durable acceptance")
        return replay | {"playback": prepared["playback"], "manifest": prepared["manifest"]}
    row, plan = _bound_break(store, sid, break_document_id, expected_revision, reserved=True, verify_overhead=False)
    prepared = _prepare(store, principal, sid, row, plan)
    if prepared.get("skipped"):
        raise Conflict("empty break policy explicitly skips playback; no local PCM run is created")
    if prepared["manifest_hash"] != manifest_hash:
        raise Conflict("reserved break changed after preparation")
    config = {"schema_version": 1, "manifest": prepared["manifest"], "manifest_hash": manifest_hash,
        "events": [], "state": "prepared", "proof_class": "local_source_consumption", "delivery_proven": False}
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    document = _put_doc(tx, principal, sid, "advert_local_run", row["name"], config)
    result = {"run_id": document["_id"], "revision": document["_rev"], "manifest_hash": manifest_hash}
    _commit(store, tx, principal, sid, idempotency_key, "run-start", receipt, digest, result)
    return result | {"playback": prepared["playback"], "manifest": prepared["manifest"]}


def run_event(store, principal, station_id, run_id, expected_revision, idempotency_key, events):
    api._list(events, "events", 64)
    if not events:
        raise StoreError("at least one actual local receipt is required")
    payload = {"run_id": run_id, "expected_revision": expected_revision, "events": events}
    sid, _, sequence, receipt, digest, replay = _begin(store, principal, station_id, idempotency_key, "run-event", payload, "operate")
    if replay is not None:
        return replay
    row = _document(store, sid, "advert_local_run", run_id); _revision(row, expected_revision)
    from .desktop_admin import assert_run_open
    assert_run_open(store, sid, run_id)
    if row["created_by"] != principal.user_id:
        raise Conflict("only the accepted local run operator can append receipts")
    config = deepcopy(row["config"])
    spans = {entry["entry_id"]: entry for entry in config["manifest"]["entries"]}
    additions = [_event(value, spans) for value in events]
    existing = {value["event_key"]: value for value in config["events"]}
    other_keys = {event["event_key"] for record in store.lookup("desktop_configurations", "station_kind", sid, "advert_local_run")
        if record["_id"] != row["_id"] for event in record["config"]["events"]}
    for event in additions:
        if event["event_key"] in other_keys:
            raise Conflict("event identity is already owned by another local run")
        if event["event_key"] in existing:
            if event != existing[event["event_key"]]:
                raise Conflict("local receipt key was reused with different evidence")
            continue
        config["events"].append(event); existing[event["event_key"]] = event
    _validate_events(config["events"], spans)
    config["state"] = _run_state(config["events"], spans)
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    document = _put_doc(tx, principal, sid, "advert_local_run", row["name"], config, row)
    result = {"run_id": document["_id"], "revision": document["_rev"]}
    return _commit(store, tx, principal, sid, idempotency_key, "run-event", receipt, digest, result)


def _report_filters(filters, sid):
    api._object(filters, {"date_from", "date_to", "campaign_id", "output_id"}, {"date_from", "date_to", "campaign_id", "output_id"})
    api._date(filters["date_from"], "date_from"); api._date(filters["date_to"], "date_to")
    _, first, last, output = asrun_reports._filters(sid, filters["date_from"], filters["date_to"], filters["output_id"])
    if filters["campaign_id"] is not None:
        api._id(filters["campaign_id"], "campaign_id")
    return first, last, output


def _in_dates(stamp, first, last, zone):
    return first <= datetime.fromisoformat(stamp).astimezone(zone).date() <= last


def _local_rows(store, sid, filters, first, last, zone):
    result = []
    for run in store.lookup("desktop_configurations", "station_kind", sid, "advert_local_run"):
        manifest = run["config"]["manifest"]
        if filters["output_id"] is not None and manifest["output_id"] != filters["output_id"]:
            continue
        events_by_entry = {}
        for event in run["config"]["events"]:
            events_by_entry.setdefault(event["entry_id"], {}).setdefault(event["occurrence"], []).append(event)
        for entry in manifest["entries"]:
            if filters["campaign_id"] is not None and entry["campaign_id"] != filters["campaign_id"]:
                continue
            occurrences = events_by_entry.get(entry["entry_id"], {})
            if not occurrences:
                if _in_dates(manifest["scheduled_at"], first, last, zone):
                    result.append({**deepcopy(entry), "run_id": run["_id"], "occurrence": None,
                        "state": "notexecuted", "consumed_frames": 0, "started_at": None, "terminal_at": None,
                        "event_keys": [], "output_id": manifest["output_id"], "main_sink": "",
                        "reason": "no_actual_source_receipt", "proof_class": "local_source_consumption", "delivery_proven": False})
                continue
            for occurrence, events in occurrences.items():
                initial, final = events[0], events[-1]
                if not any(_in_dates(event["occurred_at"], first, last, zone) for event in events):
                    continue
                result.append({**deepcopy(entry), "run_id": run["_id"], "occurrence": occurrence,
                    "state": final["state"] if final["state"] in TERMINAL else "unknown",
                    "consumed_frames": final["consumed_frames"], "started_at": initial["occurred_at"] if initial["state"] == "started" else None,
                    "terminal_at": final["occurred_at"] if final["state"] in TERMINAL else None,
                    "event_keys": [event["event_key"] for event in events], "output_id": manifest["output_id"],
                    "main_sink": final["main_sink"], "reason": final["reason"],
                    "proof_class": "local_source_consumption", "delivery_proven": False})
    result.sort(key=lambda row: (row["started_at"] or row["terminal_at"] or "", row["run_id"], row["entry_id"], row["occurrence"] or 0))
    return result


def _report_snapshot(store, principal, sid, station, filters):
    first, last, output = _report_filters(filters, sid)
    if filters["campaign_id"] is not None:
        api._campaign(store, sid, filters["campaign_id"])
    zone = ZoneInfo(station["timezone"])
    formal = asrun_reports.preview_asrun(store, principal, sid, filters["date_from"], filters["date_to"], output_id=output)
    snapshot = deepcopy(formal["snapshot"])
    campaign_id = filters["campaign_id"]
    if campaign_id:
        campaign_versions = {row["version_id"] for row in store.scan("spots") if row["campaign_id"] == campaign_id}
        snapshot["rows"] = [row for row in snapshot["rows"] if any(part.get("campaign_id") == campaign_id for part in row.get("advertising_placements", [])) or row.get("version_id") in campaign_versions]
        for row in snapshot["rows"]:
            if not any(part.get("campaign_id") == campaign_id for part in row.get("advertising_placements", [])):
                row["campaign_attribution"] = "version_match_only_not_contract_fulfillment"
        snapshot["ad_obligations"] = [row for row in snapshot["ad_obligations"] if row.get("campaign_id") == campaign_id]
        # The original all-campaign formal summary would otherwise mislead a
        # filtered snapshot. Keep only the filtered evidence rows, not totals.
        snapshot["summary"] = {"filtered_rows": len(snapshot["rows"])}
    planned = []
    for placement in store.scan("placements"):
        if placement.get("station_id") != sid or output is not None and placement.get("output_id") != output:
            continue
        spot = store.get("spots", placement["spot_id"])
        entry = store.get("plan_entries", placement["plan_entry_id"])
        if not spot or not entry or campaign_id is not None and spot["campaign_id"] != campaign_id or not _in_dates(entry["scheduled_at"], first, last, zone):
            continue
        version = store.get("versions", spot["version_id"])
        item = store.get("items", version["item_id"]) if version else None
        planned.append({"placement_id": placement["_id"], "entry_id": entry["_id"], "campaign_id": spot["campaign_id"],
            "spot_id": spot["_id"], "obligation_id": placement["obligation_id"], "plan_revision_id": entry["plan_revision_id"],
            "output_id": placement.get("output_id"), "scheduled_at": entry["scheduled_at"], "duration_ms": entry["planned_duration_ms"],
            "state": placement["state"], "title": item["title"] if item else "", "proof_class": "traffic_plan", "delivery_proven": False})
    planned.sort(key=lambda row: (row["scheduled_at"], row["placement_id"]))
    local = _local_rows(store, sid, filters, first, last, zone)
    counters = {"planned": len(planned), "formal_rows": len(snapshot["rows"]), "local_complete": 0,
        "local_interrupted": 0, "local_failed": 0, "local_notexecuted": 0, "local_unknown": 0, "delivery_proven": False}
    for row in local:
        counters["local_" + row["state"].removeprefix("local_")] += 1
    rows = [dict(row, record_class="planned") for row in planned]
    rows += [dict(row, record_class="local") for row in local]
    rows += [dict(row, record_class="formal", state=row["result"], entry_id=row["plan_entry_id"],
        proof_class="canonical_engine_and_output_observation", delivery_proven=False) for row in snapshot["rows"]]
    if campaign_id is None:
        rows += [dict(row, record_class="observation", state=row["kind"], started_at=row["occurred_at"],
            proof_class="external_output_activity" if row.get("observer_kind") == "external_monitor" else "engine_output_activity",
            reason="unlinked_output_activity_does_not_identify_advertisement", delivery_proven=False)
            for row in snapshot["unlinked_output_observations"]]
    return _bounded({"station_id": sid, "station_timezone": station["timezone"], "filters": filters, "as_of_at": _now(),
        "formal": {key: value for key, value in snapshot.items() if key != "rows"}, "formal_source_hash": formal["source_hash"],
        "rows": rows, "counters": counters, "proof_class": "separate_traffic_engine_observation_and_local_source", "delivery_proven": False})


def _report_view(row, offset=0, limit=500):
    offset = api._int(offset, "offset", 0, 2147483647)
    limit = api._int(limit, "limit", 1, 500)
    result = deepcopy(row)
    snapshot = result["config"]["snapshot"]
    rows = snapshot["rows"]
    result["config"]["snapshot"] = {key: value for key, value in snapshot.items() if key != "rows"}
    result["config"]["snapshot"]["rows"] = rows[offset:offset + limit]
    result.update(total=len(rows), offset=offset, limit=limit,
        next_offset=offset + limit if offset + limit < len(rows) else None,
        snapshot_hash=row["config"]["snapshot_hash"], snapshot_page_hash=_hash(result["config"]["snapshot"]),
        snapshot_scope="paged_view_of_immutable_snapshot", proof_class=snapshot["proof_class"])
    return _bounded(result)


def report_create(store, principal, station_id, date_from, date_to, idempotency_key, campaign_id=None, output_id=None):
    filters = {"date_from": date_from, "date_to": date_to, "campaign_id": campaign_id, "output_id": output_id}
    sid, station, sequence, receipt, digest, replay = _begin(store, principal, station_id, idempotency_key, "report-create", filters, "operate")
    if replay is not None:
        return report_get(store, principal, sid, replay["report_id"])
    snapshot = _report_snapshot(store, principal, sid, station, filters)
    config = {"schema_version": 1, "filters": filters, "snapshot": snapshot, "snapshot_hash": _hash(snapshot),
        "source_sequence": sequence, "created_at": _now()}
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    document = _put_doc(tx, principal, sid, "advert_report", "Reklam raporu " + date_from + " — " + date_to, config)
    _commit(store, tx, principal, sid, idempotency_key, "report-create", receipt, digest, {"report_id": document["_id"]})
    return _report_view(document)


@api._consistent_read
def report_get(store, principal, station_id, report_id, offset=0, limit=500):
    sid, _ = api._station(store, principal, station_id)
    row = _document(store, sid, "advert_report", report_id)
    if row["config"]["snapshot_hash"] != _hash(row["config"]["snapshot"]):
        raise Conflict("stored advertising report hash mismatch")
    return _report_view(row, offset, limit)


@api._consistent_read
def report_export(store, principal, station_id, report_id, format="json", offset=0, limit=500):
    view = report_get(store, principal, station_id, report_id, offset, limit)
    if format == "json":
        content = json.dumps(view, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False)
    elif format == "csv":
        stream = io.StringIO(newline="")
        writer = csv.writer(stream)
        fields = ["record_class", "proof_class", "delivery_proven", "run_id", "entry_id", "placement_id", "campaign_id",
            "spot_id", "output_id", "title", "state", "scheduled_at", "started_at", "terminal_at", "consumed_frames", "expected_frames", "reason"]
        writer.writerow(fields)
        for row in view["config"]["snapshot"]["rows"]:
            writer.writerow([asrun_reports._csv_cell(row.get(key, "")) for key in fields])
        content = stream.getvalue()
    else:
        raise StoreError("report export format must be json or csv")
    return _bounded({"format": format, "content": content, "filename": "radios-advert-" + view["_id"] + "." + format,
        "total": view["total"], "next_offset": view["next_offset"], "partial": view["next_offset"] is not None or offset > 0,
        "snapshot_hash": view["snapshot_hash"]})


def notifications_read(store, principal, station_id, notification_ids, idempotency_key):
    values = api._list(notification_ids, "notification_ids", 500)
    if len(values) != len(set(values)) or any(not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value) for value in values):
        raise StoreError("notification identities must be distinct SHA-256")
    sid, _, sequence, receipt, digest, replay = _begin(store, principal, station_id, idempotency_key, "notifications-read", {"notification_ids": values}, "view")
    if replay is not None:
        return replay
    current = [row for row in store.lookup("desktop_configurations", "station_kind", sid, "advert_notifications") if row["config"]["user_id"] == principal.user_id]
    if len(current) > 1:
        raise Conflict("notification reader has duplicate acknowledgement records")
    old = current[0] if current else None
    config = {"user_id": principal.user_id, "notification_ids": list(dict.fromkeys((old["config"]["notification_ids"] if old else []) + values))[-500:]}
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    document = _put_doc(tx, principal, sid, "advert_notifications", "Bildirimler " + principal.user_id, config, old)
    return _commit(store, tx, principal, sid, idempotency_key, "notifications-read", receipt, digest,
        {"document_id": document["_id"], "revision": document["_rev"], "read_count": len(config["notification_ids"])})
