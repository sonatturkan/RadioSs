"""Reviewed native programme drafts use the existing formal plan contracts.

Draft submission, content approval, plan approval, local native execution and
endpoint distribution are separate operations. No content/rights permission is
inferred from a verified local file or from the programme author's authority.
"""
from copy import deepcopy
from datetime import datetime, timedelta, timezone
import hashlib
import json
from uuid import uuid4
from zoneinfo import ZoneInfo

from . import desktop_api as api, desktop_media, desktop_playout, plans, playout
from .broadcast_services import check_entry_scope, execution_entry, require_service, resolve_execution_binding
from .catalog import NotFound
from .planning import validate_entry
from .readiness import execution_scope, readiness_at
from .snapshot_contract import validate_snapshot_contract
from .storage import Conflict, StoreError

MAX_NATIVE_SEQUENCE = 500


def _body(row):
    return {key: deepcopy(value) for key, value in row.items() if key not in {"_id", "_rev"}}


def _digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                    separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def _native_start(day, clock):
    try:
        return datetime.fromisoformat(day + "T" + clock + ":00+00:00")
    except ValueError as exc:
        raise StoreError("every formal event needs a valid native UTC start time") from exc


def _document(store, sid, document_id):
    row = store.get("desktop_configurations", api._id(document_id, "document_id"))
    if not row or row["station_id"] != sid or row["kind"] != "programme" or row["status"] != "active":
        raise NotFound("native programme document is absent or outside station")
    return row


def _source(document):
    return {"document_id": document["_id"], "document_revision": document["_rev"],
            "config_sha256": _digest(document["config"])}


def _current(store, sid, document, service_key):
    require_service(store, sid, service_key)
    rows = store.lookup("plans", "station_date_service", sid, document["config"]["date"], service_key)
    if not rows:
        return None
    plan = rows[0]
    if plan.get("native_programme_document_id") != document["_id"]:
        raise Conflict("this service/date already has a different plan; select its existing owner")
    return plan


def _checked(store, principal, station_id, document_id, service_key, permission="view"):
    sid, station = api._station(store, principal, station_id, permission)
    document = _document(store, sid, document_id)
    plan = _current(store, sid, document, service_key)
    return sid, station, document, plan


def _revision(store, plan):
    if not plan:
        return None
    return store.get("plan_revisions", plan.get("draft_revision_id") or plan.get("current_revision_id"))


def _status(store, principal, sid, document, plan):
    revision = _revision(store, plan)
    entries, validation = plans._approved_entries(store, plan, revision["_id"]) if revision else ([], None)
    blockers = []
    if revision:
        try:
            binding = resolve_execution_binding(store, sid, plan["service_key"], entries)
        except StoreError as exc:
            binding = None
            blockers.append({"entry_id": None, "code": "execution_destination", "detail": str(exc)})
        if validation and (not validation["valid"] or not validation["approvable"]):
            for issue in validation.get("errors", []) + validation.get("conflicts", []):
                blockers.append(issue)
        for row in entries:
            if row["event_kind"] != "audio" or row["state"] != "planned":
                continue
            try:
                resolved = execution_entry(row, binding) if binding else row
                territory, output = execution_scope(resolved["payload"])
                start = datetime.fromisoformat(row["scheduled_at"].replace("Z", "+00:00"))
                ready = readiness_at(store, sid, row["payload"]["version_id"], slot_start=start,
                    slot_end=start + timedelta(milliseconds=row["planned_duration_ms"]),
                    require_endpoint_copy=False, territory=territory, output_id=output)
                blockers.extend({"entry_id": row["_id"], "code": code, "detail": code} for code in ready["blocked_reasons"])
            except StoreError as exc:
                blockers.append({"entry_id": row["_id"], "code": "readiness", "detail": str(exc)})
    source_matches = bool(revision and revision.get("native_source") == _source(document))
    if revision and not source_matches:
        blockers.append({"entry_id": None, "code": "native_source_changed", "detail": "Saved native programme changed; submit the latest draft before approval."})
    review_entries = []
    for row in entries:
        version = store.get("versions", row["payload"].get("version_id", "")) if row["event_kind"] == "audio" else None
        item = store.get("items", version["item_id"]) if version else None
        review_entries.append({"entry_id": row["_id"], "ordinal": row["ordinal"], "title": item["title"] if item else row["event_kind"],
            "scheduled_at": row["scheduled_at"], "duration_ms": row["planned_duration_ms"], "state": row["state"],
            "version_id": version["_id"] if version else None, "version_status": version["status"] if version else None})
    return {"document_id": document["_id"], "document_revision": document["_rev"],
            "source": _source(document), "plan": plan, "revision": revision, "entries": entries,
            "review_entries": review_entries,
            "validation": validation, "blockers": blockers, "source_matches": source_matches,
            "sequence": store.sequence, "scope": "formal_plan", "delivery_proven": False}


