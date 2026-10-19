#!/usr/bin/env python3
"""Windows platform shim (database/SDBASE/src/sdbase/_platform.py, re-exported by
tools/radios_platform.py): the C<->Python contract constants, path rules, binary and
no-follow opens, atomic replace/unlink/rmtree over open files, LockFileEx locks within
and across processes, private ACLs, process identity, named stop events and windowless
child processes — all with Turkish file names."""
import contextlib
import ctypes
import errno
import json
import msvcrt
import os
import queue
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
import unittest
import zoneinfo
from ctypes import wintypes as wt
from pathlib import Path

import _winapi

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tools"))
import radios_platform as tools_platform  # noqa: E402  (loads the shim by path)

plat = tools_platform.shim
SHIM = str(tools_platform.SHIM_PATH)
TURKISH = "Şarkılar İçerik ğüöç ı"

PRELUDE = (
    "import importlib.util, sys\n"
    "spec = importlib.util.spec_from_file_location('radios_sdbase_platform', sys.argv[1])\n"
    "plat = importlib.util.module_from_spec(spec)\n"
    "sys.modules[spec.name] = plat\n"
    "spec.loader.exec_module(plat)\n"
)

CONSOLE_PROBE = """
import ctypes, json, os, sys
from ctypes import wintypes as wt
kernel32 = ctypes.WinDLL("kernel32")
user32 = ctypes.WinDLL("user32")
kernel32.GetConsoleWindow.restype = wt.HWND
user32.IsWindowVisible.argtypes = [wt.HWND]
window = kernel32.GetConsoleWindow()
"""


@contextlib.contextmanager
def retry_budget(seconds):
    previous = plat.RETRY_BUDGET_SECONDS
    plat.RETRY_BUDGET_SECONDS = seconds
    try:
        yield
    finally:
        plat.RETRY_BUDGET_SECONDS = previous


class Child:
    """A Python child (bundled runtime, -E -s -B -X utf8, no console window) with a
    line protocol on stdin/stdout."""

    def __init__(self, code, *args, cwd=None):
        argv = plat.python_child_argv("-c", PRELUDE + textwrap.dedent(code), SHIM, *map(str, args))
        self.process = plat.popen_no_window(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE, cwd=cwd, env=plat.python_child_env())
        self.lines = queue.Queue()
        self.errors = []
        threading.Thread(target=self._pump, daemon=True).start()
        threading.Thread(target=self._pump_errors, daemon=True).start()

    def _pump(self):
        for raw in self.process.stdout:
            self.lines.put(raw.decode("utf-8").rstrip("\r\n"))
        self.lines.put(None)

    def _pump_errors(self):
        self.errors.append(self.process.stderr.read().decode("utf-8", "replace"))

    def expect(self, timeout=30.0):
        try:
            line = self.lines.get(timeout=timeout)
        except queue.Empty:
            raise AssertionError(f"child did not answer within {timeout} s; stderr: {self.errors}") from None
        if line is None:
            self.process.wait(10)
            raise AssertionError(f"child ended early (exit {self.process.returncode}); stderr: {self.errors}")
        return line

    def send(self, text):
        self.process.stdin.write((text + "\n").encode("utf-8"))
        self.process.stdin.flush()

    @property
    def pid(self):
        return self.process.pid

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(30)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            with contextlib.suppress(OSError):
                stream.close()


class TempDirTest(unittest.TestCase):
    def setUp(self):
        base = os.path.realpath(tempfile.mkdtemp(prefix="radios-plat-"))
        self.addCleanup(self._remove, base)
        self.base = os.path.join(base, TURKISH)
        os.mkdir(self.base)

    @staticmethod
    def _remove(path):
        if not plat.rmtree(path, ignore_errors=True):
            shutil.rmtree(path, ignore_errors=True)

    def path(self, *parts):
        return os.path.join(self.base, *parts)

    def write(self, name, data=b"data"):
        target = self.path(name)
        with open(target, "wb") as stream:
            stream.write(data)
        return target

    def junction(self, target, link):
        os.makedirs(target, exist_ok=True)
        _winapi.CreateJunction(target, link)
        self.addCleanup(lambda: os.path.lexists(link) and os.rmdir(link))
        return link

    def child(self, code, *args, cwd=None):
        process = Child(code, *args, cwd=cwd)
        self.addCleanup(process.close)
        return process


# --------------------------------------------------------------------------- contract


class ContractTest(unittest.TestCase):
    """Constants and rules that must be identical in C (include/radios/platform.h,
    src/core/platform*.c) and in Python."""

    def test_python_constants(self):
        self.assertEqual((plat.LOCK_SH, plat.LOCK_EX, plat.LOCK_NB, plat.LOCK_UN), (1, 2, 4, 8))
        self.assertEqual(plat.LOCK_REGION_OFFSET, 0x4000000000000000)
        self.assertEqual(plat.LOCK_REGION_LENGTH, 1)
        self.assertEqual((plat.O_NOFOLLOW, plat.O_DIRECTORY, plat.O_CLOEXEC, plat.O_NONBLOCK, plat.O_BINARY),
                         (0x00100000, 0x00200000, 0x80, 0, 0x8000))
        self.assertEqual(plat.stop_event_name(1234), "Local\\RadioS-stop-1234")
        self.assertEqual(plat.stop_event_name(), f"Local\\RadioS-stop-{os.getpid()}")
        self.assertEqual(plat.PYTHON_FLAGS, ("-E", "-s", "-B", "-X", "utf8"))
        self.assertFalse(plat.DETACHED_CREATIONFLAGS & subprocess.DETACHED_PROCESS)
        self.assertTrue(plat.DETACHED_CREATIONFLAGS & subprocess.CREATE_NO_WINDOW)
        if os.environ.get("SDBASE_FS_RETRY_SECONDS") is None:
            self.assertEqual(plat.RETRY_BUDGET_SECONDS, 2.0)
        for bad in (-1, "12", 1.5, True, 2 ** 32):
            with self.assertRaises(ValueError):
                plat.stop_event_name(bad)

    def test_header_constants_match(self):
        header = ROOT / "include" / "radios" / "platform.h"
        if not header.is_file():
            self.skipTest("C sources are not part of this tree")
        text = header.read_text(encoding="utf-8")

        def define(name):
            match = re.search(rf"#define\s+{name}\s+(?:G_GUINT64_CONSTANT\()?(0x[0-9A-Fa-f]+|\d+)", text)
            self.assertIsNotNone(match, name)
            return int(match.group(1), 0)

        self.assertEqual(define("RADIOS_LOCK_REGION_OFFSET"), plat.LOCK_REGION_OFFSET)
        self.assertEqual(define("RADIOS_LOCK_REGION_LENGTH"), plat.LOCK_REGION_LENGTH)
        for name in ("SH", "EX", "NB", "UN"):
            self.assertEqual(define(f"RADIOS_LOCK_{name}"), getattr(plat, f"LOCK_{name}"))
        for name in ("NOFOLLOW", "DIRECTORY", "CLOEXEC", "NONBLOCK", "BINARY"):
            self.assertEqual(define(f"RADIOS_O_{name}"), getattr(plat, f"O_{name}"), name)

    def test_c_sources_match(self):
        core = ROOT / "src" / "core"
        if not (core / "platform.c").is_file() or not (core / "platform_win32.c").is_file():
            self.skipTest("C sources are not part of this tree")
        platform_c = (core / "platform.c").read_text(encoding="utf-8")
        win32_c = (core / "platform_win32.c").read_text(encoding="utf-8")
        templates = re.findall(r'"(O:%sD:P\([^"]*)"', platform_c)
        python_templates = {plat.PRIVATE_FILE_SDDL, plat.PRIVATE_DIR_SDDL}
        self.assertEqual({template.replace("%s", "{sid}") for template in templates}, python_templates)
        self.assertRegex(win32_c, r'"Local\\\\RadioS-stop-%lu"')
        rights = {"FILE_READ_DATA": 0x1, "FILE_WRITE_DATA": 0x2, "FILE_APPEND_DATA": 0x4, "FILE_READ_EA": 0x8,
                  "FILE_WRITE_EA": 0x10, "FILE_EXECUTE": 0x20, "FILE_DELETE_CHILD": 0x40,
                  "FILE_WRITE_ATTRIBUTES": 0x100, "DELETE": 0x10000, "WRITE_DAC": 0x40000, "WRITE_OWNER": 0x80000,
                  "GENERIC_ALL": 0x10000000, "GENERIC_EXECUTE": 0x20000000, "GENERIC_WRITE": 0x40000000,
                  "GENERIC_READ": 0x80000000}
        match = re.search(r"sensitive\s*=\s*([^;]+);", platform_c)
        self.assertIsNotNone(match)
        names = [name.strip() for name in match.group(1).split("|")]
        self.assertEqual(sum(rights[name] for name in names), plat.PRIVATE_SENSITIVE_RIGHTS)
        budget = re.search(r"#define\s+RETRY_BUDGET_MS\s+(\d+)", platform_c)
        if budget and os.environ.get("SDBASE_FS_RETRY_SECONDS") is None:
            self.assertEqual(int(budget.group(1)) / 1000.0, plat.RETRY_BUDGET_SECONDS)

    def test_tools_module_reexports_the_one_shim(self):
        self.assertIs(tools_platform.open_fd, plat.open_fd)
        self.assertIs(tools_platform._load_shim(), plat)
        self.assertEqual(tools_platform.FLOCK_OFFSET, plat.LOCK_REGION_OFFSET)
        self.assertEqual(tools_platform.FLOCK_LENGTH, plat.LOCK_REGION_LENGTH)
        for name in plat.__all__:
            self.assertIs(getattr(tools_platform, name), getattr(plat, name), name)
        self.assertTrue(tools_platform.SHIM_PATH.samefile(ROOT / "database/SDBASE/src/sdbase/_platform.py"))


