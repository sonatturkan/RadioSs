#!/usr/bin/env python3
"""Run focused GTK regressions on a private display, without user windows."""
from pathlib import Path
import os
import select
import shutil
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) < 2:
        print("Usage: run_rotation_editor_regressions.py TEST_EXECUTABLE [TEST_ARGS]", file=sys.stderr)
        return 2
    xvfb = shutil.which("Xvfb")
    command = [xvfb] if xvfb else None
    if command is None:
        host = Path("/run/host/usr")
        loader = host / "lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"
        if loader.is_file() and (host / "bin/Xvfb").is_file():
            command = [str(loader), "--library-path", str(host / "lib/x86_64-linux-gnu"), str(host / "bin/Xvfb")]
    if command is None:
        print("Private GTK test skipped: Xvfb is unavailable.", file=sys.stderr)
        return 77
    environment = dict(os.environ, G_DEBUG="fatal-warnings", GDK_BACKEND="x11", PYTHONDONTWRITEBYTECODE="1",
                       GTK_IM_MODULE="gtk-im-context-simple", NO_AT_BRIDGE="1")
    with tempfile.TemporaryFile() as log:
        read_fd, write_fd = os.pipe()
        try:
            server = subprocess.Popen(command + ["-displayfd", str(write_fd), "-screen", "0", "1024x768x24", "-nolisten", "tcp", "-ac"],
                stdin=subprocess.DEVNULL, stdout=log, stderr=log, pass_fds=(write_fd,), env=environment)
        finally:
            os.close(write_fd)
        try:
            if not select.select([read_fd], [], [], 10)[0]:
                raise RuntimeError("Private Xvfb did not report its display.")
            display = os.read(read_fd, 64).decode("ascii").strip()
            if not display.isdigit():
                log.seek(0)
                raise RuntimeError("Private Xvfb failed: " + log.read().decode(errors="replace")[-2000:])
            environment["DISPLAY"] = ":" + display
            return subprocess.run(sys.argv[1:], env=environment, check=False, timeout=30).returncode
        finally:
            os.close(read_fd)
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill(); server.wait(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
