"""Versioned radio hour clocks and guarded draft-plan materialization.

An assignment selects a published template for one station-local weekday/hour.
Preview exposes DST gaps, missing category choices and plan conflicts. Applying
the preview writes a whole hour in one transaction; it never rewrites a played
or approved plan and never silently replaces a previously generated hour.
"""

from __future__ import annotations

from datetime import date, datetime, time, timedelta, timezone
from hashlib import sha256
import json
from typing import Any, Mapping
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo

from .identity import Principal, require
from .planning import validate_entry, validate_plan
from .readiness import readiness_at
from .schedules import _local_instant
from .storage import Conflict, Store, StoreError


# The HTTP workflow routes call these functions with JSON arguments. Only the
# automatic scheduler can bind its computed selection evidence to a plan.
_AUTO_SELECTION_CAPABILITY = object()


def _id(value: Any, name: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def _day(value: Any, name: str) -> date:
    try:
        return value if isinstance(value, date) and not isinstance(value, datetime) else date.fromisoformat(value)
    except (TypeError, ValueError) as exc:
        raise StoreError(f"{name} must be YYYY-MM-DD") from exc


def _int(value: Any, name: str, lower: int, upper: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not lower <= value <= upper:
        raise StoreError(f"{name} must be an integer in {lower}..{upper}")
    return value


def _body(row: dict) -> dict:
    return {key: value for key, value in row.items() if key not in ("_id", "_rev")}


def _hash(value: Any) -> str:
    encoded = json.dumps(value, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    return sha256(encoded).hexdigest()


def _json_object(value: Any, name: str) -> dict:
    if not isinstance(value, Mapping) or any(not isinstance(k, str) for k in value):
        raise StoreError(f"{name} must be an object with string keys")
    try:
        return json.loads(json.dumps(dict(value), ensure_ascii=False, allow_nan=False))
    except (TypeError, ValueError) as exc:
        raise StoreError(f"{name} must be finite JSON data") from exc


def _template(store: Store, station_id: str, template_id: str) -> dict:
    row = store.get("clock_templates", _id(template_id, "template_id"))
    if row is None or row["station_id"] != station_id:
        raise StoreError("clock template is outside station")
    return row


def _version(store: Store, station_id: str, version_id: str) -> tuple[dict, dict]:
    version = store.get("clock_template_versions", _id(version_id, "version_id"))
    if version is None:
        raise StoreError("clock template version does not exist")
    return version, _template(store, station_id, version["template_id"])


def _slot_template(value: Any, slot_kind: str) -> dict:
    row = _json_object(value, "entry_template")
    forbidden = {"id", "_id", "_rev", "ordinal", "scheduled_at", "plan_revision_id",
                 "clock_assignment_id", "clock_slot_id", "clock_date", "clock_source_fingerprint"}
    if forbidden.intersection(row):
        raise StoreError("clock slot template cannot set identity, position, date or source tags")
    if "ad_break_id" in row or "target_end_at" in row or \
            isinstance(row.get("timing"), dict) and "target_end_at" in row["timing"]:
        raise StoreError("clock slot cannot embed plan-specific references or absolute end dates")
    if not isinstance(row.get("event_kind"), str):
        raise StoreError("clock slot needs event_kind")
    payload = row.get("payload", {})
    if not isinstance(payload, dict):
        raise StoreError("clock slot payload must be an object")
    if slot_kind == "category":
        if row["event_kind"] != "audio" or any(k in payload for k in ("version_id", "cart_id", "cut_id")):
            raise StoreError("category slot must be audio without a fixed audio target")
    return row


def create_clock_template(store: Store, principal: Principal, station_id: str, *,
                          code: str, name: str) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    if store.get("stations", station_id) is None:
        raise StoreError("station does not exist")
    if not isinstance(code, str) or not code.strip() or not isinstance(name, str) or not name.strip():
        raise StoreError("clock code and name must be nonempty")
    now = datetime.now(timezone.utc).isoformat()
    template_id, version_id = str(uuid4()), str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.put("clock_templates", template_id, {
        "station_id": station_id, "code": code.strip(), "name": name.strip(),
        "status": "active", "current_version_id": None, "draft_version_id": version_id,
        "created_by": principal.user_id, "created_at": now})
    tx.put("clock_template_versions", version_id, {
        "template_id": template_id, "version_no": 1, "status": "draft",
        "created_by": principal.user_id, "created_at": now})
    tx.commit()
    return {"template_id": template_id, "draft_version_id": version_id}


def fork_clock_version(store: Store, principal: Principal, station_id: str,
                       template_id: str, *, expected_template_revision: int) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    template = _template(store, station_id, template_id)
    if template["_rev"] != expected_template_revision:
        raise Conflict("clock template changed")
    if template.get("draft_version_id"):
        raise Conflict("clock template already has a draft")
    current_id = template.get("current_version_id")
    current = store.get("clock_template_versions", current_id) if current_id else None
    if current is None or current["status"] != "published":
        raise Conflict("clock template has no published version")
    new_id = str(uuid4())
    old_slots = store.lookup("clock_slots", "version", current_id)
    tx = store.begin(actor=principal.user_id)
    tx.put("clock_template_versions", new_id, {
        "template_id": template["_id"], "version_no": current["version_no"] + 1,
        "status": "draft", "based_on_id": current_id,
        "created_by": principal.user_id, "created_at": datetime.now(timezone.utc).isoformat()})
    for old in old_slots:
        tx.put("clock_slots", str(uuid4()), _body(old) | {"template_version_id": new_id})
    tx.put("clock_templates", template["_id"],
           _body(template) | {"draft_version_id": new_id},
           expected_revision=template["_rev"])
    tx.commit()
    return {"template_id": template["_id"], "draft_version_id": new_id,
            "version_no": current["version_no"] + 1, "slot_count": len(old_slots)}


def put_clock_slot(store: Store, principal: Principal, station_id: str,
                   version_id: str, *, ordinal: int, offset_ms: int,
                   entry_template: Mapping[str, Any], slot_kind: str = "fixed",
                   category_id: str | None = None, slot_id: str | None = None,
                   expected_slot_revision: int | None = None) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    version, template = _version(store, station_id, version_id)
    if version["status"] != "draft" or template.get("draft_version_id") != version["_id"]:
        raise Conflict("clock slots can only be edited in the current draft")
    ordinal = _int(ordinal, "ordinal", 0, 999)
    offset_ms = _int(offset_ms, "offset_ms", 0, 3_599_999)
    if slot_kind not in {"fixed", "category"}:
        raise StoreError("slot_kind must be fixed or category")
    category = None
    if slot_kind == "category":
        category = store.get("categories", _id(category_id, "category_id"))
        if category is None or category["station_id"] != station_id:
            raise StoreError("category does not belong to station")
    elif category_id is not None:
        raise StoreError("fixed slot cannot name a category")
    identifier = _id(slot_id, "slot_id") if slot_id else str(uuid4())
    old = store.get("clock_slots", identifier)
    if old is not None and (old["template_version_id"] != version["_id"] or
                            old["_rev"] != expected_slot_revision):
        raise Conflict("clock slot changed or belongs to another version")
    if old is None and expected_slot_revision is not None:
        raise Conflict("clock slot is absent at expected revision")
    row = {"template_version_id": version["_id"], "ordinal": ordinal,
           "offset_ms": offset_ms, "slot_kind": slot_kind,
           "category_id": category["_id"] if category else None,
           "entry_template": _slot_template(entry_template, slot_kind)}
    tx = store.begin(actor=principal.user_id)
    tx.expect("clock_template_versions", version["_id"], version["_rev"])
    tx.put("clock_slots", identifier, row,
           expected_revision=old["_rev"] if old else None)
    return tx.commit()[0]


def publish_clock_version(store: Store, principal: Principal, station_id: str,
                          version_id: str, *, expected_version_revision: int,
                          expected_template_revision: int) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "approve", station_id)
    version, template = _version(store, station_id, version_id)
    if version["_rev"] != expected_version_revision or version["status"] != "draft" or \
            template["_rev"] != expected_template_revision or \
            template.get("draft_version_id") != version["_id"]:
        raise Conflict("clock draft or template changed")
    sequence = store.sequence
    slots = store.lookup("clock_slots", "version", version["_id"])
    if not slots:
        raise Conflict("clock template has no slots")
    ordered = sorted(slots, key=lambda row: (row["ordinal"], row["_id"]))
    if any(a["offset_ms"] > b["offset_ms"] for a, b in zip(ordered, ordered[1:])):
        raise Conflict("clock slot ordinal reverses time")
    fingerprint = _hash([{
        "ordinal": row["ordinal"], "offset_ms": row["offset_ms"],
        "slot_kind": row["slot_kind"], "category_id": row.get("category_id"),
        "entry_template": row["entry_template"]} for row in ordered])
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    for slot in slots:
        tx.expect("clock_slots", slot["_id"], slot["_rev"])
    tx.put("clock_template_versions", version["_id"],
           _body(version) | {"status": "published", "fingerprint": fingerprint,
                             "published_by": principal.user_id, "published_at": now},
           expected_revision=version["_rev"])
    tx.put("clock_templates", template["_id"],
           _body(template) | {"current_version_id": version["_id"],
                              "draft_version_id": None},
           expected_revision=template["_rev"])
    tx.commit()
    return {"template_id": template["_id"], "published_version_id": version["_id"],
            "slot_count": len(slots), "fingerprint": fingerprint}


def _assignment(store: Store, station_id: str, assignment_id: str) -> dict:
    row = store.get("clock_assignments", _id(assignment_id, "assignment_id"))
    if row is None or row["station_id"] != station_id:
        raise StoreError("clock assignment is outside station")
    return row


def _date_overlap(first: dict, second: dict) -> bool:
    first_end = first.get("ends_on") or "9999-12-31"
    second_end = second.get("ends_on") or "9999-12-31"
    return first["starts_on"] <= second_end and second["starts_on"] <= first_end


def assign_clock(store: Store, principal: Principal, station_id: str, *,
                 template_version_id: str, weekday: int, local_hour: int,
                 starts_on: str | date, ends_on: str | date | None = None,
                 dst_fold: int = 0, assignment_id: str | None = None,
                 expected_revision: int | None = None,
                 service_key: str = "default") -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    from .broadcast_services import require_service, service_key as checked_service_key
    service_key = checked_service_key(service_key)
    require_service(store, station_id, service_key)
    version, template = _version(store, station_id, template_version_id)
    if version["status"] != "published" or template["status"] != "active":
        raise Conflict("only an active published clock can be assigned")
    weekday = _int(weekday, "weekday", 1, 7)
    local_hour = _int(local_hour, "local_hour", 0, 23)
    dst_fold = _int(dst_fold, "dst_fold", 0, 1)
    start = _day(starts_on, "starts_on")
    end = _day(ends_on, "ends_on") if ends_on is not None else None
    if end is not None and end < start:
        raise StoreError("assignment ends before it starts")
    identifier = _id(assignment_id, "assignment_id") if assignment_id else str(uuid4())
    old = store.get("clock_assignments", identifier)
    if old is not None and (old["station_id"] != station_id or old["_rev"] != expected_revision):
        raise Conflict("clock assignment changed or belongs to another station")
    if old is not None and old["service_key"] != service_key:
        raise Conflict("clock assignment cannot move to another broadcast service")
    if old is None and expected_revision is not None:
        raise Conflict("clock assignment is absent at expected revision")
    row = {"station_id": station_id, "service_key": service_key,
           "template_version_id": version["_id"],
           "weekday": weekday, "local_hour": local_hour, "starts_on": start.isoformat(),
           "ends_on": end.isoformat() if end else None, "dst_fold": dst_fold,
           "status": "active"}
    sequence = store.sequence
    for other in store.lookup("clock_assignments", "station_weekday_hour",
                              station_id, service_key, weekday, local_hour):
        if other["_id"] != identifier and other["status"] == "active" and _date_overlap(row, other):
            raise Conflict("another active clock assignment covers this weekday/hour")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("clock_assignments", identifier, row,
           expected_revision=old["_rev"] if old else None)
    return tx.commit()[0]


def set_clock_exception(store: Store, principal: Principal, station_id: str,
                        assignment_id: str, service_date: str | date, *,
                        action: str, replacement_version_id: str | None = None,
                        expected_revision: int | None = None) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    assignment = _assignment(store, station_id, assignment_id)
    day = _day(service_date, "service_date")
    if day.isoweekday() != assignment["weekday"] or day.isoformat() < assignment["starts_on"] or \
            assignment.get("ends_on") and day.isoformat() > assignment["ends_on"]:
        raise StoreError("date is outside the assignment")
    if action not in {"skip", "replace"}:
        raise StoreError("action must be skip or replace")
    replacement = None
    if action == "replace":
        replacement, _ = _version(store, station_id, _id(replacement_version_id, "replacement_version_id"))
        if replacement["status"] != "published":
            raise Conflict("replacement clock must be published")
    elif replacement_version_id is not None:
        raise StoreError("skip action cannot have a replacement")
    prior = store.lookup("clock_exceptions", "assignment_date",
                         assignment["_id"], day.isoformat())
    old = prior[0] if prior else None
    if old is not None and old["_rev"] != expected_revision:
        raise Conflict("clock exception changed")
    if old is None and expected_revision is not None:
        raise Conflict("clock exception is absent at expected revision")
    identifier = old["_id"] if old else str(uuid4())
    return store.put("clock_exceptions", identifier, {
        "assignment_id": assignment["_id"], "service_date": day.isoformat(),
        "action": action, "replacement_version_id": replacement["_id"] if replacement else None,
        "status": "active", "changed_by": principal.user_id,
        "changed_at": datetime.now(timezone.utc).isoformat()},
        expected_revision=old["_rev"] if old else None, actor=principal.user_id)


def _current_assignment(store: Store, station_id: str, day: date, hour: int,
                        service_key: str = "default") -> dict | None:
    rows = [row for row in store.lookup("clock_assignments", "station_weekday_hour",
                                      station_id, service_key, day.isoweekday(), hour)
            if row["status"] == "active" and row["starts_on"] <= day.isoformat() and
            (row.get("ends_on") is None or day.isoformat() <= row["ends_on"])]
    if len(rows) > 1:
        raise Conflict("clock assignments overlap")
    return rows[0] if rows else None


def _category_version(store: Store, station_id: str, category_id: str,
                      version_id: str, instant: datetime,
                      planned_duration_ms: int | None, *,
                      territory: str = "*", output_id: str = "*") -> None:
    version = store.get("versions", version_id)
    if version is None:
        raise StoreError("selected category version does not exist")
    item = store.get("items", version["item_id"])
    rows = store.lookup("station_items", "station_item", station_id, item["_id"])
    association = rows[0] if rows else None
    if association is None or association.get("category_id") != category_id:
        raise StoreError("selected version is not in the clock slot category")
    duration = planned_duration_ms if isinstance(planned_duration_ms, int) and \
        not isinstance(planned_duration_ms, bool) and planned_duration_ms > 0 else 1
    slot_end = instant + timedelta(milliseconds=duration)
    ready = readiness_at(store, station_id, version_id, slot_start=instant,
                         slot_end=slot_end, require_endpoint_copy=False,
                         territory=territory, output_id=output_id)
    if not ready["ready"]:
        raise Conflict("category choice is not ready: " + ", ".join(ready["blocked_reasons"]))


def _slot_payload_scope(template: Mapping[str, Any],
                        territory: str, output_id: str) -> tuple[dict, str, str]:
    """Freeze inherited rights scope in each audio entry's own payload."""
    payload = dict(template.get("payload", {}))
    if template.get("event_kind") != "audio":
        return payload, territory, output_id
    if "territory" not in payload and "region_id" not in payload and territory != "*":
        payload["territory"] = territory
    if "output_id" not in payload and output_id != "*":
        payload["output_id"] = output_id
    from .readiness import execution_scope
    slot_territory, slot_output = execution_scope(payload)
    return payload, slot_territory, slot_output


def preview_clock_hour(store: Store, principal: Principal, station_id: str,
                       service_date: str | date, local_hour: int, *,
                       resolved_versions: Mapping[str, str] | None = None,
                       ordinal_base: int | None = None,
                       selection_provenance: Mapping[str, Any] | None = None,
                       territory: str = "*", output_id: str = "*",
                       service_key: str = "default",
                       _selection_capability: object | None = None) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "view", station_id)
    from .broadcast_services import (check_entry_scope, require_service,
                                     service_key as checked_service_key)
    service_key = checked_service_key(service_key)
    binding = require_service(store, station_id, service_key)
    if binding is not None:
        if output_id not in {"*", binding["output_id"]}:
            raise Conflict("clock output differs from broadcast service output")
        output_id = binding["output_id"]
    day = _day(service_date, "service_date")
    hour = _int(local_hour, "local_hour", 0, 23)
    base = hour * 1000 if ordinal_base is None else _int(ordinal_base, "ordinal_base", 0, 2_000_000_000)
    choices = _json_object(resolved_versions or {}, "resolved_versions")
    if selection_provenance is not None and _selection_capability is not _AUTO_SELECTION_CAPABILITY:
        raise StoreError("automatic selection provenance must come from the scheduler")
    provenance = (_json_object(selection_provenance, "selection_provenance")
                  if selection_provenance is not None else None)
    station = store.get("stations", station_id)
    if station is None:
        raise StoreError("station does not exist")
    sequence = store.sequence
    assignment = _current_assignment(store, station_id, day, hour, service_key)
    if assignment is None:
        return {"status": "no_assignment", "service_date": day.isoformat(),
                "local_hour": hour, "entries": [], "sequence": sequence}
    exception_rows = store.lookup("clock_exceptions", "assignment_date",
                                  assignment["_id"], day.isoformat())
    exception = exception_rows[0] if exception_rows and exception_rows[0]["status"] == "active" else None
    if exception and exception["action"] == "skip":
        return {"status": "skipped", "reason": "exception", "assignment_id": assignment["_id"],
                "service_date": day.isoformat(), "local_hour": hour,
                "entries": [], "sequence": sequence}
    version_id = exception["replacement_version_id"] if exception and exception["action"] == "replace" else assignment["template_version_id"]
    version, _ = _version(store, station_id, version_id)
    if version["status"] != "published":
        raise Conflict("assigned clock version is no longer published")
    zone = ZoneInfo(station["timezone"])
    instant, dst_note = _local_instant(day, time(hour, 0).isoformat(), zone,
                                       assignment["dst_fold"])
    if instant is None:
        return {"status": "skipped", "reason": "dst_gap", "assignment_id": assignment["_id"],
                "service_date": day.isoformat(), "local_hour": hour,
                "entries": [], "sequence": sequence}
    slots = sorted(store.lookup("clock_slots", "version", version_id),
                   key=lambda row: (row["ordinal"], row["_id"]))
    if not slots:
        raise Conflict("published clock has no slots")
    category_ids = {row["_id"] for row in slots if row["slot_kind"] == "category"}
    if set(choices) - category_ids:
        raise StoreError("resolved_versions contains an unknown or fixed slot")
    entries = []
    unresolved = []
    errors = []
    for slot in slots:
        at = instant + timedelta(milliseconds=slot["offset_ms"])
        payload, slot_territory, slot_output = _slot_payload_scope(
            slot["entry_template"], territory, output_id)
        if slot["slot_kind"] == "category":
            selected = choices.get(slot["_id"])
            if selected is None:
                unresolved.append(slot["_id"])
                continue
            selected = _id(selected, "resolved version")
            try:
                _category_version(store, station_id, slot["category_id"], selected,
                                  at, slot["entry_template"].get("planned_duration_ms"),
                                  territory=slot_territory, output_id=slot_output)
            except StoreError as exc:
                errors.append({"slot_id": slot["_id"], "code": "category_choice_invalid",
                               "detail": str(exc)})
                continue
            payload["version_id"] = selected
        candidate = dict(slot["entry_template"]) | {
            "payload": payload, "ordinal": base + slot["ordinal"],
            "scheduled_at": at.isoformat(), "state": "planned"}
        validation = validate_entry(candidate, station["timezone"], day)
        if not validation["valid"]:
            errors.extend({"slot_id": slot["_id"], **issue} for issue in validation["errors"])
            continue
        normalized = {key: value for key, value in validation["normalized"].items()
                      if key not in ("id", "nominal_end_at", "requires_resolution")}
        check_entry_scope(normalized, binding)
        entries.append({"slot_id": slot["_id"], "entry": normalized,
                        "warnings": validation["warnings"]})
    plan_rows = store.lookup("plans", "station_date_service", station_id,
                             day.isoformat(), service_key)
    plan = plan_rows[0] if plan_rows else None
    material_rows = store.lookup("clock_materializations", "assignment_date",
                                 assignment["_id"], day.isoformat())
    material = material_rows[0] if material_rows else None
    if plan is not None and plan.get("draft_revision_id"):
        draft = store.get("plan_revisions", plan["draft_revision_id"])
        prior_entries = store.lookup("plan_entries", "revision", draft["_id"])
    else:
        draft, prior_entries = None, []
    if not unresolved and not errors:
        replaced_ids = set(material["entry_ids"]) if material and material.get("plan_revision_id") == \
            (draft or {}).get("_id") else set()
        proposed = [_body(row) | {"id": row["_id"]} for row in prior_entries
                    if row["_id"] not in replaced_ids]
        proposed.extend(row["entry"] | {"id": row["slot_id"]} for row in entries)
        validation = validate_plan(proposed, station["timezone"], day)
        errors.extend(validation["conflicts"])
    else:
        validation = {"warnings": []}
    source = {"assignment_id": assignment["_id"], "assignment_revision": assignment["_rev"],
              "service_key": service_key,
              "exception_id": exception["_id"] if exception else None,
              "exception_revision": exception["_rev"] if exception else None,
              "version_id": version["_id"], "version_fingerprint": version["fingerprint"],
              "date": day.isoformat(), "hour": hour, "ordinal_base": base,
              "choices": choices, "entries": entries}
    if provenance is not None:
        source["selection_provenance"] = provenance
    if territory != "*" or output_id != "*":
        source["rights_scope"] = {"territory": territory, "output_id": output_id}
    fingerprint = _hash(source)
    result = {"status": "ready" if not unresolved and not errors else "needs_resolution",
              "station_id": station_id, "service_date": day.isoformat(),
              "service_key": service_key, "local_hour": hour,
              "assignment_id": assignment["_id"], "template_version_id": version["_id"],
              "exception_id": exception["_id"] if exception else None,
              "dst_note": dst_note, "ordinal_base": base,
              "source_fingerprint": fingerprint, "entries": entries,
              "selection_provenance": provenance,
              "unresolved_slot_ids": unresolved, "errors": errors,
              "warnings": validation["warnings"], "sequence": sequence,
              "plan_id": plan["_id"] if plan else None,
              "draft_revision_id": draft["_id"] if draft else None}
    if store.sequence != sequence:
        raise Conflict("clock changed during preview")
    return result


def materialize_clock_hour(store: Store, principal: Principal, station_id: str,
                           service_date: str | date, local_hour: int, *,
                           resolved_versions: Mapping[str, str] | None = None,
                           ordinal_base: int | None = None,
                           selection_provenance: Mapping[str, Any] | None = None,
                           territory: str = "*", output_id: str = "*",
                           service_key: str = "default",
                           _selection_capability: object | None = None,
                           expected_sequence: int) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    if selection_provenance is not None and _selection_capability is not _AUTO_SELECTION_CAPABILITY:
        raise StoreError("automatic selection provenance must come from the scheduler")
    preview = preview_clock_hour(store, principal, station_id, service_date,
                                 local_hour, resolved_versions=resolved_versions,
                                 ordinal_base=ordinal_base,
                                 selection_provenance=selection_provenance,
                                 territory=territory, output_id=output_id,
                                 service_key=service_key,
                                 _selection_capability=_selection_capability)
    if preview["sequence"] != expected_sequence:
        raise Conflict("database changed since clock preview")
    if preview["status"] != "ready":
        raise Conflict("clock hour is not ready to materialize")
    assignment_id = preview["assignment_id"]
    day = preview["service_date"]
    material_rows = store.lookup("clock_materializations", "assignment_date", assignment_id, day)
    prior = material_rows[0] if material_rows else None
    plan_id = preview["plan_id"]
    draft_id = preview["draft_revision_id"]
    if prior is not None:
        if prior["source_fingerprint"] != preview["source_fingerprint"]:
            raise Conflict("clock hour already materialized from a different source; edit a draft explicitly")
        if prior["plan_revision_id"] != draft_id and not (
                draft_id is None and plan_id and
                store.get("plans", plan_id).get("current_revision_id") == prior["plan_revision_id"]):
            raise Conflict("clock hour belongs to another plan revision")
        for entry_id, digest in zip(prior["entry_ids"], prior["entry_fingerprints"]):
            row = store.get("plan_entries", entry_id)
            if row is None or _hash(_body(row)) != digest:
                raise Conflict("materialized clock entry was edited")
        return {"status": "materialized", "replayed": True,
                "plan_id": prior["plan_id"], "plan_revision_id": prior["plan_revision_id"],
                "entry_ids": prior["entry_ids"], "materialization_id": prior["_id"]}
    plan = store.get("plans", plan_id) if plan_id else None
    if plan is not None and not draft_id:
        raise Conflict("date plan is approved; fork a draft explicitly")
    if plan is not None:
        draft = store.get("plan_revisions", draft_id)
        if draft is None or draft["status"] != "draft" or plan["draft_revision_id"] != draft_id:
            raise Conflict("plan draft changed")
    else:
        plan_id, draft_id = str(uuid4()), str(uuid4())
    entry_ids = [str(uuid4()) for _ in preview["entries"]]
    bodies = []
    for entry_id, item in zip(entry_ids, preview["entries"]):
        bodies.append(item["entry"] | {
            "plan_revision_id": draft_id, "clock_assignment_id": assignment_id,
            "clock_slot_id": item["slot_id"], "clock_date": day,
            "clock_source_fingerprint": preview["source_fingerprint"]})
    now = datetime.now(timezone.utc).isoformat()
    material_id = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(expected_sequence)
    if plan is None:
        tx.put("plans", plan_id, {"station_id": station_id, "service_date": day,
                                  "service_key": preview["service_key"],
                                  "state": "draft", "current_revision_id": None,
                                  "draft_revision_id": draft_id})
        tx.put("plan_revisions", draft_id, {"plan_id": plan_id, "revision_no": 1,
                                            "status": "draft", "created_by": principal.user_id,
                                            "created_at": now})
    else:
        tx.put("plans", plan_id, _body(plan), expected_revision=plan["_rev"])
        from .production import invalidate_production_for_revision
        invalidate_production_for_revision(tx, store, draft_id, "clock_hour_materialized")
    for entry_id, body in zip(entry_ids, bodies):
        tx.put("plan_entries", entry_id, body)
    material_body = {
        "assignment_id": assignment_id, "service_date": day,
        "station_id": station_id, "service_key": preview["service_key"],
        "local_hour": preview["local_hour"],
        "template_version_id": preview["template_version_id"],
        "source_fingerprint": preview["source_fingerprint"],
        "plan_id": plan_id, "plan_revision_id": draft_id,
        "entry_ids": entry_ids, "entry_fingerprints": [_hash(body) for body in bodies],
        "created_by": principal.user_id, "created_at": now}
    if preview["selection_provenance"] is not None:
        material_body["selection_provenance"] = preview["selection_provenance"]
    tx.put("clock_materializations", material_id, material_body)
    tx.commit()
    return {"status": "materialized", "replayed": False, "plan_id": plan_id,
            "plan_revision_id": draft_id, "entry_ids": entry_ids,
            "materialization_id": material_id, "warnings": preview["warnings"]}