# --------------------------------------------------------------------------- path rules


class PathRulesTest(unittest.TestCase):
    def test_absolute_and_root(self):
        for value in ("C:\\x", "c:/x", "D:\\", "\\\\srv\\share", "\\\\srv\\share\\x", "\\\\srv/share/x"):
            self.assertTrue(plat.is_absolute(value), value)
        for value in ("", "x", "\\x", "/x", "C:x", "C:", "\\\\?\\C:\\x", "\\\\.\\pipe\\x", "\\\\srv", "\\\\srv\\",
                      None, 5):
            self.assertFalse(plat.is_absolute(value), value)
        for value in ("C:\\", "C:/", "\\\\srv\\share", "\\\\srv\\share\\"):
            self.assertTrue(plat.is_root(value), value)
        for value in ("C:\\x", "C:", "\\\\srv\\share\\x", ""):
            self.assertFalse(plat.is_root(value), value)

    def test_normalize_and_canonical(self):
        self.assertEqual(plat.normalize("c:/a//b/"), "C:\\a\\b")
        self.assertEqual(plat.normalize("C:\\a\\..\\b"), "C:\\b")
        self.assertEqual(plat.normalize("\\\\?\\C:\\x\\"), "C:\\x")
        self.assertEqual(plat.normalize("\\\\?\\UNC\\srv\\share\\x"), "\\\\srv\\share\\x")
        self.assertEqual(plat.normalize("C:\\"), "C:\\")
        self.assertEqual(plat.normalize("göreli\\x"), os.path.join(os.getcwd(), "göreli", "x"))
        with self.assertRaises(ValueError):
            plat.normalize("")
        for value in ("D:\\radyo\\kayıt", "C:\\", "\\\\srv\\share\\Müzik Arşivi"):
            self.assertTrue(plat.is_canonical(value), value)
        for value in ("", "göreli", "C:\\a\\..\\b", "C:\\a\\\\b", "C:\\a\\", "\\a", "C:a", "C:/a", "c:\\a",
                      "\\\\?\\C:\\a", "C:\\a:s", "C:\\a\\.\\b", "C:\\a\tb", "C:\\CON\\x", "C:\\x.", None):
            self.assertFalse(plat.is_canonical(value), value)

    def test_equality_and_inside_are_ntfs_case_insensitive(self):
        self.assertTrue(plat.path_equal("C:\\Müzik\\Şarkı", "c:/MÜZIK/şARKı"))
        self.assertTrue(plat.same_path("C:\\ç", "c:\\Ç"))
        self.assertFalse(plat.path_equal("C:\\a\\", "C:\\a"))
        self.assertFalse(plat.path_equal("C:\\İ", "C:\\i"))          # the OS upcase table, like C
        self.assertFalse(plat.path_equal(None, "C:\\a"))
        self.assertFalse(plat.path_equal("C:\\\U0001F600x", "C:\\\U0001F600y"))   # UTF-16 lengths, not code points
        self.assertTrue(plat.path_equal("C:\\\U0001F600Ş", "c:/\U0001F600ş"))
        cases = [("C:\\\U0001F600", "C:\\\U0001F600\\x", True), ("C:\\\U0001F600a", "C:\\\U0001F600b\\x", False),
                 ("C:\\", "C:\\x", True), ("C:\\a", "C:\\a", True), ("C:\\a", "C:\\ab", False),
                 ("C:\\a\\", "C:\\a\\b", True), ("c:/Müzik", "C:\\MÜZIK\\x", True),
                 ("\\\\srv\\share", "\\\\SRV\\Share\\x", True), ("C:\\a", "D:\\a", False), ("", "C:\\", False),
                 ("C:\\a\\b", "C:\\a", False)]
        for root, path, expected in cases:
            self.assertEqual(plat.is_inside(root, path), expected, (root, path))

    def test_component_rules(self):
        valid = ["a", "Şarkı Listesi.mp3", ".gizli", "a.b", "x" * 255, "\U0001F600" * 127, "COM10", "CONSOLE",
                 "NULL", "LPT", "COM", "nul_", "a b"]
        invalid = ["", ".", "..", "a/b", "a\\b", "a:b", "a*b", "a?b", 'a"b', "a<b", "a>b", "a|b", "a\x01", "a\x7f",
                   "x.", "x ", "CON", "con", "CoN.txt", "CON .txt", "nul.tar.gz", "AUX", "PRN", "COM0", "COM1",
                   "com9.log", "LPT\u00b9", "COM\u00b2", "CONIN$", "conout$.x", "x" * 256, "\U0001F600" * 128,
                   "a\ud800b", None, 5]
        for name in valid:
            self.assertTrue(plat.component_valid(name), name)
            self.assertTrue(plat.valid_new_name(name), name)
        for name in invalid:
            self.assertFalse(plat.component_valid(name), repr(name))
            self.assertFalse(plat.valid_new_name(name), repr(name))

    def test_sanitize_component(self):
        cases = [("Live: 2020", "Live_ 2020"), ("CON", "_CON"), ("nul.mp3", "_nul.mp3"), ("a. .", "a"),
                 ("  .x.  ", "x"), ("a/b\\c", "a_b_c"), ("x\x00y\x1f", "xy"), ('a<b>c:"d|e?f*', "a_b_c__d_e_f_"),
                 ("Şarkı", "Şarkı")]
        for text, expected in cases:
            self.assertEqual(plat.sanitize_component(text, "_"), expected, text)
        self.assertEqual(plat.sanitize_component("", "yedek"), "yedek")
        self.assertEqual(plat.sanitize_component(" . ", "yedek"), "yedek")
        long = plat.sanitize_component("a" * 149 + " .b", "_")
        self.assertTrue(plat.component_valid(long) and len(long) <= 150)
        self.assertTrue(plat.component_valid(plat.sanitize_component("\U0001F600" * 200, "_", limit=200)))
        with self.assertRaises(ValueError):
            plat.sanitize_component("x", "CON")

    def test_safe_relative_parts(self):
        digest = "a" * 64
        self.assertEqual(plat.safe_relative_parts(f"RadioS.sdb.objects/aa/{digest}"),
                         ("RadioS.sdb.objects", "aa", digest))
        for value in ("", "/a", "a//b", "a/", "a/./b", "a/../b", "\\x", "D:x", "a:ads", "con.json", "a/b.",
                      "a\x00b", "a/b ", 5, None):
            with self.assertRaises(ValueError):
                plat.safe_relative_parts(value)

    def test_flavour_and_portable_form(self):
        self.assertEqual(plat.stored_path_flavour("C:\\x"), "windows")
        self.assertEqual(plat.stored_path_flavour("C:/x"), "windows")
        self.assertEqual(plat.stored_path_flavour("\\\\srv\\share\\x"), "windows")
        self.assertEqual(plat.stored_path_flavour("/home/sturkan/Masaüstü/x"), "legacy-posix")
        for value in ("//x", "rel", "", None, 3):
            self.assertIsNone(plat.stored_path_flavour(value))
        self.assertEqual(plat.to_portable("data\\database\\RadioS.sdb"), "data/database/RadioS.sdb")
        self.assertEqual(plat.from_portable("data/database/RadioS.sdb"), "data\\database\\RadioS.sdb")
        self.assertEqual(plat.to_portable(Path("data") / "Şarkı"), "data/Şarkı")


# --------------------------------------------------------------------------- opening files


