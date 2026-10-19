"""Scheduled recording definitions and evidence-bound endpoint claims.

This module schedules a capture and records an endpoint's start/end claims.
It does not capture audio, operate GPI hardware or prove that a microphone was
audible. A completed attempt must name centrally verified, rehashed bytes for
the job's exact destination version; failed and partial attempts remain visible.
"""

from __future__ import annotations

from datetime import date, datetime, time, timedelta, timezone
from hashlib import sha256
import json
from typing import Any, Mapping
from urllib.parse import urlparse
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo

from .identity import Principal, require
from .media import _digest, object_path
from .schedules import _local_instant
from .storage import Conflict, Store, StoreError


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


def _clock(value: Any, name: str) -> str:
    try:
        parsed = time.fromisoformat(value) if isinstance(value, str) else value
    except ValueError as exc:
        raise StoreError(f"{name} must be a local ISO time") from exc
    if not isinstance(parsed, time) or parsed.tzinfo is not None:
        raise StoreError(f"{name} must be a local ISO time without offset")
    return parsed.isoformat()


def _instant(value: Any, name: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00")) if isinstance(value, str) else value
    except ValueError as exc:
        raise StoreError(f"{name} must be an ISO instant") from exc
    if not isinstance(parsed, datetime) or parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError(f"{name} needs a timezone offset")
    return parsed.astimezone(timezone.utc)


def _integer(value: Any, name: str, low: int, high: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise StoreError(f"{name} must be an integer in {low}..{high}")
    return value


def _text(value: Any, name: str, max_length: int = 200) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > max_length:
        raise StoreError(f"{name} must be nonempty text up to {max_length} characters")
    return value.strip()


def _object(value: Any, name: str) -> dict:
    if not isinstance(value, Mapping) or any(not isinstance(k, str) for k in value):
        raise StoreError(f"{name} must be an object with string keys")
    try:
        return json.loads(json.dumps(dict(value), ensure_ascii=False, allow_nan=False))
    except (TypeError, ValueError) as exc:
        raise StoreError(f"{name} must be finite JSON data") from exc


def _fingerprint(value: Any) -> str:
    data = json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")
    return sha256(data).hexdigest()


def _body(row: dict) -> dict:
    return {key: value for key, value in row.items() if key not in ("_id", "_rev")}


def _source(value: Any) -> dict:
    source = _object(value, "source")
    kind = source.get("kind")
    if kind == "input" and set(source) == {"kind", "input_id"}:
        return {"kind": kind, "input_id": _text(source["input_id"], "input_id")}
    if kind == "stream" and set(source) == {"kind", "uri"}:
        uri = _text(source["uri"], "source.uri", 2048)
        parsed = urlparse(uri)
        if parsed.scheme not in {"http", "https", "rtsp", "rtmp", "srt", "udp", "icecast"} or not parsed.netloc:
            raise StoreError("source.uri needs a supported scheme and host")
        if parsed.username is not None or parsed.password is not None:
            raise StoreError("source.uri must not embed credentials")
        return {"kind": kind, "uri": uri}
    raise StoreError("source must be a named audio input or stream URI")


def _schedule(value: Any) -> dict:
    row = _object(value, "schedule")
    if set(row) - {"weekdays", "starts_on", "ends_on", "dst_fold"} or \
            not {"weekdays", "starts_on"} <= set(row):
        raise StoreError("schedule has missing or unsupported fields")
    weekdays = row["weekdays"]
    if not isinstance(weekdays, list) or not weekdays or \
            any(type(day) is not int or day not in range(1, 8) for day in weekdays) or \
            len(weekdays) != len(set(weekdays)):
        raise StoreError("schedule.weekdays must be unique ISO weekdays 1..7")
    start = _day(row["starts_on"], "starts_on")
    end = _day(row["ends_on"], "ends_on") if row.get("ends_on") is not None else None
    if end is not None and end < start:
        raise StoreError("schedule ends before it starts")
    first_match = next(start + timedelta(days=offset) for offset in range(7)
                       if (start + timedelta(days=offset)).isoweekday() in weekdays)
    if end is not None and first_match > end:
        raise StoreError("schedule date range has no active weekday")
    return {"weekdays": sorted(weekdays), "starts_on": start.isoformat(),
            "ends_on": end.isoformat() if end else None,
            "dst_fold": _integer(row.get("dst_fold", 0), "dst_fold", 0, 1)}


def _gpi(value: dict, *, start: bool) -> dict:
    required = {"kind", "device_id", "line"}
    allowed = required | ({"window_start", "window_end", "delay_ms"} if start else {"max_duration_ms"})
    if not required <= set(value) or set(value) - allowed:
        raise StoreError("GPI trigger has missing or unsupported fields")
    result = {"kind": "gpi", "device_id": _text(value["device_id"], "device_id"),
              "line": _integer(value["line"], "line", 0, 65535)}
    if start:
        if not {"window_start", "window_end"} <= set(value):
            raise StoreError("GPI start needs a recognition window")
        result.update({"window_start": _clock(value["window_start"], "window_start"),
                       "window_end": _clock(value["window_end"], "window_end"),
                       "delay_ms": _integer(value.get("delay_ms", 0), "delay_ms", 0, 3_600_000)})
    else:
        result["max_duration_ms"] = _integer(value.get("max_duration_ms"),
                                               "max_duration_ms", 1, 172_800_000)
    return result


def _start_trigger(value: Any) -> dict:
    row = _object(value, "start_trigger")
    if row.get("kind") == "gpi":
        return _gpi(row, start=True)
    if row.get("kind") == "clock" and {"kind", "local_time"} <= set(row) and \
            not set(row) - {"kind", "local_time", "grace_ms"}:
        return {"kind": "clock", "local_time": _clock(row["local_time"], "local_time"),
                "grace_ms": _integer(row.get("grace_ms", 60_000), "grace_ms", 0, 3_600_000)}
    raise StoreError("start_trigger must be clock or GPI with valid fields")


def _end_trigger(value: Any) -> dict:
    row = _object(value, "end_trigger")
    if row.get("kind") == "gpi":
        return _gpi(row, start=False)
    if row.get("kind") == "duration" and set(row) == {"kind", "duration_ms"}:
        return {"kind": "duration", "duration_ms": _integer(row["duration_ms"],
                                                              "duration_ms", 1, 172_800_000)}
    if row.get("kind") == "clock" and set(row) == {"kind", "local_time"}:
        return {"kind": "clock", "local_time": _clock(row["local_time"], "local_time")}
    raise StoreError("end_trigger must be duration, clock or GPI with valid fields")


def _job(store: Store, station_id: str, job_id: str) -> dict:
    row = store.get("recording_jobs", _id(job_id, "job_id"))
    if row is None or row["station_id"] != station_id:
        raise StoreError("recording job is outside station")
    return row


def _endpoint_permission(store: Store, principal: Principal, station_id: str,
                         endpoint_id: str) -> None:
    require(store, principal, "operate", station_id,
            endpoint_id=endpoint_id if principal.scope == "endpoint" else None)


def _editable_destination(store: Store, station_id: str, version_id: str) -> dict:
    version = store.get("versions", version_id)
    if version is None or version["status"] not in {"draft", "review"}:
        raise Conflict("recording destination must remain an editable content version")
    associations = store.lookup("station_items", "station_item",
                                station_id, version["item_id"])
    if not associations or not associations[0].get("eligible"):
        raise Conflict("recording destination is no longer eligible in station")
    return version


def create_recording_job(store: Store, principal: Principal, station_id: str, *,
                         endpoint_id: str, code: str, name: str, source: Mapping[str, Any],
                         destination_version_id: str, schedule: Mapping[str, Any],
                         start_trigger: Mapping[str, Any], end_trigger: Mapping[str, Any],
                         allow_multiple: bool = False, one_shot: bool = False) -> dict:
    station_id, endpoint_id = _id(station_id, "station_id"), _id(endpoint_id, "endpoint_id")
    destination_version_id = _id(destination_version_id, "destination_version_id")
    require(store, principal, "edit", station_id)
    endpoint = store.get("endpoints", endpoint_id)
    if endpoint is None or endpoint["station_id"] != station_id or endpoint["status"] == "retired":
        raise StoreError("recording endpoint is outside station or retired")
    _editable_destination(store, station_id, destination_version_id)
    if type(allow_multiple) is not bool or type(one_shot) is not bool:
        raise StoreError("allow_multiple and one_shot must be Boolean")
    if allow_multiple and one_shot:
        raise StoreError("a one-shot job cannot allow multiple recordings")
    row = {"station_id": station_id, "endpoint_id": endpoint_id,
           "code": _text(code, "code", 80), "name": _text(name, "name"),
           "status": "active", "source": _source(source),
           "destination_version_id": destination_version_id,
           "schedule": _schedule(schedule), "start_trigger": _start_trigger(start_trigger),
           "end_trigger": _end_trigger(end_trigger),
           "allow_multiple": allow_multiple, "one_shot": one_shot,
           "created_by": principal.user_id,
           "created_at": datetime.now(timezone.utc).isoformat()}
    identifier = str(uuid4())
    return store.put("recording_jobs", identifier, row, actor=principal.user_id)


def set_recording_job_status(store: Store, principal: Principal, station_id: str,
                             job_id: str, status: str, *, expected_revision: int) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    job = _job(store, station_id, job_id)
    if job["_rev"] != expected_revision:
        raise Conflict("recording job changed")
    if status not in {"active", "disabled"}:
        raise StoreError("recording job status must be active or disabled")
    if status == "active" and job.get("one_shot") and \
            store.lookup("recording_attempts", "job", job["_id"]):
        raise Conflict("a fired one-shot job cannot be reactivated")
    if job["status"] == status:
        return job
    return store.put("recording_jobs", job["_id"],
                     _body(job) | {"status": status, "status_changed_by": principal.user_id,
                                   "status_changed_at": datetime.now(timezone.utc).isoformat()},
                     expected_revision=job["_rev"], actor=principal.user_id)


def _window(job: dict, day: date, zone: ZoneInfo) -> dict:
    schedule = job["schedule"]
    if job["status"] != "active" or day.isoweekday() not in schedule["weekdays"] or \
            day.isoformat() < schedule["starts_on"] or \
            schedule.get("ends_on") and day.isoformat() > schedule["ends_on"]:
        return {"status": "inactive", "service_date": day.isoformat()}
    fold = schedule["dst_fold"]
    start_spec = job["start_trigger"]
    if start_spec["kind"] == "clock":
        start, note = _local_instant(day, start_spec["local_time"], zone, fold)
        if start is None:
            return {"status": "skipped", "reason": "dst_gap", "service_date": day.isoformat()}
        start_min = start.astimezone(timezone.utc)
        start_max = start_min + timedelta(milliseconds=start_spec["grace_ms"])
    else:
        start, note = _local_instant(day, start_spec["window_start"], zone, fold)
        end_day = day + timedelta(days=int(start_spec["window_end"] <= start_spec["window_start"]))
        end, end_note = _local_instant(end_day, start_spec["window_end"], zone, fold)
        if start is None or end is None:
            return {"status": "skipped", "reason": "dst_gap", "service_date": day.isoformat()}
        start_min, start_max = start.astimezone(timezone.utc), end.astimezone(timezone.utc)
        if start_max <= start_min:
            raise Conflict("GPI recognition window is empty after timezone conversion")
        note = note or end_note
    end_spec = job["end_trigger"]
    if end_spec["kind"] in {"duration", "gpi"}:
        maximum = end_spec.get("duration_ms", end_spec.get("max_duration_ms"))
        latest_end = start_max + timedelta(milliseconds=maximum)
    else:
        end_day = day + timedelta(days=int(end_spec["local_time"] <=
                                            (start_spec.get("local_time") or start_spec.get("window_start"))))
        end, end_note = _local_instant(end_day, end_spec["local_time"], zone, fold)
        if end is None:
            return {"status": "skipped", "reason": "dst_gap", "service_date": day.isoformat()}
        latest_end = end.astimezone(timezone.utc)
        note = note or end_note
    if latest_end <= start_max or latest_end - start_min > timedelta(hours=49):
        raise Conflict("recording start/end window is invalid")
    return {"status": "ready", "service_date": day.isoformat(),
            "start_window_at": start_min.isoformat(),
            "start_window_end_at": start_max.isoformat(),
            "latest_end_at": latest_end.isoformat(), "dst_note": note}


def preview_recording_job(store: Store, principal: Principal, station_id: str,
                          job_id: str, service_date: str | date) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "view", station_id)
    sequence = store.sequence
    job = _job(store, station_id, job_id)
    station = store.get("stations", station_id)
    window = _window(job, _day(service_date, "service_date"), ZoneInfo(station["timezone"]))
    if store.sequence != sequence:
        raise Conflict("recording job changed during preview")
    return window | {"job_id": job["_id"], "job_revision": job["_rev"],
                     "endpoint_id": job["endpoint_id"], "sequence": sequence}


def _signal(value: Any, spec: Mapping[str, Any], *, required: bool) -> dict | None:
    if spec["kind"] != "gpi":
        if value is not None:
            raise StoreError("clock or duration trigger cannot include a GPI signal")
        return None
    if value is None:
        if required:
            raise StoreError("GPI trigger needs a matching signal")
        return None
    row = _object(value, "trigger_signal")
    if set(row) != {"device_id", "line"} or row["device_id"] != spec["device_id"] or \
            row["line"] != spec["line"]:
        raise StoreError("GPI signal does not match the recording job")
    return row


def _event_replay(store: Store, endpoint_id: str, event_key: str,
                  fingerprint: str) -> dict | None:
    matches = store.lookup("recording_events", "endpoint_key", endpoint_id, event_key)
    if not matches:
        return None
    event = matches[0]
    if event["claim_fingerprint"] != fingerprint:
        raise Conflict("recording event key was already used for a different claim")
    attempt = store.get("recording_attempts", event["attempt_id"])
    if attempt is None:
        raise Conflict("recording event references a missing attempt")
    return {"attempt_id": attempt["_id"], "status": attempt["status"],
            "event_id": event["_id"], "replayed": True}


def start_recording_attempt(store: Store, principal: Principal, station_id: str,
                            job_id: str, service_date: str | date, *,
                            event_key: str, occurred_at: str | datetime,
                            trigger_signal: Mapping[str, Any] | None = None) -> dict:
    station_id = _id(station_id, "station_id")
    job = _job(store, station_id, job_id)
    _endpoint_permission(store, principal, station_id, job["endpoint_id"])
    day = _day(service_date, "service_date")
    event_key = _text(event_key, "event_key")
    occurred = _instant(occurred_at, "occurred_at")
    signal = _signal(trigger_signal, job["start_trigger"], required=True)
    claim = {"job_id": job["_id"], "service_date": day.isoformat(),
             "event_kind": "started", "event_key": event_key,
             "occurred_at": occurred.isoformat(), "trigger_signal": signal}
    digest = _fingerprint(claim)
    replay = _event_replay(store, job["endpoint_id"], event_key, digest)
    if replay:
        return replay
    sequence = store.sequence
    _editable_destination(store, station_id, job["destination_version_id"])
    station = store.get("stations", station_id)
    if station is None:
        raise StoreError("station does not exist")
    window = _window(job, day, ZoneInfo(station["timezone"]))
    if window["status"] != "ready":
        raise Conflict("recording job has no active window on this date")
    first = _instant(window["start_window_at"], "start_window_at")
    last = _instant(window["start_window_end_at"], "start_window_end_at")
    if occurred < first or occurred > last:
        raise Conflict("recording start claim is outside the recognition window")
    previous = store.lookup("recording_attempts", "job_date", job["_id"], day.isoformat())
    if previous and not job["allow_multiple"]:
        raise Conflict("recording job already fired on this date")
    if any(row["status"] == "recording" for row in previous):
        raise Conflict("a previous recording attempt is still open")
    delay = job["start_trigger"].get("delay_ms", 0)
    actual_start = occurred + timedelta(milliseconds=delay)
    end_spec = job["end_trigger"]
    deadline = (_instant(window["latest_end_at"], "latest_end_at") if end_spec["kind"] == "clock" else
                actual_start + timedelta(milliseconds=end_spec.get("duration_ms", end_spec.get("max_duration_ms"))))
    if deadline <= actual_start:
        raise Conflict("recording deadline is at or before actual start")
    attempt_id, event_id = str(uuid4()), str(uuid4())
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("recording_attempts", attempt_id, {
        "job_id": job["_id"], "station_id": station_id, "endpoint_id": job["endpoint_id"],
        "occurrence_date": day.isoformat(), "status": "recording",
        "start_event_key": event_key, "actual_start_at": actual_start.isoformat(),
        "deadline_at": deadline.isoformat(), "created_at": now,
        "claimed_by": principal.user_id})
    tx.put("recording_events", event_id, {
        "attempt_id": attempt_id, "endpoint_id": job["endpoint_id"],
        "event_key": event_key, "event_kind": "started",
        "occurred_at": occurred.isoformat(), "claim_fingerprint": digest,
        "trigger_signal": signal, "claimed_by": principal.user_id})
    if job["one_shot"]:
        tx.put("recording_jobs", job["_id"],
               _body(job) | {"status": "disabled", "status_reason": "one_shot_fired",
                              "status_changed_at": now}, expected_revision=job["_rev"])
    tx.commit()
    return {"attempt_id": attempt_id, "status": "recording", "event_id": event_id,
            "actual_start_at": actual_start.isoformat(),
            "deadline_at": deadline.isoformat(), "replayed": False}


def _verified_media(store: Store, job: dict, media_file_id: str) -> dict:
    media = store.get("media_files", _id(media_file_id, "media_file_id"))
    if media is None or media["version_id"] != job["destination_version_id"] or \
            media["status"] != "verified":
        raise Conflict("recording completion requires verified media for destination version")
    digest = media.get("sha256")
    size = media.get("size_bytes")
    if not isinstance(digest, str) or len(digest) != 64 or \
            any(char not in "0123456789abcdef" for char in digest) or \
            type(size) is not int or size < 0:
        raise Conflict("verified media lacks a valid digest and byte size")
    locations = store.lookup("media_locations", "file_uri", media["_id"], "sdb-object:" + digest)
    if len(locations) != 1 or locations[0]["status"] != "verified":
        raise Conflict("recording media has no verified managed location")
    path = object_path(store, digest)
    if path.is_symlink() or not path.is_file():
        raise Conflict("recording media object is missing")
    actual_digest, actual_size = _digest(path)
    if actual_digest != digest or actual_size != size:
        raise Conflict("recording media object failed SHA-256 or size verification")
    return {"media_file_id": media["_id"], "version_id": media["version_id"],
            "sha256": digest, "size_bytes": size,
            "media_revision": media["_rev"], "location_id": locations[0]["_id"],
            "location_revision": locations[0]["_rev"]}


def finish_recording_attempt(store: Store, principal: Principal, station_id: str,
                             attempt_id: str, *, event_key: str,
                             occurred_at: str | datetime, outcome: str,
                             media_file_id: str | None = None,
                             reason_code: str | None = None,
                             trigger_signal: Mapping[str, Any] | None = None) -> dict:
    station_id, attempt_id = _id(station_id, "station_id"), _id(attempt_id, "attempt_id")
    attempt = store.get("recording_attempts", attempt_id)
    if attempt is None or attempt["station_id"] != station_id:
        raise StoreError("recording attempt is outside station")
    job = _job(store, station_id, attempt["job_id"])
    _endpoint_permission(store, principal, station_id, attempt["endpoint_id"])
    event_key = _text(event_key, "event_key")
    occurred = _instant(occurred_at, "occurred_at")
    if outcome not in {"completed", "partial", "failed"}:
        raise StoreError("outcome must be completed, partial or failed")
    reason = _text(reason_code, "reason_code", 80) if reason_code is not None else None
    if outcome != "completed" and reason is None:
        raise StoreError("partial or failed recording needs reason_code")
    if outcome == "completed" and reason is not None:
        raise StoreError("completed recording cannot have a failure reason")
    if outcome == "failed" and media_file_id is not None:
        raise StoreError("failed recording cannot name completed media")
    media_id = _id(media_file_id, "media_file_id") if media_file_id is not None else None
    if outcome == "completed" and media_id is None:
        raise StoreError("completed recording needs media_file_id")
    signal = _signal(trigger_signal, job["end_trigger"], required=outcome == "completed")
    claim = {"attempt_id": attempt_id, "event_kind": "finished", "event_key": event_key,
             "occurred_at": occurred.isoformat(), "outcome": outcome,
             "reason_code": reason, "media_file_id": media_id, "trigger_signal": signal}
    digest = _fingerprint(claim)
    replay = _event_replay(store, attempt["endpoint_id"], event_key, digest)
    if replay:
        return replay
    if attempt["status"] != "recording":
        raise Conflict("recording attempt is already closed")
    start = _instant(attempt["actual_start_at"], "actual_start_at")
    if occurred <= start:
        raise StoreError("recording end must follow actual start")
    deadline = _instant(attempt["deadline_at"], "deadline_at")
    if outcome == "completed" and occurred > deadline:
        raise Conflict("recording exceeded its end deadline; record partial or failed")
    sequence = store.sequence
    evidence = _verified_media(store, job, media_id) if media_id else None
    event_id = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    if evidence:
        tx.expect("media_files", evidence["media_file_id"], evidence["media_revision"])
        tx.expect("media_locations", evidence["location_id"], evidence["location_revision"])
    tx.put("recording_attempts", attempt_id,
           _body(attempt) | {"status": outcome, "actual_end_at": occurred.isoformat(),
                             "completion_event_key": event_key, "media_file_id": media_id,
                             "media_sha256": evidence["sha256"] if evidence else None,
                             "media_size_bytes": evidence["size_bytes"] if evidence else None,
                             "reason_code": reason, "completed_by": principal.user_id},
           expected_revision=attempt["_rev"])
    tx.put("recording_events", event_id, {
        "attempt_id": attempt_id, "endpoint_id": attempt["endpoint_id"],
        "event_key": event_key, "event_kind": "finished",
        "occurred_at": occurred.isoformat(), "claim_fingerprint": digest,
        "outcome": outcome, "reason_code": reason,
        "media_file_id": media_id, "trigger_signal": signal,
        "claimed_by": principal.user_id})
    tx.commit()
    return {"attempt_id": attempt_id, "status": outcome,
            "event_id": event_id, "media_file_id": media_id, "replayed": False}
