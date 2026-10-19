#!/usr/bin/env python3
"""Native routing graph events with private DB/Pulse and independent PCM checks."""
from __future__ import annotations

import argparse
import configparser
import contextlib
import hashlib
import json
import re
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from run_audio_routing import PROJECT, terminate, tone_amplitudes, tone_file


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--report-dir", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    reuse_server = os.environ.get("PULSE_SERVER") if os.environ.get("RADIOS_TEST_REUSE_PULSE") == "1" else None
    if reuse_server and not reuse_server.startswith("unix:/tmp/radios-audio-private-"):
        raise RuntimeError("Reusable Pulse fixture must be an explicitly owned private test socket")
    daemon = os.environ.get("RADIOS_TEST_PULSEAUDIO_BIN") or shutil.which("pulseaudio")
    candidate = PROJECT / "build/stage13-pulse/pulseaudio"
    if not daemon and candidate.is_file():
        daemon = str(candidate)
    if (not daemon and not reuse_server) or any(not shutil.which(command) for command in ("pactl", "pacat")):
        print("Private native routing acceptance requires Pulse daemon, pactl and pacat.", file=sys.stderr)
        return 77
    if not os.environ.get("DISPLAY"):
        print("Native graph palette drag acceptance requires an X11 graphical session.", file=sys.stderr)
        return 77
    temporary = None
    if args.report_dir:
        directory = args.report_dir.resolve()
        directory.mkdir(mode=0o700, parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="native-routing-graph-", dir=PROJECT / "build")
        directory = Path(temporary.name)
    root = directory / "RadioS Türkçe yönlendirme grafiği"
    root.mkdir(mode=0o700)
    environment = os.environ.copy()
    environment.pop("RADIOS_SDBASE_TOKEN", None)
    environment.pop("PYTHONPATH", None)
    environment.update(PYTHONDONTWRITEBYTECODE="1", G_DEBUG="fatal-warnings", GDK_BACKEND="x11")
    processes: list[subprocess.Popen] = []
    inputs = []
    command = None
    reused_modules = []
    directory_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    server = reuse_server or f"unix:/proc/{os.getpid()}/fd/{directory_fd}/native"
    report = {"passed": False, "scope": "native GTK graph input, isolated real HTTP/DB and selected application/virtual microphone PCM",
              "host_audio_changed": False, "physical_microphone_captured": False,
              "fixture_playback_warmup_seconds": 2.3,
              "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
              "expected_case": "/native-routing-graph/drag-connect-save-reopen-pcm"}
    started = time.monotonic()
    try:
        shutil.copy2(PROJECT / "CMakeLists.txt", root / "CMakeLists.txt")
        shutil.copytree(PROJECT / "assets", root / "assets")
        shutil.copytree(PROJECT / "database/SDBASE/src", root / "database/SDBASE/src",
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        shutil.copy2(PROJECT / "database/SDBASE/pyproject.toml", root / "database/SDBASE/pyproject.toml")
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
        configuration["application"]["initial_profile"] = "routing"
        configuration["database"]["service_url"] = f"http://127.0.0.1:{port}"
        configuration["database"]["python_command"] = sys.executable
        configuration["database"]["connect_timeout_seconds"] = "1"
        configuration["database"]["request_timeout_seconds"] = "5"
        with (root / "config/radios.ini").open("w", encoding="utf-8") as output:
            configuration.write(output)
        for name, value in configuration["paths"].items():
            if name != "assets_root":
                path = root / value
                (path.parent if name == "database_file" else path).mkdir(parents=True, exist_ok=True)
        command = [sys.executable, str(root / "tools/radios_database.py"), "--project-root", str(root)]
        for action in (["init", "--station-name", "Yönlendirme Kabul İstasyonu"], ["start"]):
            result = subprocess.run(command + action, cwd=root, env=environment, capture_output=True, text=True, timeout=25)
            if result.returncode:
                raise RuntimeError("Private routing database initialization/start failed")
        (directory / "runtime").mkdir(mode=0o700)
        (directory / "state").mkdir(mode=0o700)
        environment.update(PULSE_SERVER=server, RADIOS_TEST_PULSE_SERVER=server,
            PULSE_RUNTIME_PATH=str(directory / "runtime"), PULSE_STATE_PATH=str(directory / "state"))
        daemon_environment = environment | {"DBUS_SESSION_BUS_ADDRESS": f"unix:path=/proc/{os.getpid()}/fd/{directory_fd}/no-desktop-bus"}
        pulse_configuration = directory / "private.pa"
        pulse_configuration.write_text(f'load-module module-native-protocol-unix socket="{server[5:]}" auth-anonymous=1\n'
            "load-module module-null-sink sink_name=routing_original rate=48000 channels=2\n"
            "load-module module-null-sink sink_name=routing_output rate=48000 channels=2\n"
            "load-module module-null-sink sink_name=routing_microphone rate=48000 channels=2\n"
            "load-module module-null-sink sink_name=routing_unrelated rate=48000 channels=2\n"
            "set-default-sink routing_original\nset-default-source routing_microphone.monitor\n", encoding="utf-8")
        arguments = [daemon, "--daemonize=no", "--exit-idle-time=-1", "--use-pid-file=no", "--disable-shm=yes",
                     "--high-priority=no", "--realtime=no", "--log-target=stderr", "-n", "--file=" + str(pulse_configuration)]
        module_path = os.environ.get("RADIOS_TEST_PULSE_MODULE_PATH")
        modules = PROJECT / "build/stage13-pulse/usr/lib/pulse-17.0/modules"
        if not module_path and modules.is_dir():
            module_path = str(modules)
        if module_path:
            arguments.append("--dl-search-path=" + module_path)
        with (directory / "pulse.log").open("wb") as pulse_log:
            if reuse_server:
                sink_names = ("routing_original", "routing_output", "routing_microphone") + ("routing_unrelated",)
                for sink in sink_names:
                    loaded = subprocess.run(["pactl", "--server", server, "load-module", "module-null-sink",
                        "sink_name=" + sink, "rate=48000", "channels=2"], env=environment,
                        capture_output=True, text=True, timeout=10, check=True)
                    reused_modules.append(loaded.stdout.strip())
            else:
                daemon_process = subprocess.Popen(arguments, env=daemon_environment, stdout=subprocess.DEVNULL, stderr=pulse_log)
                processes.append(daemon_process)
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    if daemon_process.poll() is not None:
                        raise RuntimeError("Owned private Pulse daemon exited")
                    result = subprocess.run(["pactl", "--server", server, "info"], env=environment, capture_output=True, timeout=3)
                    if result.returncode == 0:
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError("Owned private Pulse daemon unavailable")
            fixtures = (("selected", "Routing Fixture Selected", "routing_original", 440, 0.20),
                        ("unrelated", "Routing Fixture Unrelated", "routing_unrelated", 997, 0.30),
                        ("microphone", "Routing Fixture Microphone", "routing_microphone", 880, 0.50))
            for key, name, sink, frequency, gain in fixtures:
                path = directory / (key + ".f32le")
                tone_file(path, frequency, gain)
                stream = path.open("rb")
                inputs.append(stream)
                processes.append(subprocess.Popen(["pacat", "--server", server, "--playback", "--raw",
                    "--format=float32le", "--rate=48000", "--channels=2", "--latency-msec=30", "--device=" + sink,
                    "--client-name=" + name, "--stream-name=" + name, "--property=application.name=" + name],
                    env=environment, stdin=stream, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                result = subprocess.run(["pactl", "--server", server, "--format=json", "list", "sink-inputs"],
                    env=environment, capture_output=True, text=True, timeout=3, check=True)
                indexed = {row["properties"].get("application.name"): row for row in json.loads(result.stdout)}
                if all(name in indexed for _, name, _, _, _ in fixtures):
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError("Synthetic applications were not visible to private Pulse")
            selected = indexed["Routing Fixture Selected"]
            baseline_sinks = {name: row["sink"] for name, row in indexed.items() if name.startswith("Routing Fixture ")}
            environment.update(RADIOS_TEST_ROUTING_APPLICATION=str(selected["index"]),
                RADIOS_TEST_ROUTING_ORIGINAL_SINK=str(selected["sink"]), RADIOS_TEST_ROUTING_PROOF_DIR=str(directory))
            (directory / "screenshots").mkdir(mode=0o700)
            environment["RADIOS_ROUTING_GRAPH_CAPTURE_DIR"] = str(directory / "screenshots")
            # Fresh null sinks initially emit silence while their playback
            # clocks prime. Start the native acceptance with established PCM
            # producers, as run_audio_routing does; keep the WAV sample window
            # and its signal/isolation thresholds unchanged.
            time.sleep(2.3)
            with (directory / "native.log").open("wb") as log:
                result = subprocess.run([str(executable), str(root), "-p", report["expected_case"]], cwd=root, env=environment,
                    stdout=log, stderr=subprocess.STDOUT, timeout=100, check=False)
            if result.returncode == 77:
                report["skipped"] = True
                report["error"] = "X11/XTest native input unavailable"
                return 77
            if result.returncode:
                raise RuntimeError(f"Native routing GTK acceptance exited {result.returncode}: " +
                                   (directory / "native.log").read_text(errors="replace")[-10000:])
            native_log = (directory / "native.log").read_text(errors="replace")
            matched = re.findall(r"^ok [0-9]+ (/native-routing-graph/[^\r\n]+)$", native_log, re.MULTILINE)
            if matched != [report["expected_case"]] or "# SKIP" in native_log or "1..0" in native_log:
                raise RuntimeError("Native graph acceptance did not execute its one required case: " + native_log[-10000:])
            recording_root = root / configuration["paths"]["recording_root"]
            expected = ("graph-ui-app", "graph-ui-mix")
            groups = {prefix: sorted(recording_root.glob(prefix + "-*.wav")) for prefix in expected}
            if any(len(files) != 1 for files in groups.values()):
                raise RuntimeError("Native graph did not finalize one recording for each configured prefix")
            files = [groups[prefix][0] for prefix in expected]
            if any(not path.is_file() for path in files) or len(list(recording_root.glob("*.wav"))) != 2:
                raise RuntimeError("Native graph did not finalize exactly app-only and app+microphone WAV files")
            statistics = {path.name: tone_amplitudes(path) for path in files}
            for name, measured in statistics.items():
                amplitudes = measured["tone_amplitudes"]
                microphone = amplitudes["880"]
                if amplitudes["440"] < 0.05 or amplitudes["997"] > 0.02 or (
                        microphone < 0.05 if name.startswith("graph-ui-mix-") else microphone > 0.02):
                    raise RuntimeError("Graph edges did not produce isolated actual WAV PCM: " + str(statistics))
                if measured["frames"] < 96000:
                    raise RuntimeError("Graph WAV did not include two seconds of actual PCM")
            final_inputs = subprocess.run(["pactl", "--server", server, "--format=json", "list", "sink-inputs"],
                env=environment, capture_output=True, text=True, timeout=3, check=True)
            restored_sinks = {row["properties"].get("application.name"): row["sink"]
                for row in json.loads(final_inputs.stdout) if row["properties"].get("application.name", "").startswith("Routing Fixture ")}
            if restored_sinks != baseline_sinks:
                raise RuntimeError("Graph recording changed/restored the wrong private application route: " + str(restored_sinks))
            report["private_application_routes_restored"] = True
            report.update(passed=True, elapsed_seconds=time.monotonic() - started,
                independent_wav=statistics, screenshots=[str(path.relative_to(directory))
                    for path in sorted((directory / "screenshots").glob("*.png"))], native_log=(directory / "native.log").read_text(errors="replace"))
            print(json.dumps(report, ensure_ascii=False, sort_keys=True))
    except Exception as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
        return 1
    finally:
        for module in reversed(reused_modules):
            with contextlib.suppress(OSError, subprocess.SubprocessError):
                subprocess.run(["pactl", "--server", server, "unload-module", module], env=environment,
                    capture_output=True, text=True, timeout=10, check=False)
        for process in reversed(processes):
            terminate(process)
        for source in inputs:
            source.close()
        if command:
            with contextlib.suppress(OSError, subprocess.SubprocessError):
                subprocess.run(command + ["stop"], cwd=root, env=environment,
                    capture_output=True, text=True, timeout=15, check=False)
        os.close(directory_fd)
        with contextlib.suppress(OSError):
            (directory / "acceptance.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        if temporary:
            temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
