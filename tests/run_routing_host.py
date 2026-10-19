#!/usr/bin/env python3
"""Probe native host Pulse compatibility with only owned silent virtual audio."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import time
import uuid

from run_audio_routing import terminate, tone_amplitudes, tone_file

PROJECT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--report-dir", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    if not shutil.which("pactl") or not shutil.which("pacat"):
        raise RuntimeError("Host compatibility probe requires pactl and pacat")
    environment = os.environ.copy()
    server = environment.get("PULSE_SERVER") or f"unix:/run/user/{os.getuid()}/pulse/native"

    def pulse(*arguments: str) -> str:
        return subprocess.run(["pactl", "--server", server, *arguments], env=environment,
                              capture_output=True, text=True, timeout=5, check=True).stdout.strip()

    def items(kind: str) -> list[dict]:
        # pactl 17's JSON formatter omits module IDs on this PipeWire host.
        # Its short module table retains the real Pulse IDs and exact arguments.
        if kind == "modules":
            result = []
            for line in pulse("list", "short", "modules").splitlines():
                columns = line.split("\t", 3)
                if len(columns) >= 2 and columns[0].isdigit():
                    result.append({"index": int(columns[0]), "name": columns[1],
                                   "argument": columns[2] if len(columns) >= 3 else ""})
            return result
        result = json.loads(pulse("--format=json", "list", kind))
        for item in result:
            for field in ("index", "client", "sink", "owner_module"):
                value = item.get(field)
                if isinstance(value, str) and value.isdigit():
                    item[field] = int(value)
        return result

    temporary = None
    if args.report_dir:
        root = args.report_dir.resolve()
        root.mkdir(mode=0o700, parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="routing-host-", dir=PROJECT / "build")
        root = Path(temporary.name)
    defaults = (pulse("get-default-sink"), pulse("get-default-source"))
    baseline_modules = {item["index"] for item in items("modules")}
    nonce = uuid.uuid4().hex
    names = [f"radios_host_probe_{nonce}_{role}" for role in ("source", "main")]
    label = "External Host Fixture " + nonce
    modules: dict[int, str] = {}
    private_modules: dict[int, str] = {}
    player = native = None
    source = None
    selected = None
    report = {"passed": False, "scope": "host Pulse/PipeWire compatibility with an owned silent virtual source and MAIN sink",
              "physical_microphone_captured": False, "user_apps_captured": False,
              "default_audio_devices_changed": False}
    def remember_private(sink: dict) -> None:
        if not sink["name"].startswith("radios_external_"):
            return
        for module in items("modules"):
            if (module["index"] not in baseline_modules and module["name"] == "module-null-sink" and
                    "sink_name=" + sink["name"] in shlex.split(module["argument"])):
                private_modules[module["index"]] = sink["name"]
    try:
        for name in names:
            modules[int(pulse("load-module", "module-null-sink", "sink_name=" + name, "rate=48000", "channels=2"))] = name
        original = next(item["index"] for item in items("sinks") if item["name"] == names[0])
        raw = root / "owned-player.f32le"
        tone_file(raw, 440, .2)
        source = raw.open("rb")
        player = subprocess.Popen(["pacat", "--server", server, "--playback", "--raw", "--format=float32le",
            "--rate=48000", "--channels=2", "--latency-msec=30", "--device=" + names[0],
            "--client-name=" + label, "--stream-name=" + label, "--property=application.name=" + label],
            env=environment, stdin=source, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            selected = next((item for item in items("sink-inputs") if item.get("properties", {}).get("application.name") == label), None)
            if selected:
                break
            time.sleep(.05)
        if not selected:
            raise RuntimeError("Owned virtual host source did not appear")
        environment.update(PULSE_SERVER=server, RADIOS_TEST_PULSE_SERVER=server,
                           RADIOS_TEST_ROUTING_APPLICATION=str(selected["index"]),
                           RADIOS_TEST_ROUTING_ORIGINAL_SINK=str(original), RADIOS_TEST_HOST_OUTPUT=names[1])
        with (root / "native.log").open("wb") as log:
            native = subprocess.Popen([str(executable), str(root), "host-compat"], env=environment,
                                      stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 55
            while native.poll() is None and time.monotonic() < deadline:
                current = next((item for item in items("sink-inputs") if item["index"] == selected["index"] and
                                item["client"] == selected["client"]), None)
                if current:
                    sink = next((item for item in items("sinks") if item["index"] == current["sink"]), None)
                    if sink:
                        remember_private(sink)
                time.sleep(.05)
            if native.poll() is None:
                raise RuntimeError("Native host compatibility probe exceeded 55 seconds")
        if native.returncode:
            raise RuntimeError("Native host compatibility probe failed: " + (root / "native.log").read_text(errors="replace")[-6000:])
        audio = tone_amplitudes(root / "host-main.wav")
        measured = audio["tone_amplitudes"]
        if measured["440"] < .1 or measured["880"] > .005 or measured["997"] > .005:
            raise RuntimeError("Owned host source did not reach actual MAIN exclusively: " + str(measured))
        report.update(passed=True, audio=audio, owned_virtual_source_only=True, conditional_restore=True)
    except Exception as error:
        report["error"] = str(error)
    finally:
        terminate(native)
        # An aborted native assertion must not strand our virtual player in a
        # private route. This fence selects only our exact sink-input/client.
        if selected and player and player.poll() is None:
            try:
                current = next((item for item in items("sink-inputs") if item["index"] == selected["index"] and
                                item["client"] == selected["client"] and
                                item.get("properties", {}).get("application.name") == label), None)
                if current:
                    sink = next((item for item in items("sinks") if item["index"] == current["sink"]), None)
                    if sink:
                        remember_private(sink)
                    if current["sink"] != original:
                        pulse("move-sink-input", str(selected["index"]), str(original))
            except Exception as error:
                report["cleanup_error"] = str(error)
        terminate(player)
        if source:
            source.close()
        def owned_module(item: dict, index: int, name: str) -> bool:
            return (item["index"] == index and item.get("name") == "module-null-sink" and
                    "sink_name=" + name in shlex.split(item.get("argument", "")))

        for module, name in [*private_modules.items(), *reversed(list(modules.items()))]:
            try:
                if any(owned_module(item, module, name) for item in items("modules")):
                    pulse("unload-module", str(module))
            except Exception as error:
                report["cleanup_error"] = str(error)
        try:
            report["defaults_preserved"] = defaults == (pulse("get-default-sink"), pulse("get-default-source"))
            remaining = items("modules")
            report["owned_modules_released"] = not any(owned_module(item, module, name) for item in remaining
                for module, name in [*modules.items(), *private_modules.items()])
        except Exception as error:
            report["cleanup_error"] = str(error)
        report["passed"] = bool(report["passed"] and report.get("defaults_preserved") and
                                report.get("owned_modules_released") and not report.get("cleanup_error"))
        (root / "acceptance.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2))
        if temporary:
            temporary.cleanup()
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
