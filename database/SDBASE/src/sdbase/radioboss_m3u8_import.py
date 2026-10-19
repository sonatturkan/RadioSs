"""Import a reviewed RadioBOSS plain M3U8 as one atomic SDBASE draft plan.

The source playlist has no stable catalog IDs or broadcast times. The caller
must explicitly bind every source path to one existing version/media pair and
provide a timezone-aware start. Source paths never become catalog identities.
"""

from __future__ import annotations

from datetime import date, datetime, timedelta, timezone
import hashlib
import json
import re
from typing import Any, Mapping
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo

from .broadcast_services import (check_entry_scope, require_service,
                                 service_key as checked_service_key)
from .identity import Principal, require
from .planning import validate_entry, validate_plan
from .radioboss_m3u8 import FORMAT, MAX_SECONDS, preview_radioboss_m3u8
from .readiness import readiness_at
from .storage import Conflict, Store, StoreError


_KEY = re.compile(r"[A-Za-z0-9_.:-]{1,128}\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_MAX_APPLY_ROWS = 1000
_MAX_APPLY_BYTES = 1024 * 1024
_MAX_DURATION_MS = MAX_SECONDS * 1000


def _id(value: Any, label: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{label} must be a UUID") from exc


def _aware(value: str | datetime) -> datetime:
    try:
        result = datetime.fromisoformat(value.replace("Z", "+00:00")) \
            if isinstance(value, str) else value
    except ValueError as exc:
        raise StoreError("start_at must be an ISO timestamp") from exc
    if not isinstance(result, datetime) or result.tzinfo is None or result.utcoffset() is None:
        raise StoreError("start_at must carry a UTC offset")
    return result.astimezone(timezone.utc)


def _binding_map(value: Mapping[str, Mapping[str, str]]) -> dict[str, dict[str, str]]:
    if not isinstance(value, Mapping) or len(value) > _MAX_APPLY_ROWS:
        raise StoreError("path_bindings must be a map with at most 1000 entries")
    result = {}
    for path, binding in value.items():
        if not isinstance(path, str) or not isinstance(binding, Mapping) or \
                set(binding) != {"version_id", "media_file_id"}:
            raise StoreError("each path binding needs version_id and media_file_id")
        result[path] = {"version_id": _id(binding["version_id"], "version_id"),
                        "media_file_id": _id(binding["media_file_id"], "media_file_id")}
    return result


def _fingerprint(value: dict) -> str:
    encoded = json.dumps(value, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _duration(media: dict) -> int:
    technical = media.get("technical")
    milliseconds = technical.get("duration_ms") if isinstance(technical, dict) else None
    if milliseconds is None:
        milliseconds = media.get("duration_ms")
    if type(milliseconds) is not int or not 0 < milliseconds <= _MAX_DURATION_MS:
        raise Conflict("bound media has no plausible reviewed duration")
    return milliseconds


def apply_radioboss_m3u8(
    store: Store, principal: Principal, station_id: str, path: str, *,
    source_namespace: str, expected_source_sha256: str,
    idempotency_key: str, service_date: str, start_at: str,
    path_bindings: Mapping[str, Mapping[str, str]],
    service_key: str = "default", territory: str = "*",
) -> dict:
    """Create a draft only; no playback, approval, rights, or media import.

    Repeating the same key/source/selection returns the committed job. A changed
    source or binding is a conflict. The source hash is checked against exact
    bytes read for this call, after the caller has reviewed a preview.
    """
    station_id = _id(station_id, "station_id")
    require(store, principal, "import", station_id)
    require(store, principal, "edit", station_id)
    station = store.get("stations", station_id)
    if station is None:
        raise StoreError("station is absent")
    service_key = checked_service_key(service_key)
    if not isinstance(territory, str) or not territory.strip() or len(territory) > 128:
        raise StoreError("territory must be a nonempty code up to 128 characters")
    if not isinstance(source_namespace, str) or not _KEY.fullmatch(source_namespace):
        raise StoreError("source_namespace must be 1..128 safe characters")
    if not isinstance(idempotency_key, str) or not _KEY.fullmatch(idempotency_key):
        raise StoreError("idempotency_key must be 1..128 safe characters")
    if not isinstance(expected_source_sha256, str) or not _SHA256.fullmatch(expected_source_sha256):
        raise StoreError("expected_source_sha256 must be lowercase SHA-256")
    try:
        day = date.fromisoformat(service_date)
    except (ValueError, TypeError) as exc:
        raise StoreError("service_date must be YYYY-MM-DD") from exc
    start = _aware(start_at)
    if start.astimezone(ZoneInfo(station["timezone"])).date() != day:
        raise StoreError("start_at is outside station-local service date")
    bindings = _binding_map(path_bindings)
    namespace = f"radioboss_m3u8:{station_id}:{source_namespace}"
    request_fingerprint = _fingerprint({
        "service_date": day.isoformat(), "start_at": start.isoformat(),
        "service_key": service_key, "territory": territory,
        "bindings": bindings,
    })
    preview = preview_radioboss_m3u8(path)
    if preview["source_sha256"] != expected_source_sha256:
        raise Conflict("M3U8 bytes changed since preview")
    if preview["source_bytes"] > _MAX_APPLY_BYTES or \
            not 1 <= preview["row_count"] <= _MAX_APPLY_ROWS:
        raise StoreError("apply requires 1..1000 tracks and at most 1 MiB")
    if not preview["can_apply_as_draft"]:
        raise Conflict("M3U8 contains unsupported or invalid rows; review losses")
    source_paths = {row["source_path"] for row in preview["entries"]}
    if set(bindings) != source_paths:
        raise Conflict("every distinct source path needs exactly one explicit binding")

    existing = store.lookup("import_jobs", "source_key", namespace, idempotency_key)
    if existing:
        job = existing[0]
        if job.get("source_sha256") != expected_source_sha256 or \
                job.get("scope", {}).get("request_fingerprint") != request_fingerprint:
            raise Conflict("idempotency key belongs to another source or binding")
        return {"replayed": True, "job_id": job["_id"], **job["summary"]}

    service = require_service(store, station_id, service_key)
    if store.lookup("plans", "station_date_service", station_id,
                    day.isoformat(), service_key):
        raise Conflict("broadcast service already has a plan for this date")
    sequence = store.sequence
    selected: dict[str, dict] = {}
    for source_path, binding in bindings.items():
        version = store.get("versions", binding["version_id"])
        media = store.get("media_files", binding["media_file_id"])
        if version is None or version.get("status") != "approved" or \
                media is None or media.get("status") != "verified" or \
                media.get("version_id") != binding["version_id"]:
            raise Conflict("path binding needs an approved version and verified matching media")
        item = store.get("items", version["item_id"])
        if item is None or item.get("kind") == "advertisement" or not \
                store.lookup("station_items", "station_item", station_id, item["_id"]):
            raise Conflict("path binding is outside station or needs ad-specific traffic review")
        if not any(location["status"] in {"present", "verified"} for location in
                   store.lookup("media_locations", "file", media["_id"])):
            raise Conflict("bound media has no present managed object")
        selected[source_path] = {"version": version, "media": media,
                                 "item": item, "duration_ms": _duration(media)}

    plan_id, revision_id, job_id = str(uuid4()), str(uuid4()), str(uuid4())
    now = datetime.now(timezone.utc).isoformat()
    entries = []
    losses = list(preview["losses"])
    readiness = []
    cursor = start
    for ordinal, source in enumerate(preview["entries"]):
        chosen = selected[source["source_path"]]
        duration_ms = chosen["duration_ms"]
        try:
            end = cursor + timedelta(milliseconds=duration_ms)
        except OverflowError as exc:
            raise Conflict("playlist exceeds supported timestamp range") from exc
        declared = source["declared_duration_seconds"]
        if declared is not None and abs(declared * 1000 - duration_ms) > 1000:
            losses.append({"row": source["row"], "code": "extinf_catalog_duration_differs",
                           "declared_seconds": declared, "catalog_duration_ms": duration_ms})
        title = source["display_title"]
        if title is not None and title != chosen["item"]["title"]:
            losses.append({"row": source["row"], "code": "extinf_title_not_catalog_authority",
                           "source_title": title, "catalog_title": chosen["item"]["title"]})
        entry_id = str(uuid4())
        entries.append({
            "id": entry_id, "ordinal": ordinal, "state": "planned", "event_kind": "audio",
            "ad_break_id": None,
            "scheduled_at": cursor.isoformat(),
            "payload": {"version_id": chosen["version"]["_id"],
                        "item_id": chosen["item"]["_id"]} |
                       ({"output_id": service["output_id"]} if service else {}) |
                       ({"territory": territory} if territory != "*" else {}),
            "duration_kind": "fixed", "planned_duration_ms": duration_ms,
            "timing": {"mode": "absolute", "window_ms": 0,
                       "late_policy": "wait", "target_end_at": None},
            "transition": {"mode": "sequential", "overlap_ms": 0},
        })
        ready = readiness_at(store, station_id, chosen["version"]["_id"],
                             slot_start=cursor, slot_end=end,
                             require_endpoint_copy=False,
                             territory=territory,
                             output_id=service["output_id"] if service else "*")
        if not ready["ready"]:
            readiness.append({"row": source["row"],
                              "blocked_reasons": ready["blocked_reasons"]})
        cursor = end
    # Persist the same canonical execution fields that the snapshot contract
    # validates later (including UTC Z timestamps and explicit null fields).
    canonical_entries = []
    for entry in entries:
        checked = validate_entry(entry, station["timezone"], day.isoformat())
        if not checked["valid"]:
            raise Conflict("draft entry is invalid: " + ", ".join(
                issue["code"] for issue in checked["errors"]))
        canonical_entries.append({key: value for key, value in
                                  checked["normalized"].items()
                                  if key not in {"nominal_end_at", "requires_resolution"}})
    entries = canonical_entries
    validation = validate_plan(entries, station["timezone"], day.isoformat())
    if not validation["valid"]:
        raise Conflict("draft would violate plan rules: " + ", ".join(
            issue["code"] for issue in validation["conflicts"]))
    for entry in entries:
        check_entry_scope(entry, service)

    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("plans", plan_id, {
        "station_id": station_id, "service_date": day.isoformat(),
        "service_key": service_key,
        "state": "draft", "current_revision_id": None,
        "draft_revision_id": revision_id,
    })
    tx.put("plan_revisions", revision_id, {
        "plan_id": plan_id, "revision_no": 1, "status": "draft",
        "created_by": principal.user_id, "created_at": now,
    })
    for entry in entries:
        tx.put("plan_entries", entry["id"], {
            key: value for key, value in entry.items() if key != "id"
        } | {"plan_revision_id": revision_id})
    summary = {
        "plan_id": plan_id, "draft_revision_id": revision_id,
        "service_key": service_key,
        "entry_count": len(entries), "losses": losses,
        "readiness_blocks": readiness,
        "audio_files_imported": 0, "plan_approved": False,
    }
    tx.put("import_jobs", job_id, {
        "station_id": station_id, "source_namespace": namespace,
        "source_sha256": expected_source_sha256,
        "idempotency_key": idempotency_key, "status": "completed",
        "scope": {"format": FORMAT, "request_fingerprint": request_fingerprint,
                  "service_date": day.isoformat(), "start_at": start.isoformat(),
                  "service_key": service_key, "territory": territory,
                  "source_rows": preview["entries"], "path_bindings": bindings},
        "summary": summary, "created_at": now, "created_by": principal.user_id,
    })
    tx.commit()
    return {"replayed": False, "job_id": job_id, **summary}