@api._consistent_read
def overview(store, principal, station_id, document_id, service_key="default"):
    sid, _, document, plan = _checked(store, principal, station_id, document_id, service_key)
    return _status(store, principal, sid, document, plan)


@api._consistent_read
def prerequisites(store, principal, station_id, document_id):
    sid, station = api._station(store, principal, station_id)
    document = _document(store, sid, document_id)
    rows = []
    seen = set()
    for event in document["config"]["events"]:
        if not api.MEDIA_BINDING_KEYS.issubset(event):
            continue
        api._media_binding({key: event[key] for key in api.MEDIA_BINDING_KEYS}, sid, store._state, persisted=True)
        version = store.get("versions", event["version_id"])
        if version["_id"] in seen:
            continue
        seen.add(version["_id"])
        item = store.get("items", version["item_id"])
        association = store.lookup("station_items", "station_item", sid, item["_id"])[0]
        starts = _native_start(document["config"]["date"], event["start_time"])
        ends = starts + timedelta(milliseconds=event["duration_ms"])
        rows.append({"version": version, "item": item, "association": association,
            "starts_at": starts.astimezone(timezone.utc).isoformat(), "ends_at": ends.astimezone(timezone.utc).isoformat(),
            "rights": [grant for grant in store.lookup("rights_grants", "version", version["_id"]) if grant["station_id"] == sid]})
    return {"station_id": sid, "station": station, "document_revision": document["_rev"], "records": rows,
            "outputs": [row for row in store.lookup("station_outputs", "station", sid) if row["status"] == "active"],
            "sequence": store.sequence}


def _entries(store, sid, station, document, service_key):
    config = api.validate_configuration("programme", document["config"], sid, store._state, persisted=True)
    if "timing" in config:
        raise StoreError("native cut/finish timing is not supported by formal submission; use native scheduled playback or an explicit formal endpoint plan")
    if not config["events"]:
        raise StoreError("formal programme needs at least one verified audio event")
    binding = require_service(store, sid, service_key)
    preparation = desktop_playout._Preparation(store, sid)
    result = []
    for ordinal, event in enumerate(config["events"]):
        if event.get("start_policy", "sequential") != "sequential" or event.get("max_wait_ms", 0):
            raise StoreError("native event cut/finish or bounded wait cannot be silently converted by formal submission")
        track = preparation.track(event, ordinal)
        if not track["eligible"]:
            raise Conflict("programme event %s is unavailable: %s" % (event["id"], ", ".join(track["reasons"])))
        detail = track["detail"]
        duration = detail["version"]["duration_ms"]
        markers = detail.get("markers")
        normal_markers = {"cue_in_ms": 0, "out_ms": duration, "intro_ms": 0, "fade_start_ms": duration,
                          "fade_end_ms": duration, "auto_next_ms": duration, "overlap_ms": 0, "automatic": False}
        if markers and any(markers.get(key) != value for key, value in normal_markers.items()):
            raise StoreError("native marker draft changes playback; use native draft playback or explicitly approved canonical cues before formal submission")
        scheduled = _native_start(config["date"], event["start_time"])
        if scheduled.astimezone(ZoneInfo(station["timezone"])).date().isoformat() != config["date"]:
            raise StoreError("native UTC slot falls on a different station service date; split the programme dates before formal submission")
        candidate = {"id": str(uuid4()), "ordinal": ordinal, "event_kind": "audio", "state": "planned",
            "scheduled_at": scheduled, "duration_kind": "fixed", "planned_duration_ms": duration,
            "timing": {"mode": "absolute", "window_ms": 0, "late_policy": "wait", "target_end_at": None},
            "transition": {"mode": "sequential", "overlap_ms": 0}, "ad_break_id": None,
            "payload": {"version_id": detail["version"]["_id"], "item_id": detail["media"]["item_id"],
                        "native_event_id": event["id"]}}
        checked = validate_entry(candidate, station["timezone"], config["date"])
        if not checked["valid"]:
            raise StoreError("invalid formal event: " + ", ".join(issue["code"] for issue in checked["errors"]))
        normalized = checked["normalized"]
        body = {key: value for key, value in normalized.items() if key not in {"id", "nominal_end_at", "requires_resolution"}}
        check_entry_scope(body, binding)
        result.append((candidate["id"], body))
    return result


