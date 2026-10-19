#!/usr/bin/env python3
"""Real elapsed audio acceptance using only an owned private Pulse daemon.

The independent monitor capture and HTTP receiver check what left the native
engine. This proves local PCM/software delivery, not acoustic speaker output
or delivery to an external Icecast listener.
"""
from __future__ import annotations

import argparse
import array
import contextlib
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import wave

PROJECT = Path(__file__).resolve().parents[1]


class Receiver:
    def __init__(self, path: Path):
        self.path = path
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(1)
        self.port = self.listener.getsockname()[1]
        self.stopping = threading.Event()
        self.finished = threading.Event()
        self.error: str | None = None
        self.bytes = 0
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self) -> None:
        try:
            while not self.stopping.is_set():
                try:
                    connection, _ = self.listener.accept()
                    break
                except socket.timeout:
                    continue
            else:
                return
            with connection, self.path.open("wb") as output:
                connection.settimeout(15)
                headers = bytearray()
                while not headers.endswith(b"\r\n\r\n"):
                    data = connection.recv(1)
                    if not data or len(headers) > 32768:
                        raise ValueError("Invalid native source upload headers")
                    headers.extend(data)
                if not headers.startswith(b"PUT /programme.mp3 HTTP/1.1\r\n"):
                    raise ValueError("Native output did not use source PUT")
                if b"content-type: audio/mpeg\r\n" not in headers.lower():
                    raise ValueError("Native output was not MP3")
                if b"transfer-encoding: chunked" in headers.lower():
                    raise ValueError("Chunked HTTP would contaminate the source MP3")
                # Just as Icecast does, confirm source admission before EOF.
                connection.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n")
                while True:
                    data = connection.recv(65536)
                    if not data:
                        break
                    output.write(data)
                    self.bytes += len(data)
        except Exception as error:
            self.error = str(error)
        finally:
            self.finished.set()

    def close(self) -> None:
        self.stopping.set()
        self.listener.close()
        self.thread.join(timeout=3)