class OpenTest(TempDirTest):
    def test_binary_fidelity_and_non_inheritable(self):
        name = self.path("ikili ğ.bin")
        fd = plat.open_fd(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
        self.assertFalse(os.get_inheritable(fd))
        os.write(fd, b"a\nb\r\n\x1ac")
        os.close(fd)
        self.assertEqual(os.stat(name).st_size, 7)                  # no CRLF translation, no ^Z EOF
        fd = plat.open_fd(name, os.O_RDONLY)
        try:
            self.assertEqual(os.read(fd, 100), b"a\nb\r\n\x1ac")
        finally:
            os.close(fd)
        with self.assertRaises(FileExistsError):
            plat.open_fd(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
        with plat.open_read(name) as stream:
            self.assertEqual(stream.read(), b"a\nb\r\n\x1ac")
        self.assertEqual(plat.read_bytes_shared(name, limit=3), b"a\nb")

    def test_flag_validation(self):
        name = self.write("f")
        for flags in (os.O_RDONLY | os.O_TEXT, os.O_RDONLY | os.O_TEMPORARY, os.O_RDONLY | 0x01000000):
            with self.assertRaises(ValueError):
                plat.open_fd(name, flags)
        with self.assertRaises(OSError):
            plat.open_fd(name, os.O_WRONLY | os.O_RDWR)
        with self.assertRaises(ValueError):
            plat.open_flags(os.O_RDONLY | plat.O_NOFOLLOW)
        with self.assertRaises(ValueError):
            plat.open_flags(os.O_RDONLY, nofollow=True)
        self.assertEqual(plat.open_flags(os.O_RDONLY), os.O_BINARY | os.O_NOINHERIT)
        with self.assertRaises(ValueError):
            plat.open_fd(name, os.O_WRONLY, append_only=True)
        with self.assertRaises(ValueError):
            plat.open_fd(name, os.O_WRONLY | os.O_APPEND | os.O_TRUNC, append_only=True)

    def test_device_names_and_bad_descriptors(self):
        # GetFullPathNameW turns legacy DOS device names into \\.\ devices: a file path
        # must never open a device (same rule as the C layer).
        for device in ("NUL", "con"):
            with self.assertRaises(OSError) as caught:
                plat.open_fd(device, os.O_RDONLY)
            self.assertEqual(caught.exception.errno, errno.EINVAL, device)
            with self.assertRaises(OSError) as caught:
                plat.unlink(device)
            self.assertEqual(caught.exception.errno, errno.EINVAL, device)
        fd = plat.open_fd(self.write("kapalı"), os.O_RDONLY)
        os.close(fd)
        for operation in (lambda: plat.pread(fd, 1, 0), lambda: plat.fsync(fd), lambda: plat.flock(fd, plat.LOCK_SH),
                          lambda: plat.is_private(fd)):
            with self.assertRaises(OSError) as caught:
                operation()
            self.assertEqual(caught.exception.errno, errno.EBADF)

    def test_nofollow_truncate_and_create(self):
        name = self.write("kısaltılacak", b"12345")
        fd = plat.open_fd(name, os.O_WRONLY | os.O_TRUNC, nofollow=True)
        os.close(fd)
        self.assertEqual(os.stat(name).st_size, 0)
        fresh = self.path("yeni ş")
        fd = plat.open_fd(fresh, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | plat.O_NOFOLLOW, 0o600)
        try:
            os.write(fd, b"x")
        finally:
            os.close(fd)
        self.assertTrue(plat.is_private(fresh, exact_file_mode=True))
        fd = plat.open_fd(fresh, os.O_RDWR | os.O_CREAT | os.O_TRUNC, nofollow=True)
        os.close(fd)
        self.assertEqual(os.stat(fresh).st_size, 0)

    def test_directories(self):
        folder = self.path("klasör")
        os.mkdir(folder)
        with self.assertRaises(IsADirectoryError):
            plat.open_fd(folder, os.O_RDONLY)
        fd = plat.open_fd(folder, os.O_RDONLY | plat.O_DIRECTORY)
        try:
            self.assertTrue(stat.S_ISDIR(os.fstat(fd).st_mode))
        finally:
            os.close(fd)
        with self.assertRaises(NotADirectoryError):
            plat.open_fd(self.write("dosya"), os.O_RDONLY | plat.O_DIRECTORY)
        with self.assertRaises(IsADirectoryError):
            plat.open_fd(folder, os.O_WRONLY | plat.O_DIRECTORY)

    def test_nofollow_refuses_junctions_and_symlinks(self):
        target = self.path("hedef")
        os.mkdir(target)
        self.write(os.path.join("hedef", "iç.txt"), b"inside")
        link = self.junction(target, self.path("bağlantı"))
        for flags in (os.O_RDONLY | plat.O_NOFOLLOW, os.O_RDONLY | plat.O_DIRECTORY | plat.O_NOFOLLOW):
            with self.assertRaises(OSError) as caught:
                plat.open_fd(link, flags)
            self.assertEqual(caught.exception.errno, errno.ELOOP)
        # only the last component is no-follow for open_fd ...
        fd = plat.open_fd(os.path.join(link, "iç.txt"), os.O_RDONLY, nofollow=True)
        os.close(fd)
        # ... while open_nofollow_path refuses a link anywhere in the path
        with self.assertRaises(OSError) as caught:
            plat.open_nofollow_path(os.path.join(link, "iç.txt"))
        self.assertEqual(caught.exception.errno, errno.ELOOP)
        fd = plat.open_nofollow_path(os.path.join(target, "iç.txt"))
        try:
            self.assertEqual(os.read(fd, 10), b"inside")
        finally:
            os.close(fd)
        with self.assertRaises(OSError) as caught:
            plat.open_nofollow_path("göreli\\x")
        self.assertEqual(caught.exception.errno, errno.EINVAL)
        file_link = self.path("dosya bağlantısı")
        try:
            os.symlink(os.path.join(target, "iç.txt"), file_link)
        except OSError:
            self.skipTest("creating file symlinks needs Developer Mode or SeCreateSymbolicLinkPrivilege")
        with self.assertRaises(OSError) as caught:
            plat.open_fd(file_link, os.O_RDONLY, nofollow=True)
        self.assertEqual(caught.exception.errno, errno.ELOOP)
        with self.assertRaises(FileExistsError):                    # O_EXCL never follows a link name
            plat.open_fd(file_link, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
        for flags in (os.O_WRONLY | os.O_TRUNC, os.O_RDWR | os.O_CREAT | os.O_TRUNC):
            with self.assertRaises(OSError) as caught:              # never truncates the link or its target
                plat.open_fd(file_link, flags, nofollow=True)
            self.assertEqual(caught.exception.errno, errno.ELOOP)
            self.assertTrue(os.path.islink(file_link))
            with open(os.path.join(target, "iç.txt"), "rb") as stream:
                self.assertEqual(stream.read(), b"inside")
        fd = plat.open_fd(file_link, os.O_RDONLY)                  # following is still possible
        os.close(fd)

    def test_append_and_append_only(self):
        name = self.path("günlük.log")
        fd = plat.open_fd(name, os.O_WRONLY | os.O_CREAT | os.O_APPEND)
        os.write(fd, b"1\n")
        os.lseek(fd, 0, os.SEEK_SET)
        os.write(fd, b"2\n")
        os.ftruncate(fd, 2)                                         # O_APPEND keeps FILE_WRITE_DATA
        os.close(fd)
        fd = plat.open_fd(name, os.O_WRONLY | os.O_APPEND, append_only=True)
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            os.write(fd, b"3\n")
            with self.assertRaises(OSError):
                os.ftruncate(fd, 0)
        finally:
            os.close(fd)
        with open(name, "rb") as stream:
            self.assertEqual(stream.read(), b"1\n3\n")

    def test_open_beneath(self):
        folder = self.path("içe aktarım")
        os.mkdir(folder)
        name = "parça ş.wav"
        target = os.path.join(folder, name)
        with open(target, "wb") as stream:
            stream.write(b"RIFF")
        fd, info = plat.open_beneath(folder, name)
        try:
            self.assertTrue(stat.S_ISREG(info.st_mode))
            self.assertEqual(os.read(fd, 4), b"RIFF")
            with self.assertRaises(PermissionError):                 # deny_write: no writer may join
                open(target, "ab").close()
        finally:
            os.close(fd)
        for bad in ("..", "a\\b", "a:b", "CON", "", "x."):
            with self.assertRaises(OSError) as caught:
                plat.open_beneath(folder, bad)
            self.assertEqual(caught.exception.errno, errno.EINVAL, bad)
        os.mkdir(os.path.join(folder, "x.wav"))
        with self.assertRaises(IsADirectoryError):
            plat.open_beneath(folder, "x.wav")
        link = self.junction(folder, self.path("dizin bağlantısı"))
        with self.assertRaises(OSError) as caught:
            plat.open_beneath(link, name)
        self.assertEqual(caught.exception.errno, errno.ELOOP)
        writer = open(target, "ab")
        try:
            with retry_budget(0.2), self.assertRaises(OSError) as caught:
                plat.open_beneath(folder, name)
            self.assertEqual(caught.exception.errno, errno.EBUSY)
            fd, _info = plat.open_beneath(folder, name, deny_write=False)
            os.close(fd)
        finally:
            writer.close()

    def test_pread_keeps_the_position(self):
        name = self.write("konum", b"0123456789")
        fd = plat.open_fd(name, os.O_RDONLY)
        try:
            self.assertEqual(os.read(fd, 2), b"01")
            self.assertEqual(plat.pread(fd, 4, 3), b"3456")
            self.assertEqual(os.lseek(fd, 0, os.SEEK_CUR), 2)
            self.assertEqual(plat.pread(fd, 100, 8), b"89")
            self.assertEqual(plat.pread(fd, 10, 50), b"")
            self.assertEqual(plat.pread(fd, 0, 0), b"")
            with self.assertRaises(ValueError):
                plat.pread(fd, -1, 0)
        finally:
            os.close(fd)

    def test_fsync_family(self):
        name = self.write("kalıcı", b"x")
        fd = plat.open_fd(name, os.O_RDONLY)
        try:
            plat.fsync(fd)                                          # read-only descriptor
        finally:
            os.close(fd)
        with open(name, "ab") as stream:
            stream.write(b"y")
            stream.flush()
            plat.fsync(stream)
        plat.fsync_file(name)
        read_end, write_end = os.pipe()
        try:
            plat.fsync(write_end)                                   # pipes: nothing to flush
        finally:
            os.close(read_end)
            os.close(write_end)
        plat.fsync_dir(self.base)
        plat.fsync_tree(self.base)
        with self.assertRaises(FileNotFoundError):
            plat.fsync_dir(self.path("yok"))
        with self.assertRaises(NotADirectoryError):
            plat.fsync_dir(name)

    def test_mkstemp_private(self):
        fd, name = plat.mkstemp_private(self.base, prefix=".geçici-", suffix=".json")
        try:
            os.write(fd, b"{}\n")
        finally:
            os.close(fd)
        self.assertRegex(os.path.basename(name), r"^\.geçici-[0-9a-f]{8}\.json$")
        self.assertTrue(plat.is_private(name, exact_file_mode=True))
        with self.assertRaises(ValueError):
            plat.mkstemp_private(self.base, prefix="a/b")


# --------------------------------------------------------------------------- replace / unlink / rmtree


class ReplaceTest(TempDirTest):
    def test_replace_over_share_delete_reader(self):
        target = self.write("hedef ş.json", b"old-bytes")
        source = self.write("kaynak.tmp", b"new")
        reader = plat.open_read(target)
        try:
            self.assertEqual(reader.read(3), b"old")
            plat.replace(source, target)
            self.assertEqual(reader.read(), b"-bytes")              # the old file stays readable
        finally:
            reader.close()
        self.assertFalse(os.path.exists(source))
        with open(target, "rb") as stream:
            self.assertEqual(stream.read(), b"new")

    def test_replace_retries_while_a_plain_reader_holds_the_target(self):
        target = self.write("tutulan", b"old")
        source = self.write("yeni.tmp", b"new")
        reader = open(target, "rb")                                 # no FILE_SHARE_DELETE
        timer = threading.Timer(0.15, reader.close)
        timer.start()
        started = time.monotonic()
        try:
            plat.replace(source, target)
        finally:
            timer.join()
            reader.close()
        self.assertGreaterEqual(time.monotonic() - started, 0.1)
        with open(target, "rb") as stream:
            self.assertEqual(stream.read(), b"new")

    def test_replace_gives_up_after_the_budget(self):
        target = self.write("kilitli", b"old")
        source = self.write("yeni.tmp", b"new")
        with open(target, "rb"):
            started = time.monotonic()
            with self.assertRaises(PermissionError):
                plat.replace(source, target, timeout=0.3)
            elapsed = time.monotonic() - started
        self.assertGreaterEqual(elapsed, 0.25)
        self.assertLess(elapsed, 2.0)
        self.assertTrue(os.path.exists(source))
        with open(target, "rb") as stream:
            self.assertEqual(stream.read(), b"old")

    def test_replace_retries_a_held_source_and_refuses_directories(self):
        target = self.write("hedef", b"old")
        source = self.write("kaynak", b"new")
        holder = open(source, "rb")
        timer = threading.Timer(0.15, holder.close)
        timer.start()
        try:
            plat.replace(source, target)
        finally:
            timer.join()
            holder.close()
        folder = self.path("klasör")
        os.mkdir(folder)
        started = time.monotonic()
        with self.assertRaises(IsADirectoryError):
            plat.replace(target, folder)
        self.assertLess(time.monotonic() - started, 0.5)
        with self.assertRaises(FileNotFoundError):
            plat.replace(self.path("yok"), target)
        moved = self.path("taşınan klasör")
        plat.replace(folder, moved)                                 # directories may move to new names
        self.assertTrue(os.path.isdir(moved))

    def test_rename_noreplace(self):
        source = self.write("kaynak", b"new")
        target = self.write("var olan", b"old")
        with self.assertRaises(FileExistsError):
            plat.rename_noreplace(source, target)
        with open(source, "rb") as stream:
            self.assertEqual(stream.read(), b"new")
        with open(target, "rb") as stream:
            self.assertEqual(stream.read(), b"old")
        fresh = self.path("yeni ad")
        plat.link_noreplace(source, fresh)
        self.assertFalse(os.path.exists(source))
        stage = self.path(".aşama.partial")
        os.mkdir(stage)
        self.write(os.path.join(".aşama.partial", "manifest.json"), b"{}")
        plat.publish_dir_noreplace(stage, self.path("yedek"))
        self.assertTrue(os.path.isfile(self.path("yedek", "manifest.json")))
        os.mkdir(stage)
        with self.assertRaises(FileExistsError):
            plat.publish_dir_noreplace(stage, self.path("yedek"))
        self.assertIs(plat.link_noreplace, plat.rename_noreplace)

    def test_unlink(self):
        name = self.write("silinecek ğ", b"old")
        reader = plat.open_read(name)
        try:
            plat.unlink(name)
            self.assertFalse(os.path.lexists(name))
            with open(name, "wb") as stream:                         # the name is free at once
                stream.write(b"new")
            self.assertEqual(reader.read(), b"old")
        finally:
            reader.close()
        plat.unlink(name)
        with self.assertRaises(FileNotFoundError):
            plat.unlink(name)
        plat.unlink(name, missing_ok=True)
        plat.unlink(self.path("yok", "alt"), missing_ok=True)
        folder = self.path("klasör")
        os.mkdir(folder)
        with self.assertRaises(IsADirectoryError):
            plat.unlink(folder)
        link = self.junction(folder, self.path("bağlantı"))
        self.write(os.path.join("klasör", "iç"), b"keep")
        plat.unlink(link)
        self.assertFalse(os.path.lexists(link))
        self.assertTrue(os.path.isfile(os.path.join(folder, "iç")))
        readonly = self.write("salt okunur", b"x")
        os.chmod(readonly, stat.S_IREAD)
        plat.unlink(readonly)
        self.assertFalse(os.path.lexists(readonly))
        held = self.write("tutulan", b"x")
        with open(held, "rb"), retry_budget(0.2), self.assertLogs("radios.platform", "WARNING"):
            self.assertFalse(plat.unlink_quietly(held))                # reported, never raised
        self.assertTrue(plat.unlink_quietly(held))
        self.assertTrue(plat.unlink_quietly(held))

    def test_rmtree_never_leaves_the_tree(self):
        victim = self.path("kurban")
        os.mkdir(victim)
        precious = self.write(os.path.join("kurban", "değerli.txt"), b"keep")
        tree = self.path("ağaç")
        os.makedirs(os.path.join(tree, "a", "b ç"))
        self.write(os.path.join("ağaç", "a", "b ç", "dosya ş.txt"), b"x")
        readonly = self.write(os.path.join("ağaç", "a", "salt okunur"), b"x")
        os.chmod(readonly, stat.S_IREAD)
        held = self.write(os.path.join("ağaç", "açık.bin"), b"x")
        _winapi.CreateJunction(victim, os.path.join(tree, "a", "kaçış"))
        reader = plat.open_read(held)
        try:
            self.assertTrue(plat.rmtree(tree))
        finally:
            reader.close()
        self.assertFalse(os.path.lexists(tree))
        with open(precious, "rb") as stream:
            self.assertEqual(stream.read(), b"keep")

    def test_rmtree_errors(self):
        target = self.path("hedef")
        os.mkdir(target)
        self.write(os.path.join("hedef", "x"))
        link = self.junction(target, self.path("bağlantı"))
        with self.assertRaises(OSError) as caught:
            plat.rmtree(link)
        self.assertEqual(caught.exception.errno, errno.ELOOP)
        self.assertTrue(os.path.isfile(os.path.join(target, "x")))
        with self.assertRaises(FileNotFoundError):
            plat.rmtree(self.path("yok"))
        self.assertTrue(plat.rmtree(self.path("yok"), ignore_errors=True))
        with self.assertRaises(NotADirectoryError):
            plat.rmtree(os.path.join(target, "x"))
        blocked = self.path("engelli")
        os.mkdir(blocked)
        held = self.write(os.path.join("engelli", "tutulan"), b"x")
        with open(held, "rb"):
            with self.assertRaises(PermissionError):
                plat.rmtree(blocked, timeout=0.3)
            with self.assertLogs("radios.platform", "WARNING"):
                self.assertFalse(plat.rmtree(blocked, timeout=0.3, ignore_errors=True))
            with retry_budget(0.2), self.assertLogs("radios.platform", "WARNING"):
                self.assertFalse(plat.rmtree_quietly(blocked))
        self.assertTrue(plat.rmtree(blocked))

    def test_temporary_directory(self):
        with plat.temporary_directory(prefix=".geçici-", dir=self.base) as path:
            self.assertTrue(plat.is_private(path, directory=True))
            self.assertRegex(os.path.basename(path), r"^\.geçici-[0-9a-f]{8}$")
            self.write(os.path.join(os.path.basename(path), "iç"), b"x")
        self.assertFalse(os.path.exists(path))
        holder = None
        with self.assertRaises(KeyError), self.assertLogs("radios.platform", "WARNING"):
            with retry_budget(0.1), plat.temporary_directory(dir=self.base) as path:
                holder = open(os.path.join(path, "tutulan"), "wb")
                raise KeyError("body")                               # never masked by the cleanup
        holder.close()
        self.assertTrue(plat.rmtree(path))


# --------------------------------------------------------------------------- locks


LOCK_HOLDER = """
import os
fd = plat.open_fd(sys.argv[2], os.O_RDWR | os.O_CREAT, 0o600)
plat.flock(fd, plat.LOCK_EX if sys.argv[3] == "EX" else plat.LOCK_SH)
print("locked", flush=True)
sys.stdin.readline()
plat.flock(fd, plat.LOCK_UN)
print("released", flush=True)
sys.stdin.readline()
"""


class LockTest(TempDirTest):
    def open_lock(self, name="kilit ş.lock"):
        fd = plat.open_fd(self.path(name), os.O_RDWR | os.O_CREAT, 0o600)
        self.addCleanup(_close_quietly, fd)
        return fd

    def test_shared_exclusive_matrix_in_one_process(self):
        first, second, third = self.open_lock(), self.open_lock(), self.open_lock()
        plat.flock(first, plat.LOCK_SH | plat.LOCK_NB)
        plat.flock(second, plat.LOCK_SH | plat.LOCK_NB)
        with self.assertRaises(BlockingIOError) as caught:
            plat.flock(third, plat.LOCK_EX | plat.LOCK_NB)
        self.assertEqual(caught.exception.errno, errno.EAGAIN)
        plat.unlock(first)
        plat.unlock_and_close(second)
        plat.flock(third, plat.LOCK_EX | plat.LOCK_NB)
        plat.flock(third, plat.LOCK_EX | plat.LOCK_NB)              # re-locking is idempotent
        with self.assertRaises(BlockingIOError):
            plat.flock(first, plat.LOCK_SH | plat.LOCK_NB)
        plat.flock(third, plat.LOCK_SH | plat.LOCK_NB)              # EX -> SH conversion
        plat.flock(first, plat.LOCK_SH | plat.LOCK_NB)
        plat.unlock(first)
        plat.flock(third, plat.LOCK_EX | plat.LOCK_NB)              # SH -> EX conversion
        plat.flock(third, plat.LOCK_UN)
        plat.flock(third, plat.LOCK_UN)                             # unlocking twice is harmless
        for bad in (0, plat.LOCK_SH | plat.LOCK_EX, plat.LOCK_NB, 16, plat.LOCK_UN | plat.LOCK_EX):
            with self.assertRaises(ValueError):
                plat.flock(third, bad)

    def test_region_matches_a_raw_lockfileex_holder(self):
        # A lock taken with plain LockFileEx on the header's region (what the C layer does)
        # conflicts with the shim and vice versa; the old proposal 0x7FFFFFFF00000000 does not.
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.LockFileEx.argtypes = (ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p)
        kernel32.UnlockFileEx.argtypes = (ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p)

        class Overlapped(ctypes.Structure):
            _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t), ("Offset", wt.DWORD),
                        ("OffsetHigh", wt.DWORD), ("hEvent", ctypes.c_void_p)]

        def raw(fd, offset, lock, flags=0x3):                       # EXCLUSIVE | FAIL_IMMEDIATELY
            overlapped = Overlapped(0, 0, offset & 0xFFFFFFFF, offset >> 32, None)
            handle = msvcrt.get_osfhandle(fd)
            if lock:
                return bool(kernel32.LockFileEx(handle, flags, 0, 1, 0, ctypes.byref(overlapped)))
            return bool(kernel32.UnlockFileEx(handle, 0, 1, 0, ctypes.byref(overlapped)))

        c_side, python_side = self.open_lock(), self.open_lock()
        self.assertTrue(raw(c_side, 0x4000000000000000, True))
        with self.assertRaises(BlockingIOError):
            plat.flock(python_side, plat.LOCK_SH | plat.LOCK_NB)
        self.assertTrue(raw(c_side, 0x4000000000000000, False))
        plat.flock(python_side, plat.LOCK_SH | plat.LOCK_NB)
        self.assertFalse(raw(c_side, 0x4000000000000000, True))     # shim SH blocks a raw EX
        self.assertTrue(raw(c_side, 0x4000000000000000, True, flags=0x1))   # ... but not a raw SH
        self.assertTrue(raw(c_side, 0x4000000000000000, False))
        self.assertTrue(raw(c_side, 0x7FFFFFFF00000000, True))      # another region never conflicts
        self.assertTrue(raw(c_side, 0x7FFFFFFF00000000, False))
        plat.unlock(python_side)

    def test_region_never_blocks_content_io(self):
        holder, other = self.open_lock(), self.open_lock()
        plat.flock(holder, plat.LOCK_EX)
        os.write(other, b"content")
        self.assertEqual(plat.pread(other, 7, 0), b"content")
        with open(self.path("kilit ş.lock"), "rb") as stream:
            self.assertEqual(stream.read(), b"content")

    def test_lock_helpers(self):
        holder, waiter = self.open_lock(), self.open_lock()
        plat.lock(holder)                                           # exclusive by default
        started = time.monotonic()
        with self.assertRaises(TimeoutError):
            plat.lock(waiter, exclusive=False, timeout=0.2)
        self.assertGreaterEqual(time.monotonic() - started, 0.18)
        with self.assertRaises(BlockingIOError):
            plat.lock(waiter, shared=True, blocking=False)
        with self.assertRaises(ValueError):
            plat.lock(waiter, exclusive=True, shared=True)
        with self.assertRaises(ValueError):
            plat.lock(waiter, blocking=False, timeout=1)
        timer = threading.Timer(0.2, plat.unlock, (holder,))
        timer.start()
        started = time.monotonic()
        plat.lock(waiter, exclusive=True)                           # blocking wait for the thread
        timer.join()
        self.assertGreaterEqual(time.monotonic() - started, 0.15)
        plat.unlock(waiter)
        with plat.locked_file(self.path("bağlam.lock"), exclusive=True, create=True) as fd:
            probe = plat.open_fd(self.path("bağlam.lock"), os.O_RDONLY)
            try:
                with self.assertRaises(BlockingIOError):
                    plat.flock(probe, plat.LOCK_SH | plat.LOCK_NB)
            finally:
                os.close(probe)
            self.assertTrue(plat.is_private(fd))
        with open(self.path("bağlam.lock"), "rb") as stream:         # file objects and read-only handles
            plat.flock(stream, plat.LOCK_EX | plat.LOCK_NB)
            plat.flock(stream, plat.LOCK_UN)

    def test_locks_across_processes(self):
        path = self.path("süreçler arası.lock")
        mine = plat.open_fd(path, os.O_RDWR | os.O_CREAT, 0o600)
        self.addCleanup(_close_quietly, mine)
        exclusive = self.child(LOCK_HOLDER, path, "EX")
        self.assertEqual(exclusive.expect(), "locked")
        with self.assertRaises(BlockingIOError):
            plat.flock(mine, plat.LOCK_SH | plat.LOCK_NB)
        exclusive.send("release")
        self.assertEqual(exclusive.expect(), "released")
        plat.flock(mine, plat.LOCK_SH | plat.LOCK_NB)
        plat.flock(mine, plat.LOCK_UN)
        exclusive.send("exit")
        self.assertEqual(exclusive.process.wait(30), 0)
        shared = self.child(LOCK_HOLDER, path, "SH")
        self.assertEqual(shared.expect(), "locked")
        plat.flock(mine, plat.LOCK_SH | plat.LOCK_NB)
        plat.flock(mine, plat.LOCK_UN)
        with self.assertRaises(BlockingIOError):
            plat.flock(mine, plat.LOCK_EX | plat.LOCK_NB)
        shared.process.kill()                                       # a dead holder releases its lock
        shared.process.wait(30)
        deadline = time.monotonic() + 10
        while True:
            try:
                plat.flock(mine, plat.LOCK_EX | plat.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.02)


def set_sddl(path, sddl):
    """Test helper: replace owner and DACL of path with the given SDDL (protected)."""
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    convert = advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW
    convert.argtypes = (wt.LPCWSTR, wt.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p)
    get_dacl = advapi32.GetSecurityDescriptorDacl
    get_dacl.argtypes = (ctypes.c_void_p, ctypes.POINTER(wt.BOOL), ctypes.POINTER(ctypes.c_void_p),
                         ctypes.POINTER(wt.BOOL))
    get_owner = advapi32.GetSecurityDescriptorOwner
    get_owner.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wt.BOOL))
    set_info = advapi32.SetNamedSecurityInfoW
    set_info.restype = wt.DWORD
    set_info.argtypes = (wt.LPWSTR, ctypes.c_int, wt.DWORD, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                         ctypes.c_void_p)
    descriptor = ctypes.c_void_p()
    if not convert(sddl, 1, ctypes.byref(descriptor), None):
        raise ctypes.WinError(ctypes.get_last_error())
    present, defaulted, dacl, owner = wt.BOOL(), wt.BOOL(), ctypes.c_void_p(), ctypes.c_void_p()
    get_dacl(descriptor, ctypes.byref(present), ctypes.byref(dacl), ctypes.byref(defaulted))
    get_owner(descriptor, ctypes.byref(owner), ctypes.byref(defaulted))
    code = set_info(os.fspath(path), 1, 0x1 | 0x4 | 0x80000000, owner, None, dacl, None)
    ctypes.WinDLL("kernel32").LocalFree(descriptor)
    if code:
        raise ctypes.WinError(code)


