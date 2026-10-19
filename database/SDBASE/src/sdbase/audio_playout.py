"""Render an accepted endpoint plan's supported audio into a PCM WAV.

This is a scoped, offline audio executor. It consumes only an active endpoint
manifest, verifies the content-addressed source bytes, and writes the approved
fixed sequential schedule including silence between slots. It does not drive
an audio device, execute remote streams, or claim an on-air observation.
"""

from __future__ import annotations

from contextlib import contextmanager
from array import array
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import stat
import sys
import tempfile
from typing import Any, BinaryIO, Iterator
import wave

from . import _platform
from .endpoint import EndpointAgent
from .endpoint_journal import EndpointError
from .playback_markers import validate_frozen_markers
from .storage import StoreError


class AudioRenderError(EndpointError):
    """The accepted snapshot needs audio behavior this renderer cannot prove."""


_CHUNK_FRAMES = 8192
_PCM_SAMPLE_WIDTH = 2
_MAX_WAV_BYTES = 0xFFFFFFFF - 36
_UNSUPPORTED_PAYLOAD = frozenset({
    "cue", "cue_id", "radio_cue_id",
    "gain_db", "crossfade_ms", "ducking_envelope", "replay_gain_db",
})


def _instant(value: Any, field: str) -> datetime:
    if not isinstance(value, str):
        raise AudioRenderError(f"{field} must be an ISO timestamp")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise AudioRenderError(f"{field} must be an ISO timestamp") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise AudioRenderError(f"{field} needs a timezone offset")
    return parsed.astimezone(timezone.utc)


def _frames(milliseconds: int, rate: int) -> int:
    """Map integer milliseconds to the nearest PCM frame, half-up."""
    return (milliseconds * rate + 500) // 1000


def _elapsed_frames(start: datetime, anchor: datetime, rate: int) -> int:
    delta = start - anchor
    micros = (delta.days * 86400 + delta.seconds) * 1000000 + delta.microseconds
    if micros < 0:
        raise AudioRenderError("plan ordinal order reverses scheduled time")
    return (micros * rate + 500000) // 1000000


@contextmanager
def _open_local_file(path: Path) -> Iterator[BinaryIO]:
    # Links include junctions and mount points (name-surrogate reparse points).
    if _platform.is_link(path) or _platform.is_link(path.parent):
        raise AudioRenderError("local media path must not be a symlink")
    try:
        # Binary, share-delete, no-follow fd: a text-mode CRT fd would fold CRLF and
        # stop at 0x1A, so hashes and PCM decoding would see different bytes.
        descriptor = _platform.open_fd(path, os.O_RDONLY, nofollow=True)
    except OSError as exc:
        raise AudioRenderError("local media is missing or unreadable") from exc
    try:
        with os.fdopen(descriptor, "rb") as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                raise AudioRenderError("local media must be a regular file")
            yield stream
    except OSError as exc:
        raise AudioRenderError("local media could not be verified") from exc


def _hash_stream(stream: BinaryIO) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    while chunk := stream.read(1024 * 1024):
        size += len(chunk)
        digest.update(chunk)
    return digest.hexdigest(), size


def _hash_file(path: Path) -> tuple[str, int]:
    with _open_local_file(path) as stream:
        return _hash_stream(stream)


def _file_identity(info: os.stat_result) -> tuple[int, ...]:
    # st_ctime is the creation time on Windows, not a change time: not an identity field.
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)


def _media_path(agent: EndpointAgent, manifest: dict,
                asset: dict) -> Path:
    media_id = asset["media_file_id"]
    supplied = manifest.get("local_media", {}).get(media_id)
    expected = agent._object_path(asset["sha256"].lower())
    if not isinstance(supplied, str) or Path(supplied) != expected:
        raise AudioRenderError("local media path differs from the endpoint object store")
    path = Path(supplied)
    digest, size = _hash_file(path)
    if digest != asset["sha256"].lower() or size != asset["size_bytes"]:
        raise AudioRenderError("local media differs from its accepted manifest")
    return path


