"""Measured desktop imports and SHA-bound, unapproved marker drafts.

Object bytes use the existing media staging contract. The catalog aggregate and
the durable replay receipt publish in one journal transaction. Technical review
requires approve permission; content versions remain draft and ineligible.
"""
from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone
import math
import json
import os
from pathlib import Path
import re
import stat
from uuid import NAMESPACE_URL, uuid4, uuid5

from .catalog import NotFound, require_shared_item_permission
from .desktop_api import (_bool, _consistent_read, _id, _int, _list, _number,
                          _object, _stamp, _station, _text, add_operation,
                          operation_replay)
from .identity import require
from . import media as media_service
from .schema import KINDS
from .search import turkish_key
from .storage import Conflict, StoreError


MAX_BYTES = 2 * 1024 * 1024 * 1024
MAX_DURATION_MS = 86400000
_SHA = re.compile(r"[0-9a-f]{64}\Z")
_METADATA_KEYS = {"title", "kind", "artist", "album", "genre", "tags", "year", "comment"}
_ANALYSIS_KEYS = {"sha256", "size_bytes", "duration_ms", "sample_rate", "channels",
                  "codec", "format", "bitrate", "peak", "rms", "clipped_samples",
                  "clipped", "sample_frames", "waveform_frames_per_bin", "waveform", "rhythm", "loudness",
                  "audible"}  # audible is derived from the waveform on read and never stored
# Automatic grids from the first analyzer stay readable; players re-analyse them.
# A manual grid is an operator decision that automatic analysis never replaces.
_RHYTHM_ALGORITHMS = {"radios-onset-grid-v1", "radios-onset-grid-v2", "radios-onset-grid-v3",
                      "radios-manual-grid-v1"}
_MARKER_KEYS = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms",
                "auto_next_ms", "overlap_ms", "automatic"}
_AUTODJ_MARKER_KEYS = {"autodj_in_ms", "autodj_out_ms"}
# Who placed each AutoDJ point: the operator ("manual") or the automatic mix
# point analysis. Automatic recalculation never replaces a manual point.
# Points stored before this field existed carry no source (legacy automatic).
AUTODJ_ALGORITHM = "radios-autodj-mix-v2"
_AUTODJ_SOURCES = {"manual", AUTODJ_ALGORITHM}


def _now():
    return datetime.now(timezone.utc).isoformat()


def _data(record):
    return {key: deepcopy(value) for key, value in record.items() if not key.startswith("_")}


def _metadata(value):
    _object(value, _METADATA_KEYS, {"title", "kind"})
    result = {"title": _text(value["title"], "title", 200, True).strip(),
              "kind": _text(value["kind"], "kind", 40, True)}
    if result["kind"] not in KINDS:
        raise StoreError("unsupported catalog item kind")
    for name, limit in (("artist", 200), ("album", 200), ("genre", 120), ("comment", 2000)):
        result[name] = _text(value.get(name, ""), name, limit).strip()
    result["year"] = _text(value.get("year", ""), "year", 4)
    if result["year"] and not re.fullmatch(r"[0-9]{4}", result["year"]):
        raise StoreError("year must be empty or four digits")
    result["tags"] = [_text(tag, "tag", 80, True).strip()
                      for tag in _list(value.get("tags", []), "tags", 32)]
    if len(set(result["tags"])) != len(result["tags"]):
        raise StoreError("tags must be unique")
    return result


def _rhythm(value, duration):
    keys = {"schema", "algorithm", "bpm", "first_beat_ms", "confidence", "analyzed_ms"}
    _object(value, keys | {"downbeat_ms"}, keys)
    if type(value["schema"]) is not int or value["schema"] != 1 or value["algorithm"] not in _RHYTHM_ALGORITHMS or \
            ("downbeat_ms" in value and value["algorithm"] == "radios-onset-grid-v1"):
        raise StoreError("unsupported RadioS rhythm analysis format")
    bpm = _number(value["bpm"], "rhythm bpm", 0, 200)
    confidence = _number(value["confidence"], "rhythm confidence", 0, 1)
    analyzed = _int(value["analyzed_ms"], "rhythm analyzed_ms", 0, min(duration, 120000))
    first = _number(value["first_beat_ms"], "rhythm first_beat_ms", 0, analyzed)
    if bpm == 0:
        if confidence != 0 or first != 0:
            raise StoreError("unknown rhythm cannot claim a beatgrid or confidence")
    elif bpm < 60 or confidence < 0.45 or analyzed < 4000 or first >= analyzed:
        raise StoreError("rhythm beatgrid must have sufficient analyzed audio and confidence")
    if value["algorithm"] == "radios-manual-grid-v1" and bpm == 0:
        raise StoreError("manual rhythm grid requires a tempo")
    result = {"schema": 1, "algorithm": value["algorithm"], "bpm": bpm,
              "first_beat_ms": first, "confidence": confidence, "analyzed_ms": analyzed}
    if "downbeat_ms" in value:
        downbeat = _number(value["downbeat_ms"], "rhythm downbeat_ms", 0, analyzed)
        if bpm == 0 or downbeat >= analyzed:
            raise StoreError("rhythm bar phase requires a beatgrid inside analyzed audio")
        result["downbeat_ms"] = downbeat
    return result


def _loudness(value):
    """BS.1770 integrated programme loudness; null for a silent file."""
    keys = {"schema", "algorithm", "integrated_lufs"}
    _object(value, keys, keys)
    if type(value["schema"]) is not int or value["schema"] != 1 or value["algorithm"] != "radios-bs1770-v1":
        raise StoreError("unsupported RadioS loudness analysis format")
    lufs = value["integrated_lufs"]
    return {"schema": 1, "algorithm": value["algorithm"],
            "integrated_lufs": None if lufs is None else _number(lufs, "integrated loudness", -70, 10)}


