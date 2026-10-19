#!/usr/bin/env python3
"""Private HTTP/native transfer acceptance with real verified PCM objects."""
from datetime import date
import hashlib
import importlib
import json
import math
from pathlib import Path
import struct
import sys
from uuid import uuid4
import wave

import run_admin_ui as base


def seed(root):
    sys.path.insert(0, str(root / "tools"))
    setup = importlib.import_module("radios_database_setup")
    configuration = setup.load_config(root)
    backend = setup.backend_import(configuration)
    credentials = setup.load_credentials(configuration)
    store = backend.open_radio_store(configuration.database_file)
    store.desktop_import_root = root / "data/imports"
    media = importlib.import_module(backend.package_name + ".desktop_media")
    api = importlib.import_module(backend.package_name + ".desktop_api")
    principal = backend.authenticate(store, credentials["api_token"])
    entries = []
    for title in ("Temporary A", "Temporary B"):
        path = store.desktop_import_root / (str(uuid4()) + ".wav")
        samples = [round(16000 * math.sin(2 * math.pi * 400 * index / 8000)) for index in range(1600)]
        with wave.open(str(path), "wb") as output:
            output.setnchannels(1); output.setsampwidth(2); output.setframerate(8000)
            output.writeframes(struct.pack("<1600h", *samples))
        path.chmod(0o600)
        normalized = [value / 32768 for value in samples]
        waveform = []
        for start in range(0, len(samples), 400):
            chunk = normalized[start:start + 400]
            waveform.extend((min(chunk), max(chunk)))
        analysis = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "size_bytes": path.stat().st_size,
            "duration_ms": 200, "sample_rate": 8000, "channels": 1, "codec": "pcm_s16le", "format": "wav",
            "bitrate": 128000, "peak": max(map(abs, normalized)),
            "rms": math.sqrt(sum(value * value for value in normalized) / len(normalized)),
            "clipped": False, "clipped_samples": 0, "sample_frames": 1600,
            "waveform_frames_per_bin": 400, "waveform": waveform}
        result = media.import_media(store, principal, credentials["station_id"], path.name, title + ".wav",
            str(uuid4()), {"title": title, "kind": "music", "artist": "Verified artist", "album": "Verified album"}, analysis)
        record = result["media"]
        entries.append({"id": str(uuid4()), "kind": 0,
            "values": [title, "Verified artist", "Verified album", "99:59", "", "", "", "", ""],
            "file": "/display-only/untrusted.wav", "media_id": record["_id"], "version_id": record["version_id"],
            "media_file_id": record["media_file_id"], "media_sha256": analysis["sha256"]})
    api.put_document(store, principal, credentials["station_id"], "emergency", "Acil hazırlık",
                     {"queues": [[], entries], "preparation": {}}, str(uuid4()))
    api.put_document(store, principal, credentials["station_id"], "programme", "Mevcut program",
                     {"title": "Preserved programme", "date": date.today().isoformat(), "note": "",
                      "events": [{"id": str(uuid4()), "title": "Existing draft entry", "artist": "", "album": "",
                                  "kind": "announcement", "start_time": "", "duration_ms": 300,
                                  "version_label": "", "primary_file": "", "backup_file": "", "note": "Preserved note"}]}, str(uuid4()))


def main():
    original_copytree = base.shutil.copytree
    original_run = base.subprocess.run

    def copy_fixture(source, destination, *arguments, **options):
        result = original_copytree(source, destination, *arguments, **options)
        if Path(source) == base.PROJECT / "database/SDBASE/src":
            script = Path(destination) / "sdbase/http_server.py"
            text = script.read_text()
            anchor = "                result = self._dispatch(method, path, query, principal)\n"
            gate = '''                if method == "POST" and path == "/v1/workflows/desktop-broadcast/emergency-prepare":
                    import time as _transfer_time
                    _transfer_hold = store.root / ".transfer-hold"
                    if _transfer_hold.exists():
                        (store.root / ".transfer-ready").write_text("ready")
                    _transfer_deadline = _transfer_time.monotonic() + 5
                    while _transfer_hold.exists() and _transfer_time.monotonic() < _transfer_deadline:
                        _transfer_time.sleep(0.005)
'''
            if text.count(anchor) != 1:
                raise RuntimeError("Cannot install owned emergency transfer response gate")
            script.write_text(text.replace(anchor, anchor + gate))
        return result

    def run_command(arguments, *positional, **options):
        result = original_run(arguments, *positional, **options)
        if isinstance(arguments, list) and "--project-root" in arguments and "init" in arguments and result.returncode == 0:
            seed(Path(arguments[arguments.index("--project-root") + 1]))
        return result

    base.shutil.copytree = copy_fixture
    base.subprocess.run = run_command
    try:
        return base.main()
    finally:
        base.shutil.copytree = original_copytree
        base.subprocess.run = original_run


if __name__ == "__main__":
    raise SystemExit(main())
