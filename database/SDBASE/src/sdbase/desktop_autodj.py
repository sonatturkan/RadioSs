"""Measured AutoDJ IN/OUT assignment, with station/version CAS and replay.

The immutable native decoder waveform supplies the audible span. This workflow
changes only the two AutoDJ points; an operator's cue span, fades, intro and
manual beat grid remain unchanged. Each accepted track is one durable commit.

Every point records who placed it (``autodj_source``). A point the operator
set on the waveform is manual and is never recalculated here; missing points
and points from an earlier automatic analysis are. ``overwrite`` additionally
recalculates points of the current analysis.
"""
from __future__ import annotations

import math
from copy import deepcopy
from uuid import uuid4

from . import desktop_media as media_api
from . import desktop_playlist
from .desktop_api import (_bool, _id, _int, _number, _station,
                          add_operation, operation_replay)
from .storage import Conflict, StoreError

_OPERATION = "desktop-autodj/markers-assign"
_POINT_KEYS = ("autodj_in_ms", "autodj_out_ms")
ALGORITHM = media_api.AUTODJ_ALGORITHM
_REASONS = {
    "assigned": "AutoDJ IN ve OUT noktaları kaydedildi (IN ilk vuruşta; OUT şarkının son bölümünde, çıkışın veya enerjinin düştüğü yerin başında).",
    "existing_markers": "Mevcut AutoDJ IN ve OUT noktaları korundu (elle konan noktalar değiştirilmez).",
    "unchanged": "AutoDJ IN ve OUT noktaları yeniden hesaplandı; mevcut noktalar zaten güncel.",
    "media_unavailable": "Doğrulanmış yerel ses dosyası bulunamadı.",
    "analysis_unavailable": "Doğrulanmış ses analizi bulunamadı.",
    "no_audible_audio": "Seçilen eşikte duyulur ses bulunamadı.",
    "out_lead_exceeds_span": "OUT geri alma süresi duyulur ses aralığından uzun.",
    "preserved_marker_conflict": "Elle konan AutoDJ noktası ile hesaplanan nokta çakışıyor.",
}


def _scope(store, sid, playlist_id, record, version, media):
    if playlist_id is None:
        return
    _, draft = desktop_playlist._playlist(store, sid, playlist_id)
    binding = {"media_id": record["_id"], "version_id": version["_id"],
               "media_file_id": media["_id"], "media_sha256": media.get("sha256")}
    if not any(all(row.get(key) == value for key, value in binding.items())
               for row in desktop_playlist._tracks(store, draft["_id"])):
        raise Conflict("selected media is no longer in the current playlist draft")


def _grid(rhythm):
    """(bar origin, bar length) in ms for a dependable beat grid, else None.

    The wire schema represents a known bar phase by optional downbeat_ms;
    without it bars count from the first beat. Zero is a valid phase."""
    if not isinstance(rhythm, dict):
        return None
    bpm, confidence = rhythm.get("bpm"), rhythm.get("confidence")
    origin = rhythm.get("downbeat_ms", rhythm.get("first_beat_ms"))
    if type(bpm) not in (int, float) or type(confidence) not in (int, float) or \
            type(origin) not in (int, float) or not 60 <= bpm <= 200 or \
            not 0.45 <= confidence <= 1 or not math.isfinite(origin) or origin < 0:
        return None
    return float(origin), 240000.0 / bpm