def _close_quietly(fd):
    with contextlib.suppress(OSError):
        os.close(fd)


# --------------------------------------------------------------------------- privacy


class PrivacyTest(TempDirTest):
    def sddl(self, directory):
        template = plat.PRIVATE_DIR_SDDL if directory else plat.PRIVATE_FILE_SDDL
        return template.format(sid=plat.current_user_sid())

    @staticmethod
    def applied(path):
        # SetSecurityInfo marks a repaired DACL auto-inherited ("D:PAI"); the ACEs are the same.
        return plat.security_sddl(path).replace("D:PAI(", "D:P(")

    def test_private_directory_and_file_round_trip(self):
        folder = self.path("özel klasör")
        plat.mkdir_private(folder)
        self.assertEqual(plat.security_sddl(folder), self.sddl(True))
        self.assertTrue(plat.is_private(folder, directory=True))
        self.assertTrue(plat.owned_by_current_user(folder))
        self.assertTrue(plat.private_dir_ok(folder))
        self.assertTrue(plat.acl_supported(folder))
        self.assertFalse(plat.is_private(folder, exact_file_mode=True))
        name = os.path.join(folder, "kimlik ş.json")
        fd = plat.open_fd(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, private=True)
        os.close(fd)
        self.assertEqual(plat.security_sddl(name), self.sddl(False))
        self.assertTrue(plat.is_private(name, exact_file_mode=True))
        self.assertFalse(plat.is_private(name, directory=True))
        derived = self.path("mod 600")
        os.close(plat.open_fd(derived, os.O_WRONLY | os.O_CREAT, 0o600))     # (mode & 0o077) == 0 -> private
        self.assertEqual(plat.security_sddl(derived), self.sddl(False))
        with open(name, "rb") as stream:
            self.assertTrue(plat.is_private(stream))
            self.assertTrue(plat.is_private(stream.fileno(), exact_file_mode=True))
        temporary = plat.mkdtemp_private(prefix=".aşama-", suffix=".partial", dir=self.base)
        self.assertRegex(os.path.basename(temporary), r"^\.aşama-[0-9a-f]{8}\.partial$")
        self.assertEqual(plat.security_sddl(temporary), self.sddl(True))
        nested = self.path("üst", "orta", "yaprak")
        plat.mkdir_private(nested, parents=True)
        self.assertTrue(plat.is_private(nested, directory=True))
        with self.assertRaises(FileNotFoundError):
            plat.is_private(self.path("yok"))

    def test_public_objects_are_rejected_and_repaired(self):
        name = self.write("herkese açık.txt", b"secret")
        tools_platform.make_public_for_tests(name)                 # BUILTIN\Users: read
        self.assertFalse(plat.is_private(name))
        self.assertTrue(plat.owned_by_current_user(name))
        self.assertEqual(plat.foreign_access(name), ["S-1-5-32-545"])
        plat.make_private(name)
        self.assertTrue(plat.is_private(name, exact_file_mode=True))
        self.assertEqual(self.applied(name), self.sddl(False))
        plat.make_private(name)                                     # no-op when already private
        folder = self.path("ortak")
        plat.mkdir_private(folder)
        tools_platform.make_public_for_tests(folder, sid="S-1-1-0", rights=0x1F01FF, inheritance=3)  # Everyone:F
        self.assertFalse(plat.is_private(folder, directory=True))
        self.assertFalse(plat.private_dir_ok(folder))
        plat.mkdir_private(folder, exist_ok=True)                   # verified and repaired
        self.assertEqual(self.applied(folder), self.sddl(True))
        with self.assertRaises(FileExistsError):
            plat.mkdir_private(folder)
        with self.assertRaises(FileExistsError):
            plat.mkdir_private(name, exist_ok=True)
        with self.assertRaises(IsADirectoryError):
            plat.make_private(folder, directory=False)
        link = self.junction(folder, self.path("bağlantı"))
        with self.assertRaises(OSError) as caught:
            plat.mkdir_private(link, exist_ok=True)
        self.assertEqual(caught.exception.errno, errno.ELOOP)
        with self.assertRaises(OSError) as caught:
            plat.make_private(link)
        self.assertEqual(caught.exception.errno, errno.ELOOP)
        self.assertFalse(plat.private_dir_ok(link))

    def test_tolerated_and_inherit_only_grants(self):
        folder = self.path("izinler")
        plat.mkdir_private(folder)
        # READ_CONTROL | SYNCHRONIZE | FILE_READ_ATTRIBUTES for Users: like being able to stat a 0600 file
        tools_platform.make_public_for_tests(folder, rights=0x20000 | 0x100000 | 0x80)
        self.assertTrue(plat.is_private(folder, directory=True))
        # inherit-only read for Users: not effective on the folder, but on new children
        tools_platform.make_public_for_tests(folder, rights=0x120089, inheritance=0x8 | 0x1 | 0x2)
        self.assertTrue(plat.is_private(folder, directory=True))
        child = os.path.join(folder, "yeni")
        with open(child, "wb"):
            pass
        self.assertFalse(plat.is_private(child))
        self.assertIn("S-1-5-32-545", plat.foreign_access(child))

    def test_make_private_restores_the_canonical_form(self):
        user = plat.current_user_sid()
        name = self.write("salt okuma izni", b"x")
        # owner-only, but the user may only read: private, yet not what fchmod(0600) gives
        set_sddl(name, f"O:{user}D:P(A;;FR;;;{user})(A;;FA;;;SY)")
        self.assertTrue(plat.is_private(name))
        plat.make_private(name)
        self.assertEqual(self.applied(name), self.sddl(False))
        set_sddl(name, f"O:{user}D:P(D;;FW;;;{user})(A;;FA;;;{user})(A;;FA;;;SY)")   # a deny ACE for the user
        plat.make_private(name)
        self.assertEqual(self.applied(name), self.sddl(False))

    def test_null_dacl_is_never_private(self):
        name = self.write("boş dacl", b"x")
        advapi32 = ctypes.WinDLL("advapi32")
        set_info = advapi32.SetNamedSecurityInfoW
        set_info.restype = wt.DWORD
        set_info.argtypes = (wt.LPWSTR, ctypes.c_int, wt.DWORD, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                             ctypes.c_void_p)
        self.assertEqual(set_info(name, 1, 0x4 | 0x80000000, None, None, None, None), 0)  # protected NULL DACL
        self.assertFalse(plat.is_private(name))
        self.assertEqual(plat.foreign_access(name), ["S-1-1-0"])
        plat.make_private(name)
        self.assertTrue(plat.is_private(name))


