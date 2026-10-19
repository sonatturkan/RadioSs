#!/usr/bin/env python3
"""Reuse isolated audio fixtures, adding controlled prepare replies locally."""

from pathlib import Path
import sys

sys.dont_write_bytecode = True
import run_audio_ui


def main():
    original = run_audio_ui.shutil.copytree
    original_arguments = sys.argv

    def copy_fixture(source, destination, *args, **kwargs):
        result = original(source, destination, *args, **kwargs)
        target = Path(destination)
        if target.parts[-3:] == ("database", "SDBASE", "src"):
            path = target / "sdbase/http_server.py"
            text = path.read_text(encoding="utf-8")
            # Native fixture setup imports and binds many real sources while
            # a full application refreshes its authenticated documents. Give
            # only this copied private server a bounded test burst allowance;
            # production quotas/authentication/store behavior remain intact.
            quota = "capacity=120, refill_per_second=10"
            if text.count(quota) != 2:
                raise RuntimeError("Private native request quota anchors changed")
            text = text.replace(quota, "capacity=1200, refill_per_second=100")
            anchor = "                result = self._dispatch(method, path, query, principal)\n"
            gate = '''                if method == "POST" and path == "/v1/workflows/desktop-playout/prepare":
                    _acceptance_playout_hold = store.root / ".acceptance-playout-prepare-hold"
                    if _acceptance_playout_hold.is_file():
                        import time as _acceptance_playout_time
                        (store.root / ".acceptance-playout-prepare-ready").touch(mode=0o600)
                        _acceptance_playout_deadline = _acceptance_playout_time.monotonic() + 2.0
                        while _acceptance_playout_hold.exists() and _acceptance_playout_time.monotonic() < _acceptance_playout_deadline:
                            _acceptance_playout_time.sleep(0.005)
                    _acceptance_once = store.root / ".acceptance-playout-prepare-fail-once"
                    if _acceptance_once.is_file():
                        _acceptance_left = int(_acceptance_once.read_text(encoding="utf-8")) - 1
                        if _acceptance_left:
                            _acceptance_once.write_text(str(_acceptance_left), encoding="utf-8")
                        else:
                            _acceptance_once.unlink()
                            (store.root / ".acceptance-playout-prepare-failed-once").touch(mode=0o600)
                            raise WriteRejected("Isolated acceptance: transient continuation failure")
                    if (store.root / ".acceptance-playout-prepare-unavailable").is_file():
                        raise WriteRejected("Isolated acceptance: temporary preparation outage")
                    if (store.root / ".acceptance-playout-prepare-fail").is_file():
                        raise StoreError("Isolated acceptance: playback preparation unavailable")
'''
            if text.count(anchor) != 1:
                raise RuntimeError("Isolated prepare response control could not be installed")
            path.write_text(text.replace(anchor, anchor + gate), encoding="utf-8")
        return result

    run_audio_ui.shutil.copytree = copy_fixture
    try:
        if len(original_arguments) == 2:
            # Independent acceptance groups should not consume one another's
            # server quota or persistent rotation state. Discover every actual
            # GLib case using a fully initialized private fixture; each case
            # then gets another private fixture. Reopens within that case still
            # use its same DB. Explicit filters/other GLib arguments retain the
            # original shared-fixture behavior for diagnostics.
            cases = []
            sys.argv = [*original_arguments, "-l"]
            result = run_audio_ui.main(collect_glib_cases=cases)
            if result:
                return result
            if (not cases or len(cases) != len(set(cases)) or
                    any(not case.startswith("/native-playout/") for case in cases)):
                raise RuntimeError("Native playout GLib case discovery is empty or ambiguous")
            for case in cases:
                print("Native playout independent fixture:", case, flush=True)
                sys.argv = [*original_arguments, "-p", case]
                result = run_audio_ui.main(expected_glib_case=case, test_timeout=180 if case in {"/native-playout/general-future-refill-adjacent-reconnect-PCM", "/native-playout/weekly-programme-future-countdown-due-weighted-repeat-PCM"} else 100)
                if result:
                    return result
            return 0
        if len(original_arguments) == 4 and original_arguments[2] == "-p" and original_arguments[3].startswith("/native-playout/"):
            # A typo must not turn an empty GLib selection into acceptance.
            return run_audio_ui.main(expected_glib_case=original_arguments[3],
                test_timeout=180 if original_arguments[3] in {"/native-playout/general-future-refill-adjacent-reconnect-PCM", "/native-playout/weekly-programme-future-countdown-due-weighted-repeat-PCM"} else 100)
        return run_audio_ui.main()
    finally:
        sys.argv = original_arguments
        run_audio_ui.shutil.copytree = original


if __name__ == "__main__":
    raise SystemExit(main())
