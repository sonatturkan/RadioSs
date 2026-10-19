#!/usr/bin/env python3
"""Run DJ acceptance in the private DB and owned Pulse fixture.

The shared audio fixture records MAIN/CUE/MON into private files, preselects
unique null outputs before GTK starts and tears down only its own processes
and module identities. The production database and physical outputs are not
used by these tests.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.dont_write_bytecode = True
from run_audio_ui import main


def run_cli() -> int:
    original_arguments = sys.argv
    if len(original_arguments) != 2 or Path(original_arguments[1]).name != "test_dj_ui":
        return main()
    try:
        cases: list[str] = []
        sys.argv = [*original_arguments, "-p", "/native-dj", "-l"]
        result = main(collect_glib_cases=cases)
        if result:
            return result
        if (not cases or len(cases) != len(set(cases)) or
                any(not case.startswith("/native-dj/") for case in cases)):
            raise RuntimeError("Native DJ GLib case discovery is empty or ambiguous")
        for case in cases:
            print("Native DJ independent fixture:", case, flush=True)
            sys.argv = [*original_arguments, "-p", case]
            result = main(expected_glib_case=case)
            if result:
                return result
        return 0
    except RuntimeError as error:
        print(f"Native DJ acceptance failed: {error}", file=sys.stderr)
        return 1
    finally:
        sys.argv = original_arguments


if __name__ == "__main__":
    raise SystemExit(run_cli())
