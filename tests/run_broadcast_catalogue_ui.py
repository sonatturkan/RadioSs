#!/usr/bin/env python3
"""Real native catalogue acceptance; all media/HTTP fault gates are private."""
from pathlib import Path
import hashlib
import importlib
import math
import struct
import sys
from uuid import uuid4
import wave

import run_admin_ui as base
import run_emergency_transfer_ui as fixtures


def seed_catalogue_kinds(root):
    """Verified category records belong only to this runner's temporary store."""
    setup = importlib.import_module("radios_database_setup")
    configuration = setup.load_config(root)
    backend = setup.backend_import(configuration)
    credentials = setup.load_credentials(configuration)
    store = backend.open_radio_store(configuration.database_file)
    store.desktop_import_root = root / "data/imports"
    media = importlib.import_module(backend.package_name + ".desktop_media")
    principal = backend.authenticate(store, credentials["api_token"])
    for index, (kind, title) in enumerate((("jingle", "Scope9 Jingle"),
            ("announcement", "Scope9 Anons"), ("advertisement", "Scope9 Reklam"),
            ("program", "Scope9 Program"), ("music", "Scope9 Music"))):
        path = store.desktop_import_root / (str(uuid4()) + ".wav")
        samples = [round(12000 * math.sin(2 * math.pi * (510 + index * 90) * frame / 8000))
                   for frame in range(1600)]
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
        media.import_media(store, principal, credentials["station_id"], path.name, title + ".wav",
            str(uuid4()), {"title": title, "kind": kind, "artist": "Scope9 artist", "album": "Scope9 album"}, analysis)


def main():
    if len(sys.argv) < 2:
        return base.main()
    original_copy = base.shutil.copytree
    original_run = base.subprocess.run
    original_arguments = sys.argv
    executable = str(Path(original_arguments[1]).resolve(strict=True))
    cases = []
    discovering = False
    expected_case = None

    def copy(source, destination, *args, **kwargs):
        result = original_copy(source, destination, *args, **kwargs)
        if Path(source) == base.PROJECT / "database/SDBASE/src":
            server = Path(destination) / "sdbase/http_server.py"
            text = server.read_text()
            anchor = "                result = self._dispatch(method, path, query, principal)\n"
            gates = '''                if method == "POST" and path in {"/v1/workflows/desktop-media/get", "/v1/workflows/desktop/get", "/v1/workflows/desktop-playout/prepare"}:
                    import time as _catalogue_time
                    _catalogue_hold = store.root / ".catalogue-hold"
                    if _catalogue_hold.exists():
                        (store.root / ".catalogue-ready").touch(mode=0o600)
                        _catalogue_deadline = _catalogue_time.monotonic() + 5
                        while _catalogue_hold.exists() and _catalogue_time.monotonic() < _catalogue_deadline:
                            _catalogue_time.sleep(0.005)
'''
            if text.count(anchor) != 1:
                raise RuntimeError("Private catalogue response fault anchor changed")
            server.write_text(text.replace(anchor, anchor + gates))
        return result

    def run(arguments, *args, **kwargs):
        native = isinstance(arguments, list) and arguments and arguments[0] == executable
        if native and (discovering or expected_case):
            kwargs = kwargs | {"capture_output": True, "text": True}
        result = original_run(arguments, *args, **kwargs)
        if native and discovering and result.returncode == 0:
            paths = [line.removeprefix("# ") for line in result.stdout.splitlines()]
            cases.extend(line for line in paths if line.startswith("/native-broadcast-catalogue/"))
        elif native and expected_case:
            print(result.stdout, end="", flush=True)
            print(result.stderr, end="", file=sys.stderr, flush=True)
            if result.returncode == 0:
                lines = result.stdout.splitlines()
                outcomes = [line for line in lines if line.startswith(("ok ", "not ok "))]
                if outcomes != ["ok 1 " + expected_case] or "1..1" not in lines:
                    raise RuntimeError("Native catalogue case did not execute exactly once")
        if isinstance(arguments, list) and "--project-root" in arguments and "init" in arguments and result.returncode == 0:
            root = Path(arguments[arguments.index("--project-root") + 1])
            fixtures.seed(root)
            seed_catalogue_kinds(root)
        return result

    base.shutil.copytree, base.subprocess.run = copy, run
    try:
        if len(original_arguments) == 2:
            discovering = True; sys.argv = [*original_arguments, "-l"]
            status = base.main(); discovering = False
            if status:
                return status
            if not cases or len(cases) != len(set(cases)):
                raise RuntimeError("Native catalogue case discovery is empty or ambiguous")
            for case in cases:
                print("Native catalogue independent fixture:", case, flush=True)
                expected_case = case; sys.argv = [*original_arguments, "-p", case]
                status = base.main()
                if status:
                    return status
            return 0
        return base.main()
    finally:
        sys.argv = original_arguments
        base.shutil.copytree, base.subprocess.run = original_copy, original_run


if __name__ == "__main__":
    raise SystemExit(main())
