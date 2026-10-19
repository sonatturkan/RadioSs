#!/usr/bin/env python3
"""Run playlist selection GTK tests with private genuine HTTP fault gates."""
from pathlib import Path
import hashlib
import importlib
import math
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
    principal = backend.authenticate(store, credentials["api_token"])
    for index, title in enumerate(("Selected A", "Selected B", "Selected C")):
        path = store.desktop_import_root / (str(uuid4()) + ".wav")
        samples = [round(16000 * math.sin(2 * math.pi * (400 + index * 100) * frame / 8000)) for frame in range(1600)]
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
        media.import_media(store, principal, credentials["station_id"], path.name, title + ".wav", str(uuid4()),
                           {"title": title, "kind": "music", "artist": "Selected artist", "album": "Selected album"}, analysis)


def main():
    original_copy = base.shutil.copytree
    original_run = base.subprocess.run

    def copy(source, destination, *args, **kwargs):
        result = original_copy(source, destination, *args, **kwargs)
        if Path(source) == base.PROJECT / "database/SDBASE/src":
            server = Path(destination) / "sdbase/http_server.py"
            text = server.read_text()
            anchor = "                result = self._dispatch(method, path, query, principal)\n"
            gates = '''                if method == "POST" and path == "/v1/workflows/desktop-playlists/create" and isinstance(result, dict):
                    import time as _selection_time
                    import socket as _selection_socket
                    _selection_hold = store.root / ".selection-hold"
                    if _selection_hold.exists():
                        (store.root / ".selection-ready").touch(mode=0o600)
                        _selection_deadline = _selection_time.monotonic() + 5
                        while _selection_hold.exists() and _selection_time.monotonic() < _selection_deadline:
                            _selection_time.sleep(0.005)
                    _selection_drop = store.root / ".selection-drop"
                    if _selection_drop.exists():
                        _selection_drop.unlink()
                        self.close_connection = True
                        self.connection.shutdown(_selection_socket.SHUT_RDWR)
                        self.connection.close()
                        return
                    _selection_forge = store.root / ".selection-forge"
                    if _selection_forge.exists():
                        _selection_forge.unlink()
                        result["tracks"][0]["title"] = "Forged receipt title"
'''
            if text.count(anchor) != 1:
                raise RuntimeError("Private playlist selection fault anchor changed")
            server.write_text(text.replace(anchor, anchor + gates), encoding="utf-8")
        return result

    def run(arguments, *args, **kwargs):
        result = original_run(arguments, *args, **kwargs)
        if isinstance(arguments, list) and "--project-root" in arguments and "init" in arguments and result.returncode == 0:
            seed(Path(arguments[arguments.index("--project-root") + 1]))
        return result

    base.shutil.copytree = copy
    base.subprocess.run = run
    try:
        return base.main()
    finally:
        base.shutil.copytree = original_copy
        base.subprocess.run = original_run


if __name__ == "__main__":
    raise SystemExit(main())
