#!/usr/bin/env python3
"""Run each real high-deck mapping case in its own authenticated PCM fixture."""
from pathlib import Path
import sys

sys.dont_write_bytecode = True
import run_audio_ui


def main() -> int:
    original_arguments = sys.argv
    try:
        if len(original_arguments) == 2:
            cases: list[str] = []
            sys.argv = [*original_arguments, "-l"]
            result = run_audio_ui.main(collect_glib_cases=cases)
            if result:
                return result
            if (len(cases) != 3 or len(set(cases)) != 3 or
                    any(not case.startswith("/player-mapping/") for case in cases)):
                raise RuntimeError("Player mapping discovery must identify three distinct cases")
            for case in cases:
                print("Player mapping independent fixture:", case, flush=True)
                sys.argv = [*original_arguments, "-p", case]
                result = run_audio_ui.main(expected_glib_case=case)
                if result:
                    return result
            return 0
        if (len(original_arguments) == 4 and original_arguments[2] == "-p" and
                original_arguments[3].startswith("/player-mapping/")):
            return run_audio_ui.main(expected_glib_case=original_arguments[3])
        print("Usage: run_player_mapping_ui.py /path/to/test_player_mapping_ui [-p /player-mapping/case]",
              file=sys.stderr)
        return 2
    finally:
        sys.argv = original_arguments


if __name__ == "__main__":
    raise SystemExit(main())
