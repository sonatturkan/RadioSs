#!/usr/bin/env python3
"""Require each new probability GUI case in a separate genuine private store."""
from pathlib import Path
import re
import subprocess
import sys

CASES = (
    "/automation-probability/rotation-create-Sweeper-kind-ACK-reopen",
    "/automation-probability/scoped-transitions-selected-rest-ACK-reopen",
    "/automation-probability/invalid-percentage-UTC-draft-no-write",
    "/automation-probability/jingle-explicit-target-timing-ACK-reopen",
    "/automation-probability/simple-forms-choice-transition-jingle-update-delete-ACK-reopen",
    "/automation-probability/calendar-content-warning-draft-verified-deleted-reopen",
    "/automation-probability/soft-end-relax-ACK-invalid-reopen",
    "/automation-probability/late-create-rotation-fence",
    "/automation-probability/unified-calendar-programmes-rotation-tables-geometry",
)

def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: run_automation_probability_ui.py TEST_EXECUTABLE", file=sys.stderr)
        return 2
    project = Path(__file__).resolve().parents[1]
    binary = Path(sys.argv[1]).resolve(strict=True)
    for case in CASES:
        print("Probability GUI private HTTP fixture:", case, flush=True)
        result = subprocess.run([sys.executable, str(project / "tests/run_data_ui.py"), str(binary), "-p", case],
            cwd=project, check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        print(result.stdout, end="", flush=True)
        if result.returncode:
            return result.returncode
        lines = result.stdout.splitlines()
        plans = [line for line in lines if re.fullmatch(r"1\.\.\d+", line)]
        outcomes = [line for line in lines if re.match(r"(?:not )?ok \d+\b", line)]
        if plans != ["1..1"] or outcomes != [f"ok 1 {case}"]:
            print("Exact probability GUI case did not run without skips:", case, file=sys.stderr)
            return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
