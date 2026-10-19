"""Pure radio advertising decisions for the SDBASE document engine.

All inputs and outputs are dictionaries; this module performs no I/O. Times must
be timezone-aware ISO 8601 strings or ``datetime`` values. Intervals are
half-open: ``starts_at <= scheduled_at < ends_at``.

``select_break`` accepts a break with ``id``, ``starts_at``, ``capacity_ms``,
``station_id``, ``region_id`` and ``output_id``. Optional policies are
``max_spots`` (default: all supplied offers), ``advertiser_gap_slots`` and
``category_gap_slots`` (both default 1), and ``max_per_obligation`` (default 1).

Each offer has a unique ``id``, ``duration_ms``, and nested ``campaign``,
``spot`` and ``obligation`` dictionaries. A campaign needs ``id``,
``advertiser_id``, ``status``, ``starts_at`` and ``ends_at``. A spot needs
``id`` and ``status``; it may request ``position`` = any/first/last. An
obligation needs ``id``, ``required_plays``, ``window_start`` and
``window_end``. ``accepted_plays`` and ``reserved_plays`` default to zero.
Any of these dictionaries may restrict ``station_ids``, ``region_ids`` or
``output_ids``; the obligation may instead have a single ``station_id``.
Omitted target sets mean unrestricted, while an empty set means no targets.

Selection is deterministic best effort, not a global revenue optimizer. It
never silently overbooks capacity or an obligation. Each rejected offer has a
machine-readable reason. A makegood proposal always requires human approval.
"""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timedelta, timezone
from typing import Any, Iterable, Mapping


_CAMPAIGN_STATES = frozenset({"approved", "active"})
_START_PHASES = frozenset({"start", "manual_start"})
_COMPLETE_PHASES = frozenset({"complete", "manual_complete"})
_FAIL_PHASES = frozenset({"fail", "interrupt", "skip"})


def _instant(value: Any, field: str) -> datetime:
    if isinstance(value, datetime):
        result = value
    elif isinstance(value, str):
        try:
            result = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError as exc:
            raise ValueError(f"{field} must be an ISO 8601 instant") from exc
    else:
        raise ValueError(f"{field} must be an ISO 8601 instant")
    if result.tzinfo is None or result.utcoffset() is None:
        raise ValueError(f"{field} must include a timezone")
    return result.astimezone(timezone.utc)