# --------------------------------------------------------------------------- process identity and stop events


SLEEPER = """
print("ready", flush=True)
sys.stdin.readline()
"""

STOP_WAITER = """
import os
with plat.StopRequest() as stop:
    print("ready", os.getpid(), flush=True)
    stopped = stop.wait(30)
print("stopping" if stopped else "timeout", flush=True)
sys.exit(0 if stopped else 3)
"""


def _creation_epoch(ticks):
    return (ticks - 116444736000000000) / 10_000_000


class ProcessTest(TempDirTest):
    def test_identity_of_this_process(self):
        identity = plat.process_identity(os.getpid())
        self.assertIsNotNone(identity)
        self.assertEqual(identity, plat.process_identity(os.getpid()))
        self.assertEqual(identity[0], os.getpid())
        started = _creation_epoch(identity[1])
        self.assertLessEqual(started, time.time() + 1)
        self.assertGreaterEqual(started, plat.boot_time() - 60)
        with plat.PinnedProcess.open(os.getpid()) as pinned:
            self.assertEqual(pinned.identity, identity)
            self.assertTrue(pinned.alive())
            self.assertIsNone(pinned.exit_code())
        for bad in (0, 4, -1, True, "12", 2 ** 33, None, 1.5):
            self.assertIsNone(plat.process_identity(bad), bad)

    def test_identity_of_a_child_and_mismatch(self):
        args = ("Şarkı listesi.m3u8", "a b", 'tırnak"lı', "son\\")
        child = self.child(SLEEPER, *args)
        self.assertEqual(child.expect(), "ready")
        identity = plat.process_identity(child.pid)
        self.assertIsNotNone(identity)
        self.assertNotEqual(identity, (child.pid, identity[1] + 1))
        info = plat.process_info(child.pid)
        self.assertEqual((info.pid, info.creation_ticks), identity)
        self.assertTrue(plat.path_equal(info.image_path, plat.python_executable()))
        self.assertEqual(info.argv[-len(args):], list(args))
        self.assertEqual(info.argv[1:6], list(plat.PYTHON_FLAGS))
        pinned = plat.PinnedProcess.open(child.pid)
        self.addCleanup(pinned.close)
        self.assertFalse(pinned.wait(0.05))
        child.send("bye")
        self.assertTrue(pinned.wait(30))
        self.assertEqual(pinned.exit_code(), 0)
        self.assertFalse(pinned.alive())
        self.assertFalse(pinned.request_stop())
        self.assertIsNone(plat.process_identity(child.pid))         # exited, although handles still exist
        self.assertIsNone(plat.process_info(child.pid))

    def test_terminate_through_a_pinned_handle(self):
        child = self.child(SLEEPER)
        self.assertEqual(child.expect(), "ready")
        with plat.PinnedProcess.open(child.pid, terminate=True) as pinned:
            pinned.terminate(7)
            self.assertTrue(pinned.wait(30))
            self.assertEqual(pinned.exit_code(), 7)
            pinned.terminate(8)                                     # already gone: no error

    def test_stop_event_in_process(self):
        with plat.StopEvent() as event:
            self.assertEqual(event.name, plat.stop_event_name())
            self.assertFalse(event.is_set())
            self.assertTrue(plat.request_stop(os.getpid()))
            self.assertTrue(event.is_set())
            self.assertTrue(event.wait(0))
            event.reset()
            self.assertFalse(event.wait(0.05))
        self.assertFalse(plat.request_stop(os.getpid()))            # channel closed with its last handle
        with plat.StopRequest(console=False) as stop:
            self.assertFalse(stop.event.is_set())
            started = time.monotonic()
            self.assertTrue(plat.request_stop(os.getpid()))
            self.assertTrue(stop.wait(5))
            self.assertLess(time.monotonic() - started, 2)
        with plat.create_stop_event() as fresh:                     # a new channel never starts signalled
            self.assertFalse(fresh.is_set())

    def test_stop_event_reaches_a_child(self):
        quiet = self.child(SLEEPER)
        self.assertEqual(quiet.expect(), "ready")
        self.assertFalse(plat.request_stop(quiet.pid))              # no stop channel: refused, not killed
        self.assertIsNone(quiet.process.poll())
        child = self.child(STOP_WAITER)
        line = child.expect()
        self.assertEqual(line, f"ready {child.pid}")
        with plat.PinnedProcess.open(child.pid) as pinned:
            started = time.monotonic()
            self.assertTrue(pinned.request_stop())
            self.assertEqual(child.expect(), "stopping")
            self.assertTrue(pinned.wait(30))
            self.assertEqual(pinned.exit_code(), 0)
            self.assertLess(time.monotonic() - started, 10)