def _analysis(value):
    _object(value, _ANALYSIS_KEYS, _ANALYSIS_KEYS - {"rhythm", "loudness", "audible"})
    digest = value["sha256"]
    if not isinstance(digest, str) or not _SHA.fullmatch(digest):
        raise StoreError("analysis SHA-256 must be lowercase hex")
    result = {"sha256": digest}
    for name, low, high in (("size_bytes", 1, MAX_BYTES), ("duration_ms", 1, MAX_DURATION_MS),
                            ("sample_rate", 1, 384000), ("channels", 1, 32),
                            ("bitrate", 0, 10000000000), ("sample_frames", 1, 66355200000),
                            ("waveform_frames_per_bin", 1, 66355200000),
                            ("clipped_samples", 0, 4246732800000)):
        result[name] = _int(value[name], name, low, high)
    for name in ("codec", "format"):
        result[name] = _text(value[name], name, 80, True).strip()
    result["peak"] = _number(value["peak"], "peak", 0, 1000000)
    result["rms"] = _number(value["rms"], "rms", 0, 1000000)
    result["clipped"] = _bool(value["clipped"], "clipped")
    if result["rms"] > result["peak"] + 0.000001 or \
            result["clipped"] != (result["clipped_samples"] > 0) or \
            result["clipped_samples"] > result["sample_frames"] * result["channels"]:
        raise StoreError("analysis level or clipped-sample counts are inconsistent")
    measured_duration = result["sample_frames"] * 1000 / result["sample_rate"]
    if abs(measured_duration - result["duration_ms"]) > 1.000001:
        raise StoreError("decoded frame count does not match duration")
    waveform = _list(value["waveform"], "waveform", 4096)
    expected_bins = math.ceil(result["sample_frames"] / result["waveform_frames_per_bin"])
    if expected_bins > 2048 or not waveform or len(waveform) != expected_bins * 2:
        raise StoreError("waveform dimensions do not match decoded frames")
    result["waveform"] = [_number(sample, "waveform amplitude", -1000000, 1000000)
                          for sample in waveform]
    tolerance = max(0.000001, result["peak"] * 0.000001)
    for low, high in zip(result["waveform"][::2], result["waveform"][1::2]):
        if low > high or max(abs(low), abs(high)) > result["peak"] + tolerance:
            raise StoreError("waveform extrema do not match measured peak")
    if "rhythm" in value:
        result["rhythm"] = _rhythm(value["rhythm"], result["duration_ms"])
    if "loudness" in value:
        result["loudness"] = _loudness(value["loudness"])
    return result


def _markers(value, duration):
    _object(value, _MARKER_KEYS | _AUTODJ_MARKER_KEYS | {"autodj_source"}, _MARKER_KEYS)
    clean = {key: _int(value[key], key, 0, duration) for key in _MARKER_KEYS - {"automatic"}}
    clean["automatic"] = _bool(value["automatic"], "automatic")
    start, end = clean["cue_in_ms"], clean["out_ms"]
    if start >= end or (clean["intro_ms"] != 0 and not start <= clean["intro_ms"] <= end) or \
            not start <= clean["fade_start_ms"] <= clean["fade_end_ms"] <= end or \
            not start <= clean["auto_next_ms"] <= end or \
            clean["overlap_ms"] > min(30000, end - start):
        raise StoreError("marker draft positions exceed the verified media span")
    for key in _AUTODJ_MARKER_KEYS.intersection(value):
        clean[key] = None if value[key] is None else _int(value[key], key, start, end)
    autodj_in = clean.get("autodj_in_ms")
    autodj_out = clean.get("autodj_out_ms")
    if (autodj_in if autodj_in is not None else start) >= (autodj_out if autodj_out is not None else end):
        raise StoreError("AutoDJ entry must precede its transition point within the cue span")
    if "autodj_source" in value:
        source = value["autodj_source"]
        if not isinstance(source, dict) or set(source) - _AUTODJ_MARKER_KEYS or \
                any(entry not in _AUTODJ_SOURCES for entry in source.values()):
            raise StoreError("AutoDJ point source must be manual or a RadioS analyzer per point")
        source = {key: source[key] for key in sorted(source) if clean.get(key) is not None}
        if source:
            clean["autodj_source"] = source
    return clean


def _bound(data, state):
    version = state.get("versions", {}).get(data["version_id"], {}).get("data")
    media = state.get("media_files", {}).get(data["media_file_id"], {}).get("data")
    if not version or not media or media["version_id"] != data["version_id"] or \
            version["item_id"] != data["item_id"] or \
            not any(row["data"].get("station_id") == data["station_id"] and
                    row["data"].get("item_id") == data["item_id"]
                    for row in state.get("station_items", {}).values()):
        raise StoreError("desktop media references cross catalog item, version or station")
    return media, version


