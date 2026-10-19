"""Guarded automatic selection for category slots in a published radio clock.

This service resolves approved catalog versions for a clock hour. It preserves
the clock's fixed times and refuses to invent content for an empty category.
Selection is deterministic and its rule, candidate fingerprint and reasons are
bound to the materialized plan. It does not run a playout engine.
"""

from __future__ import annotations

from collections import Counter
from datetime import date, datetime, time, timedelta, timezone
from typing import Any
from zoneinfo import ZoneInfo

from .clocks import (_AUTO_SELECTION_CAPABILITY, _current_assignment, _day, _hash, _id, _int,
                     _slot_payload_scope,
                     materialize_clock_hour, preview_clock_hour)
from .identity import Principal, require
from .planning import explain_rotation
from .readiness import readiness_at
from .rotation import (_GAP_KEYS, _config, _identity_for_version, _recent_history,
                       evaluate_rotation)
from .schedules import _local_instant
from .storage import Conflict, Store, StoreError


_ALGORITHM = "least_recent_eligible_v1"
_RECENCY_MINUTES = 7 * 24 * 60


def _candidates(store: Store, station_id: str, category_id: str) -> list[str]:
    """Use station scope, then the item/version indexes; never search other stations."""
    found: list[str] = []
    for link in store.lookup("station_items", "station", station_id):
        if link.get("category_id") != category_id or not link.get("eligible"):
            continue
        item = store.get("items", link["item_id"])
        if item is None or item["lifecycle"] != "active":
            continue
        for version in store.lookup("versions", "item", item["_id"]):
            if version["status"] == "approved":
                found.append(version["_id"])
    return sorted(found)


def _existing_plan_history(store: Store, station_id: str, day: date,
                           excluded_entry_ids: set[str],
                           service_key: str = "default") -> list[dict]:
    plans = store.lookup("plans", "station_date_service", station_id,
                         day.isoformat(), service_key)
    if not plans:
        return []
    plan = plans[0]
    revision_id = plan.get("draft_revision_id") or plan.get("current_revision_id")
    if not revision_id:
        return []
    history: list[dict] = []
    for entry in store.lookup("plan_entries", "revision", revision_id):
        if entry["_id"] in excluded_entry_ids or entry["state"] != "planned" or \
                entry["event_kind"] != "audio":
            continue
        version_id = entry.get("payload", {}).get("version_id")
        if not version_id:
            continue
        played_at = datetime.fromisoformat(entry["scheduled_at"].replace("Z", "+00:00"))
        history.append(_identity_for_version(store, station_id, version_id, played_at) |
                       {"version_id": version_id, "played_at": played_at.isoformat()})
    return history


def _append_planned(store: Store, station_id: str, version_id: str,
                    at: datetime, history: list[dict]) -> None:
    history.append(_identity_for_version(store, station_id, version_id, at) |
                   {"version_id": version_id, "played_at": at.isoformat()})


def _latest_by_version(history: list[dict], at: datetime) -> dict[str, datetime]:
    last: dict[str, datetime] = {}
    for row in history:
        version_id = row.get("version_id")
        if not version_id:
            continue
        played = datetime.fromisoformat(row["played_at"].replace("Z", "+00:00"))
        if played > at:
            continue
        if played > last.get(version_id, datetime.min.replace(tzinfo=timezone.utc)):
            last[version_id] = played
    return last


def _slot_duration(slot: dict) -> int | None:
    value = slot["entry_template"].get("planned_duration_ms")
    return value if isinstance(value, int) and not isinstance(value, bool) and value > 0 else None


