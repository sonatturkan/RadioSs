#!/usr/bin/env python3
"""Programme acceptance reuses private real DB and owned Pulse fixtures."""
from pathlib import Path
import run_audio_ui


def main():
    original = run_audio_ui.shutil.copytree
    def copy(source, destination, *args, **kwargs):
        result = original(source, destination, *args, **kwargs)
        target = Path(destination)
        if target.parts[-3:] == ("database", "SDBASE", "src"):
            path = target / "sdbase/http_server.py"
            source_text = path.read_text()
            anchor = "                result = self._dispatch(method, path, query, principal)\n"
            gate = '''                if method == "POST" and path == "/v1/workflows/desktop-programme/prepare":
                    _programme_hold = store.root / ".acceptance-programme-prepare-hold"
                    if _programme_hold.exists():
                        import time as _programme_time
                        (store.root / ".acceptance-programme-prepare-ready").touch(mode=0o600)
                        _programme_deadline = _programme_time.monotonic() + 2
                        while _programme_hold.exists() and _programme_time.monotonic() < _programme_deadline:
                            _programme_time.sleep(0.005)
'''
            if source_text.count(anchor) != 1: raise RuntimeError("Private programme reply hold anchor changed")
            path.write_text(source_text.replace(anchor, anchor + gate))
        return result
    run_audio_ui.shutil.copytree = copy
    try: return run_audio_ui.main(test_timeout=150)
    finally: run_audio_ui.shutil.copytree = original


if __name__ == "__main__": raise SystemExit(main())
