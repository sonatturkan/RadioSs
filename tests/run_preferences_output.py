#!/usr/bin/env python3
"""Accept output formats using an owned null sink and independently captured PCM."""
from __future__ import annotations

import array
import contextlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid


def pactl(*arguments: str) -> str:
    return subprocess.run(["pactl", *arguments], check=True, capture_output=True,
                          text=True, timeout=10).stdout.strip()


def amplitude(samples: array.array, channel: int, frequency: float) -> float:
    frames = len(samples) // 2
    first, last = 100, frames - 100
    real = imaginary = 0.0
    for frame in range(first, last):
        phase = 2 * math.pi * frequency * frame / 44100
        value = samples[frame * 2 + channel]
        real += value * math.cos(phase)
        imaginary += value * math.sin(phase)
    return 2 * math.hypot(real, imaginary) / (last - first)


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: run_preferences_output.py /path/to/test_preferences_output", file=sys.stderr)
        return 2
    if any(shutil.which(command) is None for command in ("pactl", "parec")):
        print("Output format acceptance requires pactl and parec.", file=sys.stderr)
        return 77
    module = None
    capture_process = None
    try:
        pactl("info")
        executable = Path(sys.argv[1]).resolve(strict=True)
        with tempfile.TemporaryDirectory(prefix="radios-output-format-") as temporary:
            root = Path(temporary)
            capture, proof = root / "actual-44100.f32", root / "proof.json"
            sink = "radios_output_accept_" + uuid.uuid4().hex
            module = pactl("load-module", "module-null-sink", "sink_name=" + sink,
                           "rate=44100", "channels=2", "channel_map=front-left,front-right")
            if not module.isdecimal():
                raise RuntimeError("Owned virtual output identity is invalid")
            environment = os.environ.copy()
            environment["G_DEBUG"] = "fatal-warnings"
            environment["RADIOS_TEST_OUTPUT_SINK"] = sink
            environment["RADIOS_TEST_OUTPUT_CAPTURE"] = str(capture)
            environment["RADIOS_TEST_OUTPUT_PROOF"] = str(proof)
            with capture.open("wb") as stream:
                capture_process = subprocess.Popen(["parec", "--raw", "--format=float32le", "--rate=44100",
                    "--channels=2", "--latency-msec=10", "--device=" + sink + ".monitor"],
                    stdout=stream, stderr=subprocess.PIPE, env=environment)
                time.sleep(.15)
                result = subprocess.run([str(executable)], env=environment, timeout=45, check=False)
                if result.returncode:
                    return result.returncode
                if capture_process.poll() is not None:
                    raise RuntimeError("Independent monitor capture exited unexpectedly")
                capture_process.terminate()
                capture_process.wait(timeout=5)
            summary = json.loads(proof.read_text(encoding="utf-8"))
            raw = capture.read_bytes()
            first, last = summary["first_frame"], summary["last_frame"]
            pcm = array.array("f")
            pcm.frombytes(raw[first * 8:last * 8])
            if sys.byteorder != "little":
                pcm.byteswap()
            energetic = [i for i in range(len(pcm) // 2) if abs(pcm[i * 2]) > .01]
            if not energetic:
                raise RuntimeError("Actual selected output contains no measured signal")
            first_signal, last_signal = energetic[0], energetic[-1] + 1
            signal = pcm[first_signal * 2:last_signal * 2]
            duration = (last_signal - first_signal) / 44100
            left = amplitude(signal, 0, 1000)
            right = amplitude(signal, 1, 1500)
            if not .97 <= duration <= 1.03:
                raise RuntimeError(f"48 kHz source duration changed at selected 44100 Hz output: {duration}")
            if not .24 <= left <= .26 or not .115 <= right <= .135:
                raise RuntimeError(f"Independent PCM pitch/level mismatch: left={left} right={right}")
            summary.update({"capture_rate": 44100, "captured_signal_seconds": duration,
                "left_1000_hz_amplitude": left, "right_1500_hz_amplitude": right,
                "owned_virtual_output": True, "database_accessed": False,
                "physical_microphone_captured": False, "host_defaults_changed": False})
            print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
            return 0
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"Output format acceptance failed: {error}", file=sys.stderr)
        return 1
    finally:
        if capture_process is not None and capture_process.poll() is None:
            capture_process.terminate()
            with contextlib.suppress(subprocess.TimeoutExpired):
                capture_process.wait(timeout=5)
            if capture_process.poll() is None:
                capture_process.kill()
                capture_process.wait(timeout=5)
        if capture_process is not None and capture_process.stderr is not None:
            capture_process.stderr.close()
        if module is not None and module.isdecimal():
            with contextlib.suppress(OSError, subprocess.SubprocessError):
                subprocess.run(["pactl", "unload-module", module], capture_output=True, timeout=10, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