def pcm_statistics(path: Path, *, offset: int = 0) -> dict:
    """Bounded streaming statistics over PCM16 stereo, including quiet gaps."""
    frames = nonzero = square_sum = longest_quiet = quiet_run = windows = 0
    energy: list[float] = []
    with path.open("rb") as source:
        source.seek(offset)
        while block := source.read(48000 * 4 // 2):  # exactly 500 ms
            if len(block) % 4:
                raise ValueError("Partial PCM stereo frame")
            samples = array.array("h")
            samples.frombytes(block)
            if sys.byteorder != "little":
                samples.byteswap()
            power = sum(sample * sample for sample in samples)
            square_sum += power
            frames += len(samples) // 2
            nonzero += sum(sample != 0 for sample in samples)
            energy.append(math.sqrt(power / max(len(samples), 1)) / 32768)
            windows += 1
    # Restrict gap accounting to first/last measured programme signal. The
    # deliberately stopped runner stays silent while network Stop drains.
    energetic = [index for index, rms in enumerate(energy) if rms >= 0.0001]
    first = energetic[0] if energetic else 0
    last = energetic[-1] if energetic else -1
    for rms in energy[first:last + 1]:
        quiet_run = quiet_run + 1 if rms < 0.0001 else 0
        longest_quiet = max(longest_quiet, quiet_run)
    return {"frames": frames, "duration_seconds": frames / 48000,
            "nonzero_samples": nonzero, "rms": math.sqrt(square_sum / max(frames * 2, 1)) / 32768,
            "longest_interior_quiet_ms": longest_quiet * 500, "windows": windows,
            "first_signal_ms": first * 500, "last_signal_end_ms": (last + 1) * 500,
            "measured_signal_span_seconds": max(0, last + 1 - first) * 0.5}


def terminate(process: subprocess.Popen | None) -> None:
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--seconds", type=int, default=8)
    parser.add_argument("--report-dir", type=Path)
    args = parser.parse_args()
    if not 8 <= args.seconds <= 600:
        parser.error("--seconds must be 8..600; final acceptance uses 300")
    executable = args.executable.resolve(strict=True)
    daemon = os.environ.get("RADIOS_TEST_PULSEAUDIO_BIN") or shutil.which("pulseaudio")
    candidate = PROJECT / "build/stage13-pulse/pulseaudio"
    if not daemon and candidate.is_file():
        daemon = str(candidate)
    required = ("pactl", "parec", "ffmpeg", "ffprobe")
    if not daemon or any(not shutil.which(command) for command in required):
        print("Isolated real-time soak needs Pulse daemon, pactl, parec, ffmpeg and ffprobe.", file=sys.stderr)
        return 77
    root = args.report_dir.resolve() if args.report_dir else Path(tempfile.mkdtemp(prefix="audio-soak-", dir=PROJECT / "build"))
    if args.report_dir:
        root.mkdir(mode=0o700, parents=True, exist_ok=False)
    os.chmod(root, 0o700)
    daemon_process = capture_process = test_process = None
    receiver = None
    directory_fd = None
    started = time.monotonic()
    report: dict = {"passed": False, "requested_seconds": args.seconds,
                    "utc_started": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    "scope": "owned private Pulse clock, MAIN monitor, local WAV and local HTTP MP3 receiver",
                    "physical_acoustic_acceptance": "operator-required", "host_audio_changed": False}
    try:
        (root / "runtime").mkdir(mode=0o700)
        (root / "state").mkdir(mode=0o700)
        directory_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
        native = f"/proc/{os.getpid()}/fd/{directory_fd}/native"
        server = "unix:" + native
        environment = os.environ.copy()
        environment.update(PULSE_SERVER=server, RADIOS_TEST_PULSE_SERVER=server,
                           PULSE_RUNTIME_PATH=str(root / "runtime"), PULSE_STATE_PATH=str(root / "state"))
        daemon_environment = environment | {"DBUS_SESSION_BUS_ADDRESS": f"unix:path=/proc/{os.getpid()}/fd/{directory_fd}/no-desktop-bus"}
        config = root / "private.pa"
        config.write_text(f'load-module module-native-protocol-unix socket="{native}" auth-anonymous=1\n'
                          "load-module module-null-sink sink_name=radios_soak_main rate=48000 channels=2\n"
                          "load-module module-null-sink sink_name=radios_soak_cue rate=48000 channels=2\n")
        with (root / "pulse.log").open("wb") as pulse_log, (root / "monitor.pcm").open("wb") as monitor, (root / "engine.log").open("wb") as engine_log:
            daemon_process = subprocess.Popen([daemon, "--daemonize=no", "--exit-idle-time=-1", "--use-pid-file=no",
                                               "--disable-shm=yes", "--high-priority=no", "--realtime=no",
                                               "--log-target=stderr", "-n", "--file=" + str(config)],
                                              env=daemon_environment, stdout=subprocess.DEVNULL, stderr=pulse_log)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if daemon_process.poll() is not None:
                    raise RuntimeError("Owned private Pulse daemon exited")
                status = subprocess.run(["pactl", "--server", server, "info"], env=environment, capture_output=True, timeout=3)
                if status.returncode == 0:
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError("Owned private Pulse daemon unavailable")
            capture_process = subprocess.Popen(["parec", "--server", server, "--device=radios_soak_main.monitor",
                                                "--raw", "--format=s16le", "--rate=48000", "--channels=2", "--latency-msec=20"],
                                               env=environment, stdout=monitor, stderr=subprocess.DEVNULL)
            receiver = Receiver(root / "received.mp3")
            environment["RADIOS_TEST_STREAM_URL"] = f"http://127.0.0.1:{receiver.port}/programme.mp3"
            test_process = subprocess.Popen([str(executable), str(root), str(args.seconds)], env=environment,
                                            stdout=engine_log, stderr=subprocess.STDOUT)
            result = test_process.wait(timeout=args.seconds + 55)
            terminate(capture_process)
            if result:
                raise RuntimeError(f"Native soak exited {result}: " + (root / "engine.log").read_text(errors="replace")[-5000:])
            if not receiver.finished.wait(timeout=5) or receiver.error:
                raise RuntimeError("Independent MP3 receiver: " + str(receiver.error or "EOF not observed"))
        proof = json.loads((root / "engine-proof.json").read_text())
        with wave.open(str(root / "programme.wav"), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (2, 2, 48000):
                raise ValueError("Unexpected finalized WAV format")
            if wav.getnframes() != proof["record_frames"]:
                raise ValueError("Finalized RIFF count differs from native output receipt")
        wav_stats = pcm_statistics(root / "programme.wav", offset=44)
        monitor_stats = pcm_statistics(root / "monitor.pcm")
        if wav_stats["rms"] < 0.01 or monitor_stats["rms"] < 0.01:
            raise ValueError("Independent captured programme has no measured signal")
        if wav_stats["longest_interior_quiet_ms"] or monitor_stats["longest_interior_quiet_ms"]:
            raise ValueError("Sustained programme contains an interior 500 ms quiet gap")
        if monitor_stats["measured_signal_span_seconds"] < args.seconds * 0.9:
            raise ValueError("Independent monitor did not consume the real requested duration")
        if receiver.bytes != proof["stream_bytes"]:
            raise ValueError("Independent receiver MP3 byte count differs from native submitted bytes")
        probe = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_name,sample_rate,channels", "-of", "json", str(root / "received.mp3")],
                               capture_output=True, text=True, timeout=10, check=True)
        stream_info = json.loads(probe.stdout)["streams"][0]
        if stream_info != {"codec_name": "mp3", "sample_rate": "48000", "channels": 2}:
            raise ValueError("Independent decoder did not identify stereo 48 kHz MP3")
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(root / "received.mp3"), "-f", "s16le", "-acodec", "pcm_s16le", "-ar", "48000", "-ac", "2", str(root / "decoded-stream.pcm")],
                       capture_output=True, timeout=30, check=True)
        decoded_stats = pcm_statistics(root / "decoded-stream.pcm")
        if decoded_stats["rms"] < 0.01 or decoded_stats["duration_seconds"] < args.seconds * 0.9:
            raise ValueError("Independent MP3 decode lost sustained programme")
        if decoded_stats["longest_interior_quiet_ms"]:
            raise ValueError("MP3 programme contains an interior 500 ms quiet gap")
        report.update(passed=True, engine=proof, independent_wav=wav_stats, independent_main_monitor=monitor_stats,
                      independent_received_mp3={"bytes": receiver.bytes, "sha256": hashlib.sha256((root / "received.mp3").read_bytes()).hexdigest(),
                                                 "codec": stream_info, "decoded": decoded_stats},
                      pulse_log_underrun_mentions=(root / "pulse.log").read_text(errors="replace").lower().count("underrun"))
        print(f"Real audio soak passed: {proof['elapsed_us'] / 1e6:.3f}s, {proof['transitions']} transitions, {proof['append_calls']} appends, "
              f"{proof['completed_receipts']} EOF receipts, {receiver.bytes} independently received MP3 bytes.")
        return 0
    except Exception as error:
        report["error"] = str(error)
        print(str(error), file=sys.stderr)
        return 1
    finally:
        terminate(test_process)
        terminate(capture_process)
        if receiver:
            receiver.close()
        terminate(daemon_process)
        if directory_fd is not None:
            os.close(directory_fd)
        report["wall_elapsed_seconds"] = round(time.monotonic() - started, 3)
        report["owned_daemon_stopped"] = bool(daemon_process and daemon_process.poll() is not None)
        (root / "acceptance.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        if not args.report_dir and report["passed"]:
            shutil.rmtree(root)
        else:
            print("Audio proof: " + str(root / "acceptance.json"))


if __name__ == "__main__":
    def interrupted(_signum, _frame):
        raise InterruptedError("Audio soak interrupted; owned processes are being stopped")
    signal.signal(signal.SIGTERM, interrupted)
    raise SystemExit(main())
