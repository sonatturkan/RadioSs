"""Pure radio planning rules for the SDBASE storage engine.

This module does no I/O. Callers own persistence, permissions, version/copy
readiness, transactional cart state, and actual playout. Dates are offset-aware
instants; naive local times are rejected so DST gaps and repeated hours cannot
silently select the wrong broadcast instant.

Public functions accept mappings and return plain dictionaries. Error and
warning records use stable ``code`` values suitable for an API or an audit log.
"""

from __future__ import annotations

from datetime import date, datetime, time, timedelta, timezone
from hashlib import sha256
from typing import Any, Iterable, Mapping
from urllib.parse import urlparse
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError


_KINDS = frozenset({"audio", "live", "stream", "macro", "wait", "marker", "ad_break", "chain"})
_SOUND = frozenset({"audio", "live", "stream"})
_INSTANT = frozenset({"macro", "marker"})
_NON_OCCUPYING = frozenset({"macro", "marker", "ad_break"})
_STATES = frozenset({"planned", "skipped", "replaced", "cancelled"})
_UTC = timezone.utc


def _diag(code: str, detail: str, entry_id: Any = None, other_entry_id: Any = None) -> dict:
    result = {"code": code, "detail": detail}
    if entry_id is not None:
        result["entry_id"] = str(entry_id)
    if other_entry_id is not None:
        result["other_entry_id"] = str(other_entry_id)
    return result


def _iso(value: datetime | None) -> str | None:
    return value.astimezone(_UTC).isoformat().replace("+00:00", "Z") if value else None


def _aware(value: Any) -> datetime | None:
    if isinstance(value, datetime):
        parsed = value
    elif isinstance(value, str):
        try:
            parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError:
            return None
    else:
        return None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        return None
    if isinstance(parsed.tzinfo, ZoneInfo):
        roundtrip = parsed.astimezone(_UTC).astimezone(parsed.tzinfo)
        if (roundtrip.replace(tzinfo=None) != parsed.replace(tzinfo=None)
                or roundtrip.fold != parsed.fold):
            return None
    return parsed.astimezone(_UTC)


def _day(value: Any) -> date | None:
    if isinstance(value, date) and not isinstance(value, datetime):
        return value
    if isinstance(value, str):
        try:
            return date.fromisoformat(value)
        except ValueError:
            return None
    return None


def _zone(value: str) -> ZoneInfo | None:
    try:
        return ZoneInfo(value)
    except (ZoneInfoNotFoundError, TypeError, ValueError):
        return None


