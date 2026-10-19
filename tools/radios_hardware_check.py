#!/usr/bin/env python3
"""Read-only Pulse/PipeWire device inventory and physical acceptance checklist.

This command never opens a microphone, plays PCM, changes a route or volume,
or claims that enumerated hardware has been acoustically accepted.
"""
from __future__ import annotations

import argparse
import datetime
import json
from pathlib import Path
import shutil
import subprocess
import sys


MANUAL_CHECKS = [
    {"id": "main-output", "instruction": "Teknik → Ses bölümünde gerçek MAIN cihazını seçin; düşük sesle deneme parçasını çalın ve doğru fiziksel çıkışta iki kanalı dinleyin."},
    {"id": "cue-isolation", "instruction": "Ayrı CUE cihazını seçin; ÖN DİNLEME parçasını kulaklıkta dinleyin ve MAIN çıkışına karışmadığını doğrulayın."},
    {"id": "microphone-gate", "instruction": "Gerçek mikrofonu seçin; 'Mikrofon kapalı' durumunda MAIN'e ses karışmadığını, düğmeyle 'Mikrofon açık' durumuna geçince ses geldiğini ve tekrar 'Mikrofon kapalı' durumuna alınca kesildiğini doğrulayın."},
    {"id": "loss-no-autoplay", "instruction": "Harici ses arayüzü varsa yayın kapalıyken söküp takın; doğru cihaz geri gelince uygulamanın kendiliğinden çalmadığını ve mikrofonun kapalı kaldığını doğrulayın."},
    {"id": "real-listener", "instruction": "Gerçek yayın sunucusunda yalnız açıkça başlatılmış deneme yayınının harici dinleyicide duyulduğunu ve Stop sonrasında kesildiğini doğrulayın."},
    {"id": "site-long-duration", "instruction": "Kullanılacak gerçek ses arayüzü ve yayın sunucusuyla işletme süresine uygun uzun yayın yapın; kopma, ses kesilmesi, gecikme ve bellek gözlemlerini kaydedin."},
]


def query(arguments: list[str], server: str | None) -> str:
    command = ["pactl"]
    if server:
        command += ["--server", server]
    result = subprocess.run(command + arguments, capture_output=True, text=True, timeout=8, check=False)
    if result.returncode:
        raise RuntimeError(result.stderr.strip()[:1000] or "pactl failed")
    return result.stdout


def device(item: dict, source: bool = False) -> dict:
    properties = item.get("properties") or {}
    flags = item.get("flags") or []
    monitor = source and (properties.get("device.class") == "monitor" or str(item.get("name", "")).endswith(".monitor"))
    hardware = "HARDWARE" in flags and not monitor
    ports = [{"name": port.get("name"), "description": port.get("description"),
              "availability": port.get("availability"), "type": port.get("type")}
             for port in item.get("ports", [])]
    active = item.get("active_port")
    selected = next((port for port in ports if port["name"] == active), None)
    return {"name": item.get("name"), "description": item.get("description"),
            "state": item.get("state"), "driver": item.get("driver"),
            "sample_specification": item.get("sample_specification"),
            "muted": item.get("mute"), "hardware": hardware, "monitor": monitor,
            "device_api": properties.get("device.api"), "device_bus": properties.get("device.bus"),
            "active_port": active, "active_port_availability": selected.get("availability") if selected else None,
            "ports": ports}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", help="Optional explicit Pulse server; default is the current desktop server")
    parser.add_argument("--output", type=Path, help="Write the same JSON report to this file")
    args = parser.parse_args()
    report = {"schema": 1, "utc_checked": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "read_only": True, "pcm_played": False, "microphone_captured": False,
              "physical_acceptance": "operator-required", "server_reachable": False,
              "operator_checks": [{**check, "status": "pending"} for check in MANUAL_CHECKS]}
    code = 0
    try:
        if not shutil.which("pactl"):
            raise RuntimeError("pactl is required for the read-only hardware inventory")
        info = query(["info"], args.server)
        report["server_reachable"] = True
        # No daemon control, no global configuration or device ownership changes.
        report["server_info"] = {line.split(":", 1)[0].strip(): line.split(":", 1)[1].strip()
                                 for line in info.splitlines() if ":" in line}
        sinks = json.loads(query(["--format=json", "list", "sinks"], args.server))
        sources = json.loads(query(["--format=json", "list", "sources"], args.server))
        if not isinstance(sinks, list) or not isinstance(sources, list):
            raise ValueError("Unexpected Pulse device response")
        report["outputs"] = [device(item) for item in sinks]
        report["inputs"] = [device(item, source=True) for item in sources]
        report["physical_output_count"] = sum(item["hardware"] for item in report["outputs"])
        report["physical_input_count"] = sum(item["hardware"] for item in report["inputs"])
        report["enumeration_passed"] = True
    except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        report["enumeration_passed"] = False
        report["error"] = str(error)
        code = 1
    contents = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(contents, encoding="utf-8")
    sys.stdout.write(contents)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
