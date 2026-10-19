"""Mount-scoped listener-count observations and time-bucket projections.

Counts are observations reported by an authorized source, not evidence that
audio reached each listener. An absent, offline, unknown, or stale observation
never becomes a numeric zero. Samples are immutable; a source key may only be
replayed with the same canonical claim.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import json
import re
from typing import Any
from uuid import UUID, uuid4

from .identity import Principal, require
from .station_outputs import _CODE, lookup_output
from .storage import Conflict, Store, StoreError


MOUNT_STATUSES = frozenset({"active", "disabled"})
SAMPLE_STATES = frozenset({"online", "offline", "unknown"})
_NAMESPACE = re.compile(r"[A-Za-z0-9_.:-]{1,64}\Z")
_MOUNT_PATH = re.compile(r"/[A-Za-z0-9._~/-]{1,199}\Z")
_BUCKET_SECONDS = {"minute": 60, "hour": 3600, "day": 86400}
_MAX_BUCKETS = 2000
_MAX_CELLS = 10000
_MAX_SAMPLES = 500000
_MAX_FUTURE = timedelta(minutes=5)
_MAX_HISTORY = timedelta(days=366)


def _uuid(value: Any, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _instant(value: str | datetime, field: str) -> datetime:
    if isinstance(value, datetime):
        parsed = value
    elif isinstance(value, str):
        try:
            parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError as exc:
            raise StoreError(f"{field} must be an ISO timestamp") from exc
    else:
        raise StoreError(f"{field} must be an ISO timestamp")
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError(f"{field} needs a timezone offset")
    return parsed.astimezone(timezone.utc)


def _code(value: str, field: str) -> str:
    if not isinstance(value, str) or not _CODE.fullmatch(value):
        raise StoreError(f"{field} must be a short opaque code")
    return value


def _source(value: str) -> str:
    if not isinstance(value, str) or not _NAMESPACE.fullmatch(value):
        raise StoreError("source_namespace must be a 1..64 character opaque code")
    return value


def _path(value: str) -> str:
    if not isinstance(value, str) or not _MOUNT_PATH.fullmatch(value) or \
            "//" in value or any(segment in {".", ".."} for segment in value.split("/")):
        raise StoreError("mount_path must be a simple absolute mount path")
    return value


def _stale_after(value: int) -> int:
    if type(value) is not int or not 15 <= value <= 86400:
        raise StoreError("stale_after_seconds must be 15..86400")
    return value


def _sample_key(value: str) -> str:
    if not isinstance(value, str) or not 1 <= len(value) <= 160 or \
            any(ord(character) < 32 or ord(character) == 127 for character in value):
        raise StoreError("source_sample_key must be 1..160 printable characters")
    return value


def _mount(store: Store, station_id: str, mount_id: str) -> dict:
    row = store.get("listener_mounts", _uuid(mount_id, "mount_id"))
    if row is None or row["station_id"] != station_id:
        raise Conflict("listener mount is outside station")
    return row


def _output(store: Store, station_id: str, endpoint_id: str,
            output_id: str, *, active: bool) -> dict:
    endpoint = store.get("endpoints", endpoint_id)
    if endpoint is None or endpoint["station_id"] != station_id:
        raise Conflict("endpoint is outside station")
    output = lookup_output(store, station_id, endpoint_id, output_id)
    if output is None:
        raise Conflict("station output is absent")
    if active and (endpoint["status"] == "retired" or output["status"] != "active"):
        raise Conflict("listener mount output is unavailable")
    return output


def _claim_fingerprint(claim: dict) -> str:
    encoded = json.dumps(claim, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def create_listener_mount(store: Store, principal: Principal, station_id: str,
                          endpoint_id: str, output_id: str, *,
                          mount_key: str, mount_path: str,
                          source_namespace: str,
                          stale_after_seconds: int = 300) -> dict:
    """Bind an external count source and its mount to one configured output."""
    station_id = _uuid(station_id, "station_id")
    endpoint_id = _uuid(endpoint_id, "endpoint_id")
    require(store, principal, "manage", station_id)
    output_id = _code(output_id, "output_id")
    mount_key = _code(mount_key, "mount_key")
    mount_path = _path(mount_path)
    source_namespace = _source(source_namespace)
    stale_after_seconds = _stale_after(stale_after_seconds)
    sequence = store.sequence
    output = _output(store, station_id, endpoint_id, output_id, active=True)
    if store.lookup("listener_mounts", "scope_key", station_id, endpoint_id,
                    output_id, mount_key):
        raise Conflict("listener mount key already exists on output")
    if store.lookup("listener_mounts", "station_source_path", station_id,
                    source_namespace, mount_path):
        raise Conflict("listener source path is already bound in station")
    mount_id = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("listener_mounts", mount_id, {
        "station_id": station_id, "endpoint_id": endpoint_id,
        "output_id": output_id, "station_output_id": output["_id"],
        "mount_key": mount_key, "mount_path": mount_path,
        "source_namespace": source_namespace,
        "stale_after_seconds": stale_after_seconds, "status": "active",
        "created_by": principal.user_id,
        "created_at": datetime.now(timezone.utc).isoformat(),
    })
    tx.commit()
    return store.get("listener_mounts", mount_id)


def update_listener_mount(store: Store, principal: Principal, station_id: str,
                          mount_id: str, *, expected_revision: int,
                          status: str | None = None,
                          stale_after_seconds: int | None = None) -> dict:
    """Change only the operational state or freshness limit of a mount."""
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "manage", station_id)
    row = _mount(store, station_id, mount_id)
    if type(expected_revision) is not int or row["_rev"] != expected_revision:
        raise Conflict("listener mount changed")
    if status is None and stale_after_seconds is None:
        raise StoreError("status or stale_after_seconds change is required")
    if status is not None and (not isinstance(status, str) or
                               status not in MOUNT_STATUSES):
        raise StoreError("invalid listener mount status")
    body = {key: value for key, value in row.items() if key not in {"_id", "_rev"}}
    if status is not None:
        body["status"] = status
    if stale_after_seconds is not None:
        body["stale_after_seconds"] = _stale_after(stale_after_seconds)
    if body == {key: value for key, value in row.items() if key not in {"_id", "_rev"}}:
        return row
    body["updated_by"] = principal.user_id
    body["updated_at"] = datetime.now(timezone.utc).isoformat()
    return store.put("listener_mounts", row["_id"], body,
                     expected_revision=expected_revision, actor=principal.user_id)


def list_listener_mounts(store: Store, principal: Principal, station_id: str, *,
                         endpoint_id: str | None = None,
                         include_disabled: bool = True) -> list[dict]:
    station_id = _uuid(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "view", station_id)
    if type(include_disabled) is not bool:
        raise StoreError("include_disabled must be boolean")
    if endpoint_id is not None:
        endpoint_id = _uuid(endpoint_id, "endpoint_id")
        _output_endpoint = store.get("endpoints", endpoint_id)
        if _output_endpoint is None or _output_endpoint["station_id"] != station_id:
            raise Conflict("endpoint is outside station")
        rows = store.lookup("listener_mounts", "endpoint", endpoint_id)
    else:
        rows = store.lookup("listener_mounts", "station", station_id)
    result = sorted((row for row in rows if row["station_id"] == station_id and
                   (include_disabled or row["status"] == "active")),
                  key=lambda row: (row["endpoint_id"], row["output_id"],
                                   row["mount_key"]))
    if store.sequence != sequence:
        raise Conflict("listener mounts changed during listing")
    return result


def record_listener_sample(store: Store, principal: Principal, station_id: str,
                           mount_id: str, *, source_namespace: str,
                           source_sample_key: str, observed_at: str | datetime,
                           source_state: str, listeners: int | None = None) -> dict:
    """Persist one source claim; exact keyed retries return the original row."""
    station_id = _uuid(station_id, "station_id")
    require(store, principal, "operate", station_id,
            endpoint_id=principal.endpoint_id if principal.scope == "endpoint" else None)
    sequence = store.sequence
    mount = _mount(store, station_id, mount_id)
    require(store, principal, "operate", station_id,
            endpoint_id=mount["endpoint_id"])
    source_namespace = _source(source_namespace)
    if source_namespace != mount["source_namespace"]:
        raise Conflict("sample source differs from the mount binding")
    source_sample_key = _sample_key(source_sample_key)
    observed = _instant(observed_at, "observed_at")
    if not isinstance(source_state, str) or source_state not in SAMPLE_STATES:
        raise StoreError("source_state must be online, offline or unknown")
    if source_state == "online":
        if type(listeners) is not int or not 0 <= listeners <= 1000000000:
            raise StoreError("online listeners must be a nonnegative integer")
    elif listeners is not None:
        raise StoreError("offline or unknown listeners must be null")
    claim = {
        "mount_id": mount["_id"], "station_id": station_id,
        "endpoint_id": mount["endpoint_id"], "output_id": mount["output_id"],
        "source_namespace": source_namespace,
        "source_sample_key": source_sample_key,
        "observed_at": observed.isoformat(),
        "source_state": source_state, "listeners": listeners,
    }
    fingerprint = _claim_fingerprint(claim)
    previous = store.lookup("listener_samples", "mount_key", mount["_id"],
                            source_sample_key)
    if previous:
        if previous[0]["claim_fingerprint"] != fingerprint:
            raise Conflict("sample key was reused with different data")
        return previous[0] | {"replayed": True}
    now = datetime.now(timezone.utc)
    if observed - now > _MAX_FUTURE or now - observed > _MAX_HISTORY:
        raise StoreError("observed_at is outside the accepted source window")
    if mount["status"] != "active":
        raise Conflict("disabled listener mount cannot receive samples")
    _output(store, station_id, mount["endpoint_id"], mount["output_id"],
            active=True)
    sample_id = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("listener_samples", sample_id, claim | {
        "recorded_at": now.isoformat(), "reported_by": principal.user_id,
        "claim_fingerprint": fingerprint,
    })
    try:
        tx.commit()
    except Conflict:
        previous = store.lookup("listener_samples", "mount_key", mount["_id"],
                                source_sample_key)
        if not previous:
            raise
        if previous[0]["claim_fingerprint"] != fingerprint:
            raise Conflict("sample key was reused with different data")
        return previous[0] | {"replayed": True}
    return store.get("listener_samples", sample_id) | {"replayed": False}


def _latest_sample(rows: list[dict]) -> dict | None:
    return max(rows, key=lambda row: (_instant(row["observed_at"], "observed_at"),
                                      _instant(row["recorded_at"], "recorded_at"),
                                      row["_id"])) if rows else None


def get_listener_mount_status(store: Store, principal: Principal,
                              station_id: str, mount_id: str, *,
                              as_of: str | datetime | None = None) -> dict:
    """Return the latest valid count or an explicit offline/unknown state."""
    station_id = _uuid(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "view", station_id)
    mount = _mount(store, station_id, mount_id)
    at = _instant(as_of, "as_of") if as_of is not None else datetime.now(timezone.utc)
    samples = [row for row in store.lookup("listener_samples", "mount", mount["_id"])
               if _instant(row["observed_at"], "observed_at") <= at]
    latest = _latest_sample(samples)
    if mount["status"] != "active":
        state, reason = "unknown", "mount_disabled"
    elif latest is None:
        state, reason = "unknown", "no_sample"
    elif at - _instant(latest["observed_at"], "observed_at") > \
            timedelta(seconds=mount["stale_after_seconds"]):
        state, reason = "unknown", "stale"
    else:
        state, reason = latest["source_state"], None
    if store.sequence != sequence:
        raise Conflict("listener state changed during status query")
    return {
        "station_id": station_id, "mount_id": mount["_id"],
        "endpoint_id": mount["endpoint_id"], "output_id": mount["output_id"],
        "as_of": at.isoformat(), "state": state, "reason": reason,
        "listeners": latest["listeners"] if state == "online" else None,
        "last_sample_id": latest["_id"] if latest else None,
        "last_observed_at": latest["observed_at"] if latest else None,
    }


def _bucket_start(value: datetime, seconds: int) -> datetime:
    epoch = datetime(1970, 1, 1, tzinfo=timezone.utc)
    elapsed = (value - epoch) // timedelta(seconds=seconds)
    return epoch + elapsed * timedelta(seconds=seconds)


def query_listener_stats(store: Store, principal: Principal, station_id: str, *,
                         start_at: str | datetime, end_at: str | datetime,
                         bucket: str = "hour", mount_id: str | None = None,
                         include_disabled: bool = True) -> dict:
    """Group snapshots by UTC time bucket and mount, preserving missingness.

    Each bucket's latest count is a *sample*, not a duration-weighted average.
    The station total is null unless every included mount's latest sample in
    that bucket is online; this prevents source outages from looking like zero.
    """
    station_id = _uuid(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "view", station_id)
    start = _instant(start_at, "start_at")
    end = _instant(end_at, "end_at")
    if start >= end:
        raise StoreError("end_at must follow start_at")
    if not isinstance(bucket, str) or bucket not in _BUCKET_SECONDS:
        raise StoreError("bucket must be minute, hour or day")
    if type(include_disabled) is not bool:
        raise StoreError("include_disabled must be boolean")
    seconds = _BUCKET_SECONDS[bucket]
    first = _bucket_start(start, seconds)
    last = _bucket_start(end - timedelta(microseconds=1), seconds)
    count = (last - first) // timedelta(seconds=seconds) + 1
    if count > _MAX_BUCKETS:
        raise StoreError("time range exceeds maximum bucket count")
    if mount_id is not None:
        mount = _mount(store, station_id, mount_id)
        mounts = [mount] if include_disabled or mount["status"] == "active" else []
    else:
        mounts = list_listener_mounts(store, principal, station_id,
                                     include_disabled=include_disabled)
    if count * len(mounts) > _MAX_CELLS:
        raise StoreError("query exceeds maximum mount/bucket cells")
    mount_ids = {mount["_id"] for mount in mounts}
    if mount_id is not None and mounts:
        samples = store.lookup("listener_samples", "mount", mounts[0]["_id"])
    else:
        samples = store.lookup("listener_samples", "station", station_id)
    grouped: dict[tuple[str, str], list[dict]] = {}
    matched = 0
    for row in samples:
        if row["mount_id"] not in mount_ids:
            continue
        observed = _instant(row["observed_at"], "observed_at")
        if start <= observed < end:
            matched += 1
            if matched > _MAX_SAMPLES:
                raise StoreError("query has too many samples in range; narrow it")
            key = (row["mount_id"], _bucket_start(observed, seconds).isoformat())
            grouped.setdefault(key, []).append(row)
    ticks = [(first + timedelta(seconds=index * seconds)).isoformat()
             for index in range(count)]
    by_mount: list[dict] = []
    totals = {tick: {"bucket_start": tick, "listeners": 0,
                     "online_mounts": 0, "offline_mounts": 0,
                     "unknown_mounts": 0, "coverage": "complete"}
              for tick in ticks}
    for mount in mounts:
        rows = []
        for tick in ticks:
            evidence = grouped.get((mount["_id"], tick), [])
            latest = _latest_sample(evidence)
            online = [row["listeners"] for row in evidence
                      if row["source_state"] == "online"]
            state = latest["source_state"] if latest else "unknown"
            listeners = latest["listeners"] if state == "online" else None
            row = {
                "bucket_start": tick, "state": state,
                "listeners": listeners,
                "latest_observed_at": latest["observed_at"] if latest else None,
                "sample_count": len(evidence),
                "online_samples": len(online),
                "offline_samples": sum(sample["source_state"] == "offline"
                                       for sample in evidence),
                "unknown_samples": sum(sample["source_state"] == "unknown"
                                       for sample in evidence),
                "min_online_listeners": min(online) if online else None,
                "max_online_listeners": max(online) if online else None,
                "mean_online_listeners": sum(online) / len(online) if online else None,
            }
            rows.append(row)
            total = totals[tick]
            total[f"{state}_mounts"] += 1
            if listeners is None:
                total["listeners"] = None
                total["coverage"] = "incomplete"
            elif total["listeners"] is not None:
                total["listeners"] += listeners
        by_mount.append({"mount_id": mount["_id"], "mount_key": mount["mount_key"],
                         "endpoint_id": mount["endpoint_id"],
                         "output_id": mount["output_id"], "buckets": rows})
    if not mounts:
        for total in totals.values():
            total["listeners"] = None
            total["coverage"] = "no_mounts"
    if store.sequence != sequence:
        raise Conflict("listener state changed during statistics query")
    return {
        "station_id": station_id, "start_at": start.isoformat(),
        "end_at": end.isoformat(), "bucket": bucket,
        "mount_count": len(mounts), "mounts": by_mount,
        "totals": [totals[tick] for tick in ticks],
    }
