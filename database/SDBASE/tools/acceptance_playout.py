"""Bounded reference PCM executor for production acceptance, not device output.

It consumes an accepted EndpointAgent manifest, applies frozen source markers,
mixes explicit scheduled overlaps, executes one allowlisted in-process macro,
and decodes a bounded WAV response from explicitly permitted stream URLs.
"""

from array import array
from datetime import datetime, timedelta
import hashlib
import io
import json
import os
from pathlib import Path
import sys
from urllib.request import Request, urlopen
from uuid import uuid4
import wave


def _instant(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def _wav(blob):
    with wave.open(io.BytesIO(blob), "rb") as source:
        if source.getcomptype() != "NONE" or source.getsampwidth() != 2 or \
                source.getnchannels() not in (1, 2):
            raise ValueError("reference player needs mono/stereo PCM16 WAV")
        if not 8000 <= source.getframerate() <= 192000:
            raise ValueError("unsupported PCM sample rate")
        spec = (source.getframerate(), source.getnchannels())
        samples = array("h", source.readframes(source.getnframes()))
        if sys.byteorder != "little":
            samples.byteswap()
        return spec, samples


def render_reference(agent, output: Path, *, allowed_stream_urls=(),
                     max_seconds=60, run_id=None):
    """Render one finite accepted plan and durably queue catalog audio events.

    Stream/macro receipts are returned separately for diagnostic ingestion.
    No event or receipt asserts that sound reached an audio device/listener.
    """
    manifest = agent.active_manifest()
    if type(max_seconds) is not int or not 1 <= max_seconds <= 300:
        raise ValueError("reference max_seconds must be an integer in 1..300")
    revision, epoch = manifest["plan_revision_id"], agent._identity["engine_epoch"]
    entries = sorted(manifest["entries"], key=lambda value: value["ordinal"])
    if not entries or (manifest.get("production") or {}).get("placements"):
        raise ValueError("reference player needs a nonempty plan without overlays")
    if output.exists() or output.suffix != ".wav":
        raise ValueError("reference output must be a new .wav path")
    run_id = run_id or str(uuid4())
    anchor = _instant(entries[0]["scheduled_at"])
    clips, receipts, flags, pcm_spec = [], [], {}, None
    asset_map = {asset["media_file_id"]: asset for asset in manifest["media"]}
    for entry in entries:
        if entry["state"] != "planned" or entry["timing"] != {
                "mode": "absolute", "window_ms": 0, "late_policy": "wait", "target_end_at": None}:
            raise ValueError("reference player supports fixed absolute schedules only")
        kind, payload = entry["entry_kind"], entry["payload"]
        receipt = {"run_id": run_id, "plan_entry_id": entry["plan_entry_id"],
                   "entry_kind": kind, "scheduled_at": entry["scheduled_at"],
                   "output_id": manifest["execution_binding"]["output_id"]}
        if kind == "macro":
            if payload.get("command") != "acceptance.set_flag" or \
                    set(payload.get("args", {})) != {"name", "value"}:
                raise ValueError("macro is outside the reference player's explicit allowlist")
            args = payload["args"]
            if not isinstance(args["name"], str) or type(args["value"]) is not bool:
                raise ValueError("reference set_flag macro needs string name and Boolean value")
            flags[args["name"]] = args["value"]
            receipt.update({"executed": True, "macro": payload["command"], "args": args})
            receipts.append(receipt)
            continue
        if kind not in {"audio", "stream"} or entry["duration_kind"] != "fixed":
            raise ValueError("reference player supports audio, bounded WAV stream and set_flag macro")
        if entry['transition']['mode'] not in {'sequential', 'overlap'}:
            raise ValueError("reference player supports sequential or explicit overlap transitions")
        if any(payload.get(key) is not None for key in
               ("gain_db", "replay_gain_db", "ducking_envelope", "crossfade_ms")):
            raise ValueError("reference player cannot prove requested DSP")
        duration = entry["planned_duration_ms"]
        if kind == "audio":
            asset = asset_map[entry["media_file_id"]]
            if asset['size_bytes'] > 16 * 1024 * 1024:
                raise ValueError("reference audio source exceeds its 16 MiB bound")
            path = Path(manifest["local_media"][entry["media_file_id"]])
            blob = path.read_bytes()
            if hashlib.sha256(blob).hexdigest() != asset["sha256"] or len(blob) != asset["size_bytes"]:
                raise ValueError("reference media SHA/size differs from accepted manifest")
            markers = entry["playback_markers"]
            cue_in, cue_out = markers["cue_in_ms"], markers["cue_out_ms"]
            fade_in, fade_out = markers["fade_in_ms"], markers["fade_out_ms"]
            if cue_out - cue_in != duration:
                raise ValueError("reference cue span differs from fixed slot duration")
            receipt.update({"version_id": entry["version_id"], "media_file_id": entry["media_file_id"],
                            "playback_markers": markers})
        else:
            uri = payload.get("uri")
            if uri not in allowed_stream_urls:
                raise ValueError("stream URI is outside the reference player's explicit allowlist")
            # Redirects cannot silently change the explicitly permitted target.
            with urlopen(Request(uri, headers={"Accept": "audio/wav"}), timeout=5) as response:
                if response.geturl() != uri:
                    raise ValueError("reference stream redirects are unsupported")
                blob = response.read(8 * 1024 * 1024 + 1)
            if len(blob) > 8 * 1024 * 1024:
                raise ValueError("reference stream exceeds its 8 MiB bound")
            cue_in, cue_out, fade_in, fade_out = 0, duration, 0, 0
            receipt.update({"uri": uri, "stream_bytes": len(blob),
                            "stream_sha256": hashlib.sha256(blob).hexdigest()})
        spec, samples = _wav(blob)
        if pcm_spec is None:
            pcm_spec = spec
        elif pcm_spec != spec:
            raise ValueError("reference PCM sources need identical rate/channel format")
        rate, channels = spec
        start = round((_instant(entry["scheduled_at"]) - anchor).total_seconds() * rate)
        frames = round(duration * rate / 1000)
        first, last = round(cue_in * rate / 1000), round(cue_out * rate / 1000)
        if start < 0 or last - first != frames or last * channels > len(samples):
            raise ValueError("reference source cue or duration is outside PCM bounds")
        if (start + frames) / rate > max_seconds:
            raise ValueError("reference output exceeds its bounded duration")
        if (start + frames) * channels > 8_000_000:
            raise ValueError("reference output exceeds its eight million PCM sample bound")
        fi, fo = round(fade_in * rate / 1000), round(fade_out * rate / 1000)
        selected = samples[first * channels:last * channels]
        for frame in range(frames):
            gain = min(1.0, frame / fi if fi else 1.0,
                       (frames - 1 - frame) / fo if fo else 1.0)
            for channel in range(channels):
                offset = frame * channels + channel
                selected[offset] = round(selected[offset] * gain)
        receipt.update({"start_frame": start, "duration_frames": frames,
                        "transition": entry["transition"], "executed": True})
        clips.append((entry, start, selected, frames))
        receipts.append(receipt)
    if pcm_spec is None:
        raise ValueError("reference plan has no PCM")
    rate, channels = pcm_spec
    total = max(start + frames for _, start, _, frames in clips)
    mixed = [0] * (total * channels)
    for entry, start, samples, frames in clips:
        for index, sample in enumerate(samples):
            mixed[start * channels + index] += sample
    pcm = array("h", (max(-32768, min(32767, value)) for value in mixed))
    if sys.byteorder != "little":
        pcm.byteswap()
    current = agent.active_manifest()
    if current["plan_revision_id"] != revision or agent._identity["engine_epoch"] != epoch:
        raise ValueError("reference plan changed while rendering")
    temporary = output.with_name(output.name + ".partial")
    try:
        with temporary.open("xb") as stream:
            with wave.open(stream, "wb") as rendered:
                rendered.setnchannels(channels); rendered.setsampwidth(2); rendered.setframerate(rate)
                rendered.writeframes(pcm.tobytes())
            stream.flush(); os.fsync(stream.fileno())
        os.link(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    # These are engine render claims, and explicitly identify the file sink.
    for entry, start, samples, frames in clips:
        if entry["entry_kind"] != "audio":
            continue
        for phase, instant in (("start", _instant(entry["scheduled_at"])),
                               ("complete", _instant(entry["scheduled_at"]) +
                                timedelta(milliseconds=entry["planned_duration_ms"]))):
            agent.queue_playout_event({"station_id": agent.station_id, "endpoint_id": agent.endpoint_id,
                "engine_event_id": f"reference:{run_id}:{entry['plan_entry_id']}:{phase}",
                "event_type": phase, "occurred_at": instant.isoformat(), "attempt_no": 1,
                "version_id": entry["version_id"], "media_file_id": entry["media_file_id"],
                "plan_entry_id": entry["plan_entry_id"], "details": {
                    "sink": "reference_pcm_file", "on_air_proven": False,
                    "output_id": manifest["execution_binding"]["output_id"]}})
    receipt_path = output.with_suffix(".receipts.json")
    with receipt_path.open("x", encoding="utf-8") as stream:
        json.dump(receipts, stream, ensure_ascii=False);stream.flush();os.fsync(stream.fileno())
    blob = output.read_bytes()
    return {"plan_revision_id": revision, "run_id": run_id, "sample_rate_hz": rate,
            "channels": channels, "frames": total, "sha256": hashlib.sha256(blob).hexdigest(),
            "size_bytes": len(blob), "flags": flags, "receipts": receipts,
            "on_air_proven": False, "sink": "reference_pcm_file"}
