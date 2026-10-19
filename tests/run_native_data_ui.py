#!/usr/bin/env python3
"""Give independent native data cases separate genuine installation fixtures."""
from __future__ import annotations

import sys

sys.dont_write_bytecode = True
import run_data_ui


def main() -> int:
    original_arguments = sys.argv
    if len(original_arguments) < 2:
        print("Usage: run_native_data_ui.py /path/to/test_data_ui [GLib test arguments]",
              file=sys.stderr)
        return 2
    try:
        if len(original_arguments) == 2:
            # Discover the executable's actual selected cases inside a fresh
            # initialized installation. Do not maintain a parallel case list
            # that could silently leave a newly added GLib test unexecuted.
            cases: list[str] = []
            sys.argv = [*original_arguments, "-p", "/native-data", "-l"]
            result = run_data_ui.main(collect_glib_cases=cases)
            if result:
                return result
            if (not cases or len(cases) != len(set(cases)) or
                    any(not case.startswith("/native-data/") for case in cases)):
                raise RuntimeError("Native data GLib case discovery is empty or ambiguous")
            # Reopens and deliberate outages within one case keep that case's
            # own database. Other cases receive fresh stores, identities and
            # default request quotas; production limits remain unchanged.
            for case in cases:
                print("Native data independent fixture:", case, flush=True)
                sys.argv = [*original_arguments, "-p", case]
                result = run_data_ui.main(expected_glib_case=case)
                if result:
                    return result
            return 0
        # Explicit caller filters retain the original shared fixture behavior
        # for focused diagnostics. Default CTest runs use the isolated loop.
        return run_data_ui.main()
    finally:
        sys.argv = original_arguments


if __name__ == "__main__":
    raise SystemExit(main())
