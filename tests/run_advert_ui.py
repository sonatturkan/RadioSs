#!/usr/bin/env python3
"""Run native advertising acceptance with a private DB, owned Pulse outputs, and controlled replies."""

from pathlib import Path
import sys

sys.dont_write_bytecode = True
import run_audio_ui


def main():
    original = run_audio_ui.shutil.copytree

    def copy_fixture(source, destination, *args, **kwargs):
        result = original(source, destination, *args, **kwargs)
        target = Path(destination)
        if target.parts[-3:] == ("database", "SDBASE", "src"):
            path = target / "sdbase/http_server.py"
            text = path.read_text(encoding="utf-8")
            anchor = "                result = self._dispatch(method, path, query, principal)\n"
            gate = '''                if method == "POST" and path in {"/v1/workflows/desktop-advert/prepare", "/v1/workflows/desktop-advert/run-start", "/v1/workflows/desktop-playout/prepare"}:
                    _acceptance_advert_kind = "advert-run" if path.endswith("/run-start") else "advert" if "desktop-advert" in path else "playout"
                    _acceptance_advert_hold = store.root / (".acceptance-" + _acceptance_advert_kind + "-prepare-hold")
                    if _acceptance_advert_hold.is_file():
                        import time as _acceptance_advert_time
                        (store.root / (".acceptance-" + _acceptance_advert_kind + "-prepare-ready")).touch(mode=0o600)
                        _acceptance_advert_deadline = _acceptance_advert_time.monotonic() + 2.0
                        while _acceptance_advert_hold.exists() and _acceptance_advert_time.monotonic() < _acceptance_advert_deadline:
                            _acceptance_advert_time.sleep(0.005)
                    if (store.root / (".acceptance-" + _acceptance_advert_kind + "-prepare-fail")).is_file():
                        raise StoreError("Isolated acceptance: advertising preparation unavailable")
'''
            if text.count(anchor) != 1:
                raise RuntimeError("Isolated prepare response control could not be installed")
            path.write_text(text.replace(anchor, anchor + gate), encoding="utf-8")
        return result

    run_audio_ui.shutil.copytree = copy_fixture
    try:
        return run_audio_ui.main(test_timeout=240)
    finally:
        run_audio_ui.shutil.copytree = original


if __name__ == "__main__":
    raise SystemExit(main())
