"""Persisted music rotation rules and explainable, slot-aware evaluations.

Evaluation reads only completed engine attempt claims. An optional rule requires
an external output observation linked to that attempt. Neither is acoustic
proof, so decisions describe the evidence used and never approve content.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
from typing import Any, Mapping, Sequence
from uuid import UUID, uuid4

from .identity import Principal, require
from .planning import explain_rotation
from .readiness import readiness_at
from .storage import Conflict, Store, StoreError


_GAP_KEYS = ("recording_gap_minutes", "artist_gap_minutes", "group_gap_minutes")
_ARTIST_ROLES = {"primary_artist", "joint_artist", "guest_artist"}


def _id(value: Any, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _instant(value: Any, field: str) -> datetime:
    try:
        parsed = value if isinstance(value, datetime) else datetime.fromisoformat(
            str(value).replace("Z", "+00:00"))
    except ValueError as exc:
        raise StoreError(f"{field} must be an ISO instant") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError(f"{field} needs a timezone offset")
    return parsed.astimezone(timezone.utc)


def _data(row: dict) -> dict:
    return {key: value for key, value in row.items() if key not in ("_id", "_rev")}


def _config(value: Mapping[str, Any]) -> dict:
    if not isinstance(value, Mapping):
        raise StoreError("rotation config must be an object")
    if any(not isinstance(key, str) for key in value):
        raise StoreError("rotation config keys must be strings")
    unknown = set(value) - set(_GAP_KEYS) - {
        "allowed_categories", "allowed_formats", "require_output_observation"}
    if unknown:
        raise StoreError("unknown rotation config keys: " + ", ".join(sorted(unknown)))
    config: dict[str, Any] = {}
    for key in _GAP_KEYS:
        gap = value.get(key, 0)
        if isinstance(gap, bool) or not isinstance(gap, int) or gap < 0 or gap > 365 * 24 * 60:
            raise StoreError(f"{key} must be an integer from 0 to 525600")
        config[key] = gap
    for key in ("allowed_categories", "allowed_formats"):
        values = value.get(key, [])
        if not isinstance(values, list) or any(not isinstance(v, str) or not v.strip()
                                                for v in values) or len(set(values)) != len(values):
            raise StoreError(f"{key} must be a list of unique nonempty strings")
        config[key] = list(values)
    observed = value.get("require_output_observation", False)
    if not isinstance(observed, bool):
        raise StoreError("require_output_observation must be Boolean")
    config["require_output_observation"] = observed
    return config


def put_rotation_rule(store: Store, principal: Principal, station_id: str, *,
                      name: str, config: Mapping[str, Any],
                      rule_id: str | None = None,
                      expected_revision: int | None = None) -> dict:
    """Create or edit a draft rule. Activation is a separate approval action."""
    station_id = _id(station_id, "station_id")
    require(store, principal, "manage", station_id)
    if store.get("stations", station_id) is None:
        raise StoreError("station not found")
    if not isinstance(name, str) or not name.strip():
        raise StoreError("rotation rule name must be nonempty")
    identifier = _id(rule_id, "rule_id") if rule_id else str(uuid4())
    existing = store.get("rotation_rules", identifier)
    if existing is not None:
        if existing["station_id"] != station_id or existing["_rev"] != expected_revision:
            raise Conflict("rotation rule changed or belongs to another station")
        if existing["status"] == "active":
            raise Conflict("disable an active rule before editing")
    elif expected_revision is not None:
        raise Conflict("rotation rule is absent at expected revision")
    row = {"station_id": station_id, "name": name.strip(),
           "status": "draft", "config": _config(config),
           "created_by": existing.get("created_by") if existing else principal.user_id,
           "created_at": existing.get("created_at") if existing else datetime.now(timezone.utc).isoformat(),
           "updated_by": principal.user_id,
           "updated_at": datetime.now(timezone.utc).isoformat()}
    return store.put("rotation_rules", identifier, row,
                     expected_revision=existing["_rev"] if existing else None,
                     actor=principal.user_id)


def set_rotation_rule_status(store: Store, principal: Principal, station_id: str,
                             rule_id: str, status: str, *, expected_revision: int) -> dict:
    station_id, rule_id = _id(station_id, "station_id"), _id(rule_id, "rule_id")
    if not isinstance(status, str) or status not in {"draft", "active", "disabled"}:
        raise StoreError("invalid rotation rule status")
    require(store, principal, "approve" if status == "active" else "manage", station_id)
    rule = store.get("rotation_rules", rule_id)
    if rule is None or rule["station_id"] != station_id:
        raise StoreError("rotation rule does not belong to station")
    if rule["_rev"] != expected_revision:
        raise Conflict("rotation rule changed")
    if rule["status"] == status:
        return rule
    if status == "draft" and rule["status"] != "disabled":
        raise Conflict("only a disabled rule can return to draft")
    return store.put("rotation_rules", rule_id,
                     _data(rule) | {"status": status, "status_changed_by": principal.user_id,
                                    "status_changed_at": datetime.now(timezone.utc).isoformat()},
                     expected_revision=rule["_rev"], actor=principal.user_id)


def _memberships(store: Store, person_ids: set[str], at: datetime) -> dict[str, set[str]]:
    result: dict[str, set[str]] = {}
    for person_id in person_ids:
        for row in store.lookup("person_memberships", "person", person_id):
            lower = row.get("valid_from")
            upper = row.get("valid_to")
            if lower is not None and _instant(lower, "membership.valid_from") > at:
                continue
            if upper is not None and _instant(upper, "membership.valid_to") <= at:
                continue
            result.setdefault(person_id, set()).add(row["group_id"])
    return result


def _identity_for_version(store: Store, station_id: str, version_id: str,
                          at: datetime) -> dict:
    version = store.get("versions", version_id)
    if version is None:
        return {"id": version_id, "approved": False, "station_eligible": False,
                "recording_id": None, "artist_ids": [], "group_ids": [],
                "category": None, "format": None}
    item = store.get("items", version["item_id"])
    if item is None:
        raise StoreError("version references absent item")
    associations = store.lookup("station_items", "station_item", station_id, item["_id"])
    association = associations[0] if associations else None
    artists: set[str] = set()
    groups: set[str] = set()
    for row in store.lookup("contributions", "item", item["_id"]):
        if row["role"] not in _ARTIST_ROLES:
            continue
        person = store.get("people", row["person_id"])
        if person is None:
            continue
        if person["kind"] == "group":
            groups.add(person["_id"])
        elif person["kind"] == "person":
            artists.add(person["_id"])
    memberships = _memberships(store, artists, at)
    for artist in artists:
        groups.update(memberships.get(artist, set()))
    return {"id": version_id,
            "approved": version["status"] == "approved" and item["lifecycle"] == "active",
            "station_eligible": bool(association and association.get("eligible")),
            "recording_id": item.get("recording_id") or item["_id"],
            "artist_ids": sorted(artists), "group_ids": sorted(groups),
            "category": association.get("category_id") if association else None,
            "format": association.get("rotation_code") if association else None,
            "item_id": item["_id"]}


def _recent_history(store: Store, station_id: str, at: datetime,
                    max_gap: int, observed_only: bool) -> list[dict]:
    if max_gap == 0:
        return []
    cutoff = at - timedelta(minutes=max_gap)
    observed_attempts = {
        row.get("attempt_id") for row in store.lookup("output_observations", "station", station_id)
        if row["station_id"] == station_id and row.get("observer_kind") == "external_monitor"
        and row.get("kind") == "audio_present" and row.get("attempt_id")
        and _instant(row["occurred_at"], "observation.occurred_at") <= at
    } if observed_only else set()
    history: list[dict] = []
    for event in store.scan("playout_events"):
        if event["phase"] not in {"complete", "manual_complete"}:
            continue
        played_at = _instant(event["occurred_at"], "playout.occurred_at")
        if not cutoff <= played_at <= at:
            continue
        attempt = store.get("playout_attempts", event["attempt_id"])
        if attempt is None or attempt["station_id"] != station_id:
            continue
        if observed_only and attempt["_id"] not in observed_attempts:
            continue
        identity = _identity_for_version(store, station_id, attempt["version_id"], played_at)
        history.append(identity | {"version_id": attempt["version_id"],
                                   "attempt_id": attempt["_id"],
                                   "event_id": event["_id"],
                                   "played_at": played_at.isoformat()})
    return sorted(history, key=lambda row: (row["played_at"], row["event_id"]))


def _matching_history(candidate: dict, history: list[dict], at: datetime,
                      config: dict) -> list[dict]:
    matches: list[dict] = []
    artists = set(candidate["artist_ids"])
    groups = set(candidate["group_ids"])
    for prior in history:
        elapsed = at - _instant(prior["played_at"], "played_at")
        reasons = []
        if config["recording_gap_minutes"] and elapsed < timedelta(minutes=config["recording_gap_minutes"]) and \
                candidate["recording_id"] is not None and candidate["recording_id"] == prior["recording_id"]:
            reasons.append("recording_repeat")
        if config["artist_gap_minutes"] and elapsed < timedelta(minutes=config["artist_gap_minutes"]) and \
                artists.intersection(prior["artist_ids"]):
            reasons.append("artist_repeat")
        if config["group_gap_minutes"] and elapsed < timedelta(minutes=config["group_gap_minutes"]) and \
                groups.intersection(prior["group_ids"]):
            reasons.append("group_or_member_repeat")
        if reasons:
            matches.append({"attempt_id": prior["attempt_id"],
                            "event_id": prior["event_id"],
                            "version_id": prior["version_id"],
                            "played_at": prior["played_at"],
                            "elapsed_ms": int(elapsed.total_seconds() * 1000),
                            "reasons": reasons})
    return matches


def evaluate_rotation(store: Store, principal: Principal, station_id: str,
                      rule_id: str, candidate_version_ids: Sequence[str], *,
                      at: str | datetime, slot_duration_ms: int = 1,
                      territory: str = "*", output_id: str = "*") -> dict:
    """Return eligible versions and reasons without modifying approvals."""
    station_id, rule_id = _id(station_id, "station_id"), _id(rule_id, "rule_id")
    require(store, principal, "view", station_id)
    snapshot_sequence = store.sequence
    rule = store.get("rotation_rules", rule_id)
    if rule is None or rule["station_id"] != station_id or rule["status"] != "active":
        raise Conflict("rotation rule is absent, inactive or outside station")
    instant = _instant(at, "at")
    if isinstance(slot_duration_ms, bool) or not isinstance(slot_duration_ms, int) or slot_duration_ms < 1:
        raise StoreError("slot_duration_ms must be positive")
    if not isinstance(candidate_version_ids, (list, tuple)) or not candidate_version_ids:
        raise StoreError("candidate_version_ids must be a nonempty list")
    ids = [_id(value, "candidate_version_id") for value in candidate_version_ids]
    if len(ids) != len(set(ids)):
        raise StoreError("candidate_version_ids must be unique")
    config = _config(rule["config"])
    try:
        end = instant + timedelta(milliseconds=slot_duration_ms)
    except OverflowError as exc:
        raise StoreError("slot_duration_ms exceeds supported timestamp range") from exc
    candidates = [_identity_for_version(store, station_id, identifier, instant) for identifier in ids]
    max_gap = max(config[key] for key in _GAP_KEYS)
    history = _recent_history(store, station_id, instant, max_gap,
                              config["require_output_observation"])
    pure = explain_rotation(candidates, history, at=instant, rules=config)
    if pure["errors"]:
        raise StoreError("rotation evaluation failed: " + ", ".join(
            error["code"] for error in pure["errors"]))
    decisions = []
    for candidate, decision in zip(candidates, pure["decisions"]):
        version = store.get("versions", candidate["id"])
        readiness = (readiness_at(store, station_id, candidate["id"],
                                  slot_start=instant, slot_end=end,
                                  require_endpoint_copy=False,
                                  territory=territory, output_id=output_id)
                     if version is not None else {"ready": False,
                                                   "blocked_reasons": ["version_missing"]})
        reasons = list(dict.fromkeys(decision["reasons"] + readiness["blocked_reasons"]))
        decisions.append({"candidate_id": candidate["id"],
                          "eligible": not reasons, "reasons": reasons,
                          "item_id": candidate.get("item_id"),
                          "recording_id": candidate["recording_id"],
                          "artist_ids": candidate["artist_ids"],
                          "group_ids": candidate["group_ids"],
                          "category": candidate["category"],
                          "format": candidate["format"],
                          "matching_history": _matching_history(candidate, history,
                                                                 instant, config)})
    if store.sequence != snapshot_sequence:
        raise Conflict("rotation source data changed during evaluation")
    return {"station_id": station_id, "rule_id": rule_id,
            "rule_revision": rule["_rev"],
            "evaluated_at": instant.isoformat(),
            "slot_end_at": end.isoformat(),
            "candidate_version_ids": ids,
            "eligible_ids": [row["candidate_id"] for row in decisions if row["eligible"]],
            "decisions": decisions, "history_count": len(history),
            "history_evidence": ("external_monitor_claim" if config["require_output_observation"]
                                 else "engine_completion_claim"),
            "errors": []}


def record_rotation_decision(store: Store, principal: Principal, station_id: str,
                             rule_id: str, candidate_version_ids: Sequence[str], *,
                             at: str | datetime, slot_duration_ms: int = 1,
                             selected_version_id: str | None = None,
                             territory: str = "*", output_id: str = "*") -> dict:
    """Audit an evaluation and an explicit human choice; no approval is granted."""
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    sequence = store.sequence
    evaluation = evaluate_rotation(store, principal, station_id, rule_id,
                                   candidate_version_ids, at=at,
                                   slot_duration_ms=slot_duration_ms,
                                   territory=territory, output_id=output_id)
    selected = _id(selected_version_id, "selected_version_id") if selected_version_id else None
    if selected is not None and selected not in evaluation["eligible_ids"]:
        raise Conflict("selected version is not eligible under this rule and slot")
    decision_id = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("rotation_decisions", decision_id, {
        "station_id": station_id, "rule_id": evaluation["rule_id"],
        "rule_revision": evaluation["rule_revision"],
        "evaluated_at": evaluation["evaluated_at"],
        "slot_end_at": evaluation["slot_end_at"],
        "candidate_version_ids": evaluation["candidate_version_ids"],
        "decisions": evaluation["decisions"],
        "selected_version_id": selected,
        "history_evidence": evaluation["history_evidence"],
        "history_count": evaluation["history_count"],
        "created_by": principal.user_id,
        "created_at": datetime.now(timezone.utc).isoformat()})
    tx.commit()
    return store.get("rotation_decisions", decision_id)