def submit(store, principal, station_id, document_id, expected_document_revision, idempotency_key,
           service_key="default", expected_plan_revision=None):
    sequence = store.sequence
    sid, station, document, plan = _checked(store, principal, station_id, document_id, service_key, "edit")
    payload = {"document_id": document_id, "expected_document_revision": expected_document_revision,
               "service_key": service_key, "expected_plan_revision": expected_plan_revision}
    receipt, digest, replay = api.operation_replay(store, principal, sid, idempotency_key, "desktop-programme/submit", payload)
    if replay is not None:
        return replay
    if type(expected_document_revision) is not int or document["_rev"] != expected_document_revision:
        raise Conflict("saved native programme changed; reload before submitting")
    if plan and (type(expected_plan_revision) is not int or plan["_rev"] != expected_plan_revision):
        raise Conflict("formal plan changed; reload its current revision")
    if not plan and expected_plan_revision is not None:
        raise Conflict("formal plan does not exist at the requested revision")
    if plan and not plan.get("draft_revision_id"):
        raise Conflict("approved plan is immutable; explicitly fork a new draft first")
    entries = _entries(store, sid, station, document, service_key)
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id); tx.expect_sequence(sequence)
    plan_id = plan["_id"] if plan else str(uuid4())
    revision = _revision(store, plan)
    revision_id = revision["_id"] if revision else str(uuid4())
    old_entries = {}
    if revision:
        if revision["status"] != "draft":
            raise Conflict("only an unchanged draft can accept native edits")
        from .production import invalidate_production_for_revision
        invalidate_production_for_revision(tx, store, revision_id, "native_programme_resubmitted")
        old_entries = {entry["ordinal"]: entry for entry in store.lookup("plan_entries", "revision", revision_id)}
    for entry_id, body in entries:
        previous = old_entries.pop(body["ordinal"], None)
        tx.put("plan_entries", previous["_id"] if previous else entry_id,
               body | {"plan_revision_id": revision_id}, expected_revision=previous["_rev"] if previous else None)
    # The formal engine retains removed draft entries as cancelled history.
    for entry in old_entries.values():
        tx.put("plan_entries", entry["_id"], _body(entry) | {"state": "cancelled"}, expected_revision=entry["_rev"])
    revbody = _body(revision) if revision else {"plan_id": plan_id, "revision_no": 1, "status": "draft", "created_by": principal.user_id, "created_at": now}
    revbody["native_source"] = _source(document)
    tx.put("plan_revisions", revision_id, revbody, expected_revision=revision["_rev"] if revision else None)
    planbody = _body(plan) if plan else {"station_id": sid, "service_date": document["config"]["date"], "service_key": service_key, "state": "draft", "current_revision_id": None, "draft_revision_id": revision_id, "native_programme_document_id": document["_id"]}
    tx.put("plans", plan_id, planbody, expected_revision=plan["_rev"] if plan else None)
    result = {"plan_id": plan_id, "revision_id": revision_id, "plan_revision": (plan["_rev"] if plan else 0) + 1, "source": _source(document), "scope": "formal_draft", "delivery_proven": False}
    api.add_operation(tx, principal, sid, receipt, idempotency_key, "desktop-programme/submit", digest, result)
    tx.commit()
    return result


def _guard(store, principal, station_id, document_id, plan_id, revision_id, expected_plan_revision, permission):
    sid, _ = api._station(store, principal, station_id, permission)
    document = _document(store, sid, document_id)
    plan = plans._plan(store, sid, plan_id)
    if plan.get("native_programme_document_id") != document["_id"] or type(expected_plan_revision) is not int or plan["_rev"] != expected_plan_revision:
        raise Conflict("formal programme identity/revision changed")
    revision = store.get("plan_revisions", api._id(revision_id, "revision_id"))
    if not revision or revision["plan_id"] != plan["_id"]:
        raise Conflict("programme revision belongs to a different plan")
    return sid, document, plan, revision


def approve(store, principal, station_id, document_id, plan_id, revision_id, expected_plan_revision):
    sid, document, _, revision = _guard(store, principal, station_id, document_id, plan_id, revision_id, expected_plan_revision, "approve")
    if revision.get("native_source") != _source(document):
        raise Conflict("saved native programme changed; submit it before approval")
    return plans.approve_plan(store, principal, sid, plan_id, revision_id, expected_plan_revision=expected_plan_revision)


