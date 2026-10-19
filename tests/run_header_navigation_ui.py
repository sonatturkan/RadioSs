#!/usr/bin/env python3
"""Run prompt 5 GUI scenarios with separate private DB and audio fixtures."""

from pathlib import Path
import re
import subprocess
import sys


SCENARIOS = (
    "footer-relocation-diagnostic-backup-offline",
    "local-next-immutable-playlist-boundary",
    "selected-player-queue-isolation",
)


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: run_header_navigation_ui.py /path/to/test_header_navigation_ui", file=sys.stderr)
        return 2
    project = Path(__file__).resolve().parents[1]
    binary = Path(sys.argv[1]).resolve()
    wrapper = project / "tests/run_audio_ui.py"
    for index, scenario in enumerate(SCENARIOS, 1):
        case = f"/native-header-navigation/{scenario}"
        print(f"Prompt5 GUI scenario {index}/{len(SCENARIOS)}: {scenario}", flush=True)
        # Startup reads and seed writes use the normal service quota. Each
        # independent case gets fresh auth, DB, Pulse sinks and cleanup from
        # the existing wrapper. Playback within a case remains continuous.
        result = subprocess.run(
            [sys.executable, str(wrapper), str(binary), "-p", case],
            cwd=project,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        print(result.stdout, end="", flush=True)
        if result.returncode:
            return 1
        lines = result.stdout.splitlines()
        plans = [line for line in lines if re.fullmatch(r"1\.\.\d+", line)]
        outcomes = [line for line in lines if re.match(r"(?:not )?ok \d+\b", line)]
        if plans != ["1..1"] or outcomes != [f"ok 1 {case}"]:
            print(f"Prompt5 scenario did not execute its exact case without a skip: {case}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
