#!/usr/bin/env python3
"""Run genuine GTK/audio acceptance with a private DB and owned null outputs."""
from __future__ import annotations

import configparser
import contextlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import urllib.request
import uuid

PROJECT = Path(__file__).resolve().parents[1]


def pactl(*arguments: str) -> str:
    result = subprocess.run(["pactl", *arguments], check=True, capture_output=True,
                            text=True, timeout=10)
    return result.stdout.strip()


def post(url: str, token: str, path: str, body: dict) -> dict:
    payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(url + path, data=payload, method="POST",
                                    headers={"Authorization": "Bearer " + token,
                                             "Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=5) as response:
        return json.load(response)


def main(*, test_timeout: int = 100, collect_glib_cases: list[str] | None = None,
         expected_glib_case: str | None = None) -> int:
    engine_only = len(sys.argv) > 1 and sys.argv[1] == "--engine-only"
    executable_index = 2 if engine_only else 1
    if len(sys.argv) <= executable_index:
        print("Usage: run_audio_ui.py [--engine-only] /absolute/path/to/test_audio_ui [GLib test arguments]",
              file=sys.stderr)
        return 2
    for command in ("pactl", "parec"):
        if shutil.which(command) is None:
            print("Audio acceptance requires pactl and parec.", file=sys.stderr)
            return 1
    if not engine_only and not os.environ.get("DISPLAY"):
        print("Audio GTK acceptance requires an available DISPLAY.", file=sys.stderr)
        return 1
    executable = Path(sys.argv[executable_index]).resolve(strict=True)
    test_arguments = sys.argv[executable_index + 1:]
    build = PROJECT / "build"
    build.mkdir(exist_ok=True)
    environment = os.environ.copy()
    environment.pop("RADIOS_SDBASE_TOKEN", None)
    environment.pop("PYTHONPATH", None)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    environment["G_DEBUG"] = "fatal-warnings"
    modules: list[str] = []
    captures: list[subprocess.Popen] = []
    outputs = []
    command: list[str] | None = None
    root: Path | None = None
    temporary = None
    try:
        pactl("info")
        temporary = tempfile.TemporaryDirectory(prefix="native-audio-ui-", dir=build)
        root = Path(temporary.name) / "RadioS Türkçe ses kabul"
        root.mkdir()
        shutil.copy2(PROJECT / "CMakeLists.txt", root / "CMakeLists.txt")
        shutil.copytree(PROJECT / "assets", root / "assets")
        shutil.copytree(PROJECT / "database/SDBASE/src", root / "database/SDBASE/src",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        shutil.copy2(PROJECT / "database/SDBASE/pyproject.toml", root / "database/SDBASE/pyproject.toml")
        # Hold a computed media GET response only in this private fixture. This
        # makes a previously queued card-CUE response arrive after a newer
        # preview/user-off action, without changing production HTTP behavior.
        server_source = root / "database/SDBASE/src/sdbase/http_server.py"
        source = server_source.read_text(encoding="utf-8")
        anchor = "                result = self._dispatch(method, path, query, principal)\n"
        gate = '''                _acceptance_audio_gate = store.root / ".acceptance-media-get-hold"
                if method == "POST" and path == "/v1/workflows/desktop-media/get" and _acceptance_audio_gate.is_file():
                    import time as _acceptance_audio_time
                    _acceptance_audio_deadline = _acceptance_audio_time.monotonic() + 2.0
                    while _acceptance_audio_gate.exists() and _acceptance_audio_time.monotonic() < _acceptance_audio_deadline:
                        _acceptance_audio_time.sleep(0.005)
'''
        if source.count(anchor) != 1:
            raise RuntimeError("The isolated audio-response acceptance gate could not be installed")
        server_source.write_text(source.replace(anchor, anchor + gate), encoding="utf-8")
        (root / "tools").mkdir()
        for source in (PROJECT / "tools").glob("radios_database*.py"):
            shutil.copy2(source, root / "tools" / source.name)
        (root / "config").mkdir()
        shutil.copy2(PROJECT / "config/radios.example.ini", root / "config/radios.example.ini")
        configuration = configparser.ConfigParser(interpolation=None)
        configuration.read(root / "config/radios.example.ini", encoding="utf-8")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        url = f"http://127.0.0.1:{port}"
        configuration["database"]["service_url"] = url
        configuration["database"]["python_command"] = sys.executable
        configuration["database"]["connect_timeout_seconds"] = "1"
        configuration["database"]["request_timeout_seconds"] = "3"
        with (root / "config/radios.ini").open("w", encoding="utf-8") as stream:
            configuration.write(stream)
        for name, value in configuration["paths"].items():
            if name != "assets_root":
                destination = root / value
                (destination.parent if name == "database_file" else destination).mkdir(parents=True, exist_ok=True)
        command = [sys.executable, str(root / "tools/radios_database.py"), "--project-root", str(root)]
        initialized = subprocess.run(command + ["init", "--station-name", "Ses Kabul İstasyonu"],
                                     cwd=root, env=environment, capture_output=True, text=True, timeout=20)
        if initialized.returncode:
            raise RuntimeError("Isolated audio database initialization failed")
        summary = json.loads(initialized.stdout)
        if summary.get("schema_version") != 4 or summary.get("endpoint_status") != "offline":
            raise RuntimeError("Isolated audio database identity is invalid")
        started = subprocess.run(command + ["start"], cwd=root, env=environment,
                                 capture_output=True, text=True, timeout=20)
        if started.returncode:
            raise RuntimeError("Isolated audio database service did not start")
        credentials = json.loads((root / "config/credentials.json").read_text(encoding="utf-8"))
        unique = "radios_gui_" + uuid.uuid4().hex
        sinks = {}
        for bus in ("MAIN", "CUE", "MONITOR"):
            sink = unique + "_" + bus.lower()
            module = pactl("load-module", "module-null-sink", "sink_name=" + sink,
                           "rate=48000", "channels=2", "channel_map=front-left,front-right")
            if not module.isdecimal():
                raise RuntimeError("Virtual audio output module identity is invalid")
            modules.append(module)
            sinks[bus] = sink
            environment["RADIOS_TEST_" + bus + "_SINK"] = sink
            capture = root / ("capture-" + bus.lower() + ".f32")
            environment["RADIOS_TEST_" + bus + "_CAPTURE"] = str(capture)
            output = capture.open("wb")
            outputs.append(output)
            process = subprocess.Popen(["parec", "--raw", "--format=float32le", "--rate=48000",
                                        "--channels=2", "--latency-msec=10", "--device=" + sink + ".monitor"],
                                       stdout=output, stderr=subprocess.DEVNULL, env=environment)
            captures.append(process)
        # This managed configuration is committed before any application
        # window exists. Every playback test must also verify the names.
        preferences = {"main_sink": sinks["MAIN"], "cue_sink": sinks["CUE"],
                       "monitor_sink": sinks["MONITOR"],
                       "microphone_source": sinks["MONITOR"] + ".monitor",
                       "main_gain_db": 0.0, "cue_gain_db": 0.0,
                       "monitor_gain_db": 0.0, "microphone_gain_db": 0.0,
                       "cue_source": -1, "monitor_source": -1}
        environment["RADIOS_TEST_MIC_SOURCE"] = sinks["MONITOR"] + ".monitor"
        post(url, credentials["api_token"], "/v1/workflows/desktop/put",
             {"station_id": credentials["station_id"], "kind": "audio_preferences",
              "name": "Ses aygıtları", "config": preferences,
              "idempotency_key": str(uuid.uuid4())})
        invocation = [str(executable), *([] if engine_only else [str(root)]), *test_arguments]
        validate_tap = collect_glib_cases is not None or expected_glib_case is not None
        capture_options = {"capture_output": True, "text": True} if validate_tap else {}
        result = subprocess.run(invocation, cwd=root,
                                env=environment, timeout=test_timeout, check=False, **capture_options)
        if validate_tap:
            # An opt-in discovery invocation still receives the real initialized
            # private DB and owned audio outputs. Ordinary runners keep their
            # existing subprocess behavior and fixture scope.
            print(result.stdout, end="", flush=True)
            print(result.stderr, end="", file=sys.stderr, flush=True)
            if result.returncode == 0:
                lines = result.stdout.splitlines()
                plans = [int(line[3:]) for line in lines if re.fullmatch(r"1\.\.\d+", line)]
                if collect_glib_cases is not None:
                    # GLib prefixes list entries with "# " in TAP mode.
                    paths = [line[2:] if line.startswith("# /") else line
                             for line in lines if line.startswith(("/", "# /"))]
                    if len(plans) != 1 or plans[0] <= 0 or plans[0] != len(paths):
                        raise RuntimeError("GLib case discovery does not match its positive TAP plan")
                    collect_glib_cases.extend(paths)
                if expected_glib_case is not None:
                    outcomes = [line for line in lines if re.match(r"(?:not )?ok \d+\b", line)]
                    if plans != [1] or outcomes != ["ok 1 " + expected_glib_case]:
                        raise RuntimeError("Isolated GLib case did not execute its sole expected test without a skip")
        if any(process.poll() is not None for process in captures):
            raise RuntimeError("Virtual output capture stopped unexpectedly")
        return result.returncode
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Native audio acceptance failed: {error}", file=sys.stderr)
        return 1
    finally:
        for process in captures:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        for output in outputs:
            output.close()
        # Only numeric module identities created above are unloaded. The system
        # default output and all unrelated Pulse/PipeWire modules are untouched.
        for module in reversed(modules):
            with contextlib.suppress(OSError, subprocess.SubprocessError):
                subprocess.run(["pactl", "unload-module", module], capture_output=True,
                               timeout=10, check=False)
        if command and root and root.exists():
            with contextlib.suppress(OSError, subprocess.SubprocessError):
                subprocess.run(command + ["stop"], cwd=root, env=environment,
                               capture_output=True, text=True, timeout=15, check=False)
        if temporary is not None:
            temporary.cleanup()


def _run_cli() -> int:
    original_arguments = sys.argv
    # Only the default native-audio command receives independent fixtures.
    # Imported main(), engine tests and explicit GLib arguments retain their
    # existing fixture scope and public API.
    if (len(original_arguments) != 2 or
            Path(original_arguments[1]).name != "test_audio_ui"):
        return main()
    try:
        cases: list[str] = []
        sys.argv = [*original_arguments, "-p", "/native-audio", "-l"]
        result = main(collect_glib_cases=cases)
        if result:
            return result
        if (not cases or len(cases) != len(set(cases)) or
                any(not case.startswith("/native-audio/") for case in cases)):
            raise RuntimeError("Native audio GLib case discovery is empty or ambiguous")
        # Deliberate conflicts and reopens share their own case's database.
        # Other cases start with fresh identities, state and request quotas.
        for case in cases:
            print("Native audio independent fixture:", case, flush=True)
            sys.argv = [*original_arguments, "-p", case]
            result = main(expected_glib_case=case)
            if result:
                return result
        return 0
    except RuntimeError as error:
        print(f"Native audio acceptance failed: {error}", file=sys.stderr)
        return 1
    finally:
        sys.argv = original_arguments


if __name__ == "__main__":
    raise SystemExit(_run_cli())
