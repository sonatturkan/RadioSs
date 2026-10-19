"""Project listener metadata from recorded playout events and explicit intents.

This is a database projection, not an RDS, streaming, or audio-output adapter.
An ``on_air`` row means that the endpoint *reported* a start; it does not prove
the sound was received. ``next_intent`` and ``talk_intent`` are kept separate so
an upcoming title never becomes an on-air title merely by being scheduled.
"""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timedelta, timezone
import hashlib
import json
from typing import Any, Mapping
from uuid import UUID, NAMESPACE_URL, uuid5, uuid4

from .identity import Principal, require
from .station_outputs import _CODE, lookup_output
from .storage import Conflict, Store, StoreError


_START = frozenset({"start", "manual_start"})
_TERMINAL = frozenset({"complete", "manual_complete", "interrupt", "fail", "skip"})
_MAX_CLOCK_SKEW = timedelta(seconds=5)


def _id(value: Any, name: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def _instant(value: Any, name: str) -> datetime:
    try:
        result = (datetime.fromisoformat(value.replace("Z", "+00:00"))
                  if isinstance(value, str) else value)
    except ValueError as exc:
        raise StoreError(f"{name} must be an ISO timestamp") from exc
    if not isinstance(result, datetime) or result.tzinfo is None or result.utcoffset() is None:
        raise StoreError(f"{name} needs a timezone")
    return result.astimezone(timezone.utc)


def _ttl(value: int, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 86400:
        raise StoreError(f"{name} must be 1..86400 seconds")
    return value


def _scope(store: Store, station_id: str, endpoint_id: str,
           output_id: str, *, write: bool = True) -> tuple[str, str, str]:
    station_id, endpoint_id = _id(station_id, "station_id"), _id(endpoint_id, "endpoint_id")
    if not isinstance(output_id, str) or not _CODE.fullmatch(output_id):
        raise StoreError("output_id must be a 1..100 character opaque code")
    endpoint = store.get("endpoints", endpoint_id)
    if endpoint is None or endpoint["station_id"] != station_id:
        raise StoreError("endpoint is outside station")
    if write and endpoint["status"] == "retired":
        raise Conflict("retired endpoint cannot update now-playing")
    output = lookup_output(store, station_id, endpoint_id, output_id)
    if output is None:
        raise StoreError("station output is not configured")
    if write and output["status"] != "active":
        raise Conflict("disabled station output cannot update now-playing")
    return station_id, endpoint_id, output_id


def _body(row: Mapping[str, Any]) -> dict:
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _digest(value: Mapping[str, Any]) -> str:
    raw = json.dumps(value, ensure_ascii=False, sort_keys=True,
                     separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()


def _state(store: Store, station_id: str, endpoint_id: str,
           output_id: str) -> tuple[str, dict | None]:
    key = str(uuid5(NAMESPACE_URL, f"sdbase:now-playing:{station_id}:{endpoint_id}:{output_id}"))
    rows = store.lookup("now_playing_states", "scope", station_id, endpoint_id, output_id)
    if len(rows) > 1:
        raise StoreError("duplicate now-playing scope")
    if rows and rows[0]["_id"] != key:
        raise StoreError("now-playing scope has an unexpected identity")
    return key, rows[0] if rows else None


def _station_version(store: Store, station_id: str, version_id: str) -> tuple[dict, dict]:
    version = store.get("versions", version_id)
    item = store.get("items", version["item_id"]) if version else None
    if item is None or not store.lookup("station_items", "station_item",
                                        station_id, item["_id"]):
        raise StoreError("version is outside station")
    return version, item


def _display(item: dict) -> dict:
    # A catalogue's internal advertising title is not automatically public.
    title = item.get("listener_title")
    if title is None and item["kind"] == "music":
        title = item["title"]
    artist = item.get("listener_artist")
    return {"title": title if isinstance(title, str) else None,
            "artist": artist if isinstance(artist, str) else None,
            "kind": item["kind"]}


def _view(state: dict | None, now: datetime) -> dict:
    if state is None:
        return {"status": "idle", "on_air": None, "talk": None, "next": None}
    on_air = state.get("on_air")
    talk = state.get("talk_intent")
    following = state.get("next_intent")
    on_air = (on_air if on_air and _instant(on_air["expires_at"], "expires_at") > now
              else None)
    talk = (talk if talk and _instant(talk["expires_at"], "expires_at") > now
            else None)
    following = (following if following and
                 _instant(following["expires_at"], "expires_at") > now else None)
    return {"status": "talk" if talk else "on_air" if on_air else "idle",
            "on_air": deepcopy(on_air), "talk": deepcopy(talk),
            "next": deepcopy(following)}


def get_now_playing(store: Store, principal: Principal,
                    station_id: str, endpoint_id: str, output_id: str, *,
                    now: str | datetime | None = None) -> dict:
    """Return one output's current projection with expired lanes suppressed."""
    station_id, endpoint_id, output_id = _scope(store, station_id, endpoint_id,
                                                output_id, write=False)
    require(store, principal, "operate" if principal.scope == "endpoint" else "view",
            station_id, endpoint_id=endpoint_id)
    _, state = _state(store, station_id, endpoint_id, output_id)
    at = _instant(now or datetime.now(timezone.utc), "now")
    return {"station_id": station_id, "endpoint_id": endpoint_id,
            "output_id": output_id, "state_revision": state["_rev"] if state else 0,
            "as_of": at.isoformat(), **_view(state, at)}


def _decide(store: Store, principal: Principal, station_id: str,
            endpoint_id: str, output_id: str, *,
            decision_key: str, source_kind: str, source_id: str,
            lane: str, event_at: datetime, now: datetime,
            input_body: Mapping[str, Any]) -> dict:
    """Atomically append a decision and conditionally update one projection lane."""
    station_id, endpoint_id, output_id = _scope(store, station_id, endpoint_id, output_id)
    require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    if event_at > now + _MAX_CLOCK_SKEW:
        raise Conflict("metadata source time is in the future")
    fingerprint_body = ({key: value for key, value in input_body.items()
                         if key not in {"display", "attempt_finished",
                                        "output_route_source"}}
                        if source_kind in {"playout_event", "next_intent"}
                        else input_body)
    digest = _digest(fingerprint_body)
    sequence = store.sequence
    prior = store.lookup("now_playing_decisions", "scope_key", station_id,
                         endpoint_id, output_id, decision_key)
    if prior:
        if prior[0]["input_fingerprint"] != digest:
            raise Conflict("now-playing decision key was reused with different input")
        return {"decision_id": prior[0]["_id"],
                "outcome": prior[0]["outcome"], "reason": prior[0]["reason"],
                "replayed": True,
                "projection": get_now_playing(store, principal, station_id,
                                               endpoint_id, output_id, now=now)}
    state_id, current = _state(store, station_id, endpoint_id, output_id)
    watermark_field = f"{lane}_watermark"
    prior_time = (_instant(current[watermark_field], watermark_field)
                  if current and current.get(watermark_field) else None)
    before_rev = current["_rev"] if current else 0
    updated = (_body(current) if current else
               {"station_id": station_id, "endpoint_id": endpoint_id,
                "output_id": output_id, "on_air": None,
                "next_intent": None, "talk_intent": None,
                "on_air_watermark": None, "next_watermark": None,
                "talk_watermark": None, "updated_at": now.isoformat()})
    outcome, reason = "applied", "current_source"
    if prior_time is not None and event_at <= prior_time:
        outcome, reason = "ignored_stale", "event_time_not_newer_than_lane"
    elif lane == "on_air":
        phase = input_body["phase"]
        if phase in _START:
            if input_body.get("attempt_finished"):
                outcome, reason = "ignored_unmatched", "attempt_already_finished"
            elif event_at + timedelta(seconds=input_body["ttl_seconds"]) <= now:
                outcome, reason = "ignored_expired", "start_ttl_elapsed"
            else:
                updated["on_air"] = {"event_id": input_body["event_id"],
                                     "attempt_id": input_body["attempt_id"],
                                     "version_id": input_body["version_id"],
                                     "plan_entry_id": input_body.get("plan_entry_id"),
                                     "started_at": event_at.isoformat(),
                                     "expires_at": (event_at + timedelta(
                                         seconds=input_body["ttl_seconds"])).isoformat(),
                                     "display": dict(input_body["display"]),
                                     "evidence_level": "endpoint_engine_event"}
                # A queued title from before this start is no longer the next title.
                next_at = updated.get("next_watermark")
                if next_at is None or _instant(next_at, "next_watermark") <= event_at:
                    updated["next_intent"] = None
                    updated["next_watermark"] = event_at.isoformat()
        elif phase in _TERMINAL:
            on_air = updated.get("on_air")
            if on_air is None or on_air["attempt_id"] != input_body["attempt_id"]:
                outcome, reason = "ignored_unmatched", "terminal_event_is_not_current_attempt"
            else:
                updated["on_air"] = None
        else:
            raise StoreError("unsupported playout metadata phase")
    elif lane == "next":
        version_id = input_body.get("version_id")
        if version_id is None:
            updated["next_intent"] = None
        elif event_at + timedelta(seconds=input_body["ttl_seconds"]) <= now:
            outcome, reason = "ignored_expired", "next_intent_ttl_elapsed"
        else:
            updated["next_intent"] = {"version_id": version_id,
                                      "plan_entry_id": input_body.get("plan_entry_id"),
                                      "intent_key": source_id,
                                      "declared_at": event_at.isoformat(),
                                      "expires_at": (event_at + timedelta(
                                          seconds=input_body["ttl_seconds"])).isoformat(),
                                      "display": dict(input_body["display"]),
                                      "evidence_level": "operator_or_endpoint_intent"}
    elif lane == "talk":
        if not input_body["active"]:
            updated["talk_intent"] = None
        elif event_at + timedelta(seconds=input_body["ttl_seconds"]) <= now:
            outcome, reason = "ignored_expired", "talk_intent_ttl_elapsed"
        else:
            updated["talk_intent"] = {"intent_key": source_id,
                                      "label": input_body.get("label"),
                                      "declared_at": event_at.isoformat(),
                                      "expires_at": (event_at + timedelta(
                                          seconds=input_body["ttl_seconds"])).isoformat(),
                                      "evidence_level": "operator_or_endpoint_intent"}
    else:
        raise StoreError("unknown now-playing lane")
    if outcome == "applied":
        updated[watermark_field] = event_at.isoformat()
        updated["updated_at"] = now.isoformat()
    decision_id = str(uuid4())
    decision = {"station_id": station_id, "endpoint_id": endpoint_id,
                "output_id": output_id, "decision_key": decision_key,
                "source_kind": source_kind, "source_id": source_id,
                "event_at": event_at.isoformat(), "received_at": now.isoformat(),
                "input_fingerprint": digest, "outcome": outcome,
                "reason": reason, "state_revision_before": before_rev,
                "state_revision_after": before_rev + (outcome == "applied"),
                "details": dict(input_body)}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    if outcome == "applied":
        tx.put("now_playing_states", state_id, updated,
               expected_revision=current["_rev"] if current else None)
    tx.put("now_playing_decisions", decision_id, decision)
    tx.commit()
    return {"decision_id": decision_id, "outcome": outcome, "reason": reason,
            "replayed": False,
            "projection": get_now_playing(store, principal, station_id,
                                           endpoint_id, output_id, now=now)}


def project_playout_event(store: Store, principal: Principal,
                          station_id: str, endpoint_id: str, output_id: str,
                          event_id: str, *, ttl_seconds: int = 14400,
                          now: str | datetime | None = None) -> dict:
    """Use a stored start/terminal event; attempts alone cannot set on-air text."""
    station_id, endpoint_id, output_id = _scope(store, station_id, endpoint_id, output_id)
    event_id = _id(event_id, "event_id")
    require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    event = store.get("playout_events", event_id)
    if event is None or event["station_id"] != station_id or \
            event["endpoint_id"] != endpoint_id:
        raise StoreError("playout event is outside output endpoint")
    if event["phase"] not in _START | _TERMINAL:
        raise StoreError("only start and terminal playout events affect now-playing")
    details = event.get("details") or {}
    route_source = None
    if "output_ids" in details:
        outputs = details["output_ids"]
        if not isinstance(outputs, list) or output_id not in outputs:
            raise Conflict("playout event does not target this output")
        route_source = "engine_output_list"
    elif "output_id" in details:
        if details["output_id"] != output_id:
            raise Conflict("playout event does not target this output")
        route_source = "engine_output_id"
    else:
        outputs = [row for row in store.lookup("station_outputs", "endpoint", endpoint_id)
                   if row["status"] == "active"]
        if len(outputs) != 1 or outputs[0]["output_id"] != output_id:
            prior = store.lookup("now_playing_decisions", "scope_key", station_id,
                                 endpoint_id, output_id, f"playout:{event_id}")
            if not prior:
                raise Conflict("playout event has no unambiguous output route")
        route_source = "single_active_output_inferred"
    version, item = _station_version(store, station_id, event["version_id"])
    attempt = store.get("playout_attempts", event["attempt_id"])
    if attempt is None or attempt["station_id"] != station_id or \
            attempt["endpoint_id"] != endpoint_id or \
            attempt["version_id"] != version["_id"]:
        raise StoreError("playout event attempt is inconsistent")
    at = _instant(event["occurred_at"], "occurred_at")
    ttl = _ttl(ttl_seconds, "ttl_seconds")
    return _decide(store, principal, station_id, endpoint_id, output_id,
                   decision_key=f"playout:{event_id}", source_kind="playout_event",
                   source_id=event_id, lane="on_air", event_at=at,
                   now=_instant(now or datetime.now(timezone.utc), "now"),
                   input_body={"event_id": event_id, "phase": event["phase"],
                               "attempt_id": event["attempt_id"],
                               "attempt_finished": (attempt["state"] in {
                                   "completed", "interrupted", "failed", "skipped"} and
                                   _instant(attempt["last_event_at"], "last_event_at") > at),
                               "output_route_source": route_source,
                               "version_id": version["_id"],
                               "plan_entry_id": event.get("plan_entry_id"),
                               "display": _display(item), "ttl_seconds": ttl})


def submit_next_intent(store: Store, principal: Principal,
                       station_id: str, endpoint_id: str, output_id: str, *,
                       intent_key: str, occurred_at: str | datetime,
                       version_id: str | None, plan_entry_id: str | None = None,
                       ttl_seconds: int = 3600,
                       now: str | datetime | None = None) -> dict:
    """Record an explicit upcoming selection without claiming it has started."""
    station_id, endpoint_id, output_id = _scope(store, station_id, endpoint_id, output_id)
    require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    if not isinstance(intent_key, str) or not intent_key.strip() or len(intent_key) > 160:
        raise StoreError("intent_key must be 1..160 characters")
    ttl = _ttl(ttl_seconds, "ttl_seconds")
    version_id = _id(version_id, "version_id") if version_id else None
    display = {"title": None, "artist": None, "kind": None}
    if version_id:
        version, item = _station_version(store, station_id, version_id)
        if version["status"] != "approved" or item["lifecycle"] != "active":
            raise Conflict("next intent needs an approved active version")
        links = store.lookup("station_items", "station_item", station_id, item["_id"])
        if links[0]["eligible"] is not True:
            raise Conflict("next intent version is not station eligible")
        display = _display(item)
    entry_id = _id(plan_entry_id, "plan_entry_id") if plan_entry_id else None
    if entry_id:
        entry = store.get("plan_entries", entry_id)
        revision = store.get("plan_revisions", entry["plan_revision_id"]) if entry else None
        plan = store.get("plans", revision["plan_id"]) if revision else None
        if plan is None or plan["station_id"] != station_id or \
                entry["event_kind"] != "audio" or \
                entry.get("payload", {}).get("version_id") != version_id or \
                revision["status"] != "approved" or \
                plan.get("current_revision_id") != revision["_id"] or \
                plan["state"] in {"cancelled", "completed"}:
            raise StoreError("next intent needs a current approved station plan entry")
    return _decide(store, principal, station_id, endpoint_id, output_id,
                   decision_key=f"next:{intent_key}", source_kind="next_intent",
                   source_id=intent_key, lane="next",
                   event_at=_instant(occurred_at, "occurred_at"),
                   now=_instant(now or datetime.now(timezone.utc), "now"),
                   input_body={"version_id": version_id, "plan_entry_id": entry_id,
                               "display": display, "ttl_seconds": ttl})


def submit_talk_intent(store: Store, principal: Principal,
                       station_id: str, endpoint_id: str, output_id: str, *,
                       intent_key: str, occurred_at: str | datetime,
                       active: bool, label: str | None = None,
                       ttl_seconds: int = 7200,
                       now: str | datetime | None = None) -> dict:
    """Record explicit talk state; it takes display precedence over music."""
    station_id, endpoint_id, output_id = _scope(store, station_id, endpoint_id, output_id)
    require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    if not isinstance(intent_key, str) or not intent_key.strip() or len(intent_key) > 160:
        raise StoreError("intent_key must be 1..160 characters")
    if not isinstance(active, bool):
        raise StoreError("active must be boolean")
    if label is not None and (not isinstance(label, str) or len(label) > 160):
        raise StoreError("talk label must be at most 160 characters")
    ttl = _ttl(ttl_seconds, "ttl_seconds")
    return _decide(store, principal, station_id, endpoint_id, output_id,
                   decision_key=f"talk:{intent_key}", source_kind="talk_intent",
                   source_id=intent_key, lane="talk",
                   event_at=_instant(occurred_at, "occurred_at"),
                   now=_instant(now or datetime.now(timezone.utc), "now"),
                   input_body={"active": active, "label": label,
                               "ttl_seconds": ttl})