def fork(store, principal, station_id, document_id, plan_id, revision_id, expected_plan_revision):
    sid, _, plan, _ = _guard(store, principal, station_id, document_id, plan_id, revision_id, expected_plan_revision, "edit")
    if plan.get("current_revision_id") != revision_id:
        raise Conflict("only the current approved revision can be forked")
    return plans.fork_draft(store, principal, sid, plan_id, expected_plan_revision=expected_plan_revision)


@api._consistent_read
def prepare(store, principal, station_id, document_id, plan_id, revision_id, expected_plan_revision, endpoint_id):
    sid, document, _, revision = _guard(store, principal, station_id, document_id, plan_id, revision_id, expected_plan_revision, "activate")
    api._station(store, principal, sid, "operate")
    if revision.get("native_source") != _source(document):
        raise Conflict("saved native programme changed; fork, submit and approve it before local playback")
    snapshot = plans.build_snapshot(store, principal, sid, endpoint_id, revision_id)
    if len(snapshot["entries"]) > MAX_NATIVE_SEQUENCE:
        raise Conflict("native local approved playback supports at most 500 entries; use the formal endpoint adapter for this plan")
    validate_snapshot_contract(snapshot)
    if snapshot["production"]["placements"]:
        raise Conflict("native formal loader requires an audio-only plan; production placement needs its execution adapter")
    preparation = desktop_playout._Preparation(store, sid)
    tracks = []
    for entry in snapshot["entries"]:
        # Reject meanings this native runner cannot execute rather than omit them.
        if (entry["entry_kind"] != "audio" or entry["transition"] != {"mode": "sequential", "overlap_ms": 0}
                or entry["timing"]["mode"] != "absolute" or entry["timing"]["late_policy"] != "wait"
                or entry["timing"].get("target_end_at") is not None or entry.get("ad_break_id") is not None):
            raise Conflict("native formal loader cannot preserve this event/timing/transition; use its endpoint adapter")
        libraries = [row for row in store.lookup("desktop_media_library", "station", sid)
                     if row["version_id"] == entry["version_id"] and row["media_file_id"] == entry["media_file_id"] and row["status"] == "active"]
        if len(libraries) != 1:
            raise Conflict("approved version needs exactly one active native managed-media binding")
        library = libraries[0]; markers = entry["playback_markers"]
        if markers["fade_in_ms"] or markers["fade_out_ms"]:
            raise Conflict("native formal sequential loader cannot preserve frozen fade envelopes; use the endpoint adapter")
        event = {**library, "id": entry["plan_entry_id"], "media_id": library["_id"],
                 "media_sha256": markers["media_sha256"], "duration_ms": entry["planned_duration_ms"]}
        event.pop("_id", None)
        track = preparation.track(event, entry["ordinal"])
        if not track["eligible"]:
            raise Conflict("approved source unavailable: " + ", ".join(track["reasons"]))
        # This adapter's scope is a local sequential queue. Absolute slot
        # timing is kept in the immutable snapshot; the native load is an
        # explicit operator start, separate from endpoint scheduled execution.
        track["detail"]["markers"] = {**markers, "out_ms": markers["cue_out_ms"],
            "fade_start_ms": markers["cue_out_ms"] - markers["fade_out_ms"], "fade_end_ms": markers["cue_out_ms"],
            "auto_next_ms": markers["segue_out_ms"], "overlap_ms": 0, "automatic": False}
        tracks.append(track)
    if not tracks:
        raise Conflict("approved plan has no playable audio entries")
    result = {"contract_version": 1, "station_id": sid, "kind": "programme", "tracks": tracks,
        "config": {"events": [], "title": document["config"]["title"]}, "sequence": store.sequence,
        "formal_snapshot": snapshot, "scope": "local_approved_sequence", "delivery_proven": False}
    if len(json.dumps(result, ensure_ascii=False).encode()) > desktop_playout.MAX_RESPONSE_BYTES:
        raise StoreError("native formal programme response exceeds byte budget")
    return result


def distribute(store, principal, station_id, document_id, plan_id, revision_id, expected_plan_revision, endpoint_id):
    sid, _, _, _ = _guard(store, principal, station_id, document_id, plan_id, revision_id, expected_plan_revision, "activate")
    snapshot = plans.build_snapshot(store, principal, sid, endpoint_id, revision_id)
    result = playout.prepare_distribution(store, principal, sid, endpoint_id, revision_id, snapshot)
    return {"distribution": result, "scope": "endpoint_distribution_queued", "delivery_proven": False}