def _cue_bounds(entry: dict) -> tuple[int, int, int, int]:
    payload, duration_ms = entry["payload"], entry["planned_duration_ms"]
    if any(key in payload and payload[key] is not None for key in _UNSUPPORTED_PAYLOAD):
        raise AudioRenderError("audio entry requests unsupported cue or DSP behavior")
    markers = entry.get("playback_markers")
    try:
        validate_frozen_markers(markers, version_id=entry["version_id"],
                                media_file_id=entry["media_file_id"], payload=payload,
                                planned_duration_ms=duration_ms, duration_kind="fixed")
    except StoreError as exc:
        raise AudioRenderError(str(exc)) from exc
    cue_in, cue_out = markers["cue_in_ms"], markers["cue_out_ms"]
    if markers["segue_in_ms"] not in (None, cue_in) or \
            markers["segue_out_ms"] not in (None, cue_out):
        raise AudioRenderError("nontrivial segue anchors require a transition mixer")
    return cue_in, cue_out, markers["fade_in_ms"], markers["fade_out_ms"]


def _preflight(agent: EndpointAgent, manifest: dict,
               max_output_bytes: int) -> tuple[list[dict], dict]:
    if type(max_output_bytes) is not int or not 44 <= max_output_bytes <= _MAX_WAV_BYTES:
        raise AudioRenderError("max_output_bytes must be 44..4294967259")
    production = manifest.get("production")
    if production is not None and (not isinstance(production, dict) or
                                   production.get("placements") or
                                   production.get("transitions")):
        raise AudioRenderError("production placements need a different audio executor")
    entries = manifest.get("entries")
    assets = manifest.get("media")
    if not isinstance(entries, list) or not entries or not isinstance(assets, list):
        raise AudioRenderError("accepted plan has no renderable audio entries")
    asset_by_id = {asset["media_file_id"]: asset for asset in assets}
    if len(asset_by_id) != len(assets):
        raise AudioRenderError("media manifest contains duplicate identifiers")
    selected = sorted(entries, key=lambda row: row["ordinal"])
    anchor = _instant(selected[0]["scheduled_at"], "scheduled_at")
    format_spec: tuple[int, int, int] | None = None
    planned: list[dict] = []
    cursor = 0
    for entry in selected:
        if entry.get("entry_kind") != "audio" or entry.get("state") != "planned":
            raise AudioRenderError("only planned audio entries are renderable")
        if entry.get("duration_kind") != "fixed" or \
                type(entry.get("planned_duration_ms")) is not int or \
                entry["planned_duration_ms"] <= 0:
            raise AudioRenderError("audio entry needs a fixed positive duration")
        timing = entry.get("timing")
        transition = entry.get("transition")
        if not isinstance(timing, dict) or timing != {
                "mode": "absolute", "window_ms": 0, "late_policy": "wait",
                "target_end_at": None}:
            raise AudioRenderError("adaptive or target-end timing is unsupported")
        if not isinstance(transition, dict) or transition != {
                "mode": "sequential", "overlap_ms": 0}:
            raise AudioRenderError("overlap or cut transition is unsupported")
        payload = entry.get("payload")
        if not isinstance(payload, dict):
            raise AudioRenderError("audio payload must be an object")
        cue_in_ms, cue_out_ms, fade_in_ms, fade_out_ms = _cue_bounds(entry)
        asset = asset_by_id.get(entry.get("media_file_id"))
        if asset is None:
            raise AudioRenderError("audio entry lacks its accepted media")
        path = _media_path(agent, manifest, asset)
        try:
            with wave.open(str(path), "rb") as source:
                rate, channels, width = (source.getframerate(),
                                         source.getnchannels(), source.getsampwidth())
                if source.getcomptype() != "NONE" or width != _PCM_SAMPLE_WIDTH or \
                        channels not in (1, 2) or not 8000 <= rate <= 192000:
                    raise AudioRenderError("source must be mono/stereo 16-bit PCM WAV")
                if format_spec is None:
                    format_spec = (rate, channels, width)
                elif format_spec != (rate, channels, width):
                    raise AudioRenderError("all WAV sources need the same PCM format")
                start_frame = _elapsed_frames(
                    _instant(entry["scheduled_at"], "scheduled_at"), anchor, rate)
                duration_frames = _frames(entry["planned_duration_ms"], rate)
                cue_in_frame = _frames(cue_in_ms, rate)
                cue_out_frame = _frames(cue_out_ms, rate)
                fade_in_frames, fade_out_frames = _frames(fade_in_ms, rate), _frames(fade_out_ms, rate)
                if duration_frames < 1 or cue_out_frame - cue_in_frame != duration_frames or \
                        cue_out_frame > source.getnframes():
                    raise AudioRenderError("cue is outside media or not frame-aligned with slot")
                if fade_in_frames + fade_out_frames > duration_frames:
                    raise AudioRenderError("fade envelopes overlap after PCM frame alignment")
                if start_frame < cursor:
                    raise AudioRenderError("audio slots overlap after PCM frame alignment")
                gap_frames = start_frame - cursor
                cursor = start_frame + duration_frames
                if 44 + cursor * channels * width > max_output_bytes:
                    raise AudioRenderError("rendered WAV exceeds max_output_bytes")
                planned.append({
                    "plan_entry_id": entry["plan_entry_id"],
                    "media_file_id": entry["media_file_id"],
                    "source_path": path, "source_sha256": asset["sha256"].lower(),
                    "source_size_bytes": asset["size_bytes"],
                    "scheduled_at": entry["scheduled_at"],
                    "start_frame": start_frame, "duration_frames": duration_frames,
                    "gap_frames_before": gap_frames, "cue_in_frame": cue_in_frame,
                    "cue_in_ms": cue_in_ms, "cue_out_ms": cue_out_ms,
                    "fade_in_ms": fade_in_ms, "fade_out_ms": fade_out_ms,
                    "fade_in_frames": fade_in_frames, "fade_out_frames": fade_out_frames,
                    "segue_in_ms": entry["playback_markers"]["segue_in_ms"],
                    "segue_out_ms": entry["playback_markers"]["segue_out_ms"],
                    "marker_set_id": entry["playback_markers"]["marker_set_id"],
                    "marker_set_revision": entry["playback_markers"]["marker_set_revision"],
                    "marker_content_sha256": entry["playback_markers"]["content_sha256"],
                })
        except (wave.Error, EOFError, OSError) as exc:
            raise AudioRenderError("source is not a readable PCM WAV") from exc
    assert format_spec is not None
    return planned, {"rate": format_spec[0], "channels": format_spec[1],
                     "width": format_spec[2], "anchor": anchor,
                     "total_frames": cursor}