def preview_auto_clock_hour(store: Store, principal: Principal, station_id: str,
                            service_date: str | date, local_hour: int, *,
                            rule_id: str, ordinal_base: int | None = None,
                            territory: str = "*", output_id: str = "*",
                            service_key: str = "default") -> dict:
    """Select category versions and preview a complete hour with diagnostics.

    A published clock and an active rotation rule are required. A missing or
    ineligible category produces ``needs_resolution`` and a reason; no content
    is silently inserted. Existing automatic materializations replay their
    original selection and are not regenerated from a changed catalog.
    """
    station_id, rule_id = _id(station_id, "station_id"), _id(rule_id, "rule_id")
    require(store, principal, "view", station_id)
    from .broadcast_services import require_service, service_key as checked_service_key
    service_key = checked_service_key(service_key)
    binding = require_service(store, station_id, service_key)
    if binding is not None:
        if output_id not in {"*", binding["output_id"]}:
            raise Conflict("automatic clock output differs from broadcast service output")
        output_id = binding["output_id"]
    day = _day(service_date, "service_date")
    hour = _int(local_hour, "local_hour", 0, 23)
    if any(not isinstance(value, str) or not value.strip()
           for value in (territory, output_id)):
        raise StoreError("territory and output_id must be nonempty strings")
    baseline = preview_clock_hour(store, principal, station_id, day, hour,
                                  ordinal_base=ordinal_base,
                                  territory=territory, output_id=output_id,
                                  service_key=service_key)
    if baseline["status"] in {"no_assignment", "skipped"}:
        return baseline | {"auto_selection": [], "resolved_versions": {}}
    assignment_id = baseline["assignment_id"]
    material_rows = store.lookup("clock_materializations", "assignment_date",
                                 assignment_id, day.isoformat())
    prior = material_rows[0] if material_rows else None
    if prior is not None:
        provenance = prior.get("selection_provenance")
        if not isinstance(provenance, dict) or provenance.get("kind") != "automatic_rotation" or \
                provenance.get("rule_id") != rule_id:
            raise Conflict("this clock hour belongs to a different materialization policy")
        if provenance.get("territory") != territory or provenance.get("output_id") != output_id:
            raise Conflict("this clock hour was selected for another rights scope")
        choices = provenance["choices"]
        preview = preview_clock_hour(store, principal, station_id, day, hour,
                                     resolved_versions=choices, ordinal_base=ordinal_base,
                                     selection_provenance=provenance,
                                     territory=territory, output_id=output_id,
                                     service_key=service_key,
                                     _selection_capability=_AUTO_SELECTION_CAPABILITY)
        return preview | {"auto_selection": provenance.get("slots", []),
                          "resolved_versions": choices, "replay_selection": True}

    rule = store.get("rotation_rules", rule_id)
    if rule is None or rule["station_id"] != station_id or rule["status"] != "active":
        raise Conflict("active rotation rule is required for this station")
    config = _config(rule["config"])

    station = store.get("stations", station_id)
    assignment = _current_assignment(store, station_id, day, hour, service_key)
    if station is None or assignment is None:
        raise Conflict("clock assignment changed during automatic preview")
    zone = ZoneInfo(station["timezone"])
    first, _ = _local_instant(day, time(hour, 0).isoformat(), zone,
                              assignment["dst_fold"])
    if first is None:
        raise Conflict("clock hour entered a DST gap during preview")
    slots = sorted(store.lookup("clock_slots", "version", baseline["template_version_id"]),
                   key=lambda row: (row["ordinal"], row["_id"]))
    max_gap = max(config[key] for key in _GAP_KEYS)
    actual_history = _recent_history(store, station_id, first + timedelta(hours=1),
                                     max(max_gap + 60, _RECENCY_MINUTES),
                                     config["require_output_observation"])
    planned_history = _existing_plan_history(store, station_id, day, set(), service_key)
    choices: dict[str, str] = {}
    evidence: list[dict] = []
    selection_errors: list[dict] = []
    for slot in slots:
        at = first + timedelta(milliseconds=slot["offset_ms"])
        payload, slot_territory, slot_output = _slot_payload_scope(
            slot["entry_template"], territory, output_id)
        if slot["slot_kind"] == "fixed":
            if slot["entry_template"]["event_kind"] == "audio":
                version_id = payload.get("version_id")
                duration_ms = _slot_duration(slot)
                if not version_id or duration_ms is None:
                    selection_errors.append({"slot_id": slot["_id"],
                                             "code": "fixed_audio_unresolved",
                                             "detail": "fixed audio needs a version and positive planned_duration_ms"})
                    continue
                ready = readiness_at(store, station_id, version_id,
                                     slot_start=at,
                                     slot_end=at + timedelta(milliseconds=duration_ms),
                                     require_endpoint_copy=False,
                                     territory=slot_territory, output_id=slot_output)
                if not ready["ready"]:
                    selection_errors.append({"slot_id": slot["_id"],
                                             "code": "fixed_audio_not_ready",
                                             "detail": ready["blocked_reasons"]})
                _append_planned(store, station_id, version_id, at, planned_history)
            continue
        duration_ms = _slot_duration(slot)
        if duration_ms is None:
            selection_errors.append({"slot_id": slot["_id"],
                                     "code": "category_duration_required",
                                     "detail": "automatic category slots need a positive planned_duration_ms"})
            continue
        candidate_ids = _candidates(store, station_id, slot["category_id"])
        if not candidate_ids:
            selection_errors.append({"slot_id": slot["_id"], "code": "category_empty",
                                     "detail": "no approved eligible versions in category"})
            continue
        evaluated = evaluate_rotation(store, principal, station_id, rule_id,
                                      candidate_ids, at=at,
                                      slot_duration_ms=duration_ms,
                                      territory=slot_territory, output_id=slot_output)
        identities = [_identity_for_version(store, station_id, candidate_id, at)
                      for candidate_id in candidate_ids]
        scheduled = explain_rotation(identities, planned_history, at=at, rules=config)
        if scheduled["errors"]:
            raise StoreError("planned rotation evaluation failed")
        decisions = []
        for base, future in zip(evaluated["decisions"], scheduled["decisions"]):
            reasons = list(dict.fromkeys(base["reasons"] + future["reasons"]))
            decisions.append({"version_id": base["candidate_id"],
                              "eligible": not reasons, "reasons": reasons})
        eligible = [row["version_id"] for row in decisions if row["eligible"]]
        counts = dict(sorted(Counter(reason for row in decisions for reason in row["reasons"]).items()))
        item = {"slot_id": slot["_id"], "category_id": slot["category_id"],
                "scheduled_at": at.isoformat(), "candidate_count": len(candidate_ids),
                "eligible_count": len(eligible), "rejection_reasons": counts,
                "territory": slot_territory, "output_id": slot_output,
                "candidate_fingerprint": _hash(candidate_ids)}
        if not eligible:
            selection_errors.append({"slot_id": slot["_id"], "code": "no_eligible_candidate",
                                     "detail": counts})
            evidence.append(item | {"selected_version_id": None})
            continue
        last_used = _latest_by_version(actual_history + planned_history, at)
        selected = min(eligible, key=lambda identifier: (
            last_used.get(identifier, datetime.min.replace(tzinfo=timezone.utc)),
            -store.get("versions", identifier)["version_no"], identifier))
        choices[slot["_id"]] = selected
        _append_planned(store, station_id, selected, at, planned_history)
        evidence.append(item | {"selected_version_id": selected})

    provenance = {"kind": "automatic_rotation", "algorithm": _ALGORITHM,
                  "service_key": service_key,
                  "rule_id": rule_id, "rule_revision": rule["_rev"],
                  "territory": territory, "output_id": output_id,
                  "choices": choices, "slots": evidence}
    preview = preview_clock_hour(store, principal, station_id, day, hour,
                                 resolved_versions=choices, ordinal_base=ordinal_base,
                                 selection_provenance=provenance,
                                 territory=territory, output_id=output_id,
                                 service_key=service_key,
                                 _selection_capability=_AUTO_SELECTION_CAPABILITY)
    if selection_errors:
        preview["errors"].extend(selection_errors)
        preview["status"] = "needs_resolution"
    if preview["sequence"] != baseline["sequence"]:
        raise Conflict("rotation source data changed during automatic preview")
    return preview | {"auto_selection": evidence, "resolved_versions": choices,
                      "replay_selection": False}


