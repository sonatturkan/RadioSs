#!/usr/bin/env python3
"""Execute each stream metadata case in an independent private audio fixture."""
from pathlib import Path
import re
import subprocess
import sys

CASES = (
    "/stream-metadata/format-UTF8-bounds-placeholder-contract",
    "/stream-metadata/bound-player-source-switch-PCM",
    "/stream-metadata/private-accepted-plan-pause-stop",
    "/stream-metadata/automation-current-overlap-pause-stop",
    "/stream-metadata/HTTP-dedupe-failure-retry-late-close",
    "/stream-metadata/all-output-reconnect-no-empty",
    "/stream-metadata/player-jingle-advertisement-keep-last-HTTP",
    "/stream-metadata/accepted-plan-jingle-advertisement-PCM",
    "/stream-metadata/automation-inserted-jingle-marker-PCM",
    "/stream-metadata/broadcast-runs-list-fills",
    "/stream-metadata/technical-servers-runs-list",
    "/stream-metadata/device-hotplug-window-responsive",
)

def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: run_stream_metadata_ui.py /path/to/test_stream_metadata", file=sys.stderr)
        return 2
    project = Path(__file__).resolve().parents[1]
    binary = Path(sys.argv[1]).resolve(strict=True)
    runner = project / "tests/run_playout_ui.py"
    for case in CASES:
        print("Stream metadata private fixture:", case, flush=True)
        result = subprocess.run([sys.executable, str(runner), str(binary), "-p", case],
            cwd=project, check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(result.stdout, end="", flush=True)
        if result.returncode:
            return result.returncode
        lines = result.stdout.splitlines()
        plans = [line for line in lines if re.fullmatch(r"1\.\.\d+", line)]
        outcomes = [line for line in lines if re.match(r"(?:not )?ok \d+\b", line)]
        if plans != ["1..1"] or outcomes != [f"ok 1 {case}"]:
            print("Stream metadata case did not run its exact scenario without a skip:", case, file=sys.stderr)
            return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