def _write_silence(target: wave.Wave_write, frames: int, bytes_per_frame: int) -> None:
    chunk = b"\0" * (_CHUNK_FRAMES * bytes_per_frame)
    while frames:
        count = min(frames, _CHUNK_FRAMES)
        target.writeframesraw(chunk[:count * bytes_per_frame])
        frames -= count


def _linear_fades(data: bytes, entry: dict, frame_offset: int,
                   channels: int) -> bytes:
    """Apply reviewed linear PCM gain: first/last envelope frames reach zero."""
    fade_in, fade_out = entry["fade_in_frames"], entry["fade_out_frames"]
    if not fade_in and not fade_out:
        return data
    samples = array("h")
    samples.frombytes(data)
    if sys.byteorder != "little":
        samples.byteswap()
    for frame in range(len(samples) // channels):
        position = frame_offset + frame
        numerator = denominator = 1
        if fade_in and position < fade_in:
            numerator, denominator = position, max(fade_in - 1, 1)
        elif fade_out and position >= entry["duration_frames"] - fade_out:
            numerator = entry["duration_frames"] - 1 - position
            denominator = max(fade_out - 1, 1)
        if numerator != denominator:
            for channel in range(channels):
                index = frame * channels + channel
                value = samples[index]
                magnitude = (abs(value) * numerator + denominator // 2) // denominator
                samples[index] = magnitude if value >= 0 else -magnitude
    if sys.byteorder != "little":
        samples.byteswap()
    return samples.tobytes()


def _write_entry(target: wave.Wave_write, entry: dict,
                 bytes_per_frame: int) -> None:
    path = entry["source_path"]
    try:
        # Hash and decode through the same descriptor. A path replacement
        # cannot make the decoder consume different bytes from those verified.
        with _open_local_file(path) as stream:
            before = _file_identity(os.fstat(stream.fileno()))
            digest, size = _hash_stream(stream)
            if digest != entry["source_sha256"] or size != entry["source_size_bytes"]:
                raise AudioRenderError("PCM source differs before rendering")
            stream.seek(0)
            with wave.open(stream, "rb") as source:
                source.setpos(entry["cue_in_frame"])
                remaining = entry["duration_frames"]
                while remaining:
                    count = min(remaining, _CHUNK_FRAMES)
                    data = source.readframes(count)
                    if len(data) != count * bytes_per_frame:
                        raise AudioRenderError("PCM source ended before its approved slot")
                    data = _linear_fades(data, entry, entry["duration_frames"] - remaining,
                                         bytes_per_frame // _PCM_SAMPLE_WIDTH)
                    target.writeframesraw(data)
                    remaining -= count
            stream.seek(0)
            digest, size = _hash_stream(stream)
            after = _file_identity(os.fstat(stream.fileno()))
            if before != after or digest != entry["source_sha256"] or \
                    size != entry["source_size_bytes"]:
                raise AudioRenderError("PCM source changed while rendering")
    except (wave.Error, EOFError, OSError) as exc:
        raise AudioRenderError("PCM source failed while rendering") from exc


def render_active_plan_wav(agent: EndpointAgent, output_path: str | Path, *,
                           max_output_bytes: int = 1_000_000_000) -> dict:
    """Render one accepted, unfenced plan to a new PCM WAV with atomic publish.

    The WAV begins at the first planned entry's scheduled instant. Silence
    fills subsequent schedule gaps. Frozen cue offsets select source samples;
    their span must equal the fixed planned duration. Reviewed linear fade-in
    and fade-out envelopes reach zero at their first/last PCM frames. Output is a
    file artifact only, never evidence that a listener heard audio.
    """
    if not isinstance(agent, EndpointAgent):
        raise AudioRenderError("agent must be an EndpointAgent")
    manifest = agent.active_manifest()
    epoch = agent._identity["engine_epoch"]
    planned, spec = _preflight(agent, manifest, max_output_bytes)
    requested = Path(output_path).expanduser()
    if requested.name in {"", ".", ".."} or requested.suffix.lower() != ".wav":
        raise AudioRenderError("output_path must name a .wav file")
    try:
        parent = requested.parent.resolve(strict=True)
    except OSError as exc:
        raise AudioRenderError("output directory does not exist") from exc
    if not parent.is_dir():
        raise AudioRenderError("output parent is not a directory")
    target = parent / requested.name
    if target.exists() or _platform.is_link(target):
        raise AudioRenderError("output file already exists")
    if target == Path(planned[0]["source_path"]):
        raise AudioRenderError("output cannot replace endpoint media")
    temp: Path | None = None
    try:
        descriptor, name = tempfile.mkstemp(prefix=".sdb-audio-", suffix=".wav",
                                            dir=parent)
        temp = Path(name)
        with os.fdopen(descriptor, "wb") as stream:
            with wave.open(stream, "wb") as target_wave:
                target_wave.setnchannels(spec["channels"])
                target_wave.setsampwidth(spec["width"])
                target_wave.setframerate(spec["rate"])
                for entry in planned:
                    _write_silence(target_wave, entry["gap_frames_before"],
                                   spec["channels"] * spec["width"])
                    _write_entry(target_wave, entry,
                                 spec["channels"] * spec["width"])
            stream.flush()
            _platform.fsync(stream)
        current = agent.active_manifest()
        if current["plan_revision_id"] != manifest["plan_revision_id"] or \
                agent._identity["engine_epoch"] != epoch:
            raise AudioRenderError("accepted endpoint plan changed while rendering")
        digest, size = _hash_file(temp)
        if size > max_output_bytes:
            raise AudioRenderError("rendered WAV exceeds max_output_bytes")
        try:
            # No-overwrite publish (works on NTFS, FAT/exFAT and SMB; the temporary
            # name is consumed).  NTFS journals the rename: no directory flush exists.
            _platform.rename_noreplace(temp, target)
        except FileExistsError as exc:
            raise AudioRenderError("output file appeared while rendering") from exc
        temp = None
        return {
            "plan_revision_id": manifest["plan_revision_id"],
            "endpoint_id": manifest["endpoint_id"],
            "output_path": str(target), "sha256": digest,
            "size_bytes": size, "sample_rate_hz": spec["rate"],
            "channels": spec["channels"], "sample_width_bytes": spec["width"],
            "start_at": spec["anchor"].isoformat(),
            "duration_ms": round(spec["total_frames"] * 1000 / spec["rate"]),
            "entry_count": len(planned),
            "gap_duration_ms": round(sum(entry["gap_frames_before"] for entry in planned)
                                     * 1000 / spec["rate"]),
            "entries": [{key: entry[key] for key in
                         ("plan_entry_id", "media_file_id", "scheduled_at",
                          "start_frame", "duration_frames", "gap_frames_before",
                          "cue_in_ms", "cue_out_ms", "segue_in_ms", "segue_out_ms",
                          "fade_in_ms", "fade_out_ms", "marker_set_id",
                          "marker_set_revision", "marker_content_sha256")}
                        for entry in planned],
        }
    except (wave.Error, OSError) as exc:
        raise AudioRenderError("PCM WAV could not be written") from exc
    finally:
        if temp is not None:
            _platform.unlink_quietly(temp)
