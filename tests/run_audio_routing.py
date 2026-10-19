#!/usr/bin/env python3
"""Accept native routing with owned virtual Pulse sources; never touch host audio."""
from __future__ import annotations

import argparse
import array
import base64
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import wave

PROJECT = Path(__file__).resolve().parents[1]


def terminate(process: subprocess.Popen | None) -> None:
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def tone_file(path: Path, frequency: int, gain: float) -> None:
    samples = array.array("f", (gain * math.sin(2 * math.pi * frequency * frame / 48000)
                               for frame in range(48000) for _ in range(2)))
    if sys.byteorder != "little":
        samples.byteswap()
    with path.open("wb") as output:
        for _ in range(90):
            output.write(samples.tobytes())


def tone_amplitudes(path: Path) -> dict:
    with wave.open(str(path), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (2, 2, 48000):
            raise ValueError("Routing recorder did not finalize stereo PCM16 48 kHz WAV")
        frames = source.getnframes()
        if frames < 4800:
            raise ValueError("Routing recorder has fewer than 100 ms of actual frames")
        samples = array.array("h", source.readframes(frames))
    if sys.byteorder != "little":
        samples.byteswap()
    whole_left = samples[::2]
    energies = [math.sqrt(sum((value / 32768) ** 2 for value in whole_left[index:index + 240]) /
                          len(whole_left[index:index + 240]))
                for index in range(0, len(whole_left), 240)]
    energetic = [index for index, rms in enumerate(energies) if rms > 0.001]
    quiet_run = longest_quiet = 0
    if energetic:
        for rms in energies[energetic[0]:energetic[-1] + 1]:
            quiet_run = quiet_run + 1 if rms <= 0.001 else 0
            longest_quiet = max(longest_quiet, quiet_run)
    # Sample an interior one-second window, or all available after settling.
    # The full WAV independently supplies interior gap accounting above.
    offset = min(24000, max(0, frames - 48000))
    left = whole_left[offset:offset + min(48000, frames - offset)]
    amplitudes = {}
    for frequency in (440, 880, 997):
        real = sum(value / 32768 * math.cos(2 * math.pi * frequency * index / 48000)
                   for index, value in enumerate(left))
        imaginary = sum(value / 32768 * math.sin(2 * math.pi * frequency * index / 48000)
                        for index, value in enumerate(left))
        amplitudes[str(frequency)] = 2 * math.hypot(real, imaginary) / len(left)
    return {"frames": frames, "bytes": path.stat().st_size, "tone_amplitudes": amplitudes,
            "rms": math.sqrt(sum((value / 32768) ** 2 for value in left) / len(left)),
            "longest_interior_quiet_ms": longest_quiet * 5}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--report-dir", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    daemon = os.environ.get("RADIOS_TEST_PULSEAUDIO_BIN") or shutil.which("pulseaudio")
    candidate = PROJECT / "build/stage13-pulse/pulseaudio"
    if not daemon and candidate.is_file():
        daemon = str(candidate)
    if not daemon or any(not shutil.which(command) for command in ("pactl", "pacat", "ffmpeg")):
        print("Private routing acceptance requires Pulse daemon, pactl, pacat and ffmpeg.", file=sys.stderr)
        return 77
    temporary = None
    if args.report_dir:
        root = args.report_dir.resolve()
        root.mkdir(mode=0o700, parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="audio-routing-", dir=PROJECT / "build")
        root = Path(temporary.name)
    os.chmod(root, 0o700)
    environment = os.environ.copy()
    processes: list[subprocess.Popen] = []
    inputs = []
    directory_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    server = f"unix:/proc/{os.getpid()}/fd/{directory_fd}/native"
    report = {"passed": False, "scope": "isolated selected application to actual MAIN, WAV and Icecast MP3; virtual microphone recorder",
              "host_audio_changed": False, "physical_microphone_captured": False,
              "fixture_playback_warmup_seconds": 2.3,
              "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}
    started = time.monotonic()
    capture_finished = threading.Event()
    capture_error: list[str] = []
    authorization = "Basic " + base64.b64encode(b"source:routing-fixture-secret").decode()

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_arguments):
            pass

        def do_PUT(self):
            try:
                if (self.path != "/player.mp3" or self.headers.get("Authorization") != authorization or
                        self.headers.get("Content-Type") != "audio/mpeg" or
                        self.headers.get("Transfer-Encoding") or self.headers.get("Content-Length")):
                    raise ValueError("MAIN stream did not send an authenticated raw MP3 source upload")
                self.send_response(200)
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.flush()
                self.connection.settimeout(15)
                with (root / "main-player.mp3").open("wb") as output:
                    while payload := self.rfile.read1(65536):
                        output.write(payload)
            except Exception as error:
                capture_error.append(str(error))
            finally:
                capture_finished.set()
                self.close_connection = True

    http = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    http.daemon_threads = True
    http_thread = threading.Thread(target=http.serve_forever, daemon=True)
    http_thread.start()
    environment["RADIOS_TEST_ROUTING_STREAM_URL"] = f"http://127.0.0.1:{http.server_port}/player.mp3"
    try:
        (root / "runtime").mkdir(mode=0o700)
        (root / "state").mkdir(mode=0o700)
        environment.update(PULSE_SERVER=server, RADIOS_TEST_PULSE_SERVER=server,
                           PULSE_RUNTIME_PATH=str(root / "runtime"), PULSE_STATE_PATH=str(root / "state"))
        daemon_environment = environment | {"DBUS_SESSION_BUS_ADDRESS": f"unix:path=/proc/{os.getpid()}/fd/{directory_fd}/no-desktop-bus"}
        config = root / "private.pa"
        config.write_text(f'load-module module-native-protocol-unix socket="{server[5:]}" auth-anonymous=1\n'
                          "load-module module-null-sink sink_name=routing_original rate=48000 channels=2\n"
                          "load-module module-null-sink sink_name=routing_output rate=48000 channels=2\n"
                          "load-module module-null-sink sink_name=routing_microphone rate=48000 channels=2\n"
                          "set-default-sink routing_original\n"
                          "set-default-source routing_microphone.monitor\n", encoding="utf-8")
        with (root / "pulse.log").open("wb") as pulse_log:
            arguments = [daemon, "--daemonize=no", "--exit-idle-time=-1", "--use-pid-file=no",
                         "--disable-shm=yes", "--high-priority=no", "--realtime=no", "--log-target=stderr", "-n", "--file=" + str(config)]
            module_path = os.environ.get("RADIOS_TEST_PULSE_MODULE_PATH")
            candidate_modules = PROJECT / "build/stage13-pulse/usr/lib/pulse-17.0/modules"
            if not module_path and candidate_modules.is_dir():
                module_path = str(candidate_modules)
            if module_path:
                arguments.append("--dl-search-path=" + module_path)
            daemon_process = subprocess.Popen(arguments, env=daemon_environment,
                                              stdout=subprocess.DEVNULL, stderr=pulse_log)
            processes.append(daemon_process)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if daemon_process.poll() is not None:
                    raise RuntimeError("Owned private Pulse daemon exited")
                ready = subprocess.run(["pactl", "--server", server, "info"], env=environment,
                                       capture_output=True, timeout=3)
                if ready.returncode == 0:
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError("Owned private Pulse daemon unavailable")
            fixtures = (("selected", "Routing Fixture Selected", "routing_original", 440, 0.20),
                        # This unselected stream already occupies the requested
                        # output, so recording its whole monitor would leak it.
                        ("unrelated", "Routing Fixture Unrelated", "routing_output", 997, 0.30),
                        ("microphone", "Routing Fixture Microphone", "routing_microphone", 880, 0.50))
            for key, name, sink, frequency, gain in fixtures:
                path = root / (key + ".f32le")
                tone_file(path, frequency, gain)
                source = path.open("rb")
                inputs.append(source)
                processes.append(subprocess.Popen(["pacat", "--server", server, "--playback", "--raw",
                    "--format=float32le", "--rate=48000", "--channels=2", "--latency-msec=30",
                    "--device=" + sink, "--client-name=" + name, "--stream-name=" + name,
                    "--property=application.name=" + name], env=environment, stdin=source,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            deadline = time.monotonic() + 10
            indexed = {}
            while time.monotonic() < deadline:
                result = subprocess.run(["pactl", "--server", server, "--format=json", "list", "sink-inputs"],
                                        env=environment, capture_output=True, text=True, timeout=3, check=True)
                indexed = {item["properties"].get("application.name"): item for item in json.loads(result.stdout)}
                if all(name in indexed for _, name, _, _, _ in fixtures):
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError("Synthetic applications were not visible to Pulse")
            selected = indexed["Routing Fixture Selected"]
            environment.update(RADIOS_TEST_ROUTING_APPLICATION=str(selected["index"]),
                               RADIOS_TEST_ROUTING_ORIGINAL_SINK=str(selected["sink"]),
                               RADIOS_TEST_ROUTING_UNRELATED=str(indexed["Routing Fixture Unrelated"]["index"]))
            # Newly created null sinks initially prime their Pulse playback
            # clock. Start acceptance against established playing applications
            # and a producing virtual mic, as opposed to daemon startup silence.
            time.sleep(2.3)
            # Everything after this point is native C routing against this private server.
            with (root / "native.log").open("wb") as log:
                test_process = subprocess.Popen([str(executable), str(root)], env=environment,
                                                stdout=log, stderr=subprocess.STDOUT)
                processes.append(test_process)
                deadline = time.monotonic() + 95
                handled: set[str] = set()
                while test_process.poll() is None and time.monotonic() < deadline:
                    if (root / "stop-server").is_file() and "stop-server" not in handled:
                        terminate(daemon_process)
                        handled.add("stop-server")
                        (root / "server-stopped").write_text("owned daemon stopped\n", encoding="utf-8")
                    if (root / "restart-server").is_file() and "restart-server" not in handled:
                        daemon_process = subprocess.Popen(arguments, env=daemon_environment,
                                                          stdout=subprocess.DEVNULL, stderr=pulse_log)
                        processes.append(daemon_process)
                        ready_deadline = time.monotonic() + 10
                        while time.monotonic() < ready_deadline:
                            ready = subprocess.run(["pactl", "--server", server, "info"], env=environment,
                                                   capture_output=True, timeout=3)
                            if ready.returncode == 0:
                                break
                            time.sleep(0.05)
                        else:
                            raise RuntimeError("Restarted private Pulse daemon unavailable")
                        handled.add("restart-server")
                        (root / "server-restarted").write_text("owned daemon ready\n", encoding="utf-8")
                    time.sleep(0.01)
                if test_process.poll() is None:
                    raise RuntimeError("Native routing acceptance exceeded 95 seconds")
                code = test_process.returncode
            if code:
                raise RuntimeError(f"Native routing exited {code}: " + (root / "native.log").read_text(errors="replace")[-10000:])
            proof = json.loads((root / "engine-proof.json").read_text(encoding="utf-8"))
            main_proof = json.loads((root / "main-proof.json").read_text(encoding="utf-8"))
            if not main_proof.get("passed") or not capture_finished.wait(5) or capture_error:
                raise RuntimeError("MAIN source upload was not independently received: " + str(capture_error))
            subprocess.run(["ffmpeg", "-v", "error", "-i", str(root / "main-player.mp3"),
                            "-ar", "48000", "-ac", "2", "-c:a", "pcm_s16le", str(root / "main-stream-decoded.wav")],
                           env=environment, capture_output=True, timeout=20, check=True)
            main_statistics = {name: tone_amplitudes(root / name) for name in
                               ("main-player.wav", "main-player-quiet.wav", "main-stream-decoded.wav")}
            for name, statistic in main_statistics.items():
                measured = statistic["tone_amplitudes"]
                if measured["440"] < 0.07 or measured["880"] > 0.005 or measured["997"] > 0.005:
                    raise RuntimeError(f"MAIN route did not exclusively carry the selected player in {name}: {measured}")
            ratio = (main_statistics["main-player-quiet.wav"]["tone_amplitudes"]["440"] /
                     main_statistics["main-player.wav"]["tone_amplitudes"]["440"])
            if not 0.45 < ratio < 0.55:
                raise RuntimeError(f"Live external MAIN gain did not halve captured PCM: {ratio}")
            report.update(main_proof=main_proof, main_audio=main_statistics, main_gain_ratio=ratio)
            files = sorted(root.glob("route-*.wav"))
            if len(files) < 3:
                raise RuntimeError("Native test did not leave split and compressor comparison WAV files")
            statistics = {path.name: tone_amplitudes(path) for path in files}
            for key in ("route-mix-1.wav", "route-mix-2.wav"):
                measured = statistics[key]["tone_amplitudes"]
                if measured["440"] < 0.08 or measured["880"] < 0.04 or measured["997"] > 0.02:
                    raise ValueError("Recorder did not independently isolate selected application and microphone: " + str(measured))
                if statistics[key]["longest_interior_quiet_ms"] > 50:
                    raise ValueError("Producing application and microphone recording contains an interior gap")
            compressed = statistics["route-compressor.wav"]
            reference = statistics["route-mix-2.wav"]["tone_amplitudes"]["880"]
            if not (0.01 < compressed["tone_amplitudes"]["880"] < reference * 0.85):
                raise ValueError("Independent PCM did not confirm finite microphone compression")
            if compressed["tone_amplitudes"]["440"] > 0.01 or compressed["tone_amplitudes"]["997"] > 0.01:
                raise ValueError("Microphone-only recording included an unselected application")
            report.update(passed=True, elapsed_seconds=time.monotonic() - started,
                          native=proof, independent_wav=statistics)
            print(json.dumps(report, ensure_ascii=False, sort_keys=True))
    except Exception as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
        if (root / "pulse.log").is_file():
            print((root / "pulse.log").read_text(errors="replace")[-4000:], file=sys.stderr)
        return 1
    finally:
        http.shutdown()
        http.server_close()
        http_thread.join(timeout=3)
        for process in reversed(processes):
            terminate(process)
        for source in inputs:
            source.close()
        os.close(directory_fd)
        with contextlib.suppress(Exception):
            (root / "acceptance.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        if temporary:
            temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