def validate_library(data, state):
    _bound(data, state)
    provenance = {key: data[key] for key in ("source_path", "source_root", "source_relative_path") if key in data}
    if provenance:
        if len(provenance) != 3:
            raise StoreError("library source provenance requires path, root and relative path")
        relative = provenance["source_relative_path"]
        if not isinstance(relative, str) or not relative or len(relative) > 4096 or "\\" in relative or "\0" in relative or \
                relative.startswith("/") or any(part in {"", ".", ".."} for part in relative.split("/")):
            raise StoreError("library source relative path is invalid")
        path, root = provenance["source_path"], provenance["source_root"]
        if any(not isinstance(value, str) or not value.startswith("/") or len(value) > 8192 or "\0" in value or \
               os.path.normpath(value) != value for value in (path, root)) or \
                os.path.normpath(os.path.join(root, relative)) != path:
            raise StoreError("library source path does not match its root")
    _metadata({**{key: data[key] for key in _METADATA_KEYS - {"title", "kind"}},
               "title": "Bound catalog title", "kind": "other"})
    person_id = data.get("artist_id")
    if person_id is not None and _id(person_id, "artist_id") not in state.get("people", {}):
        raise StoreError("library artist references an absent person")
    if bool(data["artist"]) != bool(person_id):
        raise StoreError("library artist name and identity must agree")
    if person_id and state["people"][person_id]["data"]["display_name"] != data["artist"]:
        raise StoreError("station artist metadata does not match its person identity")
    if "primary_contribution_id" in data:
        contribution_id = data["primary_contribution_id"]
        if bool(contribution_id) != bool(person_id):
            raise StoreError("native artist requires its managed primary contribution")
        if contribution_id:
            contribution_id = _id(contribution_id, "primary_contribution_id")
            credit = state.get("contributions", {}).get(contribution_id, {}).get("data")
            owner = state.get("desktop_media_library", {}).get(credit.get("desktop_media_library_id"), {}).get("data") if credit else None
            if not credit or credit["item_id"] != data["item_id"] or credit["person_id"] != person_id or \
                    credit["role"] != "primary_artist" or not owner or owner["station_id"] != data["station_id"] or \
                    owner["media_file_id"] != data["media_file_id"]:
                raise StoreError("native library artist does not match its managed catalog credit")
    for name in ("created_at", "updated_at"):
        _stamp(data[name], name)


def validate_analysis(data, state):
    media, _ = _bound(data, state)
    analysis = _analysis(data["analysis"])
    technical = media.get("technical", {})
    if media.get("sha256") != analysis["sha256"] or media.get("size_bytes") != analysis["size_bytes"] or \
            technical.get("duration_ms") != analysis["duration_ms"] or \
            technical.get("sample_rate_hz") != analysis["sample_rate"] or \
            technical.get("channels") != analysis["channels"] or \
            technical.get("codec") != analysis["codec"] or \
            technical.get("container") != analysis["format"] or \
            technical.get("bit_rate_bps", 0) != analysis["bitrate"] or \
            data["analyzer"] != "radios-native-decoder-v1":
        raise StoreError("analysis does not match immutable media bytes and technical review")
    _stamp(data["created_at"], "created_at")


def validate_rhythm(data, state):
    media, _ = _bound(data, state)
    if media.get("status") != "verified" or data["media_sha256"] != media.get("sha256") or \
            data["analyzer"] not in _RHYTHM_ALGORITHMS:
        raise StoreError("rhythm analysis does not match verified source bytes")
    if _rhythm(data["rhythm"], media.get("technical", {}).get("duration_ms", 0))["algorithm"] != data["analyzer"]:
        raise StoreError("rhythm analyzer does not match its stored grid")
    if "loudness" in data:
        _loudness(data["loudness"])
    for name in ("created_at", "updated_at"):
        _stamp(data[name], name)


def validate_marker_draft(data, state):
    media, _ = _bound(data, state)
    if data["status"] != "draft" or data["media_sha256"] != media.get("sha256") or \
            data["media_duration_ms"] != media.get("technical", {}).get("duration_ms"):
        raise StoreError("marker draft does not match its verified media identity")
    _markers({key: data[key] for key in _MARKER_KEYS | _AUTODJ_MARKER_KEYS | {"autodj_source"} if key in data},
             data["media_duration_ms"])
    for name in ("created_at", "updated_at"):
        _stamp(data[name], name)


def guard_bound(before, after):
    if before and after and any(before.get(key) != after.get(key) for key in
            ("station_id", "item_id", "version_id", "media_file_id", "created_by", "created_at")):
        raise StoreError("desktop media station, catalog identity and creation provenance are immutable")


def validate_managed_contribution(data, state):
    owner_id = data.get("desktop_media_library_id")
    if owner_id is None:
        return
    owner_id = _id(owner_id, "desktop_media_library_id")
    owner = state.get("desktop_media_library", {}).get(owner_id, {}).get("data")
    if not owner or data["item_id"] != owner["item_id"] or data["person_id"] != owner.get("artist_id") or \
            data["role"] != "primary_artist":
        raise StoreError("managed primary artist contribution crosses library identity")
    contribution = state.get("contributions", {}).get(owner.get("primary_contribution_id"), {}).get("data")
    if contribution != data or sum(row["data"].get("desktop_media_library_id") == owner_id
                                   for row in state.get("contributions", {}).values()) != 1:
        raise StoreError("native library requires exactly one managed primary artist credit")


def guard_contribution(before, after):
    """Only native-owned credit links can be removed; global credits retain the
    original non-deletion contract. The audit/tombstone retains removed history.
    """
    if before and after is None and not before.get("desktop_media_library_id"):
        raise StoreError("unmanaged catalog contributions cannot be deleted")
    if before and after and before.get("desktop_media_library_id") != after.get("desktop_media_library_id"):
        raise StoreError("catalog credit native ownership is immutable")
    if before and before.get("desktop_media_library_id") and after and any(
            before.get(key) != after.get(key) for key in ("item_id", "role")):
        raise StoreError("managed catalog credit item and role are immutable")


def _library(store, sid, media_id):
    record = store.get("desktop_media_library", _id(media_id, "media_id"))
    if not record or record["station_id"] != sid or record["status"] != "active":
        raise NotFound("station media library record not found")
    item = store.get("items", record["item_id"])
    version = store.get("versions", record["version_id"])
    media = store.get("media_files", record["media_file_id"])
    if not item or not version or not media or media["version_id"] != version["_id"] or \
            version["item_id"] != item["_id"] or \
            not store.lookup("station_items", "station_item", sid, item["_id"]):
        raise NotFound("station media catalog binding not found")
    return record, item, version, media


def _managed_path(store, media):
    digest = media.get("sha256")
    if media.get("status") != "verified" or not isinstance(digest, str) or not _SHA.fullmatch(digest):
        return None
    try:
        location = media_service._managed_location(store, media["_id"], digest)
        path = media_service.object_path(store, digest)
        if location["status"] != "verified" or media_service.object_root(store).is_symlink() or \
                path.parent.is_symlink() or path.is_symlink() or not path.is_file():
            return None
        actual_sha, size = media_service._digest(path)
        return str(path) if actual_sha == digest and size == media.get("size_bytes") else None
    except (StoreError, OSError):
        return None