# --------------------------------------------------------------------------- child processes


CONSOLE_REPORT = CONSOLE_PROBE + """
sys.stdout.write(json.dumps({"window": bool(window), "visible": bool(window and user32.IsWindowVisible(window)),
                             "text": "Şarkı ğüşıöç İ", "argv": sys.argv[2:], "utf8": sys.flags.utf8_mode,
                             "cwd": os.getcwd(), "environment": os.environ.get("PYTHONIOENCODING")},
                            ensure_ascii=False) + "\\n")
"""

INTERMEDIATE = """
import os
grandchild = sys.argv[3]
log = plat.open_fd(sys.argv[2], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600, append_only=True)
process = plat.spawn_detached(plat.python_child_argv("-c", grandchild, sys.argv[1]), log_fd=log,
                              env=plat.python_child_env())
os.close(log)
print(process.pid, flush=True)
"""

DETACHED = PRELUDE + CONSOLE_PROBE + """
with plat.StopRequest(console=False) as stop:
    print(json.dumps({"pid": os.getpid(), "visible": bool(window and user32.IsWindowVisible(window)),
                      "text": "Şarkı"}, ensure_ascii=False), flush=True)
    stopped = stop.wait(60)
print("stopping" if stopped else "timeout", flush=True)
sys.exit(0 if stopped else 5)
"""


