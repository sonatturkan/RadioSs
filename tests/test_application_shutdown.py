#!/usr/bin/env python3
"""Real application signals and WAV finalization with an owned private service/audio.

The executable links the deployed main.c with a test-only window-present wrapper
that operates the real routing controls. No production data or host audio is used.
"""
from __future__ import annotations

import argparse
import configparser
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

from run_audio_routing import PROJECT, terminate, tone_amplitudes, tone_file


def lock_held(root: Path) -> bool:
    path = root / "data/database-service/gui.lock"
    if not path.is_file():
        return False
    with path.open("rb") as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        fcntl.flock(stream, fcntl.LOCK_UN)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    daemon = os.environ.get("RADIOS_TEST_PULSEAUDIO_BIN") or shutil.which("pulseaudio")
    candidate = PROJECT / "build/stage13-pulse/pulseaudio"
    if not daemon and candidate.is_file():
        daemon = str(candidate)
    if not daemon or any(not shutil.which(command) for command in ("pactl", "pacat")):
        print("Application shutdown acceptance requires owned Pulse daemon, pactl and pacat.", file=sys.stderr)
        return 77
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        print("Application shutdown acceptance requires an available graphical session.", file=sys.stderr)
        return 77
    report = {"passed": False, "checks": [], "production_data_used": False,
              "host_audio_changed": False, "physical_microphone_captured": False,
              "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="application-shutdown-", dir=PROJECT / "build") as temporary:
        directory = Path(temporary)
        root = directory / "RadioS Türkçe kapanış"
        root.mkdir(mode=0o700)
        environment = os.environ.copy()
        for name in ("RADIOS_SDBASE_TOKEN", "PYTHONPATH", "RADIOS_TEST_VIEW_TOKEN"):
            environment.pop(name, None)
        environment.update(PYTHONDONTWRITEBYTECODE="1", G_DEBUG="fatal-warnings")
        processes: list[subprocess.Popen] = []
        command = None
        tone_input = None
        directory_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        server = f"unix:/proc/{os.getpid()}/fd/{directory_fd}/native"
        credentials = {}

        def redact(value: str) -> str:
            for key in ("api_token", "endpoint_token"):
                if credentials.get(key):
                    value = value.replace(credentials[key], "[REDACTED]")
            return value

        def cli(action: str) -> dict:
            result = subprocess.run(command + [action], cwd=root, env=environment,
                                    capture_output=True, text=True, timeout=25)
            if result.returncode:
                raise RuntimeError(redact(f"Private database {action} failed: " + result.stderr[-1500:]))
            return json.loads(result.stdout)

        def pactl(*arguments: str) -> subprocess.CompletedProcess:
            return subprocess.run(["pactl", "--server", server, *arguments], env=environment,
                                  capture_output=True, text=True, timeout=3, check=True)

        def launch(tag: str, record: str | None) -> tuple[subprocess.Popen, dict]:
            ready = directory / (tag + ".json")
            selected = dict(environment, RADIOS_SHUTDOWN_READY=str(ready),
                            RADIOS_SHUTDOWN_RECORD=record or "")
            log_path = directory / (tag + ".log")
            with log_path.open("wb") as log:
                process = subprocess.Popen([str(executable), "--project-root", str(root), "--profile", "routing"],
                    cwd=directory, env=selected, stdout=log, stderr=subprocess.STDOUT)
            processes.append(process)
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(redact(f"Application exited {process.returncode} before readiness: " +
                        log_path.read_text(errors="replace")[-2500:]))
                if ready.is_file():
                    try:
                        state = json.loads(ready.read_text())
                    except json.JSONDecodeError:
                        time.sleep(.01)
                        continue
                    if not lock_held(root):
                        raise AssertionError("Normal application did not hold its recovery GUI lock")
                    return process, state
                time.sleep(.02)
            raise RuntimeError(redact("Real application/recording readiness timed out: " +
                log_path.read_text(errors="replace")[-2500:]))

        def stop_application(process: subprocess.Popen, requested: signal.Signals) -> None:
            process.send_signal(requested)
            code = process.wait(timeout=15)
            if code != 0:
                raise AssertionError(f"Normal application exited {code} for {requested.name}; expected graceful exit0")
            if lock_held(root):
                raise AssertionError("Normal application left GUI recovery lock held after shutdown")

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
            cli("init")
            credentials = json.loads((root / "config/credentials.json").read_text())
            cli("start")
            initial_status = cli("check")
            initial_pid = initial_status["pid"]
            initial_sequence = initial_status["sequence"]
            identity_hash = hashlib.sha256((root / "config/credentials.json").read_bytes()).hexdigest()
            (directory / "runtime").mkdir(mode=0o700)
            (directory / "state").mkdir(mode=0o700)
            environment.update(PULSE_SERVER=server, RADIOS_TEST_PULSE_SERVER=server,
                PULSE_RUNTIME_PATH=str(directory / "runtime"), PULSE_STATE_PATH=str(directory / "state"))
            daemon_environment = environment | {"DBUS_SESSION_BUS_ADDRESS":
                f"unix:path=/proc/{os.getpid()}/fd/{directory_fd}/no-desktop-bus"}
            pulse_config = directory / "private.pa"
            pulse_config.write_text(f'load-module module-native-protocol-unix socket="{server[5:]}" auth-anonymous=1\n'
                "load-module module-null-sink sink_name=routing_output rate=48000 channels=2\n"
                "load-module module-null-sink sink_name=routing_microphone rate=48000 channels=2\n"
                "set-default-sink routing_output\nset-default-source routing_microphone.monitor\n")
            pulse_arguments = [daemon, "--daemonize=no", "--exit-idle-time=-1", "--use-pid-file=no", "--disable-shm=yes",
                "--high-priority=no", "--realtime=no", "--log-target=stderr", "-n", "--file=" + str(pulse_config)]
            module_path = os.environ.get("RADIOS_TEST_PULSE_MODULE_PATH")
            modules = PROJECT / "build/stage13-pulse/usr/lib/pulse-17.0/modules"
            if not module_path and modules.is_dir():
                module_path = str(modules)
            if module_path:
                pulse_arguments.append("--dl-search-path=" + module_path)
            with (directory / "pulse.log").open("wb") as log:
                pulse = subprocess.Popen(pulse_arguments, env=daemon_environment,
                                         stdout=subprocess.DEVNULL, stderr=log)
            processes.append(pulse)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if pulse.poll() is not None:
                    raise RuntimeError("Owned private Pulse daemon exited")
                try:
                    pactl("info")
                    break
                except subprocess.CalledProcessError:
                    time.sleep(.05)
            else:
                raise RuntimeError("Owned private Pulse daemon unavailable")
            tone = directory / "microphone.f32le"
            tone_file(tone, 880, .50)
            tone_input = tone.open("rb")
            processes.append(subprocess.Popen(["pacat", "--server", server, "--playback", "--raw",
                "--format=float32le", "--rate=48000", "--channels=2", "--latency-msec=30",
                "--device=routing_microphone", "--client-name=Shutdown Synthetic Microphone"],
                env=environment, stdin=tone_input, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            for requested in (signal.SIGTERM, signal.SIGINT):
                tag = requested.name.lower()
                process, state = launch(tag + "-record", tag + ".wav")
                if not state["active"] or state["frames"] < 96000:
                    raise AssertionError("Readiness did not establish an actual active recorder")
                path = Path(state["path"])
                if path.parent != root / configuration["paths"]["recording_root"]:
                    raise AssertionError("Test recorder escaped its private project recording root")
                stop_application(process, requested)
                data = path.read_bytes()
                if (data[:4], data[8:12], data[36:40]) != (b"RIFF", b"WAVE", b"data"):
                    raise AssertionError("Signal shutdown did not leave the expected WAV structure")
                if struct.unpack_from("<I", data, 4)[0] != len(data) - 8 or struct.unpack_from("<I", data, 40)[0] != len(data) - 44:
                    raise AssertionError("Signal shutdown did not finalize RIFF/data length fields")
                if path.stat().st_mode & 0o777 != 0o600:
                    raise AssertionError("Signal-finalized recording is not private0600")
                measured = tone_amplitudes(path)
                if measured["frames"] < 96000 or measured["tone_amplitudes"]["880"] < .05:
                    raise AssertionError("Finalized WAV lacks independently measured real synthetic input")
                if json.loads(pactl("--format=json", "list", "source-outputs").stdout):
                    raise AssertionError("Signal shutdown left capture streams connected to private Pulse")
                check = cli("check")
                if check["pid"] != initial_pid or check["sequence"] != initial_sequence:
                    raise AssertionError("Application signal stopped/replaced its service or changed bootstrap data")
                expected_files = {p.name for p in path.parent.glob("*.wav")}
                recording_hash = hashlib.sha256(data).hexdigest()
                reopened, state = launch(tag + "-reopen", None)
                if state["active"] or state["frames"]:
                    raise AssertionError("Reopening after signal restarted microphone capture")
                stop_application(reopened, requested)
                if expected_files != {p.name for p in path.parent.glob("*.wav")} or recording_hash != hashlib.sha256(path.read_bytes()).hexdigest():
                    raise AssertionError("Safe reopen modified or created recording data")
                check = cli("check")
                if check["pid"] != initial_pid or check["integrity"] != "ok" or check["sequence"] != initial_sequence:
                    raise AssertionError("Private service did not remain healthy across shutdown/reopen")
                if hashlib.sha256((root / "config/credentials.json").read_bytes()).hexdigest() != identity_hash:
                    raise AssertionError("Signal shutdown/reopen changed installation identity")
                report["checks"].append({"signal": requested.name, "passed": True, "exit_code": 0,
                    "gui_lock_released": True, "service_continued": True, "reopened_without_capture": True,
                    "wav_finalized": True, "independent_wav": measured})
            report.update(passed=True, checks_total=len(report["checks"]),
                          elapsed_seconds=round(time.monotonic() - started, 3))
        except Exception as error:
            report["error"] = redact(str(error))
            print(report["error"], file=sys.stderr)
        finally:
            for process in reversed(processes):
                terminate(process)
            if tone_input:
                tone_input.close()
            if command:
                with contextlib.suppress(OSError, subprocess.SubprocessError):
                    subprocess.run(command + ["stop"], cwd=root, env=environment,
                                   capture_output=True, timeout=15)
            os.close(directory_fd)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, sort_keys=True))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
