"""Local emergency-alert data workflow; no CAP decoder or audio engine.

An alert can preempt normal programming in a player, but these records never
rewrite approved plans. A completed attempt is an engine report, and a linked
output observation is still a separate signal claim, not proof of the spoken
alert content. CAP parsing, signature validation and jurisdiction rules belong
in separately verified adapters.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import re
from typing import Iterable
from uuid import UUID, uuid4

from .identity import Principal, require
from .readiness import readiness_at
from .station_outputs import lookup_output
from .storage import Conflict, Store, StoreError


PURPOSES = frozenset({"actual", "test"})
ATTEMPT_TERMINALS = frozenset({"completed", "failed", "interrupted"})
_NAMESPACE = re.compile(r"[A-Za-z0-9_.:-]{1,64}\Z")
_CODE = re.compile(r"[A-Za-z0-9_.:-]{1,100}\Z")
_REASON = re.compile(r"[a-z][a-z0-9_]{0,63}\Z")
_LANGUAGE = re.compile(r"[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8}){0,3}\Z")


def _uuid(value: str, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _instant(value: str | datetime, field: str) -> datetime:
    try:
        result = (datetime.fromisoformat(value.replace("Z", "+00:00"))
                  if isinstance(value, str) else value)
    except ValueError as exc:
        raise StoreError(f"{field} must be an ISO 8601 instant") from exc
    if (not isinstance(result, datetime) or result.tzinfo is None or
            result.utcoffset() is None):
        raise StoreError(f"{field} must have a timezone")
    return result.astimezone(timezone.utc)


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _body(row: dict) -> dict:
    return {key: value for key, value in row.items() if not key.startswith("_")}


def _code(value: str, field: str) -> str:
    if not isinstance(value, str) or not _CODE.fullmatch(value):
        raise StoreError(f"{field} must be a short opaque code")
    return value


def _codes(values: Iterable[str], field: str) -> list[str]:
    if isinstance(values, (str, bytes)) or not isinstance(values, (list, tuple, set)):
        raise StoreError(f"{field} must be a list of codes")
    return sorted({_code(value, field) for value in values})


def _alert(store: Store, station_id: str, alert_id: str) -> dict:
    alert = store.get("emergency_alerts", _uuid(alert_id, "alert_id"))
    if alert is None or alert["station_id"] != station_id:
        raise Conflict("alert is not in station")
    return alert


def _endpoint(store: Store, station_id: str, endpoint_id: str) -> dict:
    endpoint = store.get("endpoints", endpoint_id)
    if endpoint is None or endpoint["station_id"] != station_id or endpoint["status"] == "retired":
        raise Conflict("endpoint is not available in station")
    return endpoint


def _version_in_station(store: Store, station_id: str, version_id: str) -> dict:
    version = store.get("versions", version_id)
    if version is None or not store.lookup("station_items", "station_item",
                                           station_id, version["item_id"]):
        raise Conflict("alert media version is not in station")
    return version


def register_alert(store: Store, principal: Principal, station_id: str, *,
                   source_namespace: str, external_alert_key: str,
                   purpose: str, starts_at: str | datetime,
                   ends_at: str | datetime, geo_codes: Iterable[str],
                   output_ids: Iterable[str], repeat_interval_seconds: int = 0,
                   max_attempts: int | None = None,
                   media_version_id: str | None = None,
                   tts_text: str | None = None,
                   tts_language: str | None = None) -> dict:
    """Register one scoped alert; repeated source keys must have equal data.

    ``geo_codes`` are exact local routing codes. No country-specific hierarchy
    is assumed. Empty lists mean no location/output and are rejected.
    """
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "operate", station_id)
    if principal.scope != "user":
        raise StoreError("alert registration requires a user/service principal")
    if purpose not in PURPOSES:
        raise StoreError("alert purpose must be actual or test")
    if purpose == "test":
        require(store, principal, "manage", station_id)
    if not isinstance(source_namespace, str) or not _NAMESPACE.fullmatch(source_namespace):
        raise StoreError("source_namespace must be an opaque 1..64 character name")
    external_alert_key = _code(external_alert_key, "external_alert_key")
    start, end = _instant(starts_at, "starts_at"), _instant(ends_at, "ends_at")
    if end <= start:
        raise StoreError("alert end must follow start")
    geos, outputs = _codes(geo_codes, "geo_codes"), _codes(output_ids, "output_ids")
    if not geos or not outputs:
        raise StoreError("alert needs at least one geo code and output id")
    if (isinstance(repeat_interval_seconds, bool) or
            not isinstance(repeat_interval_seconds, int) or
            repeat_interval_seconds < 0 or repeat_interval_seconds > 86400):
        raise StoreError("repeat_interval_seconds must be 0..86400")
    if (max_attempts is not None and
            (isinstance(max_attempts, bool) or not isinstance(max_attempts, int) or
             max_attempts < 1 or max_attempts > 10000)):
        raise StoreError("max_attempts must be 1..10000")
    media_version_id = (_uuid(media_version_id, "media_version_id")
                        if media_version_id is not None else None)
    if tts_text is not None and (not isinstance(tts_text, str) or
                                 not tts_text.strip() or len(tts_text) > 4000 or
                                 any(ord(char) < 32 and char not in "\n\r\t"
                                     for char in tts_text)):
        raise StoreError("tts_text must be 1..4000 printable characters")
    if not media_version_id and not tts_text:
        raise StoreError("alert needs media_version_id or tts_text")
    if tts_text and (not isinstance(tts_language, str) or
                     not _LANGUAGE.fullmatch(tts_language)):
        raise StoreError("tts_language is required for TTS text")
    if not tts_text and tts_language is not None:
        raise StoreError("tts_language requires tts_text")
    identity = {"station_id": station_id, "source_namespace": source_namespace,
                "external_alert_key": external_alert_key, "purpose": purpose,
                "starts_at": start.isoformat(), "ends_at": end.isoformat(),
                "geo_codes": geos, "output_ids": outputs,
                "repeat_interval_seconds": repeat_interval_seconds,
                "max_attempts": max_attempts, "media_version_id": media_version_id,
                "tts_text": tts_text.strip() if tts_text else None,
                "tts_language": tts_language}
    prior = store.lookup("emergency_alerts", "source_key", station_id,
                         source_namespace, external_alert_key)
    if prior:
        row = prior[0]
        if any(row.get(key) != value for key, value in identity.items()):
            raise Conflict("external alert key was reused with changed content")
        return row | {"replayed": True}
    sequence = store.sequence
    if media_version_id:
        _version_in_station(store, station_id, media_version_id)
    identifier = str(uuid4())
    data = identity | {"status": "active", "received_at": _now(),
                       "created_by": principal.user_id, "cancelled_at": None,
                       "cancelled_by": None, "cancel_reason": None}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("emergency_alerts", identifier, data)
    try:
        tx.commit()
    except Conflict:
        prior = store.lookup("emergency_alerts", "source_key", station_id,
                             source_namespace, external_alert_key)
        if not prior:
            raise
        row = prior[0]
        if any(row.get(key) != value for key, value in identity.items()):
            raise Conflict("external alert key was reused with changed content")
        return row | {"replayed": True}
    return store.get("emergency_alerts", identifier) | {"replayed": False}


def cancel_alert(store: Store, principal: Principal, station_id: str,
                 alert_id: str, *, expected_revision: int,
                 reason_code: str = "source_cancelled") -> dict:
    """Stop new starts; any already started attempt needs its own final event."""
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "operate", station_id)
    if principal.scope != "user":
        raise StoreError("alert cancellation requires a user/service principal")
    if not isinstance(reason_code, str) or not _REASON.fullmatch(reason_code):
        raise StoreError("invalid cancellation reason code")
    row = _alert(store, station_id, alert_id)
    if row["_rev"] != expected_revision or row["status"] != "active":
        raise Conflict("alert changed or is already cancelled")
    return store.put("emergency_alerts", row["_id"],
                     _body(row) | {"status": "cancelled", "cancelled_at": _now(),
                                   "cancelled_by": principal.user_id,
                                   "cancel_reason": reason_code},
                     expected_revision=expected_revision,
                     actor=principal.user_id)


def _eligible(store: Store, alert: dict, endpoint_id: str, output_id: str,
              geo_code: str, at: datetime) -> dict:
    _endpoint(store, alert["station_id"], endpoint_id)
    mapping = lookup_output(store, alert["station_id"], endpoint_id, output_id)
    if mapping is None or mapping["status"] != "active" or \
            geo_code not in mapping["geo_codes"]:
        raise Conflict("station output is inactive or does not serve claimed geo code")
    if alert["status"] != "active" or not (
            _instant(alert["starts_at"], "starts_at") <= at <
            _instant(alert["ends_at"], "ends_at")):
        raise Conflict("alert is cancelled or outside its time window")
    if output_id not in alert["output_ids"] or geo_code not in alert["geo_codes"]:
        raise Conflict("alert is outside output or location scope")
    return mapping


def _representation(store: Store, alert: dict, endpoint_id: str,
                    output_id: str, at: datetime, tts_capable: bool) -> dict:
    version_id = alert.get("media_version_id")
    media_blockers: list[str] = []
    if version_id:
        result = readiness_at(store, alert["station_id"], version_id,
                              slot_start=at, slot_end=at + timedelta(seconds=1),
                              endpoint_id=endpoint_id, require_endpoint_copy=True,
                              output_id=output_id)
        if result["ready"]:
            return {"representation_kind": "audio_version",
                    "version_id": version_id,
                    "media_file_id": result["media_file_id"]}
        media_blockers = result["blocked_reasons"]
    if alert.get("tts_text") and tts_capable:
        return {"representation_kind": "tts_text", "version_id": None,
                "media_file_id": None, "tts_language": alert["tts_language"]}
    raise Conflict("alert representation unavailable: " +
                   ", ".join(media_blockers or ["tts_not_available"]))


def start_alert_attempt(store: Store, principal: Principal, station_id: str,
                        endpoint_id: str, alert_id: str, *,
                        attempt_key: str, output_id: str, geo_code: str,
                        started_at: str | datetime, tts_capable: bool = False,
                        authorize_test: bool = False) -> dict:
    """Record a player start claim with a pinned representation and scope."""
    station_id = _uuid(station_id, "station_id")
    endpoint_id = _uuid(endpoint_id, "endpoint_id")
    require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    if not isinstance(tts_capable, bool) or not isinstance(authorize_test, bool):
        raise StoreError("tts_capable and authorize_test must be booleans")
    attempt_key = _code(attempt_key, "attempt_key")
    output_id, geo_code = _code(output_id, "output_id"), _code(geo_code, "geo_code")
    started = _instant(started_at, "started_at")
    alert = _alert(store, station_id, alert_id)
    if alert["purpose"] == "test":
        if not authorize_test or principal.scope != "user":
            raise Conflict("test alert needs explicit authorized user action")
        require(store, principal, "manage", station_id)
    prior = store.lookup("emergency_alert_attempts", "endpoint_key",
                         endpoint_id, attempt_key)
    if prior:
        row = prior[0]
        if (row["alert_id"] != alert["_id"] or row["station_id"] != station_id or
                row["output_id"] != output_id or row["geo_code"] != geo_code or
                row["started_at"] != started.isoformat()):
            raise Conflict("attempt key was reused for a different alert start")
        return row | {"replayed": True}
    sequence = store.sequence
    output_mapping = _eligible(store, alert, endpoint_id, output_id, geo_code, started)
    prior_for_alert = store.lookup("emergency_alert_attempts", "alert", alert["_id"])
    same_output = [row for row in prior_for_alert
                   if row["endpoint_id"] == endpoint_id and
                   row["output_id"] == output_id]
    if alert["max_attempts"] is not None and len(same_output) >= alert["max_attempts"]:
        raise Conflict("alert attempt limit reached for output")
    if same_output:
        latest = max(_instant(row["started_at"], "started_at") for row in same_output)
        if started < latest + timedelta(seconds=alert["repeat_interval_seconds"]):
            raise Conflict("alert repeat interval has not elapsed")
    for row in store.lookup("emergency_alert_attempts", "endpoint", endpoint_id):
        if row["output_id"] == output_id and row["state"] == "started":
            raise Conflict("another alert attempt is active on output")
    choice = _representation(store, alert, endpoint_id, output_id, started, tts_capable)
    identifier = str(uuid4())
    body = {"station_id": station_id, "endpoint_id": endpoint_id,
            "alert_id": alert["_id"], "attempt_key": attempt_key,
            "output_id": output_id, "geo_code": geo_code,
            "station_output_id": output_mapping["_id"],
            "station_output_revision": output_mapping["_rev"],
            "purpose": alert["purpose"], "state": "started",
            "started_at": started.isoformat(), "ended_at": None,
            "reported_by": principal.user_id, "finish_key": None,
            "finish_reason": None, "finished_by": None} | choice
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("emergency_alert_attempts", identifier, body)
    try:
        tx.commit()
    except Conflict:
        prior = store.lookup("emergency_alert_attempts", "endpoint_key",
                             endpoint_id, attempt_key)
        if not prior:
            raise
        row = prior[0]
        if (row["alert_id"] != alert["_id"] or row["station_id"] != station_id or
                row["output_id"] != output_id or row["geo_code"] != geo_code or
                row["started_at"] != started.isoformat()):
            raise Conflict("attempt key was reused for a different alert start")
        return row | {"replayed": True}
    return store.get("emergency_alert_attempts", identifier) | {"replayed": False}


def finish_alert_attempt(store: Store, principal: Principal, station_id: str,
                         endpoint_id: str, attempt_id: str, *,
                         finish_key: str, state: str,
                         ended_at: str | datetime,
                         reason_code: str | None = None,
                         expected_revision: int | None = None) -> dict:
    """Record completion, failure or interruption of one player attempt."""
    station_id, endpoint_id = (_uuid(station_id, "station_id"),
                               _uuid(endpoint_id, "endpoint_id"))
    require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    finish_key = _code(finish_key, "finish_key")
    if state not in ATTEMPT_TERMINALS:
        raise StoreError("attempt terminal state must be completed, failed or interrupted")
    if state == "completed" and reason_code is not None:
        raise StoreError("completed attempt cannot have a failure reason")
    if state != "completed" and (not isinstance(reason_code, str) or
                                  not _REASON.fullmatch(reason_code)):
        raise StoreError("failed/interrupted attempt needs a reason code")
    ended = _instant(ended_at, "ended_at")
    row = store.get("emergency_alert_attempts", _uuid(attempt_id, "attempt_id"))
    if row is None or row["station_id"] != station_id or row["endpoint_id"] != endpoint_id:
        raise Conflict("attempt is outside station or endpoint")
    if ended < _instant(row["started_at"], "started_at"):
        raise StoreError("attempt end precedes start")
    if row["state"] != "started":
        if (row["state"] == state and row.get("finish_key") == finish_key and
                row.get("finish_reason") == reason_code and
                row.get("ended_at") == ended.isoformat()):
            return row | {"replayed": True}
        raise Conflict("attempt is already terminal")
    if expected_revision is not None and row["_rev"] != expected_revision:
        raise Conflict("attempt changed")
    return store.put("emergency_alert_attempts", row["_id"],
                     _body(row) | {"state": state, "ended_at": ended.isoformat(),
                                   "finish_key": finish_key,
                                   "finish_reason": reason_code,
                                   "finished_by": principal.user_id},
                     expected_revision=row["_rev"], actor=principal.user_id) | {
                         "replayed": False}


def link_output_observation(store: Store, principal: Principal, station_id: str,
                            endpoint_id: str, attempt_id: str,
                            observation_id: str) -> dict:
    """Link a separate output signal claim to one alert playout attempt."""
    station_id, endpoint_id = (_uuid(station_id, "station_id"),
                               _uuid(endpoint_id, "endpoint_id"))
    require(store, principal, "operate", station_id, endpoint_id=endpoint_id)
    attempt = store.get("emergency_alert_attempts", _uuid(attempt_id, "attempt_id"))
    observation = store.get("output_observations", _uuid(observation_id, "observation_id"))
    if (attempt is None or observation is None or
            attempt["station_id"] != station_id or
            observation["station_id"] != station_id or
            attempt["endpoint_id"] != endpoint_id or
            observation["endpoint_id"] != endpoint_id or
            observation.get("details", {}).get("output_id") != attempt["output_id"] or
            observation.get("event_id") is not None or
            observation.get("attempt_id") is not None):
        raise Conflict("output observation is not independent and output-matched")
    observed = _instant(observation["occurred_at"], "occurred_at")
    start = _instant(attempt["started_at"], "started_at")
    end = (_instant(attempt["ended_at"], "ended_at")
           if attempt.get("ended_at") else datetime.now(timezone.utc))
    if observed < start or observed > end + timedelta(seconds=30):
        raise Conflict("output observation is outside alert attempt window")
    prior = store.lookup("emergency_alert_observation_links", "observation",
                         observation["_id"])
    if prior:
        if prior[0]["attempt_id"] != attempt["_id"]:
            raise Conflict("observation is already linked to another alert attempt")
        return prior[0] | {"replayed": True}
    sequence = store.sequence
    body = {"station_id": station_id, "endpoint_id": endpoint_id,
            "alert_id": attempt["alert_id"], "attempt_id": attempt["_id"],
            "observation_id": observation["_id"],
            "observation_kind": observation["kind"],
            "observer_kind": observation.get("observer_kind", "endpoint_self_report"),
            "linked_at": _now(), "linked_by": principal.user_id}
    identifier = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("emergency_alert_observation_links", identifier, body)
    try:
        tx.commit()
    except Conflict:
        prior = store.lookup("emergency_alert_observation_links", "observation",
                             observation["_id"])
        if not prior or prior[0]["attempt_id"] != attempt["_id"]:
            raise
        return prior[0] | {"replayed": True}
    return store.get("emergency_alert_observation_links", identifier) | {"replayed": False}


def list_alerts(store: Store, principal: Principal, station_id: str, *,
                include_cancelled: bool = False) -> list[dict]:
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "view", station_id)
    if not isinstance(include_cancelled, bool):
        raise StoreError("include_cancelled must be a boolean")
    rows = store.lookup("emergency_alerts", "station", station_id)
    if not include_cancelled:
        rows = [row for row in rows if row["status"] == "active"]
    return sorted(rows, key=lambda row: (row["starts_at"], row["_id"]))