def _mix_points(analysis, first, last, rhythm, out_lead_ms):
    """Where a DJ mixes inside the audible span [first, last].

    IN is the first bar start of a dependable grid, otherwise the first
    sound. OUT is found in the final part of the song from the waveform
    energy of every bar (two-second windows without a grid): a bar is low
    when its floor (quietest third of its bins) falls clearly below the
    song body. The start of the trailing low run - a beats-only or quiet
    outro, or a long fade - is OUT when it lasts at least four bars (eight
    seconds). A song that keeps its energy to the end is left two bars (four
    seconds), or the larger configured lead, before the energy finally falls
    away. OUT never lies in the first 55% of the sound and sits on a bar
    start when the grid is dependable. None when the span is too short."""
    grid = _grid(rhythm)
    origin, period = grid if grid else (0.0, 2000.0)
    bin_ms = analysis["waveform_frames_per_bin"] * 1000.0 / analysis["sample_rate"]
    # Very coarse measurements must contribute several bins per window.
    window = period * max(1, math.ceil(4 * bin_ms / period))
    waveform = analysis["waveform"]
    peaks = [max(abs(low), abs(high)) for low, high in zip(waveform[::2], waveform[1::2])]
    rows, index = [], math.ceil((first - origin) / window - 1e-9)
    while origin + (index + 1) * window <= last + 1e-6:
        start = origin + index * window
        values = sorted(peaks[max(0, int(start / bin_ms)):max(0, int((start + window) / bin_ms))])
        if values:
            third = max(1, len(values) // 3)
            rows.append((start, sum(values[:third]) / third, values[len(values) // 2]))
        index += 1
    if len(rows) < 8:
        return None
    body = sorted(row[1] for row in rows)[int(len(rows) * .75)]
    body_median = sorted(row[2] for row in rows)[int(len(rows) * .75)]
    if body <= 0 or body_median <= 0:
        return None

    def low(row):
        return row[1] < .6 * body or (row[1] < .75 * body and row[2] < .9 * body_median)

    def bar_floor(point):
        return origin + math.floor((point - origin) / period + 1e-9) * period if grid else point

    entry = origin + math.ceil((first - origin) / period - 1e-9) * period if grid else first
    if entry >= last:
        entry = first
    late = first + .55 * (last - first)
    # Earliest window of the trailing low run. One isolated strong window
    # (a fill or a final hit) inside the run does not end it.
    run, tolerated = None, False
    for index in range(len(rows) - 1, -1, -1):
        if low(rows[index]):
            run = index
        elif run is not None and not tolerated and index > 0 and low(rows[index - 1]):
            tolerated = True
        else:
            break
    room = max(out_lead_ms, 2 * period if grid else 4000.0)
    if run is not None and rows[run][0] >= late and \
            last - rows[run][0] >= (4 * period if grid else 8000.0):
        point = rows[run][0]
    else:
        alive = [row for row in rows if row[1] >= .3 * body or row[2] >= .5 * body_median]
        drop = min(last, alive[-1][0] + window) if alive else last
        point = bar_floor(drop - room)
        if point < late:
            point = bar_floor(last - room)
    if not entry < point < last:
        return None
    return int(round(entry)), int(round(point))


def _points(analysis, cue_in, cue_out, threshold_db, out_lead_ms, rhythm=None):
    """Conservative bin edges; the final bin can be shorter than the hop.

    Mix points come from the song's energy and beat grid. Spans too short to
    judge keep the measured sound bounds (minus the lead)."""

    threshold = 10 ** (threshold_db / 20)
    hop, rate = analysis["waveform_frames_per_bin"], analysis["sample_rate"]
    frames, waveform = analysis["sample_frames"], analysis["waveform"]
    first = last = None
    for index in range(len(waveform) // 2):
        start = index * hop * 1000 // rate
        end = min((index + 1) * hop, frames) * 1000 // rate
        if end <= cue_in or start >= cue_out:
            continue
        if max(abs(waveform[index * 2]), abs(waveform[index * 2 + 1])) >= threshold:
            first = max(cue_in, start) if first is None else first
            last = min(cue_out, end)
    if first is None or last is None or first >= last:
        return None, "no_audible_audio"
    mix = _mix_points(analysis, first, last, rhythm, out_lead_ms)
    if mix:
        return mix, None
    if last - out_lead_ms <= first:
        return None, "out_lead_exceeds_span"
    return (first, last - out_lead_ms), None


def assign_markers(store, principal, station_id, media_id, version_id,
                   media_sha256, expected_revision, expected_marker_revision,
                   idempotency_key, threshold_db=-45, out_lead_ms=0,
                   overwrite=False, playlist_id=None, replace_manual=False):
    """Assign one exact verified recording; unavailable/silent tracks skip.

    Missing points and points of an earlier automatic analysis are always
    calculated; ``overwrite`` also recalculates current automatic points.
    Manual points stay unless ``replace_manual`` is set. Optional playlist
    membership is checked again by the server immediately before preparation.
    """
    sequence = store.sequence
    sid, _ = _station(store, principal, station_id, "edit")
    media_id, version_id = _id(media_id, "media_id"), _id(version_id, "version_id")
    if not isinstance(media_sha256, str) or not media_api._SHA.fullmatch(media_sha256):
        raise StoreError("media_sha256 must be a lowercase SHA-256 digest")
    expected_revision = _int(expected_revision, "expected_revision", 1, 2147483647)
    if expected_marker_revision is not None:
        expected_marker_revision = _int(expected_marker_revision, "expected_marker_revision", 1, 2147483647)
    threshold_db = _number(threshold_db, "threshold_db", -60, -6)
    out_lead_ms = _int(out_lead_ms, "out_lead_ms", 0, 30000)
    overwrite = _bool(overwrite, "overwrite")
    replace_manual = _bool(replace_manual, "replace_manual")
    playlist_id = _id(playlist_id, "playlist_id") if playlist_id is not None else None
    payload = {"media_id": media_id, "version_id": version_id, "media_sha256": media_sha256,
               "expected_revision": expected_revision, "expected_marker_revision": expected_marker_revision,
               "threshold_db": threshold_db, "out_lead_ms": out_lead_ms,
               "overwrite": overwrite, "playlist_id": playlist_id}
    if replace_manual:
        payload["replace_manual"] = True
    receipt, request_hash, replay = operation_replay(store, principal, sid, idempotency_key, _OPERATION, payload)
    if replay is not None:
        return replay
    record, item, version, media = media_api._library(store, sid, media_id)
    if record["_rev"] != expected_revision or version["_id"] != version_id or media.get("sha256") != media_sha256:
        raise Conflict("selected media version or metadata revision changed")
    _scope(store, sid, playlist_id, record, version, media)
    rows = store.lookup("desktop_media_markers", "station_media", sid, media["_id"])
    old = rows[0] if rows else None
    if (old["_rev"] if old else None) != expected_marker_revision:
        raise Conflict("media marker draft revision changed")
    previous = (old or {}).get("autodj_source") or {}

    def replaceable(key):
        if not old or old.get(key) is None:
            return True
        source = previous.get(key)
        if source == "manual":
            return replace_manual
        return overwrite or source != ALGORITHM

    targets = [key for key in _POINT_KEYS if replaceable(key)]
    detail = media_api._snapshot(store, record, item, version, media)
    reason, clean = None, None
    if not targets:
        reason = "existing_markers"
    elif not detail["version"]["verified"] or not detail["version"]["object_available"]:
        reason = "media_unavailable"
    elif not detail["analysis"] or detail["analysis"].get("sha256") != media_sha256:
        reason = "analysis_unavailable"
    else:
        analysis = media_api._analysis(detail["analysis"])
        duration = media["technical"]["duration_ms"]
        if analysis["duration_ms"] != duration or analysis["size_bytes"] != media["size_bytes"]:
            raise StoreError("analysis does not match selected verified media")
        clean = {key: deepcopy(old[key]) for key in
                 media_api._MARKER_KEYS | media_api._AUTODJ_MARKER_KEYS | {"autodj_source"}
                 if old and key in old}
        if not old:
            clean = {"cue_in_ms": 0, "intro_ms": 0, "out_ms": duration,
                     "fade_start_ms": duration, "fade_end_ms": duration,
                     "auto_next_ms": duration, "overlap_ms": 0, "automatic": False}
        points, reason = _points(analysis, clean["cue_in_ms"], clean["out_ms"], threshold_db, out_lead_ms,
                                 detail["analysis"].get("rhythm"))
        if points:
            source = {key: previous[key] for key in _POINT_KEYS
                      if key in previous and key not in targets and clean.get(key) is not None}
            for key, point in zip(_POINT_KEYS, points):
                if key in targets:
                    clean[key] = point
                    source[key] = ALGORITHM
            clean.pop("autodj_source", None)
            if source:
                clean["autodj_source"] = {key: source[key] for key in sorted(source)}
            if clean["autodj_in_ms"] >= clean["autodj_out_ms"]:
                reason = "preserved_marker_conflict"
            elif old and all(old.get(key) == clean.get(key) for key in (*_POINT_KEYS, "autodj_source")):
                reason = "unchanged"
            else:
                clean = media_api._markers(clean, duration)
    status = "skipped" if reason else "updated"
    reason = reason or "assigned"
    now = media_api._now()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    if status == "updated":
        marker_id = old["_id"] if old else str(uuid4())
        draft = {"station_id": sid, "item_id": item["_id"], "version_id": version_id,
                 "media_file_id": media["_id"], "media_sha256": media_sha256,
                 "media_duration_ms": media["technical"]["duration_ms"], "status": "draft", **clean,
                 "created_by": old["created_by"] if old else principal.user_id,
                 "created_at": old["created_at"] if old else now,
                 "updated_by": principal.user_id, "updated_at": now}
        native = {**media_api._data(record), "updated_by": principal.user_id, "updated_at": now}
        tx.put("desktop_media_markers", marker_id, draft, expected_revision=expected_marker_revision)
        tx.put("desktop_media_library", media_id, native, expected_revision=expected_revision)
        detail["media"] = {**detail["media"], **native, "_rev": expected_revision + 1}
        detail["markers"] = {**draft, "_id": marker_id, "_rev": (expected_marker_revision or 0) + 1}
    result = {"status": status, "reason": reason, "reason_text": _REASONS[reason], "detail": deepcopy(detail)}
    add_operation(tx, principal, sid, receipt, idempotency_key, _OPERATION, request_hash, result)
    tx.commit()
    return result