def _whole(value: Any, field: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ValueError(f"{field} must be an integer >= {minimum}")
    return value


def _id(value: Any, field: str) -> str:
    if value is None or not str(value).strip():
        raise ValueError(f"{field} is required")
    return str(value)


def _target_reasons(source: Mapping[str, Any], slot: Mapping[str, Any]) -> list[str]:
    reasons: list[str] = []
    for field, target in (("station_ids", "station_id"),
                          ("region_ids", "region_id"),
                          ("output_ids", "output_id")):
        allowed = source.get(field)
        if allowed is None:
            continue
        if isinstance(allowed, (str, bytes)) or not isinstance(allowed, (list, tuple, set)):
            reasons.append(f"invalid_{field}")
        elif slot.get(target) is None or str(slot[target]) not in {str(x) for x in allowed}:
            reasons.append(f"{target}_not_targeted")
    single_station = source.get("station_id")
    if single_station is not None and str(single_station) != str(slot.get("station_id")):
        reasons.append("station_id_not_targeted")
    return reasons


def _window(source: Mapping[str, Any], start_field: str, end_field: str,
            *, optional: bool = False) -> tuple[datetime | None, datetime | None]:
    start_value = source.get(start_field)
    end_value = source.get(end_field)
    if optional and start_value is None and end_value is None:
        return None, None
    start = _instant(start_value, start_field) if start_value is not None else None
    end = _instant(end_value, end_field) if end_value is not None else None
    if not optional and (start is None or end is None):
        raise ValueError(f"{start_field} and {end_field} are required")
    if start is not None and end is not None and end <= start:
        raise ValueError(f"{end_field} must be later than {start_field}")
    return start, end


def _within(at: datetime, start: datetime | None, end: datetime | None) -> bool:
    return (start is None or start <= at) and (end is None or at < end)


def _overlaps(start: datetime, end: datetime, valid_start: datetime | None,
              valid_end: datetime | None) -> bool:
    return (valid_start is None or valid_start < end) and (valid_end is None or valid_end > start)


def _offer_reasons(offer: Mapping[str, Any], slot: Mapping[str, Any],
                   *, at: datetime | None = None, break_end: datetime | None = None) -> list[str]:
    reasons: list[str] = []
    try:
        _id(offer.get("id"), "offer.id")
        _whole(offer.get("duration_ms"), "duration_ms", minimum=1)
        campaign = offer["campaign"]
        spot = offer["spot"]
        obligation = offer["obligation"]
        if not all(isinstance(part, Mapping) for part in (campaign, spot, obligation)):
            raise ValueError("campaign, spot and obligation must be objects")
        _id(campaign.get("id"), "campaign.id")
        _id(campaign.get("advertiser_id"), "campaign.advertiser_id")
        _id(spot.get("id"), "spot.id")
        _id(obligation.get("id"), "obligation.id")
        required = _whole(obligation.get("required_plays"), "required_plays", minimum=1)
        accepted = _whole(obligation.get("accepted_plays", 0), "accepted_plays")
        reserved = _whole(obligation.get("reserved_plays", 0), "reserved_plays")
        priority = offer.get("priority", obligation.get("priority", 0))
        _whole(priority, "priority")
        campaign_window = _window(campaign, "starts_at", "ends_at")
        spot_window = _window(spot, "starts_at", "ends_at", optional=True)
        obligation_window = _window(obligation, "window_start", "window_end")
        if accepted + reserved >= required:
            reasons.append("obligation_satisfied")
        if campaign.get("status") not in _CAMPAIGN_STATES:
            reasons.append("campaign_not_active")
        if spot.get("status") != "approved":
            reasons.append("spot_not_approved")
        if spot.get("campaign_id") is not None and str(spot["campaign_id"]) != str(campaign["id"]):
            reasons.append("spot_campaign_mismatch")
        if (obligation.get("campaign_id") is not None
                and str(obligation["campaign_id"]) != str(campaign["id"])):
            reasons.append("obligation_campaign_mismatch")
        if spot.get("position", "any") not in {"any", "first", "last"}:
            reasons.append("invalid_position")
        reasons.extend(_target_reasons(campaign, slot))
        reasons.extend(_target_reasons(spot, slot))
        reasons.extend(_target_reasons(obligation, slot))
        if at is not None:
            for label, (start, end) in (("campaign", campaign_window),
                                        ("spot", spot_window),
                                        ("obligation", obligation_window)):
                if not _within(at, start, end):
                    reasons.append(f"{label}_outside_window")
            if spot.get("approved_at") is not None and _instant(spot["approved_at"], "approved_at") > at:
                reasons.append("spot_approval_in_future")
        elif break_end is not None:
            break_start = _instant(slot["starts_at"], "break.starts_at")
            for label, (start, end) in (("campaign", campaign_window),
                                        ("spot", spot_window),
                                        ("obligation", obligation_window)):
                if not _overlaps(break_start, break_end, start, end):
                    reasons.append(f"{label}_outside_window")
            if spot.get("approved_at") is not None and _instant(spot["approved_at"], "approved_at") >= break_end:
                reasons.append("spot_approval_in_future")
    except (KeyError, TypeError, ValueError) as exc:
        reasons.append(f"invalid_offer:{exc}")
    return list(dict.fromkeys(reasons))


def _categories(offer: Mapping[str, Any]) -> set[str]:
    value = offer.get("competitive_categories",
                      offer.get("campaign", {}).get("competitive_categories", []))
    if isinstance(value, str):
        return {value} if value else set()
    if not isinstance(value, (list, tuple, set)):
        return set()
    return {str(x) for x in value if str(x)}


def _separation_reason(selected: list[Mapping[str, Any]], offer: Mapping[str, Any],
                       advertiser_gap: int, category_gap: int) -> str | None:
    advertiser = str(offer["campaign"]["advertiser_id"])
    categories = _categories(offer)
    for index, preceding in enumerate(selected):
        between = len(selected) - index - 1
        if (between < advertiser_gap
                and advertiser == str(preceding["campaign"]["advertiser_id"])):
            return "advertiser_separation"
        if between < category_gap and categories & _categories(preceding):
            return "category_separation"
    return None


def _rank(offer: Mapping[str, Any]) -> tuple[Any, ...]:
    obligation = offer["obligation"]
    remaining = (obligation["required_plays"] - obligation.get("accepted_plays", 0)
                 - obligation.get("reserved_plays", 0))
    return (-offer.get("priority", obligation.get("priority", 0)),
            _instant(obligation["window_end"], "window_end"), -remaining,
            str(offer["id"]))


def select_break(break_slot: Mapping[str, Any], offers: Iterable[Mapping[str, Any]]) -> dict:
    """Propose a capacity-safe break, with exact reasons for excluded offers.

    ``selected`` is in playout order and contains each offer's actual
    ``scheduled_at``. The result is a proposal, not an executed broadcast.
    """
    slot = dict(break_slot)
    slot_id = _id(slot.get("id"), "break.id")
    slot_start = _instant(slot.get("starts_at"), "break.starts_at")
    capacity = _whole(slot.get("capacity_ms"), "capacity_ms")
    all_offers = [dict(x) for x in offers]
    max_spots = _whole(slot.get("max_spots", max(1, len(all_offers))),
                       "max_spots", minimum=1)
    advertiser_gap = _whole(slot.get("advertiser_gap_slots", 1), "advertiser_gap_slots")
    category_gap = _whole(slot.get("category_gap_slots", 1), "category_gap_slots")
    max_per_obligation = _whole(slot.get("max_per_obligation", 1), "max_per_obligation", minimum=1)
    _id(slot.get("station_id"), "break.station_id")
    _id(slot.get("output_id"), "break.output_id")
    if slot.get("region_id") is not None:
        _id(slot["region_id"], "break.region_id")
    end = slot_start + timedelta(milliseconds=capacity)

    seen: set[str] = set()
    rejected: list[dict] = []
    valid: list[dict] = []
    for offer in all_offers:
        key = str(offer.get("id", ""))
        reasons = _offer_reasons(offer, slot, break_end=end)
        if key in seen:
            reasons.append("duplicate_offer_id")
        seen.add(key)
        duration = offer.get("duration_ms")
        if isinstance(duration, int) and not isinstance(duration, bool) and duration > capacity:
            reasons.append("capacity_exceeded")
        if reasons:
            rejected.append({"offer_id": key or None, "reasons": list(dict.fromkeys(reasons))})
        else:
            valid.append(offer)
    valid.sort(key=_rank)

    firsts = [None, *[o for o in valid if o["spot"].get("position") == "first"][:16]]
    lasts = [None, *[o for o in valid if o["spot"].get("position") == "last"][:16]]
    normal = [o for o in valid if o["spot"].get("position", "any") == "any"]
    best: list[dict] = []
    best_score: tuple[Any, ...] | None = None

    for first in firsts:
        for last in lasts:
            if first is not None and last is not None and first["id"] == last["id"]:
                continue
            selected: list[dict] = []
            used = 0
            per_obligation: dict[str, int] = {}
            if first is not None:
                if _offer_reasons(first, slot, at=slot_start):
                    continue
                selected.append(first)
                used += first["duration_ms"]
                oid = str(first["obligation"]["id"])
                per_obligation[oid] = 1
            reserve_ms = last["duration_ms"] if last is not None else 0
            if used + reserve_ms > capacity or len(selected) + int(last is not None) > max_spots:
                continue

            remaining_normal = normal.copy()
            while remaining_normal and len(selected) + int(last is not None) < max_spots:
                chosen = None
                for candidate in remaining_normal:
                    oid = str(candidate["obligation"]["id"])
                    available = (candidate["obligation"]["required_plays"]
                                 - candidate["obligation"].get("accepted_plays", 0)
                                 - candidate["obligation"].get("reserved_plays", 0))
                    if per_obligation.get(oid, 0) >= min(available, max_per_obligation):
                        continue
                    if used + candidate["duration_ms"] + reserve_ms > capacity:
                        continue
                    if _offer_reasons(candidate, slot,
                                      at=slot_start + timedelta(milliseconds=used)):
                        continue
                    if _separation_reason(selected, candidate, advertiser_gap, category_gap):
                        continue
                    chosen = candidate
                    break
                if chosen is None:
                    break
                selected.append(chosen)
                used += chosen["duration_ms"]
                oid = str(chosen["obligation"]["id"])
                per_obligation[oid] = per_obligation.get(oid, 0) + 1
                remaining_normal.remove(chosen)

            if last is not None:
                oid = str(last["obligation"]["id"])
                available = (last["obligation"]["required_plays"]
                             - last["obligation"].get("accepted_plays", 0)
                             - last["obligation"].get("reserved_plays", 0))
                if per_obligation.get(oid, 0) >= min(available, max_per_obligation):
                    continue
                if (_offer_reasons(last, slot, at=slot_start + timedelta(milliseconds=used))
                        or _separation_reason(selected, last, advertiser_gap, category_gap)):
                    continue
                selected.append(last)
                used += last["duration_ms"]
            priority = sum(o.get("priority", o["obligation"].get("priority", 0)) for o in selected)
            score = (priority, len(selected), used, tuple(str(o["id"]) for o in selected))
            if best_score is None or score[:3] > best_score[:3] or (
                    score[:3] == best_score[:3] and score[3] < best_score[3]):
                best, best_score = selected, score

    selected_ids = {str(o["id"]) for o in best}
    used_ms = 0
    placements: list[dict] = []
    for position, offer in enumerate(best):
        placements.append({"offer_id": str(offer["id"]), "spot_id": str(offer["spot"]["id"]),
                           "campaign_id": str(offer["campaign"]["id"]),
                           "obligation_id": str(offer["obligation"]["id"]),
                           "position": position + 1,
                           "scheduled_at": (slot_start + timedelta(milliseconds=used_ms)).isoformat(),
                           "duration_ms": offer["duration_ms"],
                           "is_makegood": bool(offer.get("is_makegood", False))})
        used_ms += offer["duration_ms"]

    for offer in valid:
        if str(offer["id"]) in selected_ids:
            continue
        position = offer["spot"].get("position", "any")
        reasons: list[str] = []
        if position == "first" and placements:
            reasons.append("first_position_taken")
        elif position == "last" and placements:
            reasons.append("last_position_taken_or_incompatible")
        if used_ms + offer["duration_ms"] > capacity or len(placements) >= max_spots:
            reasons.append("capacity_exceeded")
        selected_for_obligation = sum(
            1 for placement in placements
            if placement["obligation_id"] == str(offer["obligation"]["id"]))
        if selected_for_obligation >= max_per_obligation:
            reasons.append("per_break_obligation_limit")
        if _separation_reason(best, offer, advertiser_gap, category_gap):
            reasons.append("advertiser_or_category_separation")
        if not reasons:
            reasons.append("lower_selection_priority")
        rejected.append({"offer_id": str(offer["id"]), "reasons": reasons})

    planned_by_obligation: dict[str, int] = {}
    for placement in placements:
        key = placement["obligation_id"]
        planned_by_obligation[key] = planned_by_obligation.get(key, 0) + 1
    obligations: dict[str, dict] = {}
    for offer in all_offers:
        obligation = offer.get("obligation")
        if not isinstance(obligation, Mapping) or not obligation.get("id"):
            continue
        oid = str(obligation["id"])
        try:
            remaining = max(0, _whole(obligation["required_plays"], "required_plays", minimum=1)
                            - _whole(obligation.get("accepted_plays", 0), "accepted_plays")
                            - _whole(obligation.get("reserved_plays", 0), "reserved_plays")
                            - planned_by_obligation.get(oid, 0))
        except (KeyError, ValueError):
            continue
        obligations[oid] = {"obligation_id": oid, "remaining_after_break": remaining}
    rejected_by_offer = {row["offer_id"]: row["reasons"] for row in rejected}
    for oid, row in obligations.items():
        reasons = []
        for offer in all_offers:
            obligation = offer.get("obligation")
            if isinstance(obligation, Mapping) and str(obligation.get("id")) == oid:
                reasons.extend(rejected_by_offer.get(str(offer.get("id")), []))
        row["reasons"] = sorted(set(reasons)) or ["additional_placement_needed"]
    return {"break_id": slot_id, "selected": placements,
            "used_ms": used_ms, "remaining_capacity_ms": capacity - used_ms,
            "rejected": rejected,
            "unfilled_obligations": [o for o in obligations.values()
                                     if o["remaining_after_break"] > 0]}


def calculate_fulfillment(obligations: Iterable[Mapping[str, Any]],
                          events: Iterable[Mapping[str, Any]],
                          observations: Iterable[Mapping[str, Any]] = (),
                          *, placements: Iterable[Mapping[str, Any]] | None = None,
                          require_output: bool = True,
                          evidence_tolerance_seconds: int = 2) -> dict:
    """Count distinct completed play attempts with linked output evidence.

    Events need ``id``, ``attempt_id``, ``obligation_id``, ``station_id``,
    ``endpoint_id``, ``output_id``, ``phase`` and ``occurred_at``. Observations
    need ``attempt_id``, ``station_id``, ``endpoint_id``, ``output_id``,
    ``observation_type`` and ``observed_at``. An ``audio_present`` observation
    establishes output activity, not an audio fingerprint of the exact advert.
    """
    tolerance = _whole(evidence_tolerance_seconds, "evidence_tolerance_seconds")
    obligation_map: dict[str, dict] = {}
    for raw in obligations:
        obligation = dict(raw)
        oid = _id(obligation.get("id"), "obligation.id")
        if oid in obligation_map:
            raise ValueError(f"duplicate obligation: {oid}")
        _whole(obligation.get("required_plays"), "required_plays", minimum=1)
        _window(obligation, "window_start", "window_end")
        obligation_map[oid] = obligation
    placement_map: dict[str, dict] | None = None
    if placements is not None:
        placement_map = {}
        for raw in placements:
            placement = dict(raw)
            placement_id = _id(placement.get("id"), "placement.id")
            for field in ("obligation_id", "station_id", "output_id"):
                _id(placement.get(field), f"placement.{field}")
            if placement_id in placement_map:
                raise ValueError(f"duplicate placement: {placement_id}")
            placement_map[placement_id] = placement

    grouped: dict[str, list[dict]] = {}
    invalid_records: list[dict] = []
    event_ids: dict[str, dict] = {}
    conflicted_attempts: set[str] = set()
    for raw in events:
        event = dict(raw)
        try:
            eid = _id(event.get("id"), "event.id")
            aid = _id(event.get("attempt_id"), "event.attempt_id")
            _instant(event.get("occurred_at"), "event.occurred_at")
            if event.get("phase") not in _START_PHASES | _COMPLETE_PHASES | _FAIL_PHASES:
                raise ValueError("unsupported event.phase")
        except ValueError as exc:
            invalid_records.append({"kind": "event", "id": event.get("id"), "reason": str(exc)})
            continue
        if eid in event_ids:
            if event != event_ids[eid]:
                conflicted_attempts.update({aid, str(event_ids[eid].get("attempt_id"))})
                invalid_records.append({"kind": "event", "id": eid,
                                        "reason": "event_id_payload_conflict"})
            continue
        event_ids[eid] = event
        grouped.setdefault(aid, []).append(event)

    observed: dict[str, list[dict]] = {}
    observation_ids: dict[str, dict] = {}
    for raw in observations:
        observation = dict(raw)
        try:
            aid = _id(observation.get("attempt_id"), "observation.attempt_id")
            observation_id = _id(observation.get("id"), "observation.id")
            _instant(observation.get("observed_at"), "observation.observed_at")
            if observation.get("observation_type") not in {
                    "audio_present", "silence", "off_air", "monitor_error"}:
                raise ValueError("unsupported observation_type")
        except ValueError as exc:
            invalid_records.append({"kind": "observation", "id": observation.get("id"),
                                    "reason": str(exc)})
            continue
        if observation_id in observation_ids:
            if observation != observation_ids[observation_id]:
                conflicted_attempts.update({aid, str(observation_ids[observation_id].get("attempt_id"))})
                invalid_records.append({"kind": "observation", "id": observation_id,
                                        "reason": "observation_id_payload_conflict"})
            continue
        observation_ids[observation_id] = observation
        observed.setdefault(aid, []).append(observation)

    attempts: list[dict] = []
    accepted_by_obligation: dict[str, int] = {oid: 0 for oid in obligation_map}
    for aid, phases in sorted(grouped.items()):
        starts = [e for e in phases if e["phase"] in _START_PHASES]
        completes = [e for e in phases if e["phase"] in _COMPLETE_PHASES]
        failures = [e for e in phases if e["phase"] in _FAIL_PHASES]
        reasons: list[str] = []
        if aid in conflicted_attempts:
            reasons.append("evidence_id_payload_conflict")
        if failures:
            reasons.append("failed_or_interrupted")
        if not starts:
            reasons.append("missing_start")
        if not completes:
            reasons.append("missing_complete")
        if len({_instant(e["occurred_at"], "occurred_at") for e in starts}) > 1:
            reasons.append("ambiguous_start")
        if len({_instant(e["occurred_at"], "occurred_at") for e in completes}) > 1:
            reasons.append("ambiguous_complete")
        first = starts[0] if starts else phases[0]
        identity_fields = ("obligation_id", "placement_id", "station_id",
                           "region_id", "output_id", "endpoint_id")
        for field in ("obligation_id", "station_id", "output_id", "endpoint_id"):
            if first.get(field) is None:
                reasons.append(f"missing_{field}")
        for event in phases:
            if any(event.get(field) != first.get(field) for field in identity_fields):
                reasons.append("attempt_identity_conflict")
                break
        obligation_id = str(first.get("obligation_id"))
        obligation = obligation_map.get(obligation_id)
        if obligation is None:
            reasons.append("unknown_obligation")
        if placement_map is not None:
            placement_id = first.get("placement_id")
            placement = placement_map.get(str(placement_id)) if placement_id is not None else None
            if placement is None:
                reasons.append("unknown_or_missing_placement")
            elif any(placement.get(field) is not None
                     and str(placement[field]) != str(first.get(field))
                     for field in ("obligation_id", "station_id", "region_id",
                                   "output_id", "spot_id", "campaign_id")):
                reasons.append("placement_identity_conflict")
        start_at = _instant(starts[0]["occurred_at"], "occurred_at") if starts else None
        complete_at = _instant(completes[0]["occurred_at"], "occurred_at") if completes else None
        if start_at is not None and complete_at is not None and complete_at < start_at:
            reasons.append("complete_before_start")
        if obligation is not None and start_at is not None:
            window_start, window_end = _window(obligation, "window_start", "window_end")
            if not _within(start_at, window_start, window_end):
                reasons.append("obligation_outside_window")
            reasons.extend(_target_reasons(obligation, first))

        evidence_level = "started" if starts else "none"
        if completes:
            evidence_level = "engine_completed"
        output_evidence_id = None
        matching_output: list[dict] = []
        negative_output: list[dict] = []
        output_identity_conflict = False
        monitor_unavailable = False
        if start_at is not None and complete_at is not None:
            for obs in observed.get(aid, []):
                at = _instant(obs["observed_at"], "observed_at")
                if not (start_at - timedelta(seconds=tolerance) <= at
                        <= complete_at + timedelta(seconds=tolerance)):
                    continue
                if any(obs.get(field) != first.get(field)
                       for field in ("station_id", "output_id", "endpoint_id")):
                    output_identity_conflict = True
                    continue
                if obs["observation_type"] == "audio_present":
                    matching_output.append(obs)
                elif obs["observation_type"] in {"silence", "off_air"}:
                    negative_output.append(obs)
                elif obs["observation_type"] == "monitor_error":
                    monitor_unavailable = True
        if output_identity_conflict:
            reasons.append("output_identity_conflict")
        if negative_output:
            reasons.append("negative_output_observation")
        if monitor_unavailable and not matching_output and require_output:
            reasons.append("monitor_unavailable")
        if matching_output:
            matching_output.sort(key=lambda o: (_instant(o["observed_at"], "observed_at"),
                                                str(o.get("id", ""))))
            output_evidence_id = matching_output[0].get("id")
            evidence_level = "output_observed"
        elif require_output and completes:
            reasons.append("missing_output_evidence")

        soft_reasons = {"missing_start", "missing_complete", "missing_output_evidence",
                        "monitor_unavailable"}
        hard_failure = any(reason not in soft_reasons for reason in reasons)
        state = "rejected" if hard_failure else ("pending" if reasons else "accepted")
        if state == "accepted" and obligation is not None:
            accepted_by_obligation[obligation_id] += 1
        attempts.append({"attempt_id": aid, "obligation_id": obligation_id,
                         "placement_id": first.get("placement_id"), "state": state,
                         "evidence_level": evidence_level,
                         "start_at": start_at.isoformat() if start_at else None,
                         "complete_at": complete_at.isoformat() if complete_at else None,
                         "output_evidence_id": output_evidence_id,
                         "reasons": list(dict.fromkeys(reasons))})

    summaries = []
    for oid, obligation in obligation_map.items():
        required = obligation["required_plays"]
        accepted = accepted_by_obligation[oid]
        summaries.append({"obligation_id": oid, "required_plays": required,
                          "accepted_plays": accepted,
                          "shortfall": max(0, required - accepted),
                          "overdelivery": max(0, accepted - required),
                          "state": "fulfilled" if accepted >= required else "unfilled"})
    return {"obligations": summaries, "attempts": attempts,
            "invalid_records": invalid_records,
            "output_evidence_required": require_output,
            "placement_validation": placement_map is not None}


def suggest_makegoods(obligations: Iterable[Mapping[str, Any]],
                      offers: Iterable[Mapping[str, Any]],
                      future_breaks: Iterable[Mapping[str, Any]],
                      *, fulfillment: Mapping[str, Any], as_of: str | datetime) -> dict:
    """Propose unfilled obligation replacements in future compatible breaks.

    Proposals never change historical fulfillment and always need approval.
    Expired windows are reported for manual contract review, not silently
    extended. ``fulfillment`` is the result of ``calculate_fulfillment``.
    """
    now = _instant(as_of, "as_of")
    obligation_map = {_id(o.get("id"), "obligation.id"): dict(o) for o in obligations}
    accepted = {str(row["obligation_id"]): row["accepted_plays"]
                for row in fulfillment.get("obligations", [])}
    missing_summaries = set(obligation_map) - set(accepted)
    if missing_summaries:
        raise ValueError("fulfillment is missing obligations: "
                         + ", ".join(sorted(missing_summaries)))
    offered = [deepcopy(dict(offer)) for offer in offers]
    reserved: dict[str, int] = {oid: 0 for oid in obligation_map}
    proposals: list[dict] = []
    for break_slot in sorted(future_breaks,
                             key=lambda slot: _instant(slot.get("starts_at"), "break.starts_at")):
        start = _instant(break_slot.get("starts_at"), "break.starts_at")
        if start <= now:
            continue
        adjusted: list[dict] = []
        for base in offered:
            raw_obligation = base.get("obligation")
            if not isinstance(raw_obligation, Mapping):
                continue
            oid = str(raw_obligation.get("id"))
            if oid not in obligation_map:
                continue
            item = deepcopy(base)
            item["obligation"] = dict(obligation_map[oid])
            item["obligation"]["accepted_plays"] = accepted.get(oid, 0)
            item["obligation"]["reserved_plays"] = reserved[oid]
            item["is_makegood"] = True
            adjusted.append(item)
        result = select_break(break_slot, adjusted)
        for selected in result["selected"]:
            oid = selected["obligation_id"]
            reserved[oid] += 1
            proposals.append({"break_id": result["break_id"],
                              "offer_id": selected["offer_id"],
                              "spot_id": selected["spot_id"],
                              "obligation_id": oid,
                              "scheduled_at": selected["scheduled_at"],
                              "requires_approval": True,
                              "reason": "unfilled_obligation"})
    unresolved = []
    for oid, obligation in obligation_map.items():
        needed = max(0, obligation["required_plays"] - accepted.get(oid, 0) - reserved[oid])
        if not needed:
            continue
        window_end = _instant(obligation["window_end"], "window_end")
        unresolved.append({"obligation_id": oid, "remaining": needed,
                           "reason": ("window_expired_requires_contract_approval"
                                      if window_end <= now else "no_eligible_future_capacity")})
    return {"proposals": proposals, "unresolved": unresolved}
