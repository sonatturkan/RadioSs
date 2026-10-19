"""Native reusable playlists and atomic draft-plan materialization.

A playlist version contains ordered fixed events, published smart blocks or
podcast feed references. Materialization resolves all dynamic content and validates
the complete proposed plan before one sequence-fenced journal transaction.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
from hashlib import sha256
import json
from typing import Any, Mapping
from uuid import UUID, uuid4
from zoneinfo import ZoneInfo

from .broadcast_services import check_entry_scope, require_service, service_key as checked_service_key
from .identity import Principal, require
from .planning import validate_entry, validate_plan
from .readiness import execution_scope, readiness_at
from .storage import Conflict, Store, StoreError


_MAX_ENTRIES = 500
_TEMPLATE_KEYS = {"event_kind", "payload", "duration_kind", "planned_duration_ms",
                  "timing", "transition"}
_EVENT_KINDS = {"audio", "live", "stream", "macro", "wait", "marker"}


def _id(value: Any, name: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{name} must be a UUID") from exc


def _body(row: dict) -> dict:
    return {key: value for key, value in row.items() if key not in {"_id", "_rev"}}


def _text(value: Any, name: str, limit: int = 200) -> str:
    if not isinstance(value, str) or not value.strip() or len(value.strip()) > limit:
        raise StoreError(f"{name} must be 1..{limit} characters")
    return value.strip()


def _integer(value: Any, name: str, minimum: int, maximum: int) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise StoreError(f"{name} must be an integer in {minimum}..{maximum}")
    return value


def _instant(value: Any, name: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00")) if isinstance(value, str) else value
    except ValueError as exc:
        raise StoreError(f"{name} must be an offset-aware ISO timestamp") from exc
    if not isinstance(parsed, datetime) or parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError(f"{name} must be an offset-aware ISO timestamp")
    return parsed.astimezone(timezone.utc)


def _json_object(value: Any, name: str) -> dict:
    if not isinstance(value, Mapping) or any(not isinstance(key, str) for key in value):
        raise StoreError(f"{name} must be a JSON object")
    try:
        result = json.loads(json.dumps(dict(value), ensure_ascii=False, allow_nan=False))
    except (TypeError, ValueError, OverflowError) as exc:
        raise StoreError(f"{name} must be finite JSON data") from exc
    if not isinstance(result, dict) or any(not isinstance(key, str) for key in result):
        raise StoreError(f"{name} must have string keys")
    return result


def _hash(value: Any) -> str:
    return sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                             separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def _playlist(store: Store, station_id: str, playlist_id: str) -> dict:
    row = store.get("playlists", _id(playlist_id, "playlist_id"))
    if row is None or row["station_id"] != station_id:
        raise StoreError("playlist is outside station")
    return row


def _version(store: Store, station_id: str, version_id: str) -> tuple[dict, dict]:
    row = store.get("playlist_versions", _id(version_id, "version_id"))
    if row is None:
        raise StoreError("playlist version does not exist")
    return row, _playlist(store, station_id, row["playlist_id"])


def _template(store: Store, station_id: str, value: Any) -> dict:
    row = _json_object(value, "entry_template")
    if set(row) - _TEMPLATE_KEYS or not isinstance(row.get("event_kind"), str) or \
            row["event_kind"] not in _EVENT_KINDS:
        raise StoreError("playlist event template has unsupported fields or event kind")
    payload = row.get("payload", {})
    if not isinstance(payload, dict):
        raise StoreError("playlist payload must be an object")
    kind = row["event_kind"]
    if kind == "audio":
        version_id = _id(payload.get("version_id"), "audio version_id")
        version = store.get("versions", version_id)
        if version is None or not store.lookup("station_items", "station_item",
                                               station_id, version["item_id"]):
            raise StoreError("playlist audio version is outside station")
        if payload.get("cart_id") is not None or payload.get("cut_id") is not None:
            raise StoreError("playlist audio must use a fixed version, not an unresolved cart")
        payload["version_id"] = version_id
        if payload.get("item_id") is not None:
            payload["item_id"] = _id(payload["item_id"], "audio item_id")
            if payload["item_id"] != version["item_id"]:
                raise StoreError("playlist audio item does not own its version")
    candidate = row | {"ordinal": 0, "scheduled_at": "2026-01-01T12:00:00Z",
                       "state": "planned", "payload": payload}
    report = validate_entry(candidate, "UTC", "2026-01-01")
    if not report["valid"]:
        raise StoreError("invalid playlist event template: " +
                         ", ".join(issue["code"] for issue in report["errors"]))
    duration = report["normalized"]["planned_duration_ms"]
    if kind in {"audio", "live", "stream", "wait"} and \
            (type(duration) is not int or not 1 <= duration <= 86_400_000):
        raise StoreError("timed playlist events need a fixed 1..86400000 ms duration")
    if kind in {"macro", "marker"} and duration is not None:
        raise StoreError("instant playlist events cannot have a duration")
    if report["normalized"]["transition"]["mode"] != "sequential" or \
            report["normalized"]["timing"]["mode"] != "absolute":
        raise StoreError("playlist templates use sequential absolute timing")
    return {key: report["normalized"][key] for key in
            ("event_kind", "payload", "duration_kind", "planned_duration_ms",
             "timing", "transition")}


def create_playlist(store: Store, principal: Principal, station_id: str, *,
                    code: str, name: str) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "edit", station_id)
    if store.get("stations", station_id) is None:
        raise StoreError("station does not exist")
    playlist_id, version_id = str(uuid4()), str(uuid4())
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("playlists", playlist_id, {"station_id": station_id,
            "code": _text(code, "code", 100), "name": _text(name, "name"),
            "status": "active", "current_version_id": None,
            "draft_version_id": version_id, "created_by": principal.user_id,
            "created_at": now})
    tx.put("playlist_versions", version_id, {"playlist_id": playlist_id,
            "version_no": 1, "status": "draft", "created_by": principal.user_id,
            "created_at": now})
    tx.commit()
    return {"playlist_id": playlist_id, "draft_version_id": version_id,
            "version_no": 1}


def list_playlists(store: Store, principal: Principal, station_id: str, *,
                   include_disabled: bool = False) -> list[dict]:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "view", station_id)
    if type(include_disabled) is not bool:
        raise StoreError("include_disabled must be boolean")
    rows = [row for row in store.lookup("playlists", "station", station_id)
            if include_disabled or row["status"] == "active"]
    rows.sort(key=lambda row: (row["code"].casefold(), row["_id"]))
    if store.sequence != sequence:
        raise Conflict("playlists changed during listing")
    return rows


def list_playlist_versions(store: Store, principal: Principal, station_id: str,
                           playlist_id: str) -> list[dict]:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "view", station_id)
    playlist = _playlist(store, station_id, playlist_id)
    versions = store.lookup("playlist_versions", "playlist", playlist["_id"])
    versions.sort(key=lambda row: (row["version_no"], row["_id"]))
    result = []
    for version in versions:
        entries = [row for row in store.lookup("playlist_entries", "version", version["_id"])
                   if version["status"] == "draft" or row.get("status", "active") == "active"]
        entries.sort(key=lambda row: (row["ordinal"], row["_id"]))
        result.append(version | {"entries": entries})
    if store.sequence != sequence:
        raise Conflict("playlist changed during version listing")
    return result


def set_playlist_status(store: Store, principal: Principal, station_id: str,
                        playlist_id: str, status: str, *,
                        expected_revision: int) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "manage", station_id)
    playlist = _playlist(store, station_id, playlist_id)
    if not isinstance(status, str) or status not in {"active", "disabled"}:
        raise StoreError("playlist status must be active or disabled")
    if playlist["_rev"] != expected_revision:
        raise Conflict("playlist changed")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("playlists", playlist["_id"], _body(playlist) | {
        "status": status, "status_changed_by": principal.user_id,
        "status_changed_at": datetime.now(timezone.utc).isoformat()},
        expected_revision=playlist["_rev"])
    return tx.commit()[0]


def put_playlist_entry(store: Store, principal: Principal, station_id: str,
                       version_id: str, *, ordinal: int, source_kind: str,
                       entry_template: dict | None = None,
                       smart_block_version_id: str | None = None,
                       podcast_feed_id: str | None = None,
                       entry_id: str | None = None,
                       expected_entry_revision: int | None = None) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "edit", station_id)
    version, playlist = _version(store, station_id, version_id)
    if version["status"] != "draft" or playlist.get("draft_version_id") != version["_id"]:
        raise Conflict("only current playlist draft is editable")
    ordinal = _integer(ordinal, "ordinal", 0, 2_000_000_000)
    feed_id = None
    if source_kind == "event":
        if smart_block_version_id is not None or podcast_feed_id is not None:
            raise StoreError("fixed playlist entry cannot name a smart block or podcast feed")
        template = _template(store, station_id, entry_template)
        block_version_id = None
    elif source_kind == "smart_block":
        from .smart_blocks import _version as block_version
        if entry_template not in (None, {}) or podcast_feed_id is not None:
            raise StoreError("smart block entry cannot embed a fixed event or podcast feed")
        block_version, block = block_version(store, station_id,
                                              _id(smart_block_version_id,
                                                  "smart_block_version_id"))
        if block_version["status"] != "published" or block["status"] != "active":
            raise Conflict("playlist needs an active published smart block")
        template, block_version_id = {}, block_version["_id"]
    elif source_kind == "podcast_feed":
        from .podcast import _feed
        if entry_template not in (None, {}) or smart_block_version_id is not None:
            raise StoreError("podcast feed entry cannot embed a fixed event or smart block")
        feed = _feed(store, station_id, _id(podcast_feed_id, "podcast_feed_id"))
        if feed["status"] != "active":
            raise Conflict("playlist needs an active podcast feed")
        template, block_version_id, feed_id = {}, None, feed["_id"]
    else:
        raise StoreError("source_kind must be event, smart_block or podcast_feed")
    identifier = _id(entry_id, "entry_id") if entry_id else str(uuid4())
    existing = store.get("playlist_entries", identifier)
    if existing is not None and (existing["playlist_version_id"] != version["_id"] or
                                 existing["_rev"] != expected_entry_revision):
        raise Conflict("playlist entry changed or belongs to another version")
    if existing is None and expected_entry_revision is not None:
        raise Conflict("playlist entry is absent at expected revision")
    if (existing is None or existing.get("status", "active") != "active") and \
            len([row for row in store.lookup("playlist_entries", "version", version["_id"])
                 if row.get("status", "active") == "active"]) >= _MAX_ENTRIES:
        raise Conflict("playlist has reached its entry limit")
    now = datetime.now(timezone.utc).isoformat()
    body = {"playlist_version_id": version["_id"], "ordinal": ordinal,
            "active_ordinal": ordinal, "status": "active",
            "source_kind": source_kind, "entry_template": template,
            "smart_block_version_id": block_version_id,
            "podcast_feed_id": feed_id,
            "created_by": existing.get("created_by") if existing else principal.user_id,
            "created_at": existing.get("created_at") if existing else now,
            "updated_by": principal.user_id, "updated_at": now}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("playlist_entries", identifier, body,
           expected_revision=existing["_rev"] if existing else None)
    return tx.commit()[0]


def remove_playlist_entry(store: Store, principal: Principal, station_id: str,
                          version_id: str, entry_id: str, *,
                          expected_entry_revision: int) -> dict:
    """Retire one draft entry while preserving its audit trail and ordinal."""
    station_id, entry_id = _id(station_id, "station_id"), _id(entry_id, "entry_id")
    sequence = store.sequence
    require(store, principal, "edit", station_id)
    version, playlist = _version(store, station_id, version_id)
    if version["status"] != "draft" or playlist.get("draft_version_id") != version["_id"]:
        raise Conflict("only current playlist draft is editable")
    row = store.get("playlist_entries", entry_id)
    if row is None or row["playlist_version_id"] != version["_id"] or \
            row["_rev"] != expected_entry_revision or row.get("status", "active") != "active":
        raise Conflict("playlist entry changed, is removed or belongs to another version")
    body = _body(row) | {"status": "removed", "active_ordinal": None,
                         "removed_by": principal.user_id,
                         "removed_at": datetime.now(timezone.utc).isoformat()}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("playlist_entries", entry_id, body, expected_revision=row["_rev"])
    return tx.commit()[0]


def publish_playlist_version(store: Store, principal: Principal, station_id: str,
                             version_id: str, *, expected_version_revision: int,
                             expected_playlist_revision: int) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "approve", station_id)
    version, playlist = _version(store, station_id, version_id)
    if version["status"] != "draft" or playlist.get("draft_version_id") != version["_id"] or \
            version["_rev"] != expected_version_revision or \
            playlist["_rev"] != expected_playlist_revision:
        raise Conflict("playlist draft or parent changed")
    entries = sorted((row for row in store.lookup("playlist_entries", "version", version["_id"])
                      if row.get("status", "active") == "active"),
                     key=lambda row: (row["ordinal"], row["_id"]))
    if not 1 <= len(entries) <= _MAX_ENTRIES:
        raise Conflict("playlist needs 1..500 entries")
    for entry in entries:
        if entry["source_kind"] == "event":
            _template(store, station_id, entry["entry_template"])
        elif entry["source_kind"] == "smart_block":
            from .smart_blocks import _version as block_version
            linked, block = block_version(store, station_id, entry["smart_block_version_id"])
            if linked["status"] != "published" or block["status"] != "active":
                raise Conflict("playlist references an unpublished or inactive smart block")
        elif entry["source_kind"] == "podcast_feed":
            from .podcast import _feed
            if _feed(store, station_id, entry["podcast_feed_id"])["status"] != "active":
                raise Conflict("playlist references an inactive podcast feed")
        else:
            raise Conflict("playlist has an unknown source kind")
    fingerprint = _hash([{"ordinal": row["ordinal"],
                          "source_kind": row["source_kind"],
                          "entry_template": row["entry_template"],
                          "smart_block_version_id": row.get("smart_block_version_id"),
                          "podcast_feed_id": row.get("podcast_feed_id")}
                         for row in entries])
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("playlist_versions", version["_id"], _body(version) |
           {"status": "published", "fingerprint": fingerprint,
            "published_by": principal.user_id, "published_at": now},
           expected_revision=version["_rev"])
    tx.put("playlists", playlist["_id"], _body(playlist) |
           {"current_version_id": version["_id"], "draft_version_id": None},
           expected_revision=playlist["_rev"])
    tx.commit()
    return {"playlist_id": playlist["_id"],
            "published_version_id": version["_id"],
            "entry_count": len(entries), "fingerprint": fingerprint}


def fork_playlist_draft(store: Store, principal: Principal, station_id: str,
                        playlist_id: str, *, expected_playlist_revision: int) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "edit", station_id)
    playlist = _playlist(store, station_id, playlist_id)
    if playlist["_rev"] != expected_playlist_revision or playlist.get("draft_version_id"):
        raise Conflict("playlist changed or already has a draft")
    current = store.get("playlist_versions", playlist.get("current_version_id")) if playlist.get("current_version_id") else None
    if current is None or current["status"] != "published":
        raise Conflict("playlist has no published version")
    old_entries = [row for row in store.lookup("playlist_entries", "version", current["_id"])
                   if row.get("status", "active") == "active"]
    new_id, now = str(uuid4()), datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("playlist_versions", new_id, {"playlist_id": playlist["_id"],
            "version_no": current["version_no"] + 1, "status": "draft",
            "based_on_id": current["_id"], "created_by": principal.user_id,
            "created_at": now})
    for old in old_entries:
        tx.put("playlist_entries", str(uuid4()), {"playlist_version_id": new_id,
                "ordinal": old["ordinal"], "active_ordinal": old["ordinal"],
                "status": "active", "source_kind": old["source_kind"],
                "entry_template": old["entry_template"],
                "smart_block_version_id": old.get("smart_block_version_id"),
                "podcast_feed_id": old.get("podcast_feed_id"),
                "created_by": principal.user_id, "created_at": now,
                "forked_from_entry_id": old["_id"]})
    tx.put("playlists", playlist["_id"], _body(playlist) |
           {"draft_version_id": new_id}, expected_revision=playlist["_rev"])
    tx.commit()
    return {"playlist_id": playlist["_id"], "draft_version_id": new_id,
            "version_no": current["version_no"] + 1,
            "copied_entry_count": len(old_entries)}


def _fixed_entry(store: Store, station_id: str, template: dict, at: datetime,
                 *, zone: str, day, territory: str, output_id: str,
                 ordinal: int, binding: dict | None,
                 expected_media_file_id: str | None = None) -> dict:
    payload = dict(template["payload"])
    kind = template["event_kind"]
    if kind == "audio" and "territory" not in payload and "region_id" not in payload and territory != "*":
        payload["territory"] = territory
    if kind in {"audio", "live", "stream"} and "output_id" not in payload and output_id != "*":
        payload["output_id"] = output_id
    candidate = template | {"payload": payload, "ordinal": ordinal,
                            "scheduled_at": at.isoformat(), "state": "planned"}
    validated = validate_entry(candidate, zone, day)
    if not validated["valid"]:
        raise Conflict("playlist event invalid at requested time: " +
                       ", ".join(issue["code"] for issue in validated["errors"]))
    body = {key: value for key, value in validated["normalized"].items()
            if key not in {"id", "nominal_end_at", "requires_resolution"}}
    check_entry_scope(body, binding)
    if kind == "audio":
        scope_territory, scope_output = execution_scope(body["payload"])
        end = at + timedelta(milliseconds=body["planned_duration_ms"])
        ready = readiness_at(store, station_id, body["payload"]["version_id"],
                             slot_start=at, slot_end=end,
                             require_endpoint_copy=False,
                             territory=scope_territory, output_id=scope_output)
        if not ready["ready"]:
            raise Conflict("playlist audio is not ready: " + ", ".join(ready["blocked_reasons"]))
        if expected_media_file_id is not None and \
                ready["media_file_id"] != expected_media_file_id:
            raise Conflict("playlist podcast media differs from the verified full duration")
    return body


def preview_playlist(store: Store, principal: Principal, station_id: str,
                     version_id: str, start_at: str, *,
                     service_key: str = "default", territory: str = "*",
                     output_id: str = "*", ordinal_base: int = 0) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "view", station_id)
    version, playlist = _version(store, station_id, version_id)
    if version["status"] != "published" or playlist["status"] != "active":
        raise Conflict("playlist must be active and published")
    key = checked_service_key(service_key)
    binding = require_service(store, station_id, key)
    if binding is not None:
        if output_id not in {"*", binding["output_id"]}:
            raise Conflict("playlist output differs from broadcast service")
        output_id = binding["output_id"]
    if any(not isinstance(v, str) or not v.strip() for v in (territory, output_id)):
        raise StoreError("territory and output_id must be nonempty strings")
    base = _integer(ordinal_base, "ordinal_base", 0, 2_000_000_000)
    start = _instant(start_at, "start_at")
    station = store.get("stations", station_id)
    if station is None:
        raise StoreError("station does not exist")
    day = start.astimezone(ZoneInfo(station["timezone"])).date()
    source_rows = sorted((row for row in store.lookup("playlist_entries", "version", version["_id"])
                          if row.get("status", "active") == "active"),
                         key=lambda row: (row["ordinal"], row["_id"]))
    if not 1 <= len(source_rows) <= _MAX_ENTRIES:
        raise Conflict("published playlist has no usable entries")
    cursor, output, errors = start, [], []
    for source in source_rows:
        if len(output) >= _MAX_ENTRIES:
            errors.append({"source_entry_id": source["_id"], "code": "too_many_resolved_entries"})
            break
        try:
            if source["source_kind"] == "event":
                body = _fixed_entry(store, station_id, source["entry_template"], cursor,
                                    zone=station["timezone"], day=day,
                                    territory=territory, output_id=output_id,
                                    ordinal=base + len(output), binding=binding)
                output.append({"entry": body, "playlist_entry_id": source["_id"]})
                cursor += timedelta(milliseconds=body["planned_duration_ms"] or 0)
            elif source["source_kind"] == "smart_block":
                from .smart_blocks import _select_entries, _version as block_version
                block, parent = block_version(store, station_id,
                                              source["smart_block_version_id"])
                if block["status"] != "published" or parent["status"] != "active":
                    raise Conflict("playlist smart block is not active and published")
                resolved = _select_entries(store, station_id, block, cursor,
                                           station_timezone=station["timezone"],
                                           territory=territory, output_id=output_id,
                                           ordinal_base=base + len(output),
                                           binding=binding)
                if resolved["status"] != "ready":
                    raise Conflict("playlist smart block did not meet target duration")
                if len(output) + len(resolved["entries"]) > _MAX_ENTRIES:
                    raise Conflict("playlist resolves to more than 500 plan entries")
                output.extend(item | {"playlist_entry_id": source["_id"],
                                      "smart_block_version_id": block["_id"]}
                              for item in resolved["entries"])
                cursor = _instant(resolved["next_at"], "next_at")
            elif source["source_kind"] == "podcast_feed":
                from .podcast import _feed, latest_episode
                feed = _feed(store, station_id, source["podcast_feed_id"])
                if feed["status"] != "active":
                    raise Conflict("playlist podcast feed is disabled")
                episode = latest_episode(store, principal, station_id, feed["_id"],
                                         approved_only=True)
                if episode is None:
                    raise Conflict("playlist podcast has no approved, verified episode")
                media = store.get("media_files", episode["media_file_id"])
                technical = media.get("technical") if media else None
                duration = technical.get("duration_ms") if isinstance(technical, dict) else None
                if type(duration) is not int or not 1 <= duration <= 86_400_000:
                    raise Conflict("playlist podcast lacks a verified full duration")
                template = {"event_kind": "audio", "duration_kind": "fixed",
                            "planned_duration_ms": duration,
                            "payload": {"item_id": episode["item_id"],
                                        "version_id": episode["version_id"],
                                        "podcast_episode_id": episode["_id"]}}
                body = _fixed_entry(store, station_id, template, cursor,
                                    zone=station["timezone"], day=day,
                                    territory=territory, output_id=output_id,
                                    ordinal=base + len(output), binding=binding,
                                    expected_media_file_id=episode["media_file_id"])
                output.append({"entry": body, "playlist_entry_id": source["_id"],
                               "podcast_feed_id": feed["_id"],
                               "podcast_episode_id": episode["_id"]})
                cursor += timedelta(milliseconds=duration)
            else:
                raise Conflict("playlist source has unsupported kind")
        except (StoreError, OverflowError) as exc:
            errors.append({"source_entry_id": source["_id"], "code": "source_unresolved",
                           "detail": str(exc)})
            break
    plan_rows = store.lookup("plans", "station_date_service", station_id,
                             day.isoformat(), key)
    plan = plan_rows[0] if plan_rows else None
    draft = store.get("plan_revisions", plan["draft_revision_id"]) if plan and plan.get("draft_revision_id") else None
    if plan and (draft is None or draft["status"] != "draft"):
        errors.append({"code": "approved_plan_requires_fork"})
    if not errors:
        existing = store.lookup("plan_entries", "revision", draft["_id"]) if draft else []
        proposed = [_body(row) | {"id": row["_id"]} for row in existing]
        proposed.extend(item["entry"] | {"id": str(uuid4())} for item in output)
        checked = validate_plan(proposed, station["timezone"], day)
        errors.extend(checked["conflicts"])
        warnings = checked["warnings"]
    else:
        warnings = []
    if store.sequence != sequence:
        raise Conflict("playlist changed during preview")
    fingerprint = _hash({"version_id": version["_id"], "start_at": start.isoformat(),
                         "territory": territory, "output_id": output_id,
                         "entries": output})
    return {"status": "ready" if not errors else "needs_resolution",
            "station_id": station_id, "playlist_id": playlist["_id"],
            "playlist_version_id": version["_id"], "service_key": key,
            "service_date": day.isoformat(), "start_at": start.isoformat(),
            "entries": output, "errors": errors, "warnings": warnings,
            "end_at": cursor.isoformat(), "sequence": sequence,
            "fingerprint": fingerprint,
            "plan_id": plan["_id"] if plan else None,
            "draft_revision_id": draft["_id"] if draft else None}


def _materialize_entries(store: Store, principal: Principal, station_id: str,
                         entries: list[dict], start_at: str, *,
                         service_key: str, expected_sequence: int,
                         source: dict) -> dict:
    """Common fenced writer for a resolved playlist or standalone smart block."""
    sequence = store.sequence
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    if type(expected_sequence) is not int or sequence != expected_sequence:
        raise Conflict("database changed since preview")
    key = checked_service_key(service_key)
    binding = require_service(store, station_id, key)
    station = store.get("stations", station_id)
    if station is None:
        raise StoreError("station does not exist")
    start = _instant(start_at, "start_at")
    day = start.astimezone(ZoneInfo(station["timezone"])).date().isoformat()
    if not entries or len(entries) > _MAX_ENTRIES:
        raise Conflict("materialization needs 1..500 resolved entries")
    source = _json_object(source, "source")
    key_hash = _hash({"source": source, "start_at": start.isoformat(),
                      "service_key": key, "entries": entries})
    plan_rows = store.lookup("plans", "station_date_service", station_id, day, key)
    plan = plan_rows[0] if plan_rows else None
    draft = store.get("plan_revisions", plan["draft_revision_id"]) if plan and plan.get("draft_revision_id") else None
    if plan and (draft is None or draft["status"] != "draft"):
        raise Conflict("date plan is approved; fork a draft explicitly")
    existing = store.lookup("plan_entries", "revision", draft["_id"]) if draft else []
    if any(row.get("playlist_materialization_key") == key_hash for row in existing):
        raise Conflict("this playlist or smart block was already materialized")
    ids = [str(uuid4()) for _ in entries]
    bodies = []
    for entry_id, item in zip(ids, entries):
        body = _json_object(item["entry"], "resolved entry")
        checked = validate_entry(body | {"id": entry_id}, station["timezone"], day)
        if not checked["valid"] or checked["requires_resolution"]:
            raise Conflict("resolved entry is no longer valid")
        check_entry_scope(body, binding)
        for field in ("playlist_entry_id", "smart_block_version_id",
                      "podcast_feed_id", "podcast_episode_id"):
            if field in item:
                body[field] = _id(item[field], field)
        body.update(source)
        body["playlist_materialization_key"] = key_hash
        bodies.append(body)
    proposal = [_body(row) | {"id": row["_id"]} for row in existing]
    proposal.extend(body | {"id": entry_id} for body, entry_id in zip(bodies, ids))
    checked_plan = validate_plan(proposal, station["timezone"], day)
    if not checked_plan["valid"]:
        raise Conflict("playlist conflicts with the draft plan: " +
                       ", ".join(issue["code"] for issue in checked_plan["conflicts"]))
    if plan is None:
        plan_id, draft_id = str(uuid4()), str(uuid4())
    else:
        plan_id, draft_id = plan["_id"], draft["_id"]
    now = datetime.now(timezone.utc).isoformat()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(expected_sequence)
    if plan is None:
        tx.put("plans", plan_id, {"station_id": station_id, "service_date": day,
                                  "service_key": key, "state": "draft",
                                  "current_revision_id": None,
                                  "draft_revision_id": draft_id})
        tx.put("plan_revisions", draft_id, {"plan_id": plan_id,
                                            "revision_no": 1, "status": "draft",
                                            "created_by": principal.user_id,
                                            "created_at": now})
    else:
        tx.put("plans", plan_id, _body(plan), expected_revision=plan["_rev"])
        from .production import invalidate_production_for_revision
        invalidate_production_for_revision(tx, store, draft_id,
                                           "playlist_materialized")
    for entry_id, body in zip(ids, bodies):
        tx.put("plan_entries", entry_id, body | {"plan_revision_id": draft_id})
    tx.commit()
    return {"status": "materialized", "plan_id": plan_id,
            "plan_revision_id": draft_id, "entry_ids": ids,
            "entry_count": len(ids), "materialization_key": key_hash,
            "warnings": checked_plan["warnings"]}


def materialize_playlist(store: Store, principal: Principal, station_id: str,
                         version_id: str, start_at: str, *,
                         expected_sequence: int, service_key: str = "default",
                         territory: str = "*", output_id: str = "*",
                         ordinal_base: int = 0) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    preview = preview_playlist(store, principal, station_id, version_id, start_at,
                               service_key=service_key, territory=territory,
                               output_id=output_id, ordinal_base=ordinal_base)
    if preview["sequence"] != expected_sequence:
        raise Conflict("database changed since playlist preview")
    if preview["status"] != "ready":
        raise Conflict("playlist is not ready to materialize")
    return _materialize_entries(store, principal, station_id,
                                preview["entries"], preview["start_at"],
                                service_key=service_key,
                                expected_sequence=expected_sequence,
                                source={"playlist_id": preview["playlist_id"],
                                        "playlist_version_id": preview["playlist_version_id"],
                                        "playlist_fingerprint": preview["fingerprint"]})
