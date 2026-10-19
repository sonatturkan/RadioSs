"""Read-only, loss-reported export of an approved SDBASE plan to plain M3U8."""

from __future__ import annotations

from datetime import datetime, timedelta
from typing import Mapping
from uuid import UUID

from .identity import Principal, require
from .radioboss_m3u8 import (FORMAT, MAX_BYTES, MAX_SECONDS, MAX_TRACKS,
                            _LINE_BREAKS, absolute_media_path)
from .readiness import execution_scope, readiness_at
from .storage import Conflict, Store, StoreError


_WATCHED = frozenset({
    "plans", "plan_revisions", "plan_entries", "items", "versions",
    "station_items", "media_files", "media_locations", "rights_grants",
})


def _id(value: str, label: str) -> str:
    try:
        return str(UUID(str(value)))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError(f"{label} must be a UUID") from exc


def export_radioboss_m3u8(store: Store, principal: Principal, station_id: str,
                          plan_id: str, revision_id: str,
                          path_by_media_file: Mapping[str, str]) -> dict:
    """Produce UTF-8 M3U8 text and report everything the format cannot carry.

    Only a station's current approved revision is eligible. The caller supplies
    each destination absolute file path and must copy/verify audio separately.
    An export with skipped entries is visibly incomplete; it is never silently
    presented as an equivalent broadcast plan.
    """
    station_id = _id(station_id, "station_id")
    plan_id = _id(plan_id, "plan_id")
    revision_id = _id(revision_id, "revision_id")
    require(store, principal, "view", station_id)
    if not isinstance(path_by_media_file, Mapping):
        raise StoreError("path_by_media_file must be a map")
    generation = store.collection_generation(_WATCHED)
    plan = store.get("plans", plan_id)
    revision = store.get("plan_revisions", revision_id)
    if plan is None or plan.get("station_id") != station_id or \
            plan.get("current_revision_id") != revision_id or \
            revision is None or revision.get("plan_id") != plan_id or \
            revision.get("status") != "approved":
        raise Conflict("export requires the current approved station plan")
    entries = sorted(store.lookup("plan_entries", "revision", revision_id),
                     key=lambda row: (row["ordinal"], row["scheduled_at"], row["_id"]))
    if len(entries) > MAX_TRACKS:
        raise StoreError("plan exceeds 10000 M3U8 rows")
    lines = ["#EXTM3U"]
    skipped: list[dict] = []
    losses: list[dict] = []
    exported: list[str] = []
    for entry in entries:
        entry_id = entry["_id"]
        if entry.get("state") != "planned" or entry.get("event_kind") != "audio":
            skipped.append({"entry_id": entry_id, "code": "not_planned_audio",
                            "event_kind": entry.get("event_kind"),
                            "state": entry.get("state")})
            continue
        payload = entry.get("payload") or {}
        version_id = _id(payload.get("version_id"), "entry.version_id")
        version = store.get("versions", version_id)
        item = store.get("items", version["item_id"]) if version else None
        duration = entry.get("planned_duration_ms")
        if version is None or item is None or type(duration) is not int or duration <= 0:
            skipped.append({"entry_id": entry_id, "code": "version_or_duration_unavailable"})
            continue
        if version.get("status") != "approved" or \
                not store.lookup("station_items", "station_item", station_id, item["_id"]):
            skipped.append({"entry_id": entry_id, "code": "version_outside_approved_station_catalog"})
            continue
        start = datetime.fromisoformat(entry["scheduled_at"].replace("Z", "+00:00"))
        territory, output_id = execution_scope(payload)
        ready = readiness_at(store, station_id, version_id, slot_start=start,
                             slot_end=start + timedelta(milliseconds=duration),
                             require_endpoint_copy=False, territory=territory,
                             output_id=output_id)
        if not ready["ready"] or ready["media_file_id"] is None:
            skipped.append({"entry_id": entry_id, "code": "not_distributable",
                            "blocked_reasons": ready["blocked_reasons"]})
            continue
        media_id = ready["media_file_id"]
        path = path_by_media_file.get(media_id)
        if not absolute_media_path(path) or "?" in path or "%" in path:
            skipped.append({"entry_id": entry_id,
                            "code": "explicit_absolute_non_dynamic_media_path_required",
                            "media_file_id": media_id})
            continue
        title = item.get("title")
        if not isinstance(title, str) or not title or any(
                ord(char) < 32 or ord(char) == 127 or char in _LINE_BREAKS
                for char in title):
            skipped.append({"entry_id": entry_id, "code": "unsafe_extinf_title"})
            continue
        seconds = (duration + 500) // 1000
        if seconds > MAX_SECONDS:
            skipped.append({"entry_id": entry_id, "code": "duration_out_of_m3u8_subset_range"})
            continue
        if seconds * 1000 != duration:
            losses.append({"entry_id": entry_id, "code": "extinf_seconds_rounded",
                           "planned_duration_ms": duration,
                           "extinf_seconds": seconds})
        if item.get("kind") == "advertisement":
            losses.append({"entry_id": entry_id, "code": "ad_traffic_not_represented"})
        lines.extend((f"#EXTINF:{seconds},{title}", path))
        exported.append(entry_id)
        losses.append({"entry_id": entry_id, "code": "plan_semantics_not_represented",
                       "omitted": ["scheduled_at", "timing", "transition", "rights",
                                   "station_output", "version_and_media_hash",
                                   "approvals", "audit_history"]})
    if store.collection_generation(_WATCHED) != generation:
        raise Conflict("plan or catalog changed during M3U8 export")
    text = "\r\n".join(lines) + "\r\n"
    if len(text.encode("utf-8")) > MAX_BYTES:
        raise StoreError("M3U8 export exceeds 2 MiB")
    return {
        "format": FORMAT, "station_id": station_id,
        "plan_id": plan_id, "revision_id": revision_id,
        "m3u8_text": text, "row_count": len(exported),
        "exported_entry_ids": exported,
        "complete": not skipped and bool(exported),
        "skipped": skipped, "losses": losses,
        "requires_human_review": True,
        "limits": [
            "Only the documented plain RadioBOSS M3U8 subset is emitted.",
            "Audio is not copied; supplied destination paths are not verified on RadioBOSS.",
            "RadioBOSS playlist execution is not equivalent to SDBASE plan approval or as-run proof.",
        ],
    }
