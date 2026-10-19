"""Planned versus engine-reported playout and output-observation snapshots.

These reports preserve evidence classes separately. A completed engine event
does not establish acoustic output, and an audio-present observation does not
identify the exact song or advertisement. This module does not certify
royalties, advertising delivery, or legal compliance.
"""

from __future__ import annotations

import csv
from datetime import date, datetime, timedelta, timezone
import hashlib
import io
import json
import re
from typing import Any
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo

from .identity import Principal, require
from .storage import Conflict, Store, StoreError


_KEY = re.compile(r"[A-Za-z0-9_.:-]{1,160}\Z")
_OUTPUT = re.compile(r"[A-Za-z0-9_.:-]{1,100}\Z")
_TERMINAL = {"complete", "manual_complete", "fail", "interrupt", "skip"}
_START = {"start", "manual_start"}


def _uuid(value: str, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _instant(value: str | datetime, field: str) -> datetime:
    try:
        instant = (datetime.fromisoformat(value.replace("Z", "+00:00"))
                   if isinstance(value, str) else value)
    except ValueError as exc:
        raise StoreError(f"{field} must be an ISO 8601 instant") from exc
    if (not isinstance(instant, datetime) or instant.tzinfo is None or
            instant.utcoffset() is None):
        raise StoreError(f"{field} must include a timezone")
    return instant.astimezone(timezone.utc)


def _day(value: str, field: str) -> date:
    try:
        return date.fromisoformat(value)
    except (ValueError, TypeError) as exc:
        raise StoreError(f"{field} must be YYYY-MM-DD") from exc


def _filters(station_id: str, date_from: str, date_to: str,
             output_id: str | None) -> tuple[str, date, date, str | None]:
    station_id = _uuid(station_id, "station_id")
    first, last = _day(date_from, "date_from"), _day(date_to, "date_to")
    if last < first or (last - first).days > 366:
        raise StoreError("report date range must be 1..367 inclusive days")
    if output_id is not None and (not isinstance(output_id, str) or
                                  not _OUTPUT.fullmatch(output_id)):
        raise StoreError("output_id must be a short opaque code")
    return station_id, first, last, output_id


def _hash(value: Any) -> str:
    try:
        data = json.dumps(value, ensure_ascii=False, sort_keys=True,
                          separators=(",", ":"), allow_nan=False).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise StoreError("report data must be finite JSON") from exc
    return hashlib.sha256(data).hexdigest()


def _source_row(row: dict) -> dict:
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _add_source(sources: dict[tuple[str, str], dict], collection: str,
                row: dict | None) -> None:
    if row is not None:
        sources[(collection, row["_id"])] = {
            "collection": collection, "id": row["_id"], "revision": row["_rev"],
            "data": _source_row(row),
        }


def _in_range(instant: datetime, first: date, last: date,
              zone: ZoneInfo) -> bool:
    return first <= instant.astimezone(zone).date() <= last


def _event_output(events: list[dict]) -> set[str]:
    return {row["details"]["output_id"] for row in events
            if isinstance(row.get("details"), dict) and
            isinstance(row["details"].get("output_id"), str) and
            row["details"]["output_id"]}


def _attempt_state(events: list[dict], attempt: dict) -> tuple[str, list[str]]:
    if not events:
        return "unknown", ["no_event"]
    ordered = sorted(events, key=lambda row: (
        _instant(row["occurred_at"], "occurred_at"), row["_id"]))
    phases = [row["phase"] for row in ordered]
    issues = []
    for row in ordered:
        for field in ("station_id", "endpoint_id", "version_id", "plan_entry_id"):
            if field in row and row[field] != attempt.get(field):
                issues.append(f"{field}_mismatch")
    if issues:
        return "unknown", list(dict.fromkeys(issues))
    for row, phase in zip(ordered, phases):
        if phase in {"complete", "manual_complete"}:
            matching_start = ("manual_start" if phase == "manual_complete" else "start")
            complete_at = _instant(row["occurred_at"], "occurred_at")
            if not any(other["phase"] == matching_start and
                       _instant(other["occurred_at"], "occurred_at") <= complete_at
                       for other in ordered):
                issues.append("complete_without_matching_start")
    if issues:
        return "unknown", list(dict.fromkeys(issues))
    if any(phase in {"complete", "manual_complete"} for phase in phases):
        return "completed", []
    last = phases[-1]
    if last in _START:
        return "started", []
    if last in {"fail", "interrupt"}:
        return "failed", []
    if last == "skip":
        return "skipped", []
    return "unknown", ["no_start_or_terminal_event"]


def _row_state(entry: dict | None, attempt_rows: list[dict],
               as_of: datetime) -> str:
    if any(row["state"] == "completed" for row in attempt_rows):
        return "completed"
    if attempt_rows:
        latest = max(attempt_rows, key=lambda row: (
            row["last_event_at"] or "", row["attempt_id"]))
        if latest["state"] in {"started", "failed", "skipped"}:
            return latest["state"]
        return "unknown"
    if entry is not None:
        if entry["state"] in {"skipped", "replaced", "cancelled"}:
            return "skipped"
        if _instant(entry["scheduled_at"], "scheduled_at") > as_of:
            return "planned"
    return "unknown"


def _build(store: Store, station_id: str, first: date, last: date,
           output_id: str | None, as_of: datetime) -> tuple[dict, str]:
    station = store.get("stations", station_id)
    if station is None:
        raise StoreError("station is absent")
    zone = ZoneInfo(station["timezone"])
    sources: dict[tuple[str, str], dict] = {}
    _add_source(sources, "stations", station)
    plans: dict[str, dict] = {}
    day = first
    while day <= last:
        for plan in store.lookup("plans", "station_date", station_id, day.isoformat()):
            plans[plan["_id"]] = plan
            _add_source(sources, "plans", plan)
        day += timedelta(days=1)
    scheduled_plan_ids = set(plans)
    all_revisions = [row for row in store.scan("plan_revisions")
                     if row["plan_id"] in plans]
    revisions = {row["_id"]: row for row in all_revisions}
    for revision in all_revisions:
        _add_source(sources, "plan_revisions", revision)
    selected: dict[str, str] = {}
    for plan in plans.values():
        candidates = [row for row in all_revisions
                      if row["plan_id"] == plan["_id"] and
                      row["status"] in {"approved", "superseded"} and
                      row.get("approved_at") and
                      _instant(row["approved_at"], "approved_at") <= as_of]
        if candidates:
            selected[plan["_id"]] = max(
                candidates, key=lambda row: (row["revision_no"], row["_id"]))["_id"]
    current_entries: dict[str, dict] = {}
    for revision_id in selected.values():
        for entry in store.lookup("plan_entries", "revision", revision_id):
            current_entries[entry["_id"]] = entry
            _add_source(sources, "plan_entries", entry)

    station_attempts = store.lookup("playout_attempts", "station", station_id)
    relevant_attempts: dict[str, dict] = {}
    relevant_events: dict[str, list[dict]] = {}
    historical_entries: dict[str, dict] = {}
    cross_date_entries: dict[str, dict] = {}
    cross_date_dates: dict[str, str] = {}
    for attempt in station_attempts:
        events = [event for event in store.lookup("playout_events", "attempt", attempt["_id"])
                  if _instant(event["occurred_at"], "occurred_at") <= as_of]
        if not events:
            continue
        entry_id = attempt.get("plan_entry_id")
        if entry_id:
            entry = current_entries.get(entry_id) or store.get("plan_entries", entry_id)
            revision = revisions.get(entry["plan_revision_id"]) if entry else None
            if revision is None and entry is not None:
                revision = store.get("plan_revisions", entry["plan_revision_id"])
            plan = plans.get(revision["plan_id"]) if revision else None
            if plan is None and revision is not None:
                plan = store.get("plans", revision["plan_id"])
            if plan is None or plan["station_id"] != station_id:
                continue
            if plan["_id"] not in scheduled_plan_ids:
                in_range_events = [event for event in events if _in_range(
                    _instant(event["occurred_at"], "occurred_at"), first, last, zone)]
                if not in_range_events:
                    continue
                cross_date_entries[entry_id] = entry
                cross_date_dates[entry_id] = min(
                    _instant(event["occurred_at"], "occurred_at").astimezone(zone)
                    .date().isoformat() for event in in_range_events)
                plans[plan["_id"]] = plan
                revisions[revision["_id"]] = revision
                _add_source(sources, "plans", plan)
                _add_source(sources, "plan_revisions", revision)
            elif entry_id not in current_entries:
                historical_entries[entry_id] = entry
            _add_source(sources, "plan_entries", entry)
        elif not any(_in_range(_instant(event["occurred_at"], "occurred_at"),
                               first, last, zone) for event in events):
            continue
        relevant_attempts[attempt["_id"]] = attempt
        relevant_events[attempt["_id"]] = events
        _add_source(sources, "playout_attempts", attempt)
        for event in events:
            _add_source(sources, "playout_events", event)

    observations: dict[str, list[dict]] = {}
    unlinked_observations: list[dict] = []
    unscoped_observations = 0
    for observation in store.lookup("output_observations", "station", station_id):
        occurred = _instant(observation["occurred_at"], "occurred_at")
        if occurred > as_of:
            continue
        known_output = (observation.get("details") or {}).get("output_id")
        if output_id is not None and known_output != output_id:
            if known_output is None and _in_range(occurred, first, last, zone):
                unscoped_observations += 1
            continue
        attempt_id = observation.get("attempt_id")
        if attempt_id in relevant_attempts:
            observations.setdefault(attempt_id, []).append(observation)
            _add_source(sources, "output_observations", observation)
        elif _in_range(occurred, first, last, zone):
            unlinked_observations.append({
                "observation_id": observation["_id"],
                "endpoint_id": observation["endpoint_id"],
                "output_id": known_output, "kind": observation["kind"],
                "observer_kind": observation.get("observer_kind"),
                "observer_id": observation.get("observer_id"),
                "occurred_at": observation["occurred_at"],
                "linked_attempt_id": attempt_id,
            })
            _add_source(sources, "output_observations", observation)

    attempts_by_entry: dict[str | None, list[dict]] = {}
    all_event_outputs_by_entry: dict[str | None, set[str]] = {}
    unknown_attempt_output = 0
    for attempt in relevant_attempts.values():
        events = relevant_events[attempt["_id"]]
        event_outputs = _event_output(events)
        all_event_outputs_by_entry.setdefault(attempt.get("plan_entry_id"), set()).update(
            event_outputs)
        attempt_output = next(iter(event_outputs)) if len(event_outputs) == 1 else None
        if output_id is not None and attempt_output != output_id:
            if not event_outputs or len(event_outputs) > 1:
                unknown_attempt_output += 1
            continue
        obs = observations.get(attempt["_id"], [])
        state, evidence_issues = _attempt_state(events, attempt)
        start_times = [_instant(row["occurred_at"], "occurred_at") for row in events
                       if row["phase"] in _START]
        complete_times = [_instant(row["occurred_at"], "occurred_at") for row in events
                          if row["phase"] in {"complete", "manual_complete"}]
        first_start = min(start_times) if start_times else None
        last_complete = max(complete_times) if complete_times else None
        ordered_events = sorted(events, key=lambda row: (row["occurred_at"], row["_id"]))
        attempts_by_entry.setdefault(attempt.get("plan_entry_id"), []).append({
            "attempt_id": attempt["_id"], "endpoint_id": attempt["endpoint_id"],
            "version_id": attempt["version_id"], "attempt_no": attempt.get("attempt_no"),
            "state": state, "last_event_at": max(
                (event["occurred_at"] for event in events), default=None),
            "event_ids": [event["_id"] for event in ordered_events],
            "event_phases": [event["phase"] for event in ordered_events],
            "output_id": attempt_output,
            "evidence_issues": evidence_issues,
            "output_scope_issue": ("conflicting_event_outputs" if len(event_outputs) > 1
                                   else "unknown_event_output" if not event_outputs else None),
            "output_observations": [{
                "observation_id": row["_id"], "kind": row["kind"],
                "observer_kind": row.get("observer_kind"),
                "observer_id": row.get("observer_id"),
                "occurred_at": row["occurred_at"],
                "output_id": (row.get("details") or {}).get("output_id"),
                "output_scope_matches": (attempt_output is not None and
                                          (row.get("details") or {}).get("output_id") ==
                                          attempt_output),
                "time_window_matches": (state == "completed" and
                                        first_start is not None and
                                        last_complete is not None and
                                        first_start - timedelta(seconds=2) <=
                                        _instant(row["occurred_at"], "occurred_at") <=
                                        last_complete + timedelta(seconds=2)),
            } for row in sorted(obs, key=lambda row: (row["occurred_at"], row["_id"]))],
        })

    placements_by_entry: dict[str, list[dict]] = {}
    included_entry_ids = set(current_entries) | set(historical_entries) | set(cross_date_entries)
    for placement in store.scan("placements"):
        if placement["plan_entry_id"] in included_entry_ids:
            placements_by_entry.setdefault(placement["plan_entry_id"], []).append(placement)
            _add_source(sources, "placements", placement)
    all_entries = current_entries | historical_entries | cross_date_entries
    rows: list[dict] = []
    unscoped_planned = 0
    obligation_summary: dict[str, dict] = {}
    obligation_observation_ids: dict[str, set[str]] = {}
    for entry in sorted(all_entries.values(), key=lambda row: (
            row["scheduled_at"], row["ordinal"], row["_id"])):
        revision = revisions[entry["plan_revision_id"]]
        plan = plans[revision["plan_id"]]
        attempts = attempts_by_entry.get(entry["_id"], [])
        planned_output = ((entry.get("payload") or {}).get("output_id")
                          if isinstance(entry.get("payload"), dict) else None)
        known_outputs = ({planned_output} if isinstance(planned_output, str)
                         and planned_output else set())
        known_outputs.update(all_event_outputs_by_entry.get(entry["_id"], set()))
        if output_id is not None and output_id not in known_outputs:
            if not known_outputs:
                unscoped_planned += 1
            continue
        version_id = ((entry.get("payload") or {}).get("version_id")
                      if entry["event_kind"] == "audio" else None)
        version = store.get("versions", version_id) if version_id else None
        item = store.get("items", version["item_id"]) if version else None
        _add_source(sources, "versions", version)
        _add_source(sources, "items", item)
        placement_rows: list[dict] = []
        for placement in placements_by_entry.get(entry["_id"], []):
            spot = store.get("spots", placement["spot_id"])
            obligation = store.get("obligations", placement["obligation_id"])
            campaign = store.get("campaigns", spot["campaign_id"]) if spot else None
            for name, record in (("spots", spot), ("obligations", obligation),
                                 ("campaigns", campaign)):
                _add_source(sources, name, record)
            issues = []
            if spot is None or obligation is None or campaign is None:
                issues.append("missing_ad_reference")
            else:
                if version_id != spot["version_id"]:
                    issues.append("spot_version_differs_from_plan")
                if obligation["station_id"] != station_id or \
                        obligation["campaign_id"] != campaign["_id"]:
                    issues.append("obligation_scope_conflict")
            placement_rows.append({
                "placement_id": placement["_id"], "placement_state": placement["state"],
                "spot_id": placement["spot_id"],
                "campaign_id": campaign["_id"] if campaign else None,
                "obligation_id": placement["obligation_id"], "issues": issues,
            })
            if obligation and obligation["station_id"] == station_id:
                summary = obligation_summary.setdefault(obligation["_id"], {
                    "obligation_id": obligation["_id"],
                    "campaign_id": obligation["campaign_id"],
                    "contract_total_required_plays": obligation["required_plays"],
                    "window_start": obligation["window_start"],
                    "window_end": obligation["window_end"],
                    "scheduled_placements_in_report": 0,
                    "engine_completed_placements_in_report": 0,
                    "audio_present_observations_in_report": 0,
                })
                summary["scheduled_placements_in_report"] += 1
                if any(row["state"] == "completed" and
                       row["version_id"] == (spot["version_id"] if spot else None)
                       for row in attempts):
                    summary["engine_completed_placements_in_report"] += 1
                observed_ids = obligation_observation_ids.setdefault(obligation["_id"], set())
                observed_ids.update(
                    observation["observation_id"] for attempt in attempts
                    if spot and attempt["version_id"] == spot["version_id"]
                    for observation in attempt["output_observations"]
                    if observation["kind"] == "audio_present" and
                    observation["output_scope_matches"] and
                    observation["time_window_matches"])
        completed = sum(row["state"] == "completed" for row in attempts)
        audio_present = sum(
            observation["kind"] == "audio_present" and
            observation["output_scope_matches"] and
            observation["time_window_matches"] for attempt in attempts
            for observation in attempt["output_observations"])
        external_audio_present = sum(
            observation["kind"] == "audio_present" and
            observation["output_scope_matches"] and
            observation["time_window_matches"] and
            observation["observer_kind"] == "external_monitor" and
            observation.get("observer_id") is not None
            for attempt in attempts for observation in attempt["output_observations"])
        rows.append({
            "row_kind": ("cross_date_attempt" if entry["_id"] in cross_date_entries
                         else "plan_entry"),
            "service_date": cross_date_dates.get(entry["_id"], plan["service_date"]),
            "planned_service_date": plan["service_date"],
            "plan_id": plan["_id"], "plan_revision_id": revision["_id"],
            "planning_basis": ("cross_date_attempt" if entry["_id"] in cross_date_entries
                               else "selected_revision" if
                               selected.get(plan["_id"]) == revision["_id"]
                               else "superseded_attempt"),
            "plan_entry_id": entry["_id"], "ordinal": entry["ordinal"],
            "scheduled_at": entry["scheduled_at"],
            "event_kind": entry["event_kind"], "planned_state": entry["state"],
            "result": _row_state(entry, attempts, as_of),
            "version_id": version_id, "title": item["title"] if item else None,
            "output_id": (next(iter(known_outputs)) if len(known_outputs) == 1 else None),
            "output_scope_issue": ("conflicting_planned_and_actual_outputs" if
                                   len(known_outputs) > 1 else
                                   "unknown_output" if not known_outputs else None),
            "attempts": attempts, "engine_completed_attempts": completed,
            "audio_present_observations": audio_present,
            "external_audio_present_observations": external_audio_present,
            "advertising_placements": placement_rows,
        })

    for attempt in attempts_by_entry.get(None, []):
        version = store.get("versions", attempt["version_id"])
        item = store.get("items", version["item_id"]) if version else None
        _add_source(sources, "versions", version)
        _add_source(sources, "items", item)
        first_event_at = min(
            (event["occurred_at"] for event in relevant_events[attempt["attempt_id"]]))
        rows.append({
            "row_kind": "off_plan", "service_date": _instant(
                first_event_at, "occurred_at").astimezone(zone).date().isoformat(),
            "planned_service_date": None,
            "plan_id": None, "plan_revision_id": None,
            "planning_basis": "off_plan", "plan_entry_id": None,
            "ordinal": None, "scheduled_at": None, "event_kind": "audio",
            "planned_state": None, "result": attempt["state"],
            "version_id": attempt["version_id"],
            "title": item["title"] if item else None,
            "output_id": attempt["output_id"],
            "output_scope_issue": attempt["output_scope_issue"],
            "attempts": [attempt],
            "engine_completed_attempts": int(attempt["state"] == "completed"),
            "audio_present_observations": sum(
                row["kind"] == "audio_present" and row["output_scope_matches"] and
                row["time_window_matches"]
                for row in attempt["output_observations"]),
            "external_audio_present_observations": sum(
                row["kind"] == "audio_present" and row["output_scope_matches"] and
                row["time_window_matches"] and
                row["observer_kind"] == "external_monitor" and
                row.get("observer_id") is not None
                for row in attempt["output_observations"]),
            "advertising_placements": [],
        })
    rows.sort(key=lambda row: (row["service_date"], row["scheduled_at"] or "",
                               row["plan_entry_id"] or "", row["row_kind"]))
    for obligation_id, ids in obligation_observation_ids.items():
        obligation_summary[obligation_id]["audio_present_observations_in_report"] = len(ids)
    summary = {state: sum(row["result"] == state for row in rows)
               for state in ("planned", "started", "completed", "skipped", "failed", "unknown")}
    snapshot = {
        "station_id": station_id, "date_from": first.isoformat(),
        "date_to": last.isoformat(), "output_id": output_id,
        "as_of_at": as_of.isoformat(), "summary": summary,
        "rows": rows,
        "ad_obligations": sorted(obligation_summary.values(),
                                 key=lambda row: row["obligation_id"]),
        "plans_without_approved_revision_as_of": sorted(
            plan_id for plan_id in scheduled_plan_ids if plan_id not in selected),
        "unlinked_output_observations": sorted(
            unlinked_observations,
            key=lambda row: (row["occurred_at"], row["observation_id"])),
        "scope_gaps": {"planned_entries_unknown_output": unscoped_planned,
                       "attempts_unknown_or_conflicting_output": unknown_attempt_output,
                       "observations_unknown_output": unscoped_observations},
        "evidence_notes": [
            "completed is an engine claim, not acoustic proof",
            "audio_present indicates output activity, not exact content identity",
            "audio_present counts require an explicit matching output id",
            "audio_present counts require matching completed-attempt time windows",
            "obligation required plays cover the full obligation window, not only this report",
        ],
    }
    source_digest = _hash([sources[key] for key in sorted(sources)])
    return snapshot, source_digest


def preview_asrun(store: Store, principal: Principal, station_id: str,
                  date_from: str, date_to: str, *,
                  output_id: str | None = None,
                  as_of_at: str | datetime | None = None) -> dict:
    """Compute a read-only report with a checked source sequence."""
    station_id, first, last, output_id = _filters(
        station_id, date_from, date_to, output_id)
    require(store, principal, "view", station_id)
    as_of = (_instant(as_of_at, "as_of_at") if as_of_at is not None
             else datetime.now(timezone.utc))
    sequence = store.sequence
    snapshot, source_hash = _build(store, station_id, first, last, output_id, as_of)
    if store.sequence != sequence:
        raise Conflict("report source changed during preview; retry")
    return {"source_sequence": sequence, "source_hash": source_hash,
            "snapshot_hash": _hash(snapshot), "snapshot": snapshot}


def create_report_run(store: Store, principal: Principal, station_id: str,
                      date_from: str, date_to: str, *, idempotency_key: str,
                      output_id: str | None = None,
                      as_of_at: str | datetime | None = None) -> dict:
    """Persist one immutable snapshot; the same key returns the same report."""
    station_id, first, last, output_id = _filters(
        station_id, date_from, date_to, output_id)
    require(store, principal, "operate", station_id)
    if not isinstance(idempotency_key, str) or not _KEY.fullmatch(idempotency_key):
        raise StoreError("idempotency_key must be an opaque 1..160 character code")
    explicit_as_of = (_instant(as_of_at, "as_of_at") if as_of_at is not None else None)
    previous = store.lookup("asrun_report_runs", "station_key", station_id,
                            idempotency_key)
    if previous:
        row = previous[0]
        if (row["date_from"] != first.isoformat() or
                row["date_to"] != last.isoformat() or
                row.get("output_id") != output_id or
                (explicit_as_of is not None and
                 row["as_of_at"] != explicit_as_of.isoformat())):
            raise Conflict("report key was reused for different filters")
        return row | {"replayed": True}
    as_of = explicit_as_of or datetime.now(timezone.utc)
    sequence = store.sequence
    snapshot, source_hash = _build(store, station_id, first, last, output_id, as_of)
    report_id = str(uuid4())
    record = {"station_id": station_id, "idempotency_key": idempotency_key,
              "date_from": first.isoformat(), "date_to": last.isoformat(),
              "output_id": output_id, "as_of_at": as_of.isoformat(),
              "status": "completed", "source_sequence": sequence,
              "source_hash": source_hash, "snapshot_hash": _hash(snapshot),
              "snapshot": snapshot, "created_by": principal.user_id,
              "created_at": datetime.now(timezone.utc).isoformat()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("asrun_report_runs", report_id, record)
    try:
        tx.commit()
    except Conflict:
        previous = store.lookup("asrun_report_runs", "station_key", station_id,
                                idempotency_key)
        if not previous:
            raise
        row = previous[0]
        if (row["date_from"] != first.isoformat() or
                row["date_to"] != last.isoformat() or
                row.get("output_id") != output_id or
                (explicit_as_of is not None and
                 row["as_of_at"] != explicit_as_of.isoformat())):
            raise Conflict("report key was reused for different filters")
        return row | {"replayed": True}
    return store.get("asrun_report_runs", report_id) | {"replayed": False}


def get_report_run(store: Store, principal: Principal, station_id: str,
                   report_id: str) -> dict:
    station_id, report_id = _uuid(station_id, "station_id"), _uuid(report_id, "report_id")
    require(store, principal, "view", station_id)
    row = store.get("asrun_report_runs", report_id)
    if row is None or row["station_id"] != station_id:
        raise Conflict("report run is outside station")
    if _hash(row["snapshot"]) != row["snapshot_hash"]:
        raise StoreError("stored report snapshot hash does not match")
    return row


def _csv_cell(value: Any) -> str:
    if value is None:
        return ""
    cell = str(value)
    if cell and cell.lstrip()[0:1] in "=+-@\t\r\n":
        return "'" + cell
    return cell


def export_report_run(store: Store, principal: Principal, station_id: str,
                      report_id: str, *, format: str = "json") -> str:
    """Export a stored snapshot; CSV escapes spreadsheet formulas in text."""
    row = get_report_run(store, principal, station_id, report_id)
    if format == "json":
        return json.dumps({
            "report_id": row["_id"], "source_sequence": row["source_sequence"],
            "source_hash": row["source_hash"],
            "snapshot_hash": row["snapshot_hash"],
            "snapshot": row["snapshot"],
        }, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False)
    if format != "csv":
        raise StoreError("report export format must be json or csv")
    fields = ("row_kind", "service_date", "planned_service_date", "scheduled_at", "plan_entry_id",
              "planning_basis", "title", "version_id", "output_id", "result",
              "attempt_count", "engine_completed_attempts",
              "audio_present_observations", "external_audio_present_observations",
              "campaign_id", "obligation_id",
              "placement_id", "contract_total_required_plays",
              "scheduled_placements_in_report",
              "engine_completed_placements_in_report")
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(fields)
    for result in row["snapshot"]["rows"]:
        placements = result["advertising_placements"] or [None]
        for placement in placements:
            values = {"row_kind": result["row_kind"],
                      "service_date": result["service_date"],
                      "planned_service_date": result["planned_service_date"],
                      "scheduled_at": result["scheduled_at"],
                      "plan_entry_id": result["plan_entry_id"],
                      "planning_basis": result["planning_basis"],
                      "title": result["title"], "version_id": result["version_id"],
                      "output_id": result["output_id"], "result": result["result"],
                      "attempt_count": len(result["attempts"]),
                      "engine_completed_attempts": result["engine_completed_attempts"],
                      "audio_present_observations": result["audio_present_observations"],
                      "external_audio_present_observations": result[
                          "external_audio_present_observations"],
                      "campaign_id": placement["campaign_id"] if placement else None,
                      "obligation_id": placement["obligation_id"] if placement else None,
                      "placement_id": placement["placement_id"] if placement else None}
            writer.writerow([_csv_cell(values.get(field)) for field in fields])
    for obligation in row["snapshot"]["ad_obligations"]:
        values = {"row_kind": "obligation", "obligation_id": obligation["obligation_id"],
                  "campaign_id": obligation["campaign_id"],
                  "contract_total_required_plays": obligation["contract_total_required_plays"],
                  "scheduled_placements_in_report": obligation["scheduled_placements_in_report"],
                  "engine_completed_placements_in_report": obligation["engine_completed_placements_in_report"]}
        writer.writerow([_csv_cell(values.get(field)) for field in fields])
    return output.getvalue()
