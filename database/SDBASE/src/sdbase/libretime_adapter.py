"""Read-only LibreTime playout-v2 projection of an approved RadioS snapshot.

LibreTime itself is not made to open .sdb. This adapter publishes only the six
response shapes consumed by its playout ``get_schedule`` function. Integer IDs
are durably assigned in the RadioS store; unsupported execution events fail
closed instead of disappearing from the broadcast schedule.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
from typing import Any, Mapping
from urllib.parse import unquote, urlsplit
from uuid import UUID, uuid4

from .identity import Principal, require
from .plans import build_snapshot
from .snapshot_contract import validate_snapshot_contract
from .storage import Conflict, Store, StoreError


_MIME = {"wav": "audio/wav", "wave": "audio/wav", "mp3": "audio/mpeg",
         "mpeg": "audio/mpeg", "ogg": "audio/ogg", "opus": "audio/ogg",
         "flac": "audio/flac", "aac": "audio/aac", "m4a": "audio/mp4",
         "mp4": "audio/mp4"}
_DSP_FIELDS = {"cue", "cue_id", "radio_cue_id",
               "gain_db", "crossfade_ms", "ducking_envelope", "replay_gain_db"}


def _instant(value: str) -> datetime:
    try:
        result = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (TypeError, AttributeError, ValueError) as exc:
        raise StoreError("LibreTime source entry needs an ISO timestamp") from exc
    if result.tzinfo is None or result.utcoffset() is None:
        raise StoreError("LibreTime source entry needs a UTC offset")
    return result.astimezone(timezone.utc)


def _iso(value: datetime) -> str:
    return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def _clock_ms(value: int) -> str:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < 86400000:
        raise StoreError("LibreTime cue/fade duration is outside one day")
    seconds, ms = divmod(value, 1000)
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    return f"{hours:02}:{minutes:02}:{seconds:02}.{ms:03}"


def _verified_uri(value: str, expected: Mapping[str, Any]) -> str:
    if not isinstance(value, str) or not value.strip():
        raise StoreError("every LibreTime file needs an accessible URI")
    source = value.strip()
    if source.startswith("/"):
        source = Path(source).as_uri()
    parsed = urlsplit(source)
    if parsed.scheme == "file":
        if parsed.netloc not in {"", "localhost"}:
            raise StoreError("LibreTime local file URI cannot name another host")
        path = Path(unquote(parsed.path))
        if not path.is_file() or path.is_symlink():
            raise StoreError("LibreTime local media file is absent or a symlink")
        digest = hashlib.sha256()
        size = 0
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                size += len(chunk)
                digest.update(chunk)
        if size != expected["size_bytes"] or digest.hexdigest() != expected["sha256"]:
            raise Conflict("LibreTime local file differs from the approved media manifest")
    elif parsed.scheme == "https" and parsed.hostname and not parsed.username and not parsed.password:
        pass
    else:
        raise StoreError("LibreTime file URI must be local file or HTTPS")
    return source


def _assigned_ids(store: Store, station_id: str, keys: set[tuple[str, str]],
                  actor: str, expected_sequence: int) -> dict[tuple[str, str], int]:
    existing = store.lookup("libretime_ids", "station", station_id)
    assigned = {(row["source_kind"], row["source_key"]): row["external_id"]
                for row in existing}
    next_id = max((row["external_id"] for row in existing), default=0) + 1
    missing = sorted(keys - assigned.keys())
    if not missing:
        if store.sequence != expected_sequence:
            raise Conflict("source changed during LibreTime projection")
        return {key: assigned[key] for key in keys}
    if next_id + len(missing) - 1 > 2147483647:
        raise StoreError("LibreTime integer identity space is exhausted")
    tx = store.begin(actor=actor)
    tx.expect_sequence(expected_sequence)
    created_at = datetime.now(timezone.utc).isoformat()
    for kind, key in missing:
        tx.put("libretime_ids", str(uuid4()), {
            "station_id": station_id, "source_kind": kind, "source_key": key,
            "external_id": next_id, "created_at": created_at})
        assigned[(kind, key)] = next_id
        next_id += 1
    tx.commit()
    return {key: assigned[key] for key in keys}


def project_plan_for_libretime(store: Store, principal: Principal, station_id: str,
                               endpoint_id: str, revision_id: str, *,
                               media_urls: dict[str, str], show_name: str = "RadioS") -> dict:
    """Project approved audio/stream events into LibreTime playout API data.

    ``media_urls`` maps each approved media_file_id to a verified local file or
    an operator-published HTTPS URL. A live LibreTime server is not mutated.
    """
    try:
        station_id, endpoint_id, revision_id = (str(UUID(str(station_id))),
                                                str(UUID(str(endpoint_id))),
                                                str(UUID(str(revision_id))))
    except (ValueError, TypeError, AttributeError) as exc:
        raise StoreError("LibreTime projection identifiers must be UUIDs") from exc
    require(store, principal, "manage", station_id)
    if not isinstance(media_urls, dict) or any(not isinstance(k, str) or
                                                not isinstance(v, str)
                                                for k, v in media_urls.items()):
        raise StoreError("media_urls must map media IDs to URI strings")
    if not isinstance(show_name, str) or not show_name.strip() or len(show_name) > 200:
        raise StoreError("show_name must be 1..200 characters")
    sequence = store.sequence
    snapshot = build_snapshot(store, principal, station_id, endpoint_id, revision_id)
    validate_snapshot_contract(snapshot)
    if snapshot.get("production", {}).get("placements") or \
            snapshot.get("production", {}).get("transitions"):
        raise Conflict("LibreTime projection cannot reproduce RadioS production transitions")
    if not snapshot["entries"]:
        raise Conflict("LibreTime projection needs at least one playable entry")
    media_manifest = {row["media_file_id"]: row for row in snapshot["media"]}
    keys: set[tuple[str, str]] = {("plan", snapshot["plan_id"]),
                                  ("revision", revision_id)}
    files_by_source: dict[str, dict] = {}
    streams_by_source: dict[str, dict] = {}
    prepared: list[tuple[dict, datetime, datetime, tuple[str, str], int, int, int, int]] = []
    for entry in snapshot["entries"]:
        if entry["state"] != "planned" or entry["entry_kind"] not in {"audio", "stream"}:
            raise Conflict("LibreTime projection supports planned audio and webstream only")
        if entry["duration_kind"] != "fixed" or \
                type(entry["planned_duration_ms"]) is not int or \
                not 0 < entry["planned_duration_ms"] < 86400000:
            raise Conflict("LibreTime projection needs a fixed duration below one day")
        transition = entry["transition"]
        if transition.get("mode") != "sequential" or transition.get("overlap_ms", 0) != 0:
            raise Conflict("LibreTime projection cannot reproduce an overlap transition")
        if entry["timing"] != {"mode": "absolute", "window_ms": 0,
                                "late_policy": "wait", "target_end_at": None}:
            raise Conflict("LibreTime projection cannot reproduce adaptive timing")
        if any(entry["payload"].get(field) is not None for field in _DSP_FIELDS):
            raise Conflict("LibreTime projection cannot reproduce requested cue or DSP behavior")
        start = _instant(entry["scheduled_at"])
        end = start + timedelta(milliseconds=entry["planned_duration_ms"])
        if start.microsecond or end.microsecond:
            raise Conflict("LibreTime playout reader discards subsecond schedule boundaries")
        if entry["entry_kind"] == "audio":
            markers = entry["playback_markers"]
            cue_in, cue_out = markers["cue_in_ms"], markers["cue_out_ms"]
            fade_in, fade_out = markers["fade_in_ms"], markers["fade_out_ms"]
            if markers["segue_in_ms"] not in (None, cue_in) or \
                    markers["segue_out_ms"] not in (None, cue_out):
                raise Conflict("LibreTime projection cannot reproduce selected segue launch anchors")
        else:
            cue_in = entry["payload"].get("cue_in_ms", 0)
            cue_out = entry["payload"].get("cue_out_ms", entry["planned_duration_ms"])
            fade_in = fade_out = 0
            if any(entry["payload"].get(field) not in (None, 0) for field in
                   ("fade_in_ms", "fade_out_ms", "segue_in_ms", "segue_out_ms")):
                raise Conflict("LibreTime webstream cannot reproduce file fade or segue behavior")
        if type(cue_in) is not int or type(cue_out) is not int or cue_in < 0 or \
                cue_out - cue_in != entry["planned_duration_ms"]:
            raise Conflict("LibreTime cue span differs from the fixed planned duration")
        _clock_ms(cue_in)
        _clock_ms(cue_out)
        _clock_ms(fade_in)
        _clock_ms(fade_out)
        source: tuple[str, str] | None = None
        if entry["entry_kind"] == "audio":
            media_id = entry["media_file_id"]
            expected = media_manifest.get(media_id)
            media = store.get("media_files", media_id)
            if expected is None or media is None or media["status"] != "verified" or \
                    media.get("sha256") != expected["sha256"] or \
                    media.get("size_bytes") != expected["size_bytes"]:
                raise Conflict("LibreTime audio media differs from approved snapshot")
            technical = media.get("technical") or {}
            if type(technical.get("duration_ms")) is not int or \
                    cue_out > technical["duration_ms"]:
                raise Conflict("LibreTime cue exceeds verified source media duration")
            container = str(technical.get("container") or technical.get("codec") or "").lower().lstrip(".")
            mime = _MIME.get(container)
            if mime is None:
                raise Conflict("LibreTime adapter has no MIME mapping for this audio container")
            url = _verified_uri(media_urls.get(media_id), expected)
            version = store.get("versions", entry["version_id"])
            item = store.get("items", version["item_id"]) if version else None
            if item is None:
                raise Conflict("LibreTime audio catalog item is absent")
            source = ("file", media_id)
            files_by_source[media_id] = {
                "url": url, "mime": mime, "replay_gain": None,
                "size": expected["size_bytes"], "track_title": item["title"],
                "artist_name": None, "sha256": expected["sha256"]}
        else:
            if cue_in != 0 or cue_out != entry["planned_duration_ms"]:
                raise Conflict("LibreTime webstream cannot use file cue offsets")
            uri = entry["payload"].get("uri")
            parsed = urlsplit(uri) if isinstance(uri, str) else None
            if parsed is None or parsed.scheme not in {"http", "https"} or not parsed.hostname:
                raise Conflict("LibreTime webstream needs a usable HTTP(S) URI")
            source = ("webstream", uri)
            streams_by_source[uri] = {"url": uri}
        keys.add(("entry", entry["plan_entry_id"]))
        keys.add(source)
        prepared.append((entry, start, end, source, cue_in, cue_out, fade_in, fade_out))
    if store.sequence != sequence:
        raise Conflict("source changed during LibreTime projection")
    ids = _assigned_ids(store, station_id, keys, principal.user_id, sequence)
    show_id = ids[("plan", snapshot["plan_id"])]
    instance_id = ids[("revision", revision_id)]
    files = {str(ids[("file", key)]): {"id": ids[("file", key)], **value}
             for key, value in files_by_source.items()}
    streams = {str(ids[("webstream", key)]): {"id": ids[("webstream", key)], **value}
               for key, value in streams_by_source.items()}
    rows = []
    for entry, start, end, source, cue_in, cue_out, fade_in, fade_out in prepared:
        kind, key = source
        rows.append({
            "id": ids[("entry", entry["plan_entry_id"])],
            "starts_at": _iso(start), "ends_at": _iso(end),
            "instance": instance_id,
            "file": ids[source] if kind == "file" else None,
            "stream": ids[source] if kind == "webstream" else None,
            "fade_in": _clock_ms(fade_in), "fade_out": _clock_ms(fade_out),
            "cue_in": _clock_ms(cue_in), "cue_out": _clock_ms(cue_out),
            "position_status": 1, "overbooked": False,
            "sdbase_plan_entry_id": entry["plan_entry_id"],
        })
    rows.sort(key=lambda row: (row["starts_at"], row["id"]))
    start = min(start for _, start, _, _, _, _, _, _ in prepared)
    end = max(end for _, _, end, _, _, _, _, _ in prepared)
    return {
        "adapter_version": 1, "station_id": station_id,
        "source_plan_id": snapshot["plan_id"],
        "source_revision_id": revision_id,
        "source_snapshot_sha256": hashlib.sha256(json.dumps(
            snapshot, ensure_ascii=False, sort_keys=True,
            separators=(",", ":")).encode("utf-8")).hexdigest(),
        "stream_preferences": {"input_fade_transition": 0.0,
                               "message_format": 0, "message_offline": "",
                               "replay_gain_enabled": False,
                               "replay_gain_offset": 0.0},
        "schedule": rows,
        "show_instances": {str(instance_id): {"id": instance_id,
                        "show": show_id, "starts_at": _iso(start), "ends_at": _iso(end)}},
        "shows": {str(show_id): {"id": show_id, "name": show_name.strip(),
                                 "live_enabled": False}},
        "files": files, "webstreams": streams,
        "playback_markers": {entry["plan_entry_id"]: entry["playback_markers"]
                             for entry, *_ in prepared if entry["entry_kind"] == "audio"},
        "limitations": ["read_only_playout_v2_subset", "no_live_input_or_production_overlays",
                        "no_replay_gain_or_segue_launch", "fade_processing_owned_by_libretime",
                        "https_media_urls_need_external_serving",
                        "no_asrun_or_traffic_evidence_sync", "no_subsecond_schedule_boundaries"],
    }


class _Result:
    def __init__(self, value: Any):
        self.value = value

    def json(self) -> Any:
        return self.value


class LibreTimeSnapshotClient:
    """ApiClient-compatible in-process facade for LibreTime get_schedule."""

    def __init__(self, projection: Mapping[str, Any]):
        self.data = projection

    def get_stream_preferences(self) -> _Result:
        return _Result(self.data["stream_preferences"])

    def list_schedule(self, params: Mapping[str, Any] | None = None) -> _Result:
        params = params or {}
        after = _instant(params["ends_after"]) if params.get("ends_after") else None
        before = _instant(params["ends_before"]) if params.get("ends_before") else None
        rows = [dict(row) for row in self.data["schedule"] if
                (after is None or _instant(row["ends_at"]) > after) and
                (before is None or _instant(row["ends_at"]) < before) and
                (not params.get("position_status__gt") or
                 row["position_status"] > params["position_status__gt"])]
        if params.get("overbooked") is True:
            rows = []
        return _Result(rows)

    def get_show_instance(self, identifier: int) -> _Result:
        return _Result(dict(self.data["show_instances"][str(identifier)]))

    def get_show(self, identifier: int) -> _Result:
        return _Result(dict(self.data["shows"][str(identifier)]))

    def get_file(self, identifier: int) -> _Result:
        return _Result(dict(self.data["files"][str(identifier)]))

    def get_webstream(self, identifier: int) -> _Result:
        return _Result(dict(self.data["webstreams"][str(identifier)]))