def _audible_span(analysis):
    """Where sound starts and ends, from the stored min/max waveform bins.

    A bin counts as audible above -50 dB under the file peak (never below
    -60 dBFS). Automation skips leading/trailing silence of unmarked media;
    bins limit precision to the waveform hop (about 0.15 s for 5 minutes)."""
    waveform = analysis.get("waveform") or []
    hop, rate, frames = analysis.get("waveform_frames_per_bin"), analysis.get("sample_rate"), analysis.get("sample_frames")
    if len(waveform) < 2 or not hop or not rate or not frames:
        return None
    threshold = max(0.001, float(analysis.get("peak", 0)) * 0.00316)
    loud = [index for index in range(len(waveform) // 2)
            if max(abs(waveform[index * 2]), abs(waveform[index * 2 + 1])) >= threshold]
    if not loud:
        return None
    duration = analysis.get("duration_ms") or frames * 1000 // rate
    start = min(duration, loud[0] * hop * 1000 // rate)
    end = min(duration, (loud[-1] + 1) * hop * 1000 // rate)
    return {"start_ms": int(start), "end_ms": int(max(start, end))}


def _snapshot(store, record, item, version, media, *, detail=True):
    rows = store.lookup("desktop_media_analysis", "media", media["_id"])
    rhythms = store.lookup("desktop_media_rhythm", "station_media", record["station_id"], media["_id"])
    markers = store.lookup("desktop_media_markers", "station_media", record["station_id"], media["_id"])
    path = _managed_path(store, media) if detail else None
    native = {**deepcopy(record), "title": item["title"], "kind": item["kind"]}
    result = {"media": native, "version": {**deepcopy(version), "media_file_id": media["_id"],
              "sha256": media.get("sha256"), "duration_ms": media.get("technical", {}).get("duration_ms"),
              "verified": media.get("status") == "verified", "object_available": path is not None if detail else None},
              "analysis": deepcopy(rows[0]["analysis"]) if rows else None,
              "analysis_revision": rhythms[0]["_rev"] if rhythms else 0,
              "markers": deepcopy(markers[0]) if markers else None}
    if rhythms and result["analysis"]:
        result["analysis"]["rhythm"] = deepcopy(rhythms[0]["rhythm"])
        if "loudness" in rhythms[0]:
            result["analysis"]["loudness"] = deepcopy(rhythms[0]["loudness"])
    if result["analysis"]:
        audible = _audible_span(result["analysis"])
        if audible:
            result["analysis"]["audible"] = audible
    if not detail and result["analysis"]:
        result["analysis"].pop("waveform", None)
    if detail:
        result["version"]["managed_path"] = path
    return result


@_consistent_read
def list_media(store, principal, station_id, query="", kind="", limit=200, offset=0, field=""):
    sid, _ = _station(store, principal, station_id)
    query = turkish_key(_text(query, "query", 200), tolerant=True)
    field = _text(field, "field", 20)
    if field not in {"", "title", "artist", "album"}:
        raise StoreError("unsupported media search field")
    kind = _text(kind, "kind", 40)
    if kind and kind not in KINDS:
        raise StoreError("unsupported catalog item kind")
    limit = _int(limit, "limit", 1, 200)
    offset = _int(offset, "offset", 0, 2147483647)
    records = []
    for record in store.lookup("desktop_media_library", "station", sid):
        if record["status"] != "active":
            continue
        record, item, version, media = _library(store, sid, record["_id"])
        if kind and item["kind"] != kind:
            continue
        searchable = (item["title"] if field == "title" else record[field]) if field else \
            " ".join([item["title"], record["artist"], record["album"], record["genre"], *record["tags"]])
        if query and query not in turkish_key(searchable, tolerant=True):
            continue
        records.append(_snapshot(store, record, item, version, media, detail=False))
    records.sort(key=lambda row: (turkish_key(row["media"]["title"]), row["media"]["_id"]))
    page, encoded_size = [], 0
    for row in records[offset:offset + limit]:
        row_size = len(json.dumps(row, ensure_ascii=False, allow_nan=False).encode())
        if page and encoded_size + row_size > 1500000:
            break
        page.append(row)
        encoded_size += row_size
    next_offset = offset + len(page)
    return {"records": page, "total": len(records), "offset": offset, "limit": limit,
            "next_offset": next_offset if next_offset < len(records) else None,
            "sequence": store.sequence}


@_consistent_read
def get_media(store, principal, station_id, media_id):
    sid, _ = _station(store, principal, station_id)
    result = _snapshot(store, *_library(store, sid, media_id))
    # Direct native card loads also need the latest policy. Keep this outside
    # _snapshot so a prepared queue does not duplicate every rule per track.
    from .desktop_copyright import policy_snapshot
    policy, document = policy_snapshot(store, sid)
    result.update(copyright_policy=policy,
                  copyright_policy_document_id=document["_id"] if document else None,
                  copyright_policy_revision=document["_rev"] if document else 0)
    return result


@_consistent_read
def media_rights(store, principal, station_id, media_id, version_id, offset=0, limit=50, grant_id=None, intent=None):
    """Bounded metadata-only view of a picked media version's actual grants.

    Reading does not approve content, create grants or inspect managed bytes.
    The caller must use the existing explicit /v1/rights approval operation.
    """
    from .identity import Forbidden
    sid, _ = _station(store, principal, station_id)
    record, item, version, media = _library(store, sid, media_id)
    if version['_id'] != _id(version_id, 'version_id'):
        raise NotFound('selected station media version changed or is absent')
    if media.get('status') != 'verified' or not isinstance(media.get('sha256'), str) or not _SHA.fullmatch(media['sha256']):
        raise StoreError('rights selection requires verified catalog media')
    offset = _int(offset, 'rights offset', 0, 2147483647)
    limit = _int(limit, 'rights limit', 1, 100)
    grants = sorted((grant for grant in store.lookup('rights_grants', 'version', version['_id'])
                     if grant['station_id'] == sid), key=lambda grant: grant['_id'])
    outputs = sorted((row for row in store.lookup('station_outputs', 'station', sid)
                      if row['status'] == 'active'), key=lambda row: (row['endpoint_id'], row['output_id'], row['_id']))
    if len(outputs) > 200:
        raise StoreError('rights editor active output catalogue exceeds 200 records')
    try:
        require(store, principal, 'approve', sid)
        can_approve = True
    except Forbidden:
        can_approve = False
    matching_grants = []
    if intent is not None:
        fields = {'territory', 'output_id', 'starts_at', 'ends_at', 'decision', 'source_ref'}
        _object(intent, fields, fields)
        for field, bound in (('territory', 100), ('output_id', 100), ('source_ref', 500)):
            _text(intent[field], 'rights intent ' + field, bound, True)
            if not intent[field].strip():
                raise StoreError('rights intent needs nonempty operator terms')
        if intent['decision'] not in ('allow', 'deny'):
            raise StoreError('rights intent decision must be allow or deny')
        for name in ('starts_at', 'ends_at'):
            _text(intent[name], 'rights intent ' + name, 80, True)
            _stamp(intent[name], 'rights intent ' + name)
        start, end = (datetime.fromisoformat(intent[name]) for name in ('starts_at', 'ends_at'))
        if end <= start:
            raise StoreError('rights intent interval must be nonempty')
        def matches(grant):
            return all(grant.get(field) == intent[field] for field in fields - {'starts_at', 'ends_at'}) and \
                datetime.fromisoformat(grant['starts_at']) == start and datetime.fromisoformat(grant['ends_at']) == end and grant.get('approved_by') == principal.user_id
        matching_grants = [grant for grant in grants if matches(grant)]
        if len(matching_grants) > 100:
            raise StoreError('matching rights intent exceeds the bounded record count')
    if grant_id is not None:
        selected = _id(grant_id, 'rights grant_id')
        matching = next((i for i, grant in enumerate(grants) if grant['_id'] == selected), None)
        if matching is None:
            raise NotFound('selected grant is outside this station media version')
        offset = (matching // limit) * limit
    if matching_grants:
        matching = next(i for i, grant in enumerate(grants) if grant['_id'] == matching_grants[0]['_id'])
        offset = (matching // limit) * limit
    association = store.lookup('station_items', 'station_item', sid, item['_id'])[0]
    page = grants[offset:offset + limit]
    result = {'contract_version': 1, 'station_id': sid, 'media_id': record['_id'],
              'version_id': version['_id'], 'can_approve': can_approve,
              'records': [{'media': deepcopy(record), 'version': deepcopy(version),
                           'item': deepcopy(item), 'file': deepcopy(media), 'association': deepcopy(association),
                           'rights': deepcopy(page)}], 'outputs': deepcopy(outputs),
              'matching_grants': deepcopy(matching_grants), 'rights_total': len(grants), 'rights_offset': offset,
              'rights_next_offset': offset + len(page) if offset + len(page) < len(grants) else None,
              'sequence': store.sequence}
    if len(json.dumps(result, ensure_ascii=False, allow_nan=False).encode()) > 1500000:
        raise StoreError('rights metadata page exceeds the bounded response size')
    return result


def _person(store, tx, sid, name, now):
    if not name:
        return None
    # Reuse known station credits only; coincidental names in other stations are
    # not an identity assertion across tenants.
    station_items = {row["item_id"] for row in store.lookup("station_items", "station", sid)}
    known = {row["person_id"] for row in store.scan("contributions") if row["item_id"] in station_items}
    known.update(row["artist_id"] for row in store.lookup("desktop_media_library", "station", sid)
                 if row.get("artist_id"))
    for person_id in sorted(known):
        person = store.get("people", person_id)
        if person and person["display_name"] == name and person["kind"] == "person":
            return person_id
    person_id = str(uuid4())
    tx.put("people", person_id, {"display_name": name, "kind": "person", "aliases": [], "created_at": now})
    return person_id


def _source(store, import_path):
    name = _text(import_path, "import_path", 240, True)
    if name in {".", ".."} or "/" in name or "\\" in name or Path(name).name != name:
        raise StoreError("import_path must be a staged import filename")
    root = getattr(store, "desktop_import_root", None)
    if root is None:
        raise StoreError("desktop import root is not configured")
    descriptor = None
    try:
        directory_fd = os.open(Path(root), os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            descriptor = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory_fd)
        finally:
            os.close(directory_fd)
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= MAX_BYTES:
            os.close(descriptor)
            descriptor = None
            raise StoreError("staged import must be a bounded regular file")
        return descriptor, info
    except OSError as exc:
        if descriptor is not None:
            os.close(descriptor)
        raise StoreError("staged import file is unavailable or unsafe") from exc


def import_media(store, principal, station_id, import_path, source_name, idempotency_key, metadata, analysis,
                 source_path=None, source_root=None):
    return _import_media(store, principal, station_id, import_path, source_name, idempotency_key,
                         metadata, analysis, source_path, source_root, False)


def import_staged(store, principal, station_id, import_path, source_name, idempotency_key, metadata, probe,
                  source_path=None, source_root=None):
    """Register verified bytes and container tags before the parallel analysis pass.

    No waveform, tempo or loudness measurements are fabricated. Catalog content
    remains a draft; analysis-save finalizes the measured technical metadata.
    """
    return _import_media(store, principal, station_id, import_path, source_name, idempotency_key,
                         metadata, probe, source_path, source_root, True)


def _probe(value):
    fields = {"sha256", "size_bytes", "duration_ms", "sample_rate", "channels", "codec", "format", "bitrate"}
    _object(value, fields, fields)
    if not isinstance(value["sha256"], str) or not _SHA.fullmatch(value["sha256"]):
        raise StoreError("invalid probe digest")
    clean = {"sha256": value["sha256"]}
    for key, low, high in (("size_bytes", 1, MAX_BYTES), ("duration_ms", 1, MAX_DURATION_MS),
                           ("sample_rate", 1, 384000), ("channels", 1, 32), ("bitrate", 0, 10000000000)):
        clean[key] = _int(value[key], key, low, high)
    for key in ("codec", "format"):
        clean[key] = _text(value[key], key, 80, True)
    return clean


def _import_media(store, principal, station_id, import_path, source_name, idempotency_key,
                  metadata, analysis, source_path, source_root, deferred):
    sequence = store.sequence
    sid, _ = _station(store, principal, station_id, "edit")
    require(store, principal, "approve", sid)
    clean_metadata, clean_analysis = _metadata(metadata), (_probe(analysis) if deferred else _analysis(analysis))
    provenance = {}
    if source_path is not None or source_root is not None:
        source = Path(_text(source_path, "source_path", 4096, True))
        root = Path(_text(source_root, "source_root", 4096, True))
        if not source.is_absolute() or not root.is_absolute() or ".." in source.parts or ".." in root.parts:
            raise StoreError("source provenance must use absolute normalized paths")
        try:
            relative = source.relative_to(root).as_posix()
        except ValueError as exc:
            raise StoreError("source path is outside its selected root") from exc
        if relative in {"", "."}:
            raise StoreError("source path must name a file")
        provenance = {"source_path": str(source), "source_root": str(root), "source_relative_path": relative}
    source_name = _text(source_name, "source_name", 240, True)
    if source_name in {".", ".."} or "/" in source_name or "\\" in source_name:
        raise StoreError("source_name must be a safe original basename")
    payload = {"import_path": import_path, "source_name": source_name,
               "metadata": clean_metadata, "analysis": clean_analysis}
    if provenance:
        payload["source"] = provenance
    operation = "desktop-media/import-staged" if deferred else "desktop-media/import"
    receipt, request_hash, replay = operation_replay(store, principal, sid, idempotency_key, operation, payload)
    if replay is not None:
        return replay
    descriptor, before = _source(store, import_path)
    try:
        if before.st_size != clean_analysis["size_bytes"]:
            raise StoreError("staged bytes do not match measured source size")
        # The descriptor pins a non-symlink regular file. The existing stager
        # rehashes this inode, detects writes during copy and fsyncs object bytes.
        digest, size, target, reused = media_service._stage_object(store, Path(f"/proc/self/fd/{descriptor}"))
        if digest != clean_analysis["sha256"] or size != clean_analysis["size_bytes"]:
            raise StoreError("staged bytes do not match measured source SHA-256")
        actual_sha, actual_size = media_service._digest(target)
        if actual_sha != digest or actual_size != size:
            raise StoreError("managed object failed independent SHA-256 verification")
    finally:
        os.close(descriptor)
    technical_values = {"codec": clean_analysis["codec"], "container": clean_analysis["format"],
                        "duration_ms": clean_analysis["duration_ms"], "sample_rate_hz": clean_analysis["sample_rate"],
                        "channels": clean_analysis["channels"]}
    if clean_analysis["bitrate"]:
        technical_values["bit_rate_bps"] = clean_analysis["bitrate"]
    technical = media_service._technical_metadata(technical_values)
    now = _now()
    item_id, version_id, library_id, analysis_id = (str(uuid4()) for _ in range(4))
    media_id = str(uuid5(NAMESPACE_URL, f"radios-media:{version_id}:{digest}"))
    location_id = str(uuid5(NAMESPACE_URL, f"radios-location:{media_id}:{digest}"))
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    person_id = _person(store, tx, sid, clean_metadata["artist"], now)
    contribution_id = str(uuid4()) if person_id else None
    item = {"kind": clean_metadata["kind"], "title": clean_metadata["title"], "lifecycle": "draft", "created_at": now}
    version = {"item_id": item_id, "version_no": 1, "label": "Original", "status": "draft",
               "valid_from": None, "valid_to": None, "approved_at": None, "approved_by": None, "created_at": now}
    media = {"version_id": version_id, "status": "verified", "sha256": digest, "size_bytes": size,
             "source_name": source_name, "received_at": now, "technical": technical,
             "technical_reviewed_at": now, "technical_reviewed_by": principal.user_id, "verified_at": now}
    location = {"media_file_id": media_id, "storage_uri": media_service._managed_uri(digest), "root_kind": "managed",
                "status": "verified", "sha256": digest, "size_bytes": size, "last_seen_at": now, "verified_at": now}
    library = {"station_id": sid, "item_id": item_id, "version_id": version_id, "media_file_id": media_id,
               **provenance,
               **{key: clean_metadata[key] for key in _METADATA_KEYS - {"title", "kind"}},
               "artist_id": person_id, "primary_contribution_id": contribution_id,
               "status": "active", "created_by": principal.user_id, "created_at": now,
               "updated_by": principal.user_id, "updated_at": now}
    measured = {"station_id": sid, "item_id": item_id, "version_id": version_id, "media_file_id": media_id,
                "analysis": clean_analysis, "analyzer": "radios-native-decoder-v1", "created_by": principal.user_id, "created_at": now}
    tx.put("items", item_id, item)
    tx.put("station_items", str(uuid4()), {"station_id": sid, "item_id": item_id, "eligible": False,
           "category_id": None, "rotation_policy": None, "created_at": now})
    tx.put("versions", version_id, version)
    tx.put("media_files", media_id, media)
    tx.put("media_locations", location_id, location)
    tx.put("desktop_media_library", library_id, library)
    if not deferred:
        tx.put("desktop_media_analysis", analysis_id, measured)
    if person_id:
        tx.put("contributions", contribution_id, {"item_id": item_id, "person_id": person_id,
               "role": "primary_artist", "ordinal": 0, "desktop_media_library_id": library_id})
    result = {"media": {"_id": library_id, "_rev": 1, **library, "title": item["title"], "kind": item["kind"]},
              "version": {"_id": version_id, "_rev": 1, **version, "media_file_id": media_id, "sha256": digest,
                          "duration_ms": clean_analysis["duration_ms"], "verified": True, "object_available": True,
                          "managed_path": str(target)}, "analysis": None if deferred else deepcopy(clean_analysis), "analysis_revision": 0, "markers": None}
    add_operation(tx, principal, sid, receipt, idempotency_key, operation, request_hash, result)
    tx.commit()
    return result


def save_metadata(store, principal, station_id, media_id, expected_revision, idempotency_key, metadata):
    sequence = store.sequence
    sid, _ = _station(store, principal, station_id, "edit")
    media_id = _id(media_id, "media_id")
    expected_revision = _int(expected_revision, "expected_revision", 1, 2147483647)
    metadata = _metadata(metadata)
    payload = {"media_id": media_id, "expected_revision": expected_revision, "metadata": metadata}
    receipt, request_hash, replay = operation_replay(store, principal, sid, idempotency_key, "desktop-media/metadata-save", payload)
    if replay is not None:
        return replay
    record, item, version, media = _library(store, sid, media_id)
    if record["_rev"] != expected_revision:
        raise Conflict("media metadata revision changed")
    artist_changed = record["artist"] != metadata["artist"]
    if item["title"] != metadata["title"] or item["kind"] != metadata["kind"] or artist_changed:
        require_shared_item_permission(store, principal, item["_id"], "edit")
        if any(row["status"] in {"approved", "withdrawn"} for row in store.lookup("versions", "item", item["_id"])):
            raise Conflict("approved catalog identity requires a separate version workflow")
    old_contribution_id = record.get("primary_contribution_id")
    old_contribution = store.get("contributions", old_contribution_id) if old_contribution_id else None
    if artist_changed and record["artist"] and (not old_contribution or old_contribution.get("desktop_media_library_id") != media_id):
        raise Conflict("legacy artist credit requires an explicit catalog credit workflow")
    now = _now()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    person_id = _person(store, tx, sid, metadata["artist"], now)
    contribution_id = old_contribution_id
    if artist_changed:
        if person_id:
            contribution_id = old_contribution_id or str(uuid4())
            tx.put("contributions", contribution_id,
                   {"item_id": item["_id"], "person_id": person_id, "role": "primary_artist",
                    "ordinal": 0, "desktop_media_library_id": media_id},
                   expected_revision=old_contribution["_rev"] if old_contribution else None)
        elif old_contribution:
            tx.delete("contributions", old_contribution["_id"], expected_revision=old_contribution["_rev"])
            contribution_id = None
    data = {**_data(record), **{key: metadata[key] for key in _METADATA_KEYS - {"title", "kind"}},
            "artist_id": person_id, "updated_by": principal.user_id, "updated_at": now}
    if "primary_contribution_id" in record or artist_changed:
        data["primary_contribution_id"] = contribution_id
    item_data = {**_data(item), "title": metadata["title"], "kind": metadata["kind"]}
    tx.put("desktop_media_library", media_id, data, expected_revision=expected_revision)
    if item_data != _data(item):
        tx.put("items", item["_id"], item_data, expected_revision=item["_rev"])
    updated = {**data, "_id": media_id, "_rev": expected_revision + 1}
    result = _snapshot(store, updated, {**item, **item_data}, version, media)
    add_operation(tx, principal, sid, receipt, idempotency_key, "desktop-media/metadata-save", request_hash, result)
    tx.commit()
    return result


def save_analysis(store, principal, station_id, media_id, version_id, expected_revision,
                  expected_analysis_revision, idempotency_key, analysis):
    sequence = store.sequence
    sid, _ = _station(store, principal, station_id, "edit")
    media_id, version_id = _id(media_id, "media_id"), _id(version_id, "version_id")
    expected_revision = _int(expected_revision, "expected_revision", 1, 2147483647)
    expected_analysis_revision = _int(expected_analysis_revision, "expected_analysis_revision", 0, 2147483647)
    analysis = _analysis(analysis)
    payload = {"media_id": media_id, "version_id": version_id, "expected_revision": expected_revision,
               "expected_analysis_revision": expected_analysis_revision, "analysis": analysis}
    receipt, request_hash, replay = operation_replay(store, principal, sid, idempotency_key, "desktop-media/analysis-save", payload)
    if replay is not None:
        return replay
    record, item, version, media = _library(store, sid, media_id)
    existing = store.lookup("desktop_media_rhythm", "station_media", sid, media["_id"])
    measured = existing[0] if existing else None
    if record["_rev"] != expected_revision or version["_id"] != version_id or \
            (measured["_rev"] if measured else 0) != expected_analysis_revision:
        raise Conflict("media version or analysis revision changed")
    path = _managed_path(store, media)
    if not path or analysis["sha256"] != media["sha256"] or analysis["size_bytes"] != media["size_bytes"]:
        raise StoreError("analysis must match verified managed media bytes")
    analysis_id = measured["_id"] if measured else str(uuid4())
    now = _now()
    data = {"station_id": sid, "item_id": item["_id"], "version_id": version_id, "media_file_id": media["_id"],
            "media_sha256": media["sha256"], "rhythm": _rhythm(analysis.get("rhythm"), analysis["duration_ms"]),
            "analyzer": None, "created_by": principal.user_id, "created_at": now,
            "updated_by": principal.user_id, "updated_at": now}
    data["analyzer"] = data["rhythm"]["algorithm"]
    if "loudness" in analysis:
        data["loudness"] = analysis["loudness"]
    full_rows = store.lookup("desktop_media_analysis", "media", media["_id"])
    initial_analysis = not full_rows
    if not initial_analysis:
        validate_analysis({**data, "analysis": analysis, "analyzer": "radios-native-decoder-v1"}, store._state)
    if measured:
        data = _data(measured) | {key: data[key] for key in ("rhythm", "analyzer", "loudness") if key in data} | \
            {"updated_by": principal.user_id, "updated_at": now}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    if initial_analysis:
        # A header-only import has no decoded measurements yet. Finalize its
        # technical description and full waveform in the same transaction as
        # the first rhythm result; later analysis cannot change these values.
        technical = {"codec": analysis["codec"], "container": analysis["format"],
                     "duration_ms": analysis["duration_ms"], "sample_rate_hz": analysis["sample_rate"],
                     "channels": analysis["channels"]}
        if analysis["bitrate"]:
            technical["bit_rate_bps"] = analysis["bitrate"]
        updated_media = _data(media) | {"technical": media_service._technical_metadata(technical),
                                       "technical_reviewed_at": now, "technical_reviewed_by": principal.user_id}
        tx.put("media_files", media["_id"], updated_media, expected_revision=media["_rev"])
        tx.put("desktop_media_analysis", str(uuid4()), {
            "station_id": sid, "item_id": item["_id"], "version_id": version_id,
            "media_file_id": media["_id"], "analysis": analysis, "analyzer": "radios-native-decoder-v1",
            "created_by": principal.user_id, "created_at": now})
        media = updated_media | {"_id": media["_id"], "_rev": media["_rev"] + 1}
    tx.put("desktop_media_rhythm", analysis_id, data, expected_revision=expected_analysis_revision or None)
    result = _snapshot(store, record, item, version, media)
    if initial_analysis:
        result["analysis"] = deepcopy(analysis)
    if result["analysis"]:
        result["analysis"]["rhythm"] = deepcopy(data["rhythm"])
        if "loudness" in data:
            result["analysis"]["loudness"] = deepcopy(data["loudness"])
        # The first analysis is not committed yet, so _snapshot could not
        # derive the audible span that a later read returns.
        if initial_analysis:
            audible = _audible_span(result["analysis"])
            if audible:
                result["analysis"]["audible"] = audible
    result["analysis_revision"] = expected_analysis_revision + 1
    add_operation(tx, principal, sid, receipt, idempotency_key, "desktop-media/analysis-save", request_hash, result)
    tx.commit()
    return result


def save_markers(store, principal, station_id, media_id, expected_revision, expected_marker_revision, idempotency_key, markers):
    sequence = store.sequence
    sid, _ = _station(store, principal, station_id, "edit")
    media_id = _id(media_id, "media_id")
    expected_revision = _int(expected_revision, "expected_revision", 1, 2147483647)
    if expected_marker_revision is not None:
        expected_marker_revision = _int(expected_marker_revision, "expected_marker_revision", 1, 2147483647)
    record, item, version, media = _library(store, sid, media_id)
    if media["status"] != "verified":
        raise Conflict("marker drafts require verified technical media")
    clean = _markers(markers, media["technical"]["duration_ms"])
    payload = {"media_id": media_id, "expected_revision": expected_revision,
               "expected_marker_revision": expected_marker_revision, "markers": clean}
    receipt, request_hash, replay = operation_replay(store, principal, sid, idempotency_key, "desktop-media/markers-save", payload)
    if replay is not None:
        return replay
    if record["_rev"] != expected_revision:
        raise Conflict("media metadata revision changed")
    current = store.lookup("desktop_media_markers", "station_media", sid, media["_id"])
    old = current[0] if current else None
    if (old["_rev"] if old else None) != expected_marker_revision:
        raise Conflict("media marker draft revision changed")
    # Older cue editors omit AutoDJ points. Preserve them, while explicit null
    # clears a point. Validate the combined span before any mutation.
    if old:
        clean = _markers({**{key: old[key] for key in _AUTODJ_MARKER_KEYS if key in old}, **clean},
                         media["technical"]["duration_ms"])
    # Per-point provenance: an explicit source wins, a point the operator set
    # or moved is manual, and an untouched point keeps its earlier source.
    explicit = clean.pop("autodj_source", None) if "autodj_source" in markers else None
    clean.pop("autodj_source", None)
    previous = (old or {}).get("autodj_source") or {}
    source = {}
    for key in sorted(_AUTODJ_MARKER_KEYS):
        if clean.get(key) is None:
            continue
        if explicit and key in explicit:
            source[key] = explicit[key]
        elif old is not None and old.get(key) == clean[key]:
            if key in previous:
                source[key] = previous[key]
        else:
            source[key] = "manual"
    if source:
        clean["autodj_source"] = source
    now = _now()
    marker_id = old["_id"] if old else str(uuid4())
    draft = {"station_id": sid, "item_id": item["_id"], "version_id": version["_id"],
             "media_file_id": media["_id"], "media_sha256": media["sha256"],
             "media_duration_ms": media["technical"]["duration_ms"], "status": "draft", **clean,
             "created_by": old["created_by"] if old else principal.user_id,
             "created_at": old["created_at"] if old else now,
             "updated_by": principal.user_id, "updated_at": now}
    native = {**_data(record), "updated_by": principal.user_id, "updated_at": now}
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("desktop_media_markers", marker_id, draft, expected_revision=expected_marker_revision)
    tx.put("desktop_media_library", media_id, native, expected_revision=expected_revision)
    result = _snapshot(store, {**native, "_id": media_id, "_rev": expected_revision + 1}, item, version, media)
    result["markers"] = {"_id": marker_id, "_rev": (expected_marker_revision or 0) + 1, **draft}
    add_operation(tx, principal, sid, receipt, idempotency_key, "desktop-media/markers-save", request_hash, result)
    tx.commit()
    return result