class SpawnTest(TempDirTest):
    def test_python_child_argv_and_environment(self):
        argv = plat.python_child_argv("-m", "sdbase.cli")
        self.assertEqual(argv[1:], ["-E", "-s", "-B", "-X", "utf8", "-m", "sdbase.cli"])
        bundled = ROOT / "runtime" / "python" / "python.exe"
        if bundled.is_file():
            self.assertTrue(plat.path_equal(os.path.realpath(argv[0]), os.path.realpath(bundled)))
        environment = plat.python_child_env({"PythonPath": "x", "PYTHONHOME": "y", "pythonutf8": "0", "KEEP": "1"})
        self.assertEqual(environment, {"KEEP": "1", "PYTHONUTF8": "1", "PYTHONDONTWRITEBYTECODE": "1",
                                       "PYTHONIOENCODING": "utf-8"})
        self.assertNotIn("PYTHONPATH", {key.upper() for key in plat.python_child_env()})

    def test_child_without_console_window_speaks_utf8(self):
        args = ["Şarkı listesi", "a b", 'tırnak"', "İĞÜ\\"]
        process = plat.popen_no_window(plat.python_child_argv("-c", CONSOLE_REPORT, SHIM, *args),
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
                                       cwd=self.base, env=plat.python_child_env())
        output, errors = process.communicate(timeout=60)
        self.assertEqual(process.returncode, 0, errors.decode("utf-8", "replace"))
        report = json.loads(output.decode("utf-8"))                 # strict UTF-8
        self.assertFalse(report["visible"])
        self.assertEqual(report["text"], "Şarkı ğüşıöç İ")
        self.assertEqual(report["argv"], args)
        self.assertEqual(report["utf8"], 1)
        self.assertEqual(report["environment"], "utf-8")
        self.assertTrue(plat.path_equal(report["cwd"], self.base))

    def test_spawn_argument_checks(self):
        python = plat.python_executable()
        with self.assertRaises(ValueError):
            plat.popen_no_window(["python.exe", "-c", "pass"])
        with self.assertRaises(ValueError):
            plat.spawn_detached([self.write("x.cmd", b"@echo off"), "a"])
        with self.assertRaises(ValueError):
            plat.popen_no_window([python, "-c", "pass"], shell=True)
        with self.assertRaises(ValueError):
            plat.popen_no_window([python, "-c", "pass"], close_fds=False)
        with self.assertRaises(ValueError):
            plat.popen_no_window([python, "-c", "pass"], creationflags=subprocess.DETACHED_PROCESS)
        with self.assertRaises(TypeError):
            plat.popen_no_window(python)
        with self.assertRaises(ValueError):
            plat.popen_no_window(['C:\\a"b\\python.exe', "-c", "pass"])
        with self.assertRaises(ValueError):
            plat.spawn_detached([python, "-c", "pass"], log_fd=1, stdout=subprocess.DEVNULL)

    def test_detached_child_outlives_its_parent_and_never_inherits_pipes(self):
        log = self.path("servis günlüğü.log")
        with open(log, "wb") as stream:
            stream.write(b"first line\n")
        intermediate = plat.popen_no_window(plat.python_child_argv("-c", PRELUDE + textwrap.dedent(INTERMEDIATE),
                                                                   SHIM, log, DETACHED),
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
                                            env=plat.python_child_env())
        # communicate() only returns at EOF: had the detached grandchild inherited our pipe,
        # this would block until it exits.
        output, errors = intermediate.communicate(timeout=60)
        self.assertEqual(intermediate.returncode, 0, errors.decode("utf-8", "replace"))
        grandchild = int(output.decode("ascii").strip())
        pinned = plat.PinnedProcess.open(grandchild)
        self.assertIsNotNone(pinned)
        self.addCleanup(pinned.close)
        self.addCleanup(lambda: pinned.alive() and plat.request_stop(grandchild))
        self.assertTrue(pinned.alive())                             # outlived the intermediate
        deadline = time.monotonic() + 30
        report = None
        while report is None and time.monotonic() < deadline:
            with open(log, "rb") as stream:
                lines = stream.read().decode("utf-8").splitlines()
            report = next((json.loads(line) for line in lines if line.startswith("{")), None)
            time.sleep(0.05)
        self.assertIsNotNone(report, lines)
        self.assertEqual(report["pid"], grandchild)
        self.assertFalse(report["visible"])
        self.assertEqual(report["text"], "Şarkı")
        self.assertTrue(plat.request_stop(grandchild))
        self.assertTrue(pinned.wait(30))
        self.assertEqual(pinned.exit_code(), 0)
        with open(log, "rb") as stream:
            lines = stream.read().decode("utf-8").splitlines()
        self.assertEqual(lines[0], "first line")                    # appended, not overwritten
        self.assertEqual(lines[-1], "stopping")

    def test_lock_handles_never_leak_into_children(self):
        path = self.path("sızıntı.lock")
        holder = plat.open_fd(path, os.O_RDWR | os.O_CREAT, 0o600)
        plat.flock(holder, plat.LOCK_EX)
        child = self.child(SLEEPER)
        self.assertEqual(child.expect(), "ready")
        detached = plat.spawn_detached(plat.python_child_argv("-c", PRELUDE + textwrap.dedent(STOP_WAITER), SHIM),
                                       env=plat.python_child_env())
        self.addCleanup(lambda: detached.poll() is None and detached.kill())
        deadline = time.monotonic() + 30
        while plat.process_identity(detached.pid) is None and time.monotonic() < deadline:
            time.sleep(0.05)
        plat.unlock_and_close(holder)
        probe = plat.open_fd(path, os.O_RDWR)
        try:
            plat.flock(probe, plat.LOCK_EX | plat.LOCK_NB)          # nobody inherited the locked handle
        finally:
            plat.unlock_and_close(probe)
        deadline = time.monotonic() + 30
        while not plat.request_stop(detached.pid) and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertEqual(detached.wait(30), 0)


