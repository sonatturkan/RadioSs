"""Recurring radio schedule rules with explicit occurrence and series edits.

Rules produce station-local dates and wall times. A skipped daylight-saving gap
is visible in previews; ambiguous hours use the stored earlier/later choice.
Projection never changes a plan. Materialization writes only a current draft
revision and records its source, so approved plan revisions stay untouched.
"""

from __future__ import annotations

from datetime import date, datetime, time, timedelta, timezone
from hashlib import sha256
import json
from calendar import monthrange
from typing import Any, Mapping
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo

from .identity import Principal, require
from .planning import validate_entry, validate_plan
from .storage import Conflict, Store, StoreError


_FREQUENCIES = {"weekly", "biweekly", "monthly_nth", "monthly_last"}
_MAX_PREVIEW_DAYS = 3660


def _id(value: Any, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _day(value: Any, field: str) -> date:
    if isinstance(value, date) and not isinstance(value, datetime):
        return value
    try:
        return date.fromisoformat(value)
    except (TypeError, ValueError) as exc:
        raise StoreError(f"{field} must be YYYY-MM-DD") from exc


def _clock(value: Any, field: str) -> str:
    try:
        parsed = value if isinstance(value, time) else time.fromisoformat(value)
    except (TypeError, ValueError) as exc:
        raise StoreError(f"{field} must be an ISO local time") from exc
    if parsed.tzinfo is not None:
        raise StoreError(f"{field} must not contain a timezone offset")
    return parsed.isoformat()


def _fold(value: Any) -> int:
    if value == "earlier" or type(value) is int and value == 0:
        return 0
    if value == "later" or type(value) is int and value == 1:
        return 1
    raise StoreError("dst_fold must be earlier/later or 0/1")


def _body(row: dict) -> dict:
    return {key: value for key, value in row.items() if key not in ("_id", "_rev")}


def _rule(value: Mapping[str, Any], starts_on: date) -> dict:
    if not isinstance(value, Mapping):
        raise StoreError("recurrence rule must be an object")
    frequency = value.get("frequency")
    weekday = value.get("weekday")
    if not isinstance(frequency, str) or frequency not in _FREQUENCIES:
        raise StoreError("unsupported recurrence frequency")
    if isinstance(weekday, bool) or not isinstance(weekday, int) or weekday not in range(1, 8):
        raise StoreError("weekday must be ISO weekday 1..7")
    allowed = {"frequency", "weekday"}
    result: dict[str, Any] = {"frequency": frequency, "weekday": weekday}
    if frequency in {"weekly", "biweekly"}:
        allowed.add("anchor_date")
        first_weekday = starts_on + timedelta(days=(weekday - starts_on.isoweekday()) % 7)
        anchor = _day(value["anchor_date"], "anchor_date") if "anchor_date" in value else first_weekday
        if anchor > first_weekday or anchor.isoweekday() != weekday:
            raise StoreError("anchor_date must be a matching weekday no later than the first occurrence")
        result["anchor_date"] = anchor.isoformat()
    elif frequency == "monthly_nth":
        allowed.add("nth")
        nth = value.get("nth")
        if isinstance(nth, bool) or not isinstance(nth, int) or nth not in range(1, 6):
            raise StoreError("monthly_nth requires nth in 1..5")
        result["nth"] = nth
    if set(value) != allowed and not (frequency in {"weekly", "biweekly"} and
                                      set(value) == allowed - {"anchor_date"}):
        raise StoreError("recurrence rule has missing or unsupported keys")
    return result


def _template(value: Mapping[str, Any]) -> dict:
    if not isinstance(value, Mapping):
        raise StoreError("entry_template must be an object")
    if any(key in value for key in ("id", "scheduled_at", "plan_revision_id",
                                    "schedule_series_id", "schedule_date")):
        raise StoreError("entry_template cannot set identity, date or plan revision")
    if not isinstance(value.get("event_kind"), str) or \
            isinstance(value.get("ordinal"), bool) or not isinstance(value.get("ordinal"), int) or \
            value["ordinal"] < 0 or not isinstance(value.get("payload", {}), Mapping):
        raise StoreError("entry_template needs event_kind, nonnegative ordinal and object payload")
    try:
        copy = json.loads(json.dumps(dict(value), ensure_ascii=False, allow_nan=False))
    except (TypeError, ValueError) as exc:
        raise StoreError("entry_template must be JSON data") from exc
    return copy


def _series(store: Store, station_id: str, series_id: str) -> dict:
    series = store.get("schedule_series", series_id)
    if series is None or series.get("station_id") != station_id:
        raise StoreError("schedule series does not belong to station")
    return series


def _matching(rule: Mapping[str, Any], day: date) -> bool:
    if day.isoweekday() != rule["weekday"]:
        return False
    frequency = rule["frequency"]
    if frequency == "weekly":
        return True
    if frequency == "biweekly":
        anchor = date.fromisoformat(rule["anchor_date"])
        anchor_monday = anchor - timedelta(days=anchor.isoweekday() - 1)
        candidate_monday = day - timedelta(days=day.isoweekday() - 1)
        return ((candidate_monday - anchor_monday).days // 7) % 2 == 0
    if frequency == "monthly_nth":
        return (day.day - 1) // 7 + 1 == rule["nth"]
    return day.day + 7 > monthrange(day.year, day.month)[1]


def _local_instant(day: date, clock: str, zone: ZoneInfo,
                   dst_fold: int) -> tuple[datetime | None, str | None]:
    naive = datetime.combine(day, time.fromisoformat(clock))
    choices = []
    for fold in (0, 1):
        local = naive.replace(tzinfo=zone, fold=fold)
        roundtrip = local.astimezone(timezone.utc).astimezone(zone)
        if roundtrip.replace(tzinfo=None) == naive:
            choices.append(local)
    if not choices:
        return None, "dst_gap"
    if len(choices) == 2 and choices[0].utcoffset() != choices[1].utcoffset():
        return choices[dst_fold], "dst_ambiguous_" + ("earlier" if dst_fold == 0 else "later")
    return choices[0], None


def _segments(store: Store, series_id: str) -> list[dict]:
    rows = [row for row in store.lookup("schedule_segments", "series", series_id)
            if row["status"] == "active"]
    return sorted(rows, key=lambda row: (row["starts_on"], row["_id"]))


def _segment_on(rows: list[dict], day: date) -> dict | None:
    matches = [row for row in rows if row["starts_on"] <= day.isoformat() and
               (row.get("end_before") is None or day.isoformat() < row["end_before"])]
    if len(matches) > 1:
        raise Conflict("schedule segments overlap")
    return matches[0] if matches else None


def _exception(store: Store, series_id: str, day: date) -> dict | None:
    rows = store.lookup("schedule_exceptions", "series_date", series_id, day.isoformat())
    return rows[0] if rows and rows[0]["status"] == "active" else None


def _fingerprint(value: Mapping[str, Any]) -> str:
    payload = json.dumps(value, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    return sha256(payload).hexdigest()


def _project(store: Store, series: dict, rows: list[dict], day: date) -> dict | None:
    if series["status"] != "active" or day < date.fromisoformat(series["starts_on"]) or \
            (series.get("ends_on") and day > date.fromisoformat(series["ends_on"])):
        return None
    segment = _segment_on(rows, day)
    if segment is None or not _matching(segment["rule"], day):
        return None
    exception = _exception(store, series["_id"], day)
    if exception and exception["action"] == "skip":
        core = {"status": "skipped", "service_date": day.isoformat(),
                "reason": "explicit_exception", "entry": None}
    else:
        clock = (exception.get("local_time") if exception and exception["action"] == "replace"
                 and exception.get("local_time") else segment["local_time"])
        template = (exception.get("entry_template") if exception and exception["action"] == "replace"
                    and exception.get("entry_template") else segment["entry_template"])
        station = store.get("stations", series["station_id"])
        instant, dst_note = _local_instant(day, clock, ZoneInfo(station["timezone"]),
                                           segment["dst_fold"])
        if instant is None:
            core = {"status": "skipped", "service_date": day.isoformat(),
                    "reason": "dst_gap", "entry": None}
        else:
            candidate = dict(template) | {"scheduled_at": instant.isoformat()}
            validation = validate_entry(candidate, station["timezone"], day)
            if not validation["valid"]:
                core = {"status": "invalid", "service_date": day.isoformat(),
                        "reason": "invalid_entry", "entry": candidate,
                        "errors": validation["errors"]}
            else:
                normalized = validation["normalized"]
                entry = {key: value for key, value in normalized.items()
                         if key not in ("id", "nominal_end_at", "requires_resolution")}
                from .broadcast_services import check_entry_scope, require_service
                try:
                    check_entry_scope(entry, require_service(
                        store, series["station_id"], series["service_key"]))
                except Conflict as exc:
                    core = {"status": "invalid", "service_date": day.isoformat(),
                            "reason": "service_output_mismatch", "entry": entry,
                            "errors": [{"code": "service_output_mismatch",
                                        "detail": str(exc)}]}
                else:
                    core = {"status": "ready", "service_date": day.isoformat(),
                            "reason": dst_note, "entry": entry,
                            "warnings": validation["warnings"]}
    return core | {"series_id": series["_id"], "segment_id": segment["_id"],
                   "exception_id": exception["_id"] if exception else None,
                   "source_fingerprint": _fingerprint(core)}


def _orphan_projection(series: dict, day: date) -> dict:
    reason = "series_disabled" if series["status"] != "active" else "no_longer_recurrence"
    core = {"status": "skipped", "service_date": day.isoformat(),
            "reason": reason, "entry": None}
    return core | {"series_id": series["_id"], "segment_id": None,
                   "exception_id": None, "source_fingerprint": _fingerprint(core)}


def create_schedule_series(store: Store, principal: Principal, station_id: str, *,
                           title: str, starts_on: str | date, rule: Mapping[str, Any],
                           local_time: str | time, entry_template: Mapping[str, Any],
                           ends_on: str | date | None = None,
                           dst_fold: str | int = "earlier",
                           service_key: str = "default") -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    from .broadcast_services import (check_entry_scope, require_service,
                                     service_key as checked_service_key)
    service_key = checked_service_key(service_key)
    binding = require_service(store, station_id, service_key)
    station = store.get("stations", station_id)
    if station is None:
        raise StoreError("station not found")
    if not isinstance(title, str) or not title.strip():
        raise StoreError("series title must be nonempty")
    start = _day(starts_on, "starts_on")
    end = _day(ends_on, "ends_on") if ends_on is not None else None
    if end is not None and end < start:
        raise StoreError("ends_on precedes starts_on")
    dst_fold = _fold(dst_fold)
    normalized_rule = _rule(rule, start)
    normalized_template = _template(entry_template)
    check_entry_scope(normalized_template, binding)
    clock = _clock(local_time, "local_time")
    first = next((start + timedelta(days=offset) for offset in range(14)
                  if _matching(normalized_rule, start + timedelta(days=offset))), None)
    if first is None and normalized_rule["frequency"] in {"monthly_nth", "monthly_last"}:
        first = next((start + timedelta(days=offset) for offset in range(70)
                      if _matching(normalized_rule, start + timedelta(days=offset))), None)
    if first is None or end is not None and first > end:
        raise StoreError("series has no occurrence in its date range")
    series_id, segment_id = str(uuid4()), str(uuid4())
    now = datetime.now(timezone.utc).isoformat()
    segment = {"series_id": series_id, "starts_on": start.isoformat(),
               "end_before": None, "rule": normalized_rule, "local_time": clock,
               "entry_template": normalized_template, "dst_fold": dst_fold,
               "status": "active", "created_by": principal.user_id, "created_at": now}
    _check_segment_template(store, station_id, segment,
                            end.isoformat() if end else None, require_occurrence=True)
    tx = store.begin(actor=principal.user_id)
    tx.put("schedule_series", series_id, {
        "station_id": station_id, "service_key": service_key,
        "title": title.strip(), "status": "active",
        "starts_on": start.isoformat(), "ends_on": end.isoformat() if end else None,
        "created_by": principal.user_id, "created_at": now,
        "updated_by": principal.user_id, "updated_at": now})
    tx.put("schedule_segments", segment_id, segment)
    tx.commit()
    return {"series_id": series_id, "service_key": service_key,
            "segment_id": segment_id,
            "series_revision": 1, "first_occurrence_date": first.isoformat()}


def preview_occurrences(store: Store, principal: Principal, station_id: str,
                        series_id: str, *, from_date: str | date | None = None,
                        through_date: str | date | None = None,
                        limit: int = 100) -> dict:
    station_id, series_id = _id(station_id, "station_id"), _id(series_id, "series_id")
    require(store, principal, "view", station_id)
    sequence = store.sequence
    series = _series(store, station_id, series_id)
    zone = ZoneInfo(store.get("stations", station_id)["timezone"])
    start = _day(from_date, "from_date") if from_date is not None else datetime.now(zone).date()
    end = _day(through_date, "through_date") if through_date is not None else start + timedelta(days=90)
    if end < start or (end - start).days > _MAX_PREVIEW_DAYS:
        raise StoreError("preview range must be ordered and at most 3660 days")
    if isinstance(limit, bool) or not isinstance(limit, int) or limit < 1 or limit > 1000:
        raise StoreError("limit must be 1..1000")
    rows = _segments(store, series_id)
    material_by_date = {row["service_date"]: row for row in
                        store.lookup("schedule_occurrences", "series", series_id)}
    occurrences = []
    truncated = False
    next_from_date = None
    for offset in range((end - start).days + 1):
        day = start + timedelta(days=offset)
        projection = _project(store, series, rows, day)
        material = material_by_date.get(day.isoformat())
        if projection is None and material is None:
            continue
        if projection is None:
            projection = _orphan_projection(series, day)
        if len(occurrences) >= limit:
            truncated = True
            next_from_date = day.isoformat()
            break
        plan = store.get("plans", material["plan_id"]) if material and material.get("plan_id") else None
        occurrences.append(projection | {
            "materialization": ({"status": material["status"],
                                 "plan_id": material.get("plan_id"),
                                 "plan_revision_id": material.get("plan_revision_id"),
                                 "plan_entry_id": material.get("plan_entry_id"),
                                 "plan_state": plan["state"] if plan else None,
                                 "stale": material["source_fingerprint"] != projection["source_fingerprint"]}
                                if material else None)})
    if store.sequence != sequence:
        raise Conflict("schedule changed during preview")
    return {"series_id": series_id, "service_key": series["service_key"],
            "series_revision": series["_rev"],
            "from_date": start.isoformat(), "through_date": end.isoformat(),
            "occurrences": occurrences, "truncated": truncated,
            "next_from_date": next_from_date}


def _approved_impacts(store: Store, series_id: str, start: date,
                      end: date | None = None) -> list[dict]:
    impacts = []
    for row in store.lookup("schedule_occurrences", "series", series_id):
        day = date.fromisoformat(row["service_date"])
        if day < start or end is not None and day > end or not row.get("plan_id"):
            continue
        plan = store.get("plans", row["plan_id"])
        approved_id = plan.get("current_revision_id") if plan else None
        if approved_id and not any(entry.get("schedule_series_id") == series_id and
                                   entry.get("schedule_date") == row["service_date"]
                                   for entry in store.lookup("plan_entries", "revision", approved_id)):
            approved_id = None
        if approved_id:
            impacts.append({"service_date": row["service_date"],
                            "plan_id": plan["_id"],
                            "approved_revision_id": approved_id,
                            "plan_unchanged": True})
    return sorted(impacts, key=lambda item: item["service_date"])


def _check_segment_template(store: Store, station_id: str, replacement: dict,
                            series_end: str | None, *,
                            require_occurrence: bool) -> None:
    station = store.get("stations", station_id)
    zone = ZoneInfo(station["timezone"])
    start = date.fromisoformat(replacement["starts_on"])
    last = start + timedelta(days=370)
    if series_end:
        last = min(last, date.fromisoformat(series_end))
    if replacement.get("end_before"):
        last = min(last, date.fromisoformat(replacement["end_before"]) - timedelta(days=1))
    first_valid = None
    if last >= start:
        for offset in range((last - start).days + 1):
            day = start + timedelta(days=offset)
            if not _matching(replacement["rule"], day):
                continue
            instant, _ = _local_instant(day, replacement["local_time"], zone,
                                        replacement["dst_fold"])
            if instant is not None:
                first_valid = (day, instant)
                break
    if first_valid is None and require_occurrence:
        raise StoreError("replacement has no valid occurrence in its date range")
    if first_valid is None:
        for offset in range(8):
            day = start + timedelta(days=offset)
            instant, _ = _local_instant(day, replacement["local_time"], zone,
                                        replacement["dst_fold"])
            if instant is not None:
                first_valid = (day, instant)
                break
    if first_valid is None:
        raise StoreError("replacement local time has no valid validation date")
    validation = validate_entry(replacement["entry_template"] |
                                {"scheduled_at": first_valid[1].isoformat()},
                                station["timezone"], first_valid[0])
    if not validation["valid"]:
        raise StoreError("invalid replacement template: " + ", ".join(
            issue["code"] for issue in validation["errors"]))


def edit_schedule(store: Store, principal: Principal, station_id: str,
                  series_id: str, *, scope: str,
                  expected_series_revision: int,
                  occurrence_date: str | date | None = None,
                  action: str | None = None,
                  rule: Mapping[str, Any] | None = None,
                  local_time: str | time | None = None,
                  entry_template: Mapping[str, Any] | None = None,
                  dst_fold: str | int | None = None) -> dict:
    """Edit one occurrence, all occurrences from a pivot, or the whole series.

    Scope edits alter schedule sources only. Materialized draft entries must be
    explicitly regenerated; approved revisions remain unchanged and are listed
    in the result if their dates are affected.
    """
    station_id, series_id = _id(station_id, "station_id"), _id(series_id, "series_id")
    require(store, principal, "edit", station_id)
    sequence = store.sequence
    series = _series(store, station_id, series_id)
    if series["_rev"] != expected_series_revision:
        raise Conflict("schedule series changed")
    if not isinstance(scope, str) or scope not in {"single", "future", "all"}:
        raise StoreError("scope must be single, future or all")
    rows = _segments(store, series_id)
    if not rows:
        raise Conflict("series has no active segment")
    pivot = (_day(occurrence_date, "occurrence_date")
             if scope != "all" else date.fromisoformat(series["starts_on"]))
    if pivot < date.fromisoformat(series["starts_on"]) or \
            series.get("ends_on") and pivot > date.fromisoformat(series["ends_on"]):
        raise StoreError("edit date is outside series range")
    source = _segment_on(rows, pivot)
    if source is None or (scope != "all" and not _matching(source["rule"], pivot)):
        raise StoreError("edit date is not a recurrence occurrence")
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    if scope == "single":
        if rule is not None or dst_fold is not None:
            raise StoreError("single occurrence edits use local_time or entry_template")
        if not isinstance(action, str) or action not in {"skip", "replace", "reset"}:
            raise StoreError("single occurrence action must be skip, replace or reset")
        if action == "replace" and local_time is None and entry_template is None:
            raise StoreError("replacement needs local_time or entry_template")
        if action != "replace" and (local_time is not None or entry_template is not None):
            raise StoreError("skip/reset cannot contain replacement fields")
        prior_rows = store.lookup("schedule_exceptions", "series_date",
                                  series_id, pivot.isoformat())
        prior = prior_rows[0] if prior_rows else None
        if action == "reset" and prior is None:
            raise Conflict("no exception exists for this occurrence")
        identifier = prior["_id"] if prior else str(uuid4())
        exception = {"series_id": series_id, "service_date": pivot.isoformat(),
                     "action": action, "status": "inactive" if action == "reset" else "active",
                     "local_time": _clock(local_time, "local_time") if local_time is not None else None,
                     "entry_template": _template(entry_template) if entry_template is not None else None,
                     "updated_by": principal.user_id, "updated_at": now}
        if action == "replace":
            station = store.get("stations", station_id)
            chosen_clock = exception["local_time"] or source["local_time"]
            chosen_template = exception["entry_template"] or source["entry_template"]
            instant, _ = _local_instant(pivot, chosen_clock,
                                        ZoneInfo(station["timezone"]), source["dst_fold"])
            if instant is None:
                raise StoreError("single occurrence replacement falls in a DST gap")
            validation = validate_entry(chosen_template | {"scheduled_at": instant.isoformat()},
                                        station["timezone"], pivot)
            if not validation["valid"]:
                raise StoreError("invalid single occurrence replacement: " + ", ".join(
                    issue["code"] for issue in validation["errors"]))
        tx.put("schedule_exceptions", identifier, exception,
               expected_revision=prior["_rev"] if prior else None)
        segment_id = source["_id"]
        affected_start = affected_end = pivot
    else:
        if action is not None:
            raise StoreError("series edits use rule, time, template or dst_fold")
        if all(value is None for value in (rule, local_time, entry_template, dst_fold)):
            raise StoreError("series edit needs at least one changed field")
        new_clock = _clock(local_time, "local_time") if local_time is not None else None
        new_template = _template(entry_template) if entry_template is not None else None
        new_fold = _fold(dst_fold) if dst_fold is not None else None
        segment_ids = []
        if scope == "all":
            # Keep the existing segment boundaries and any earlier future edits.
            # Only the fields supplied by this operation change in every segment.
            new_rule = _rule(rule, pivot) if rule is not None else None
            for segment in rows:
                replacement = {
                    "series_id": series_id, "starts_on": segment["starts_on"],
                    "end_before": segment.get("end_before"),
                    "rule": new_rule if new_rule is not None else segment["rule"],
                    "local_time": new_clock if new_clock is not None else segment["local_time"],
                    "entry_template": new_template if new_template is not None else segment["entry_template"],
                    "dst_fold": new_fold if new_fold is not None else segment["dst_fold"],
                    "status": "active", "created_by": principal.user_id,
                    "created_at": now}
                _check_segment_template(store, station_id, replacement,
                                        series.get("ends_on"), require_occurrence=False)
                tx.put("schedule_segments", segment["_id"],
                       _body(segment) | {"status": "superseded"},
                       expected_revision=segment["_rev"])
                identifier = str(uuid4())
                tx.put("schedule_segments", identifier, replacement)
                segment_ids.append(identifier)
        else:
            replacement = {
                "series_id": series_id, "starts_on": pivot.isoformat(),
                "end_before": None,
                "rule": _rule(rule, pivot) if rule is not None else source["rule"],
                "local_time": new_clock if new_clock is not None else source["local_time"],
                "entry_template": new_template if new_template is not None else source["entry_template"],
                "dst_fold": new_fold if new_fold is not None else source["dst_fold"],
                "status": "active", "created_by": principal.user_id,
                "created_at": now}
            _check_segment_template(store, station_id, replacement,
                                    series.get("ends_on"), require_occurrence=True)
            for segment in rows:
                if segment["starts_on"] >= pivot.isoformat():
                    tx.put("schedule_segments", segment["_id"],
                           _body(segment) | {"status": "superseded"},
                           expected_revision=segment["_rev"])
                elif segment.get("end_before") is None or segment["end_before"] > pivot.isoformat():
                    tx.put("schedule_segments", segment["_id"],
                           _body(segment) | {"end_before": pivot.isoformat()},
                           expected_revision=segment["_rev"])
            identifier = str(uuid4())
            tx.put("schedule_segments", identifier, replacement)
            segment_ids.append(identifier)
        segment_id = segment_ids[0]
        affected_start = pivot
        affected_end = None
    tx.put("schedule_series", series_id,
           _body(series) | {"updated_by": principal.user_id,
                            "updated_at": now, "last_edit_scope": scope},
           expected_revision=series["_rev"])
    tx.commit()
    return {"series_id": series_id, "series_revision": series["_rev"] + 1,
            "scope": scope, "segment_id": segment_id,
            "segment_ids": segment_ids if scope != "single" else [segment_id],
            "affected_from": affected_start.isoformat(),
            "approved_plan_dates_unchanged": _approved_impacts(
                store, series_id, affected_start, affected_end)}


def set_schedule_status(store: Store, principal: Principal, station_id: str,
                        series_id: str, status: str, *,
                        expected_series_revision: int) -> dict:
    station_id, series_id = _id(station_id, "station_id"), _id(series_id, "series_id")
    require(store, principal, "edit", station_id)
    if not isinstance(status, str) or status not in {"active", "disabled"}:
        raise StoreError("status must be active or disabled")
    series = _series(store, station_id, series_id)
    if series["_rev"] != expected_series_revision:
        raise Conflict("schedule series changed")
    if status == series["status"]:
        return series
    return store.put("schedule_series", series_id,
                     _body(series) | {"status": status,
                                      "updated_by": principal.user_id,
                                      "updated_at": datetime.now(timezone.utc).isoformat()},
                     expected_revision=series["_rev"], actor=principal.user_id)


_ENTRY_FIELDS = ("ordinal", "state", "event_kind", "scheduled_at", "payload",
                 "duration_kind", "planned_duration_ms", "timing", "transition",
                 "ad_break_id")


def _entry_hash(row: Mapping[str, Any]) -> str:
    return _fingerprint({key: row.get(key) for key in _ENTRY_FIELDS})


def _draft_entry(store: Store, revision_id: str, series_id: str,
                 service_date: str) -> dict | None:
    candidates = [row for row in store.lookup("plan_entries", "revision", revision_id)
                  if row.get("schedule_series_id") == series_id and
                  row.get("schedule_date") == service_date]
    if len(candidates) > 1:
        raise Conflict("draft has duplicate entries for one schedule occurrence")
    return candidates[0] if candidates else None


def _plan_for_date(store: Store, station_id: str, service_date: str,
                   service_key: str = "default") -> dict | None:
    rows = store.lookup("plans", "station_date_service", station_id,
                        service_date, service_key)
    return rows[0] if rows else None


def _occurrence_body(series_id: str, day: date, projection: dict, *,
                     status: str, plan_id: str | None = None,
                     revision_id: str | None = None,
                     entry_id: str | None = None,
                     entry_hash: str | None = None,
                     principal_id: str) -> dict:
    return {"series_id": series_id, "service_date": day.isoformat(),
            "status": status, "source_fingerprint": projection["source_fingerprint"],
            "source_segment_id": projection["segment_id"],
            "source_exception_id": projection["exception_id"],
            "reason": projection.get("reason"),
            "plan_id": plan_id, "plan_revision_id": revision_id,
            "plan_entry_id": entry_id, "entry_fingerprint": entry_hash,
            "applied_by": principal_id,
            "applied_at": datetime.now(timezone.utc).isoformat()}


def materialize_occurrence(store: Store, principal: Principal, station_id: str,
                           series_id: str, service_date: str | date, *,
                           expected_series_revision: int) -> dict:
    """Apply one projected occurrence to a draft plan in a single transaction.

    Reapplication is idempotent when the source and draft entry still match.
    Approved revisions are never written; a forked draft may be regenerated
    through the schedule tags copied with its entries.
    """
    station_id, series_id = _id(station_id, "station_id"), _id(series_id, "series_id")
    require(store, principal, "edit", station_id)
    sequence = store.sequence
    series = _series(store, station_id, series_id)
    from .broadcast_services import require_service
    require_service(store, station_id, series["service_key"])
    if series["_rev"] != expected_series_revision:
        raise Conflict("schedule series changed")
    day = _day(service_date, "service_date")
    prior_rows = store.lookup("schedule_occurrences", "series_date",
                              series_id, day.isoformat())
    prior = prior_rows[0] if prior_rows else None
    projection = _project(store, series, _segments(store, series_id), day)
    if projection is None:
        if prior is None:
            raise StoreError("date is not an active or previously materialized occurrence")
        projection = _orphan_projection(series, day)
    if projection["status"] == "invalid":
        raise StoreError("occurrence is invalid: " + ", ".join(
            issue["code"] for issue in projection.get("errors", [])))
    occurrence_id = prior["_id"] if prior else str(uuid4())
    plan = _plan_for_date(store, station_id, day.isoformat(), series["service_key"])
    if prior is not None and prior.get("plan_id") and \
            (plan is None or plan["_id"] != prior["plan_id"]):
        raise Conflict("materialized occurrence references a different date plan")
    if prior is not None and prior["source_fingerprint"] == projection["source_fingerprint"] and \
            prior["status"] == ("skipped" if projection["status"] == "skipped" else "materialized"):
        linked = store.get("plan_entries", prior["plan_entry_id"]) if prior.get("plan_entry_id") else None
        if linked is None and prior.get("plan_entry_id"):
            raise Conflict("materialized entry is missing")
        if linked is not None and prior.get("entry_fingerprint") != _entry_hash(linked):
            raise Conflict("materialized entry was changed outside the schedule")
        return {"occurrence_id": occurrence_id, "status": prior["status"],
                "plan_id": prior.get("plan_id"),
                "plan_revision_id": prior.get("plan_revision_id"),
                "plan_entry_id": prior.get("plan_entry_id"), "replayed": True}

    draft_revision = None
    candidate = None
    if plan is not None and plan.get("draft_revision_id"):
        draft_revision = store.get("plan_revisions", plan["draft_revision_id"])
        if draft_revision is None or draft_revision["status"] != "draft":
            raise Conflict("plan draft revision is missing or locked")
        candidate = _draft_entry(store, draft_revision["_id"], series_id,
                                 day.isoformat())
    if prior is not None and prior.get("plan_entry_id"):
        previous_entry = store.get("plan_entries", prior["plan_entry_id"])
        if previous_entry is None:
            raise Conflict("previous occurrence entry is missing")
        if prior.get("entry_fingerprint") != _entry_hash(previous_entry):
            raise Conflict("previous occurrence entry was changed outside the schedule")
        if candidate is not None and prior.get("entry_fingerprint") != _entry_hash(candidate):
            raise Conflict("forked draft occurrence was changed outside the schedule")
        if candidate is None and (prior["status"] == "materialized" or
                                  previous_entry["plan_revision_id"] == (draft_revision or {}).get("_id")):
            raise Conflict("draft occurrence entry is missing")

    if projection["status"] == "skipped":
        if prior is not None and prior.get("plan_entry_id") and candidate is None:
            raise Conflict("approved occurrence cannot be skipped without a draft fork")
        tx = store.begin(actor=principal.user_id)
        tx.expect_sequence(sequence)
        if candidate is not None:
            cancelled = _body(candidate) | {"state": "cancelled"}
            tx.put("plan_entries", candidate["_id"], cancelled,
                   expected_revision=candidate["_rev"])
            tx.put("plans", plan["_id"], _body(plan), expected_revision=plan["_rev"])
            linked_plan, linked_revision, linked_entry = (plan["_id"],
                draft_revision["_id"], candidate["_id"])
            entry_hash = _entry_hash(cancelled)
        else:
            linked_plan = linked_revision = linked_entry = entry_hash = None
        tx.put("schedule_occurrences", occurrence_id,
               _occurrence_body(series_id, day, projection, status="skipped",
                                plan_id=linked_plan, revision_id=linked_revision,
                                entry_id=linked_entry, entry_hash=entry_hash,
                                principal_id=principal.user_id),
               expected_revision=prior["_rev"] if prior else None)
        tx.commit()
        return {"occurrence_id": occurrence_id, "status": "skipped",
                "reason": projection["reason"], "plan_id": linked_plan,
                "plan_revision_id": linked_revision,
                "plan_entry_id": linked_entry, "replayed": False}

    if plan is not None and draft_revision is None:
        raise Conflict("date plan has no editable draft; fork it explicitly")
    if prior is not None and prior.get("plan_entry_id") and candidate is None:
        raise Conflict("forked draft occurrence entry is missing")
    new_plan = plan is None
    plan_id = str(uuid4()) if new_plan else plan["_id"]
    revision_id = str(uuid4()) if new_plan else draft_revision["_id"]
    entry_id = candidate["_id"] if candidate else str(uuid4())
    entry_body = dict(projection["entry"]) | {
        "plan_revision_id": revision_id,
        "schedule_series_id": series_id,
        "schedule_date": day.isoformat(),
        "schedule_source_fingerprint": projection["source_fingerprint"]}
    station = store.get("stations", station_id)
    if not new_plan:
        existing_entries = store.lookup("plan_entries", "revision", revision_id)
        proposed = [(_body(row) | {"id": row["_id"]}) for row in existing_entries
                    if row["_id"] != entry_id]
    else:
        proposed = []
    proposed.append(entry_body | {"id": entry_id})
    plan_validation = validate_plan(proposed, station["timezone"], day)
    if not plan_validation["valid"]:
        raise Conflict("draft plan would conflict: " + ", ".join(
            issue["code"] for issue in plan_validation["conflicts"]))
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    if new_plan:
        tx.put("plans", plan_id, {
            "station_id": station_id, "service_date": day.isoformat(),
            "service_key": series["service_key"],
            "state": "draft", "current_revision_id": None,
            "draft_revision_id": revision_id})
        tx.put("plan_revisions", revision_id, {
            "plan_id": plan_id, "revision_no": 1, "status": "draft",
            "created_by": principal.user_id, "created_at": now})
    else:
        tx.put("plans", plan_id, _body(plan), expected_revision=plan["_rev"])
    tx.put("plan_entries", entry_id, entry_body,
           expected_revision=candidate["_rev"] if candidate else None)
    tx.put("schedule_occurrences", occurrence_id,
           _occurrence_body(series_id, day, projection, status="materialized",
                            plan_id=plan_id, revision_id=revision_id,
                            entry_id=entry_id, entry_hash=_entry_hash(entry_body),
                            principal_id=principal.user_id),
           expected_revision=prior["_rev"] if prior else None)
    tx.commit()
    return {"occurrence_id": occurrence_id, "status": "materialized",
            "plan_id": plan_id, "plan_revision_id": revision_id,
            "plan_entry_id": entry_id,
            "plan_revision": 1 if new_plan else plan["_rev"] + 1,
            "warnings": plan_validation["warnings"], "replayed": False}
