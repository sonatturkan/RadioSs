"""Unit tests of packaging/windows/sync_runtime.py (CTest: packaging_sync_runtime).

Run with the interpreter that runs the tool in production (pinned MSYS2 UCRT64 python,
``-X utf8 -B -I``); standard library only, Windows only. Every test works in its own
temporary directory; nothing outside it is touched.
"""

from __future__ import annotations

import ctypes
import json
import os
import pathlib
import sys
import tempfile
import unittest
from ctypes import wintypes
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import sync_runtime  # noqa: E402  (module under test, imported from packaging/windows)

GENERIC_READ = 0x80000000
FILE_SHARE_READ = 0x00000001
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL


class DenyWrite:
    """Holds a handle that denies write sharing, like the image mapping of a running exe."""

    def __init__(self, path: pathlib.Path) -> None:
        self.path = path
        self.handle = None

    def __enter__(self) -> "DenyWrite":
        handle = kernel32.CreateFileW(str(self.path), GENERIC_READ, FILE_SHARE_READ, None, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, None)
        if handle in (None, INVALID_HANDLE_VALUE):
            raise ctypes.WinError(ctypes.get_last_error())
        self.handle = handle
        return self

    def __exit__(self, *exc) -> None:
        kernel32.CloseHandle(self.handle)


def write(path: pathlib.Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def snapshot(root: pathlib.Path) -> dict[str, bytes]:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(root.rglob("*")) if p.is_file()}


class SyncRuntimeTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="radios-sync-")
        base = pathlib.Path(self._tmp.name)
        self.stage = base / "stage"
        self.prefix = base / "runtime"
        write(self.stage / "bin" / "RadioS.exe", b"exe-v2")
        write(self.stage / "bin" / "libglib-2.0-0.dll", b"glib-v2")
        write(self.stage / "lib" / "gdk-pixbuf-2.0" / "2.10.0" / "loaders.cache", b"cache\n\n")
        write(self.stage / "share" / "icons" / "Adwaita" / "index.theme", b"[Icon Theme]\n")
        write(self.stage / "share" / "icons" / "Adwaita" / "icon-theme.cache", b"icon-cache")
        write(self.stage / "etc" / "gtk-3.0" / "settings.ini", b"[Settings]\n")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_sync(self, *extra: str) -> int:
        argv = ["--stage", str(self.stage), "--prefix", str(self.prefix), "--keep", "python",
                "--keep", "share/zoneinfo", "--version", "0.19.0", "--build-type", "Release", *extra]
        return sync_runtime.main(argv)

    def make_kept_trees(self) -> dict[str, bytes]:
        write(self.prefix / "python" / "python.exe", b"python")
        write(self.prefix / "python" / "Lib" / "tzdata" / "__init__.py", b"# tzdata\n")
        write(self.prefix / "share" / "zoneinfo" / "Europe" / "Istanbul", b"TZif")
        return {k: v for k, v in snapshot(self.prefix).items() if k.startswith(("python/", "share/zoneinfo/"))}

    def test_fresh_prefix_is_created_with_manifest(self) -> None:
        self.assertEqual(self.run_sync(), 0)
        expected = snapshot(self.stage)
        actual = {k: v for k, v in snapshot(self.prefix).items() if k != sync_runtime.MANIFEST}
        self.assertEqual(actual, expected)
        manifest = json.loads((self.prefix / sync_runtime.MANIFEST).read_text(encoding="utf-8"))
        self.assertEqual(manifest["version"], "0.19.0")
        self.assertEqual(manifest["build_type"], "Release")
        self.assertEqual(sorted(manifest["files"]), sorted(expected))
        self.assertNotIn(b"\r\n", (self.prefix / sync_runtime.MANIFEST).read_bytes())

    def test_update_changes_adds_removes_and_keeps(self) -> None:
        self.assertEqual(self.run_sync(), 0)
        kept = self.make_kept_trees()
        write(self.prefix / "bin" / "obsolete.dll", b"old")
        write(self.stage / "bin" / "RadioS.exe", b"exe-v3")
        write(self.stage / "bin" / "libnew.dll", b"new")
        (self.stage / "etc" / "gtk-3.0" / "settings.ini").unlink()
        self.assertEqual(self.run_sync(), 0)
        files = snapshot(self.prefix)
        self.assertEqual(files["bin/RadioS.exe"], b"exe-v3")
        self.assertEqual(files["bin/libnew.dll"], b"new")
        self.assertNotIn("bin/obsolete.dll", files)
        self.assertNotIn("etc/gtk-3.0/settings.ini", files)
        self.assertFalse((self.prefix / "etc").exists(), "empty directories must be removed")
        for rel, data in kept.items():
            self.assertEqual(files.get(rel), data, f"kept tree changed: {rel}")
        leftovers = [k for k in files if sync_runtime.TMP_NEW in k or sync_runtime.TMP_OLD in k]
        self.assertEqual(leftovers, [])

    def test_up_to_date_prefix_is_not_rewritten(self) -> None:
        self.assertEqual(self.run_sync(), 0)
        exe = self.prefix / "bin" / "RadioS.exe"
        before = exe.stat().st_mtime_ns
        self.assertEqual(self.run_sync(), 0)
        self.assertEqual(exe.stat().st_mtime_ns, before)

    def test_running_exe_refuses_and_changes_nothing(self) -> None:
        self.assertEqual(self.run_sync(), 0)
        write(self.stage / "bin" / "libglib-2.0-0.dll", b"glib-v3")
        write(self.stage / "bin" / "libnew.dll", b"new")
        before = snapshot(self.prefix)
        with DenyWrite(self.prefix / "bin" / "RadioS.exe"):
            self.assertEqual(self.run_sync(), 1)
        self.assertEqual(snapshot(self.prefix), before)
        self.assertEqual(self.run_sync(), 0)
        self.assertEqual((self.prefix / "bin" / "libglib-2.0-0.dll").read_bytes(), b"glib-v3")

    def test_busy_dll_refuses_and_changes_nothing(self) -> None:
        self.assertEqual(self.run_sync(), 0)
        write(self.stage / "bin" / "libglib-2.0-0.dll", b"glib-v3")
        write(self.stage / "bin" / "RadioS.exe", b"exe-v3")
        before = snapshot(self.prefix)
        with DenyWrite(self.prefix / "bin" / "libglib-2.0-0.dll"):
            self.assertEqual(self.run_sync(), 1)
        self.assertEqual(snapshot(self.prefix), before)

    def test_failure_during_switch_rolls_back(self) -> None:
        self.assertEqual(self.run_sync(), 0)
        write(self.stage / "bin" / "RadioS.exe", b"exe-v3")
        write(self.stage / "bin" / "libglib-2.0-0.dll", b"glib-v3")
        write(self.stage / "bin" / "libnew.dll", b"new")
        before = snapshot(self.prefix)
        real_replace = os.replace
        calls = {"n": 0}

        def flaky_replace(src, dst):
            calls["n"] += 1
            if calls["n"] == 4:   # after the first changed file was switched completely
                raise OSError(5, "simulated failure")
            return real_replace(src, dst)

        with mock.patch.object(sync_runtime.os, "replace", side_effect=flaky_replace):
            self.assertEqual(self.run_sync(), 1)
        after = snapshot(self.prefix)
        self.assertEqual(after, before)

    def test_foreign_prefix_is_refused(self) -> None:
        write(self.prefix / "config" / "radios.ini", b"[station]\n")
        write(self.prefix / "src" / "main.c", b"int main(void){return 0;}\n")
        before = snapshot(self.prefix)
        self.assertEqual(self.run_sync(), 1)
        self.assertEqual(snapshot(self.prefix), before)

    def test_stage_without_exe_is_refused(self) -> None:
        (self.stage / "bin" / "RadioS.exe").unlink()
        self.assertEqual(self.run_sync(), 1)
        self.assertFalse(self.prefix.exists())

    def test_nested_stage_and_prefix_are_refused(self) -> None:
        argv = ["--stage", str(self.stage), "--prefix", str(self.stage / "bin")]
        self.assertEqual(sync_runtime.main(argv), 1)

    def test_invalid_keep_is_a_usage_error(self) -> None:
        with self.assertRaises(SystemExit) as raised:
            self.run_sync("--keep", "../outside")
        self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    if os.name != "nt":
        print("test_sync_runtime.py yalnız Windows'ta çalışır / Windows only", file=sys.stderr)
        sys.exit(1)
    unittest.main(verbosity=2)