def materialize_auto_clock_hour(store: Store, principal: Principal, station_id: str,
                                service_date: str | date, local_hour: int, *,
                                rule_id: str, expected_sequence: int,
                                ordinal_base: int | None = None,
                                territory: str = "*", output_id: str = "*",
                                service_key: str = "default") -> dict:
    """Commit the selected hour only if its preview still matches the store."""
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    preview = preview_auto_clock_hour(store, principal, station_id,
                                      service_date, local_hour, rule_id=rule_id,
                                      ordinal_base=ordinal_base,
                                      territory=territory, output_id=output_id,
                                      service_key=service_key)
    if preview["sequence"] != expected_sequence:
        raise Conflict("database changed since automatic clock preview")
    if preview["status"] != "ready":
        raise Conflict("automatic clock hour is not ready to materialize")
    result = materialize_clock_hour(store, principal, station_id,
                                    service_date, local_hour,
                                    resolved_versions=preview["resolved_versions"],
                                    ordinal_base=ordinal_base,
                                    selection_provenance=preview["selection_provenance"],
                                    territory=territory, output_id=output_id,
                                    service_key=service_key,
                                    _selection_capability=_AUTO_SELECTION_CAPABILITY,
                                    expected_sequence=expected_sequence)
    return result | {"auto_selection": preview["auto_selection"]}
