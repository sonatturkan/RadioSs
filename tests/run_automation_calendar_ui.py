#!/usr/bin/env python3
"""Run each native timetable case against its own genuine SDBASE installation."""
from __future__ import annotations

import sys

sys.dont_write_bytecode = True
import run_data_ui


def main() -> int:
    original_arguments = sys.argv
    if len(original_arguments) < 2:
        print("Usage: run_automation_calendar_ui.py TEST_EXECUTABLE [GLib test arguments]",
              file=sys.stderr)
        return 2
    try:
        if len(original_arguments) == 2:
            # Each case owns its calendar, library and recovery journal. Reopen
            # assertions within a case keep that same store; the next case must
            # start with empty dates and independent revision history.
            cases: list[str] = []
            sys.argv = [*original_arguments, "-l"]
            result = run_data_ui.main(collect_glib_cases=cases)
            if result:
                return result
            # Every case of one executable shares its suite prefix, e.g.
            # /native-automation-calendar/ or /native-player-schema/.
            first = cases[0] if cases else ""
            prefix = first[:first.find("/", 1) + 1] if first.startswith("/") and first.find("/", 1) > 0 else ""
            if (not cases or not prefix or len(cases) != len(set(cases)) or
                    any(not case.startswith(prefix) for case in cases)):
                raise RuntimeError("Native calendar case discovery is empty or ambiguous")
            for case in cases:
                print("Native calendar independent fixture:", case, flush=True)
                sys.argv = [*original_arguments, "-p", case]
                result = run_data_ui.main(expected_glib_case=case)
                if result:
                    return result
            return 0
        # Keep explicit GLib filters and listing options available for focused
        # diagnostics, including callers selecting one case with -p.
        return run_data_ui.main()
    finally:
        sys.argv = original_arguments


if __name__ == "__main__":
    raise SystemExit(main())