# --------------------------------------------------------------------------- space, access, links, time zones, tools


class MiscTest(TempDirTest):
    def test_disk_usage_and_access(self):
        total, available = plat.disk_usage(self.base)
        self.assertGreater(total, 0)
        self.assertGreater(available, 0)
        self.assertLessEqual(available, total)
        self.assertEqual(total, shutil.disk_usage(self.base).total)
        file_total, file_available = plat.disk_usage(self.write("dosya"))   # a file: its volume
        self.assertEqual(file_total, total)
        self.assertLess(abs(file_available - plat.disk_free(self.base)), 1 << 30)
        with self.assertRaises(FileNotFoundError):
            plat.disk_usage(self.path("yok"))
        self.assertTrue(plat.dir_writable(self.base))
        self.assertFalse(plat.dir_writable(self.path("yok")))
        self.assertEqual(sorted(os.listdir(self.base)), ["dosya"])  # the probe file is gone
        self.assertIsInstance(plat.long_paths_enabled(), bool)

    def test_sync_tree(self):
        tree = self.path("kopya ağacı")
        os.makedirs(os.path.join(tree, "alt ç", "derin"))
        for relative in ("a.bin", os.path.join("alt ç", "b ş.bin"), os.path.join("alt ç", "derin", "c")):
            with open(os.path.join(tree, relative), "wb") as stream:
                stream.write(b"veri")
        self.assertEqual(plat.sync_tree(tree), 3)
        self.assertEqual(plat.sync_tree(os.path.join(tree, "alt ç", "derin")), 1)
        with self.assertRaises(NotADirectoryError):
            plat.sync_tree(os.path.join(tree, "a.bin"))
        with self.assertRaises(FileNotFoundError):
            plat.sync_tree(self.path("yok"))

    def test_links(self):
        target = self.path("gerçek")
        os.mkdir(target)
        link = self.junction(target, self.path("kısayol"))
        self.assertTrue(plat.is_link(link))
        self.assertTrue(plat.is_link_like(link))
        self.assertTrue(plat.is_link(os.lstat(link)))
        self.assertFalse(plat.is_link(target))
        self.assertFalse(plat.is_link(self.path("yok")))
        entries = {entry.name: entry for entry in os.scandir(self.base)}
        self.assertTrue(plat.entry_is_link(entries["kısayol"]))
        self.assertTrue(plat.entry_is_link_like(entries["kısayol"]))
        self.assertFalse(plat.entry_is_link(entries["gerçek"]))
        self.assertTrue(plat.has_link_component(os.path.join(link, "alt", "dosya")))
        self.assertFalse(plat.has_link_component(os.path.join(target, "alt", "dosya")))
        with open(os.path.join(target, "iç"), "wb") as stream:
            stream.write(b"x")
        with self.assertRaises(OSError) as caught:
            plat.sync_tree(link)
        self.assertEqual(caught.exception.errno, errno.ELOOP)
        self.assertEqual(plat.sync_tree(self.base), 1)              # the junction is not entered

    def test_time_zones(self):
        self.assertEqual(plat.canonical_timezone("Europe/Istanbul"), "Europe/Istanbul")
        self.assertEqual(plat.canonical_timezone("UTC"), "UTC")
        for bad in ("europe/istanbul", "Europe/Istanbul ", "Europe/Istanbul.", "Europe\\Istanbul", "../x", "", None):
            with self.assertRaises(zoneinfo.ZoneInfoNotFoundError):
                plat.canonical_timezone(bad)
        plat.check_timezone_database()

    def test_process_setup_is_idempotent(self):
        plat.configure_process()
        plat.configure_process()
        plat.configure_stdio_utf8()
        self.assertIn(sys.stdout.encoding.lower().replace("_", "-"), ("utf-8", "utf8"))

    def test_tools_helpers(self):
        bundled = ROOT / "runtime" / "python" / "python.exe"
        if bundled.is_file():
            self.assertEqual(tools_platform.resolve_python(None, ROOT), str(bundled))
            self.assertEqual(tools_platform.resolve_python("python3", ROOT), str(bundled))
            self.assertEqual(tools_platform.resolve_python("runtime/python/python.exe", ROOT), str(bundled))
            self.assertEqual(tools_platform.python_argv("python3", ROOT), [str(bundled), *plat.PYTHON_FLAGS])
        self.assertEqual(tools_platform.resolve_python(sys.executable, ROOT), sys.executable)
        with self.assertRaises(FileNotFoundError):
            tools_platform.resolve_python("C:\\yok\\python.exe", ROOT)
        with self.assertRaises(FileNotFoundError):
            tools_platform.resolve_python(sys.executable[:-4] + ".bat", ROOT)
        with self.assertRaises(ValueError):
            tools_platform.resolve_python("py", ROOT)
        for drive_or_root_relative in ("C:python.exe", "\\python\\python.exe", "/python/python.exe"):
            with self.assertRaises(ValueError):
                tools_platform.resolve_python(drive_or_root_relative, ROOT)
        self.assertEqual(tools_platform.PRIVATE_ALLOWED_SIDS, {"S-1-5-18", "S-1-5-32-544", "S-1-3-4"})
        self.assertEqual(tools_platform.display_command(["C:\\a b\\x.exe", "ş", 'q"']), '"C:\\a b\\x.exe" ş q\\"')
        self.assertTrue(any(entry.pid == os.getpid() and entry.name.lower().endswith(".exe")
                            for entry in tools_platform.iter_processes()))
        self.assertRegex(tools_platform.boot_time_iso(), r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
        opener = tools_platform.no_proxy_opener()
        proxies = [handler for handler in opener.handlers if type(handler).__name__ == "ProxyHandler"]
        self.assertTrue(all(not handler.proxies for handler in proxies))
        warnings = tools_platform.install_warnings(ROOT)
        self.assertTrue(all(isinstance(item, str) for item in warnings))
        log = self.path("servis.log")
        fd = tools_platform.open_log_append(log)
        try:
            os.write(fd, b"bir\n")
        finally:
            os.close(fd)
        fd = tools_platform.open_log_append(log)
        try:
            os.write(fd, b"iki\n")
        finally:
            os.close(fd)
        with open(log, "rb") as stream:
            self.assertEqual(stream.read(), b"bir\niki\n")
        self.assertTrue(plat.is_private(log))
        folder = self.path("ağaç kökü")
        os.makedirs(os.path.join(folder, "alt"))
        tools_platform.make_public_for_tests(folder, inheritance=3)
        self.assertTrue(tools_platform.ensure_private_tree(folder))
        self.assertTrue(plat.is_private(folder, directory=True))
        self.assertFalse(tools_platform.ensure_private_tree(folder))
        tools_platform.make_private_dir(folder)                      # exist_ok defaults to True
        fd, name = tools_platform.private_mkstemp(folder, ".kurulum-")
        os.close(fd)
        self.assertTrue(plat.is_private(name, exact_file_mode=True))
        self.assertIs(tools_platform.publish_noreplace, plat.rename_noreplace)


class TurkishScenarioTest(TempDirTest):
    """One workflow through the shim in a directory full of Turkish letters."""

    def test_workflow(self):
        store = os.path.join(self.base, "Veritabanı İçerik")
        plat.mkdir_private(store)
        meta = os.path.join(store, "RadioS.sdb.meta")
        temporary = os.path.join(store, ".RadioS.sdb.meta.tmp")
        with os.fdopen(plat.open_fd(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, private=True), "w",
                       encoding="utf-8", newline="\n") as stream:
            stream.write('{"istasyon": "Şehir FM"}\n')
            stream.flush()
            plat.fsync(stream)
        plat.replace(temporary, meta)
        plat.fsync_dir(store)
        with open(meta, "rb") as stream:
            self.assertEqual(stream.read(), '{"istasyon": "Şehir FM"}\n'.encode("utf-8"))
        self.assertTrue(plat.is_private(meta, exact_file_mode=True))
        with plat.locked_file(os.path.join(store, "RadioS.sdb.lock"), exclusive=False, create=True):
            pass
        fd = plat.open_nofollow_path(meta)
        os.close(fd)
        archive = os.path.join(self.base, "Arşiv Ğ")
        plat.rename_noreplace(store, archive)
        child = self.child(SLEEPER, cwd=archive)
        self.assertEqual(child.expect(), "ready")
        self.assertTrue(plat.path_equal(plat.process_info(child.pid).argv[0], plat.python_executable()))
        child.send("bye")
        self.assertEqual(child.process.wait(30), 0)
        self.assertTrue(plat.rmtree(archive))
        self.assertFalse(os.path.exists(archive))


if __name__ == "__main__":
    unittest.main()