def _nonempty(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _positive_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def _nonnegative_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def _option(entry: Mapping, group: str, nested_key: str, flat_key: str, default: Any) -> Any:
    nested = entry.get(group)
    if isinstance(nested, Mapping) and nested_key in nested:
        return nested[nested_key]
    return entry.get(flat_key, default)


def _parse_entry(entry: Mapping[str, Any], zone: ZoneInfo | None,
                 service_date: date | None) -> dict:
    errors: list[dict] = []
    warnings: list[dict] = []
    entry_id = entry.get("id")
    ordinal = entry.get("ordinal")
    if not _nonnegative_int(ordinal):
        errors.append(_diag("invalid_ordinal", "ordinal must be a nonnegative integer", entry_id))
    kind = entry.get("event_kind", entry.get("kind"))
    if not isinstance(kind, str):
        kind = None
    if kind not in _KINDS:
        errors.append(_diag("invalid_event_kind", "unsupported tagged event kind", entry_id))
    if "kind" in entry and "event_kind" in entry and entry["kind"] != entry["event_kind"]:
        errors.append(_diag("conflicting_event_kind", "kind and event_kind disagree", entry_id))
    state = entry.get("state", "planned")
    if not isinstance(state, str) or state not in _STATES:
        errors.append(_diag("invalid_state", "unsupported plan entry state", entry_id))
    start = _aware(entry.get("scheduled_at"))
    if start is None:
        errors.append(_diag("timezone_required", "scheduled_at needs a valid UTC offset", entry_id))
    elif zone is not None and service_date is not None and start.astimezone(zone).date() != service_date:
        errors.append(_diag("outside_service_date", "entry start is outside the station local service date", entry_id))

    payload = entry.get("payload", {})
    if not isinstance(payload, Mapping):
        errors.append(_diag("invalid_payload", "payload must be an object", entry_id))
        payload = {}
    payload = dict(payload)
    if kind == "audio":
        if not (_nonempty(payload.get("version_id")) or _nonempty(payload.get("cart_id"))):
            errors.append(_diag("audio_target_required", "audio needs version_id or cart_id", entry_id))
        if payload.get("cut_id") is not None and not _nonempty(payload.get("cart_id")):
            errors.append(_diag("cut_without_cart", "cut_id requires cart_id", entry_id))
        if payload.get("item_id") is not None and not _nonempty(payload.get("item_id")):
            errors.append(_diag("invalid_item_id", "item_id must be nonempty", entry_id))
    elif kind == "live" and not _nonempty(payload.get("source_id")):
        errors.append(_diag("live_source_required", "live needs source_id", entry_id))
    elif kind == "stream":
        source = payload.get("source_id")
        uri = payload.get("uri")
        if not (_nonempty(source) or _nonempty(uri)):
            errors.append(_diag("stream_source_required", "stream needs source_id or uri", entry_id))
        if uri is not None:
            parsed = urlparse(uri) if isinstance(uri, str) else None
            if parsed is None or parsed.scheme not in {"http", "https", "rtsp", "rtmp", "srt", "udp", "icecast"} or not parsed.netloc:
                errors.append(_diag("invalid_stream_uri", "stream uri needs a supported scheme and host", entry_id))
    elif kind == "macro":
        if not _nonempty(payload.get("command")):
            errors.append(_diag("macro_command_required", "macro needs command", entry_id))
        if "args" in payload and not isinstance(payload["args"], Mapping):
            errors.append(_diag("invalid_macro_args", "macro args must be an object", entry_id))
    elif kind == "marker" and not _nonempty(payload.get("label")):
        errors.append(_diag("marker_label_required", "marker needs label", entry_id))
    elif kind == "ad_break" and not _nonempty(payload.get("break_id")):
        errors.append(_diag("ad_break_id_required", "ad break needs break_id", entry_id))
    elif kind == "chain" and not _nonempty(payload.get("target_plan_id")):
        errors.append(_diag("chain_target_required", "chain needs target_plan_id", entry_id))

    duration_ms = entry.get("planned_duration_ms")
    duration_kind = entry.get("duration_kind")
    if duration_kind is None:
        duration_kind = ("none" if kind in _INSTANT else
                         "fixed" if duration_ms is not None else "unknown")
    allowed_duration = ({"none"} if kind in _INSTANT else
                        {"fixed", "estimated", "unknown"} if kind in {"audio", "live", "stream", "chain"} else
                        {"fixed", "estimated", "unknown"} if kind == "ad_break" else
                        {"fixed", "unknown"})
    if not isinstance(duration_kind, str) or duration_kind not in allowed_duration:
        errors.append(_diag("invalid_duration_kind", "duration kind is invalid for event kind", entry_id))
    if isinstance(duration_kind, str) and duration_kind in {"fixed", "estimated"} and not _positive_int(duration_ms):
        errors.append(_diag("positive_duration_required", "known or estimated duration needs positive milliseconds", entry_id))
    if isinstance(duration_kind, str) and duration_kind in {"unknown", "none"} and duration_ms is not None:
        errors.append(_diag("duration_must_be_null", "unknown or instantaneous duration must be null", entry_id))
    if kind == "wait" and duration_kind == "unknown" and not _nonempty(payload.get("release_signal")):
        errors.append(_diag("wait_release_required", "unknown wait needs release_signal", entry_id))

    timing = entry.get("timing", {})
    transition = entry.get("transition", {})
    if not isinstance(timing, Mapping):
        errors.append(_diag("invalid_timing", "timing must be an object", entry_id))
        timing = {}
    if not isinstance(transition, Mapping):
        errors.append(_diag("invalid_transition", "transition must be an object", entry_id))
        transition = {}
    timing_mode = _option(entry, "timing", "mode", "timing_mode", "absolute")
    window_ms = _option(entry, "timing", "window_ms", "start_window_ms", 0)
    late_policy = _option(entry, "timing", "late_policy", "late_policy", "wait")
    target_raw = _option(entry, "timing", "target_end_at", "target_end_at", None)
    target_end = _aware(target_raw) if target_raw is not None else None
    transition_mode = _option(entry, "transition", "mode", "transition_kind", "sequential")
    overlap_ms = _option(entry, "transition", "overlap_ms", "overlap_ms", 0)
    if not isinstance(timing_mode, str) or timing_mode not in {"absolute", "flexible"} or not _nonnegative_int(window_ms) or \
            (timing_mode == "absolute" and window_ms != 0) or \
            (timing_mode == "flexible" and window_ms == 0):
        errors.append(_diag("invalid_start_window", "absolute needs zero window; flexible needs a positive window", entry_id))
    if not isinstance(late_policy, str) or late_policy not in {"wait", "cut", "skip"}:
        errors.append(_diag("invalid_late_policy", "late_policy must be wait, cut or skip", entry_id))
    if not isinstance(transition_mode, str) or transition_mode not in {"sequential", "overlap", "cut"} or not _nonnegative_int(overlap_ms) or \
            (transition_mode == "overlap" and overlap_ms == 0) or \
            (transition_mode != "overlap" and overlap_ms != 0):
        errors.append(_diag("invalid_transition_rule", "overlap transition needs positive overlap_ms", entry_id))
    if target_raw is not None and target_end is None:
        errors.append(_diag("invalid_target_end", "target_end_at needs a valid UTC offset", entry_id))
    elif target_end is not None and start is not None and target_end <= start:
        errors.append(_diag("invalid_target_end", "target_end_at must follow scheduled_at", entry_id))
    if kind in _INSTANT and target_raw is not None:
        errors.append(_diag("instant_target_end", "instantaneous event cannot have target_end_at", entry_id))
    if kind in _NON_OCCUPYING and transition_mode != "sequential":
        errors.append(_diag("control_transition", "control or container event cannot overlap or cut audio", entry_id))
    if kind in {"wait", "chain"} and transition_mode == "overlap":
        errors.append(_diag("non_audio_overlap", "wait and chain events cannot use audio overlap", entry_id))

    ad_break_id = entry.get("ad_break_id")
    if ad_break_id is not None and (kind != "audio" or not _nonempty(ad_break_id)):
        errors.append(_diag("invalid_ad_break_link", "only audio may name an ad_break_id", entry_id))
    if kind != "audio" and any(k in payload for k in ("version_id", "item_id", "cart_id", "cut_id")):
        errors.append(_diag("non_audio_version", "non-audio payload cannot name catalog audio", entry_id))
    requires_resolution = kind == "audio" and not _nonempty(payload.get("version_id"))
    if requires_resolution:
        warnings.append(_diag("cart_cut_unresolved", "select and freeze a version before approval", entry_id))

    nominal_end = None
    if start is not None and _positive_int(duration_ms) and isinstance(duration_kind, str) and duration_kind in {"fixed", "estimated"}:
        try:
            nominal_end = start + timedelta(milliseconds=duration_ms)
        except OverflowError:
            errors.append(_diag("duration_overflow", "duration exceeds the supported timestamp range", entry_id))
    if start is not None and nominal_end is not None and zone is not None:
        local_start, local_end = start.astimezone(zone), nominal_end.astimezone(zone)
        if local_end.date() != local_start.date():
            warnings.append(_diag("crosses_midnight", "slot continues into another local calendar date", entry_id))
        if local_end.utcoffset() != local_start.utcoffset():
            warnings.append(_diag("dst_transition", "elapsed duration crosses a station offset change", entry_id))
    normalized = {
        "id": str(entry_id) if entry_id is not None else None,
        "ordinal": ordinal, "state": state, "event_kind": kind,
        "scheduled_at": _iso(start), "payload": payload,
        "duration_kind": duration_kind, "planned_duration_ms": duration_ms,
        "timing": {"mode": timing_mode, "window_ms": window_ms,
                   "late_policy": late_policy, "target_end_at": _iso(target_end)},
        "transition": {"mode": transition_mode, "overlap_ms": overlap_ms},
        "ad_break_id": ad_break_id, "nominal_end_at": _iso(nominal_end),
        "requires_resolution": requires_resolution,
    }
    return {"normalized": normalized, "errors": errors, "warnings": warnings,
            "start": start, "nominal_end": nominal_end, "target_end": target_end}


def validate_entry(entry: Mapping[str, Any], station_timezone: str = "UTC",
                   service_date: date | str | None = None) -> dict:
    """Validate one tagged plan entry without reading a database.

    ``scheduled_at`` must include an offset. The normalized timestamp is UTC.
    A cart-only audio entry is structurally valid but needs cut resolution
    before a plan can be approved or distributed.
    """
    if not isinstance(entry, Mapping):
        return {"valid": False, "normalized": None,
                "errors": [_diag("invalid_entry", "entry must be an object")], "warnings": [],
                "requires_resolution": False}
    zone = _zone(station_timezone)
    day = _day(service_date) if service_date is not None else None
    report = _parse_entry(entry, zone, day)
    errors = list(report["errors"])
    if zone is None:
        errors.append(_diag("invalid_station_timezone", "station_timezone must be an IANA zone"))
    if service_date is not None and day is None:
        errors.append(_diag("invalid_service_date", "service_date must be an ISO date"))
    return {"valid": not errors, "normalized": report["normalized"],
            "errors": errors, "warnings": report["warnings"],
            "requires_resolution": report["normalized"]["requires_resolution"]}


def validate_plan(entries: Iterable[Mapping[str, Any]], station_timezone: str,
                  service_date: date | str) -> dict:
    """Validate order, rollover, ad-break membership and permitted transitions.

    A known duration uses elapsed milliseconds in UTC. A flexible entry may
    shift within its window to wait for a previous event. An absolute entry
    requires an explicit cut or permitted audio overlap when anchors collide.
    Unknown duration requires a later cut or flexible wait; the latter remains
    a run-time decision and is reported as such.
    """
    zone = _zone(station_timezone)
    day = _day(service_date)
    conflicts: list[dict] = []
    warnings: list[dict] = []
    if zone is None:
        conflicts.append(_diag("invalid_station_timezone", "station_timezone must be an IANA zone"))
    if day is None:
        conflicts.append(_diag("invalid_service_date", "service_date must be an ISO date"))
    try:
        source = list(entries)
    except TypeError:
        source = []
        conflicts.append(_diag("invalid_entries", "entries must be iterable"))
    parsed = []
    for index, entry in enumerate(source):
        if not isinstance(entry, Mapping):
            conflicts.append(_diag("invalid_entry", f"entry at index {index} must be an object"))
            continue
        record = _parse_entry(entry, zone, day)
        if record["normalized"]["id"] is None:
            record["normalized"]["id"] = f"#{index}"
        conflicts.extend(record["errors"])
        warnings.extend(record["warnings"])
        parsed.append(record)
    normalized = [record["normalized"] for record in parsed]
    if not parsed:
        conflicts.append(_diag("empty_plan", "plan has no entries"))
    if conflicts:
        return {"valid": False, "approvable": False, "entries": normalized,
                "conflicts": conflicts, "warnings": warnings}
    by_id = {record["normalized"]["id"]: record for record in parsed}
    if len(by_id) != len(parsed):
        conflicts.append(_diag("duplicate_entry_id", "plan entry IDs must be unique"))
    by_ordinal: dict[int, str] = {}
    for record in parsed:
        row = record["normalized"]
        if row["ordinal"] in by_ordinal:
            conflicts.append(_diag("duplicate_ordinal", "plan entry ordinals must be unique",
                                   row["id"], by_ordinal[row["ordinal"]]))
        by_ordinal[row["ordinal"]] = row["id"]
        parent_id = row["ad_break_id"]
        if parent_id is not None:
            parent = by_id.get(str(parent_id))
            if parent is None or parent["normalized"]["event_kind"] != "ad_break":
                conflicts.append(_diag("missing_ad_break", "linked ad break is absent", row["id"], parent_id))
            else:
                parent_start, parent_end = parent["start"], parent["nominal_end"]
                child_end = record["nominal_end"]
                if record["start"] < parent_start or (parent_end is not None and
                                                      (record["start"] >= parent_end or
                                                       child_end is not None and child_end > parent_end)):
                    conflicts.append(_diag("outside_ad_break", "audio placement exceeds ad break window",
                                           row["id"], parent_id))
    active: list[tuple[dict, datetime | None]] = []
    last_anchor: datetime | None = None
    last_entry_id: str | None = None
    for record in sorted(parsed, key=lambda value: value["normalized"]["ordinal"]):
        row = record["normalized"]
        if row["state"] != "planned":
            continue
        start = record["start"]
        if last_anchor is not None and start < last_anchor:
            conflicts.append(_diag("ordinal_time_conflict", "ordinal order reverses scheduled time",
                                   row["id"], last_entry_id))
        last_anchor, last_entry_id = start, row["id"]
        row["effective_start_at"] = _iso(start)
        row["effective_end_at"] = _iso(record["nominal_end"])
        if row["event_kind"] in _NON_OCCUPYING:
            continue
        active = [(prior, prior_end) for prior, prior_end in active
                  if prior_end is None or prior_end > start]
        effective_start = start
        if active:
            prior_ids = ", ".join(prior["normalized"]["id"] for prior, _ in active)
            transition = row["transition"]["mode"]
            timing = row["timing"]
            if transition == "cut":
                warnings.append(_diag("cuts_previous", f"entry cuts active entries {prior_ids}", row["id"]))
                active.clear()
            elif transition == "overlap" and len(active) == 1 and \
                    row["event_kind"] in _SOUND and active[0][0]["normalized"]["event_kind"] in _SOUND and \
                    active[0][1] is not None and \
                    active[0][1] - start <= timedelta(milliseconds=row["transition"]["overlap_ms"]):
                if active[0][0]["normalized"]["duration_kind"] == "estimated":
                    warnings.append(_diag("estimated_overlap", "overlap uses an estimated prior end", row["id"],
                                          active[0][0]["normalized"]["id"]))
            elif timing["late_policy"] == "skip":
                warnings.append(_diag("would_skip_at_anchor", f"entry conflicts with {prior_ids}; skip policy applies",
                                      row["id"]))
                row["effective_start_at"] = None
                row["effective_end_at"] = None
                continue
            elif timing["mode"] == "flexible" and timing["late_policy"] == "wait":
                if any(prior_end is None for _, prior_end in active):
                    warnings.append(_diag("runtime_wait", f"release of {prior_ids} has unknown time", row["id"]))
                    row["effective_start_at"] = None
                    row["effective_end_at"] = None
                    active.clear()
                    active.append((record, None))
                    continue
                latest_end = max(prior_end for _, prior_end in active)
                if latest_end <= start + timedelta(milliseconds=timing["window_ms"]):
                    effective_start = latest_end
                    warnings.append(_diag("flexible_shift", "entry shifts within its start window", row["id"]))
                    active.clear()
                else:
                    conflicts.append(_diag("start_window_exceeded", f"prior entry ends beyond flexible window: {prior_ids}",
                                           row["id"]))
                    continue
            else:
                conflicts.append(_diag("unpermitted_overlap", f"active entries: {prior_ids}", row["id"]))
                continue
        end = None
        if record["nominal_end"] is not None:
            end = effective_start + (record["nominal_end"] - start)
        target_end = record["target_end"]
        if target_end is not None and target_end <= effective_start:
            conflicts.append(_diag("target_end_before_start", "target end precedes the effective start", row["id"]))
            continue
        if target_end is not None and (end is None or end > target_end):
            if row["timing"]["late_policy"] == "cut":
                end = target_end
                warnings.append(_diag("cuts_at_target_end", "target end trims the slot", row["id"]))
            else:
                conflicts.append(_diag("target_end_missed", "slot cannot meet target end without cut policy", row["id"]))
        row["effective_start_at"] = _iso(effective_start)
        row["effective_end_at"] = _iso(end)
        active.append((record, end))
    unresolved = [row["id"] for row in normalized if row["state"] == "planned" and row["requires_resolution"]]
    return {"valid": not conflicts, "approvable": not conflicts and not unresolved,
            "entries": normalized, "conflicts": conflicts, "warnings": warnings,
            "unresolved_cart_entries": unresolved}


def _clock(value: Any) -> time | None:
    if isinstance(value, time):
        return value.replace(tzinfo=None)
    if isinstance(value, str):
        try:
            parsed = time.fromisoformat(value)
            return parsed.replace(tzinfo=None)
        except ValueError:
            return None
    return None


def select_cart_cut(cuts: Iterable[Mapping[str, Any]], *, at: datetime | str,
                    station_timezone: str, mode: str = "sequential",
                    last_cut_id: str | None = None, seed: str | None = None,
                    slot_end_at: datetime | str | None = None) -> dict:
    """Select a verified eligible cut; return state for the caller to commit.

    Each cut must explicitly carry ``status='active'``, ``approved=True``,
    ``station_eligible=True`` and ``central_media_verified=True``. The caller
    must commit ``next_state`` with the selected plan entry atomically.
    """
    zone, instant = _zone(station_timezone), _aware(at)
    end = _aware(slot_end_at) if slot_end_at is not None else \
        (instant + timedelta(milliseconds=1) if instant else None)
    errors = []
    if zone is None:
        errors.append(_diag("invalid_station_timezone", "station_timezone must be an IANA zone"))
    if instant is None or end is None or end <= instant:
        errors.append(_diag("invalid_cart_slot", "at and slot_end_at must be valid ordered instants"))
    if not isinstance(mode, str) or mode not in {"sequential", "weighted"}:
        errors.append(_diag("invalid_selection_mode", "mode must be sequential or weighted"))
    if errors:
        return {"selected": None, "decisions": [], "next_state": None, "errors": errors}
    decisions = []
    eligible = []
    local = instant.astimezone(zone)
    try:
        source_cuts = list(cuts)
    except TypeError:
        return {"selected": None, "decisions": [], "next_state": None,
                "errors": [_diag("invalid_cuts", "cuts must be iterable")]}
    seen_ids: set[str] = set()
    for raw in source_cuts:
        if not isinstance(raw, Mapping):
            decisions.append({"cut_id": None, "eligible": False, "reasons": ["invalid_cut"]})
            continue
        cut = dict(raw)
        reasons = []
        cut_id = cut.get("id")
        if not _nonempty(cut_id) or not _nonempty(cut.get("version_id")):
            reasons.append("missing_identity")
        elif cut_id in seen_ids:
            reasons.append("duplicate_cut_id")
        else:
            seen_ids.add(cut_id)
        if cut.get("status") != "active":
            reasons.append("disabled")
        if cut.get("approved") is not True:
            reasons.append("unapproved")
        if cut.get("station_eligible") is not True:
            reasons.append("station_ineligible")
        if cut.get("central_media_verified") is not True:
            reasons.append("central_media_unverified")
        lower, upper = cut.get("valid_from"), cut.get("valid_to")
        lower_dt, upper_dt = _aware(lower) if lower is not None else None, _aware(upper) if upper is not None else None
        if lower is not None and lower_dt is None or upper is not None and upper_dt is None:
            reasons.append("invalid_validity_window")
        if lower_dt is not None and lower_dt > instant or upper_dt is not None and upper_dt < end:
            reasons.append("outside_validity_window")
        weekdays = cut.get("weekdays")
        if weekdays is not None:
            if not isinstance(weekdays, (list, tuple, set)) or local.isoweekday() not in weekdays:
                reasons.append("weekday_blocked")
        begin, finish = cut.get("local_start"), cut.get("local_end")
        if begin is not None or finish is not None:
            first, last = _clock(begin), _clock(finish)
            if first is None or last is None or first == last:
                reasons.append("invalid_local_window")
            else:
                current = local.timetz().replace(tzinfo=None)
                inside = first <= current < last if first < last else current >= first or current < last
                if not inside:
                    reasons.append("outside_local_window")
        if not _nonnegative_int(cut.get("ordinal", 0)):
            reasons.append("invalid_ordinal")
        if mode == "weighted" and not _positive_int(cut.get("weight", 1)):
            reasons.append("invalid_weight")
        decisions.append({"cut_id": cut_id, "eligible": not reasons, "reasons": reasons})
        if not reasons:
            eligible.append(cut)
    primary = [cut for cut in eligible if cut.get("is_fallback") is not True]
    pool = primary or [cut for cut in eligible if cut.get("is_fallback") is True]
    if not pool:
        return {"selected": None, "decisions": decisions, "next_state": None,
                "errors": [_diag("no_eligible_cut", "cart has no eligible primary or fallback cut")]}
    pool.sort(key=lambda cut: (cut.get("ordinal", 0), str(cut["id"])))
    if mode == "sequential":
        ids = [str(cut["id"]) for cut in pool]
        selected = pool[(ids.index(str(last_cut_id)) + 1) % len(pool)] if str(last_cut_id) in ids else pool[0]
    else:
        material = seed if seed is not None else f"{_iso(instant)}:{','.join(str(cut['id']) for cut in pool)}"
        ticket = int.from_bytes(sha256(str(material).encode("utf-8")).digest()[:8], "big")
        target = ticket % sum(cut.get("weight", 1) for cut in pool)
        selected = pool[-1]
        for cut in pool:
            weight = cut.get("weight", 1)
            if target < weight:
                selected = cut
                break
            target -= weight
    return {"selected": dict(selected), "decisions": decisions,
            "next_state": {"last_cut_id": str(selected["id"])}, "errors": [],
            "used_fallback": not bool(primary)}


def _ids(value: Any) -> set[str]:
    if value is None:
        return set()
    if isinstance(value, str):
        return {value} if value else set()
    if isinstance(value, (list, tuple, set)):
        return {str(item) for item in value if item is not None and str(item)}
    return set()


def explain_rotation(candidates: Iterable[Mapping[str, Any]],
                     history: Iterable[Mapping[str, Any]], *,
                     at: datetime | str, rules: Mapping[str, Any]) -> dict:
    """Explain music exclusion by recording, artist, group and format rules.

    ``rules.artist_groups`` maps solo artist IDs to their group IDs. A group
    played recently blocks a member's solo item, and the reverse, within the
    configured group window. This function makes no random selection.
    """
    instant = _aware(at)
    if instant is None or not isinstance(rules, Mapping):
        return {"decisions": [], "eligible_ids": [],
                "errors": [_diag("invalid_rotation_context", "at must be aware and rules an object")]}
    try:
        candidates = list(candidates)
        history = list(history)
    except TypeError:
        return {"decisions": [], "eligible_ids": [],
                "errors": [_diag("invalid_rotation_rows", "candidates and history must be iterable")]}
    gaps = {}
    for key in ("recording_gap_minutes", "artist_gap_minutes", "group_gap_minutes"):
        value = rules.get(key, 0)
        if not _nonnegative_int(value):
            return {"decisions": [], "eligible_ids": [],
                    "errors": [_diag("invalid_rotation_gap", f"{key} must be nonnegative integer")]}
        gaps[key] = timedelta(minutes=value)
    membership = rules.get("artist_groups", {})
    if not isinstance(membership, Mapping):
        return {"decisions": [], "eligible_ids": [],
                "errors": [_diag("invalid_artist_groups", "artist_groups must map artists to groups")]}
    allowed_categories = _ids(rules.get("allowed_categories"))
    allowed_formats = _ids(rules.get("allowed_formats"))

    def groups_for(item: Mapping[str, Any]) -> set[str]:
        artists = _ids(item.get("artist_ids"))
        groups = _ids(item.get("group_ids"))
        for artist in artists:
            groups.update(_ids(membership.get(artist)))
        return groups

    recent = []
    max_gap = max(gaps.values())
    for play in history:
        if not isinstance(play, Mapping):
            continue
        played = _aware(play.get("played_at"))
        if played is not None and played <= instant and instant - played < max_gap:
            recent.append((play, played, groups_for(play)))
    decisions = []
    for candidate in candidates:
        if not isinstance(candidate, Mapping):
            decisions.append({"candidate_id": None, "eligible": False, "reasons": ["invalid_candidate"]})
            continue
        candidate_id = candidate.get("id")
        reasons = []
        if candidate.get("approved") is False:
            reasons.append("unapproved")
        if candidate.get("station_eligible") is False:
            reasons.append("station_ineligible")
        if allowed_categories and str(candidate.get("category")) not in allowed_categories:
            reasons.append("category_blocked")
        if allowed_formats and str(candidate.get("format")) not in allowed_formats:
            reasons.append("format_blocked")
        artists = _ids(candidate.get("artist_ids"))
        groups = groups_for(candidate)
        for play, played, prior_groups in recent:
            elapsed = instant - played
            if gaps["recording_gap_minutes"] and elapsed < gaps["recording_gap_minutes"] and \
                    candidate.get("recording_id") is not None and \
                    candidate.get("recording_id") == play.get("recording_id"):
                reasons.append("recording_repeat")
            if gaps["artist_gap_minutes"] and elapsed < gaps["artist_gap_minutes"] and \
                    artists & _ids(play.get("artist_ids")):
                reasons.append("artist_repeat")
            if gaps["group_gap_minutes"] and elapsed < gaps["group_gap_minutes"] and groups & prior_groups:
                reasons.append("group_or_member_repeat")
        reasons = list(dict.fromkeys(reasons))
        decisions.append({"candidate_id": candidate_id, "eligible": not reasons, "reasons": reasons})
    return {"decisions": decisions,
            "eligible_ids": [row["candidate_id"] for row in decisions if row["eligible"]],
            "errors": []}
