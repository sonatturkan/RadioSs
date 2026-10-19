"""Tools-side RadioS platform layer (Windows only).

Loads the one canonical shim ``database/SDBASE/src/sdbase/_platform.py`` by file
path (``importlib.util.spec_from_file_location``), so every constant and rule —
lock region, private SDDL, privacy checks, stop-event name, retry budget — exists
exactly once for the service, the tools and the tests.  Everything the shim
exports is re-exported here; this module only adds helpers the tools need on top
(python_command resolution, append-only service log, process listing,
installation warnings and test support).
"""
from __future__ import annotations

import ctypes
import datetime
import importlib.util
import os
import subprocess
import sys
import urllib.request
import uuid
from ctypes import wintypes as wt
from pathlib import Path
from typing import Iterator, NamedTuple

_ROOT = Path(__file__).resolve().parents[1]
SHIM_PATH = _ROOT / "database" / "SDBASE" / "src" / "sdbase" / "_platform.py"
_MODULE_NAME = "radios_sdbase_platform"


def _load_shim():
    """The shim module object; reuses a copy of the same file that is already imported
    (sdbase._platform, or an earlier load by path) so its state is not duplicated."""
    wanted = os.path.normcase(str(SHIM_PATH))
    for module in list(sys.modules.values()):
        location = getattr(module, "__file__", None)
        if location and os.path.normcase(os.path.abspath(location)) == wanted:
            return module
    spec = importlib.util.spec_from_file_location(_MODULE_NAME, SHIM_PATH)
    if spec is None or spec.loader is None:
        raise ImportError(f"RadioS platform layer not found: {SHIM_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[_MODULE_NAME] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(_MODULE_NAME, None)
        raise
    return module


shim = _load_shim()
globals().update({name: getattr(shim, name) for name in shim.__all__})

# Names of the tools specification for the shared contract constants.
FLOCK_OFFSET = shim.LOCK_REGION_OFFSET
FLOCK_LENGTH = shim.LOCK_REGION_LENGTH

# ----------------------------------------------------------------------------- files


def open_nofollow(path, flags: int, mode: int = 0o600, *, private: bool = False) -> int:
    """Binary, non-inheritable, share-delete descriptor; the last component must not be a
    link (OSError ELOOP).  private=True gives a file created by this call the private
    descriptor.  Opening a directory raises IsADirectoryError."""
    return shim.open_fd(path, flags, mode, nofollow=True, private=private)


def open_log_append(path) -> int:
    """Service log for a detached child: private when created, never a link, owned by the
    current user, and an append-only handle (FILE_APPEND_DATA without FILE_WRITE_DATA),
    so the child — which writes through its own CRT descriptor starting at offset 0 —
    still appends instead of overwriting the log."""
    descriptor = shim.open_fd(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600, nofollow=True, private=True,
                              append_only=True)
    try:
        if not os.path.isfile(path) or not shim.owned_by_current_user(descriptor):
            raise PermissionError(13, "the service log must be a regular file owned by the current user", str(path))
        if not shim.is_private(descriptor):
            shim.make_private(descriptor)             # Linux: fchmod(fd, 0o600)
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def make_private_dir(path, *, parents: bool = False, exist_ok: bool = True) -> None:
    """mkdir_private() with the tools' default exist_ok=True (verify + repair if owned)."""
    shim.mkdir_private(path, parents=parents, exist_ok=exist_ok)


def private_mkstemp(dir, prefix: str = "", suffix: str = "") -> tuple[int, str]:
    """Private temporary file created atomically (CREATE_NEW with the private SDDL)."""
    return shim.mkstemp_private(dir=dir, prefix=prefix or "tmp", suffix=suffix)


def ensure_private_tree(path) -> bool:
    """Windows equivalent of running under umask 077 for an existing tree: when `path` is
    a real directory owned by the current user on an ACL volume and not private yet, give
    it the protected private DACL (inheritable ACEs propagate to the children).  Returns
    True when it changed something."""
    if shim.is_link(path) or not os.path.isdir(path) or not shim.acl_supported(path):
        return False
    if not shim.owned_by_current_user(path) or shim.is_private(path, directory=True):
        return False
    shim.make_private(path, directory=True)
    return True


publish_noreplace = shim.rename_noreplace

# ----------------------------------------------------------------------------- python_command (D4)

_DEFAULT_PYTHON_NAMES = frozenset({"python3", "python", "python3.exe", "python.exe"})


def resolve_python(python_command: str | None, project_root) -> str:
    """Interpreter for [database] python_command (D4), identical to the C rule:
    an explicit path (absolute, or containing a separator/drive and then relative to the
    project root) must exist and be an .exe — configured but missing fails closed;
    empty or the shipped default name (python3/python) means the bundled runtime
    <root>\\runtime\\python\\python.exe, else <root>\\python\\python.exe (package layout).
    PATH, py.exe and the Microsoft Store alias are never consulted."""
    root = Path(project_root)
    value = (python_command or "").strip()
    if value and (any(char in value for char in "\\/:")):
        candidate = Path(value) if shim.is_absolute(value) else root / shim.from_portable(value)
        if candidate.suffix.lower() != ".exe" or not candidate.is_file():
            raise FileNotFoundError(2, "Yapılandırılan Python yorumlayıcısı bulunamadı (config\\radios.ini [database] "
                                       "python_command)", str(candidate))
        return str(candidate)
    if value and value.lower() not in _DEFAULT_PYTHON_NAMES:
        raise ValueError(f"python_command must be a path to python.exe or the default 'python3', not {value!r}")
    for candidate in (root / "runtime" / "python" / "python.exe", root / "python" / "python.exe"):
        if candidate.is_file():
            return str(candidate)
    raise FileNotFoundError(2, "Python çalışma ortamı bulunamadı (runtime\\python)", str(root / "runtime" / "python"))


def python_argv(python_command: str | None, project_root) -> list[str]:
    """[interpreter, -E, -s, -B, -X, utf8]; append the script and its arguments."""
    return [resolve_python(python_command, project_root), *shim.PYTHON_FLAGS]


def display_command(argv) -> str:
    """The command line CreateProcessW receives (MSVCRT quoting) — also what an operator
    can paste into cmd.exe/PowerShell to run a command by hand."""
    return subprocess.list2cmdline([os.fspath(item) for item in argv])


def boot_time_iso() -> str:
    """Informational boot instant (UTC, seconds).  Moves with clock changes: never use it
    for identity checks (identity = pid + creation time)."""
    instant = datetime.datetime.fromtimestamp(round(shim.boot_time()), datetime.timezone.utc)
    return instant.isoformat().replace("+00:00", "Z")


def no_proxy_opener() -> urllib.request.OpenerDirector:
    """urllib opener without proxies: on Windows urllib reads the registry proxy and the
    '<local>' bypass does not cover 127.0.0.1."""
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))

# ----------------------------------------------------------------------------- processes


class ProcessEntry(NamedTuple):
    pid: int
    parent_pid: int
    name: str                           # executable file name, e.g. "RadioS.exe"


class _PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_CreateToolhelp32Snapshot = _k32.CreateToolhelp32Snapshot
_CreateToolhelp32Snapshot.restype, _CreateToolhelp32Snapshot.argtypes = ctypes.c_void_p, (wt.DWORD, wt.DWORD)
_Process32FirstW = _k32.Process32FirstW
_Process32FirstW.restype, _Process32FirstW.argtypes = wt.BOOL, (ctypes.c_void_p, ctypes.POINTER(_PROCESSENTRY32W))
_Process32NextW = _k32.Process32NextW
_Process32NextW.restype, _Process32NextW.argtypes = wt.BOOL, (ctypes.c_void_p, ctypes.POINTER(_PROCESSENTRY32W))
_CloseHandle = _k32.CloseHandle
_CloseHandle.restype, _CloseHandle.argtypes = wt.BOOL, (ctypes.c_void_p,)


def iter_processes() -> Iterator[ProcessEntry]:
    """Snapshot of the running processes (replaces scanning /proc).  Use
    shim.process_info(pid) for the full image path and argv of a candidate."""
    snapshot = _CreateToolhelp32Snapshot(0x2, 0)                     # TH32CS_SNAPPROCESS
    if snapshot in (None, ctypes.c_void_p(-1).value):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = _PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        more = _Process32FirstW(snapshot, ctypes.byref(entry))
        while more:
            yield ProcessEntry(entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile)
            more = _Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        _CloseHandle(snapshot)

# ----------------------------------------------------------------------------- installation warnings

# Rights that let another principal change or replace files below the project root.
_WRITE_RIGHTS = 0x2 | 0x4 | 0x10 | 0x40 | 0x100 | 0x10000 | 0x40000 | 0x80000 | 0x10000000 | 0x40000000
# Controlled Folder Access protects these known folders by default.
_PROTECTED_KNOWN_FOLDERS = ("FDD39AD0-238F-46AF-ADB4-6C85480369C7",   # Documents
                            "B4BFCC3A-DB2C-424C-B029-7FE99A87C641",   # Desktop
                            "33E28130-4E1E-4676-835A-98395C3BC3BB",   # Pictures
                            "4BD8D571-6D19-48D3-BE97-422220080E43",   # Music
                            "18989B1D-99B5-455B-841C-AB7C74E4DDFC",   # Videos
                            "1777F761-68AD-4D8A-87BD-30B759FA33DD")   # Favorites


def _known_folder(folder_id: str) -> str | None:
    shell32 = ctypes.WinDLL("shell32")
    ole32 = ctypes.WinDLL("ole32")
    guid = (ctypes.c_ubyte * 16).from_buffer_copy(uuid.UUID(folder_id).bytes_le)
    result = ctypes.c_void_p()
    shell32.SHGetKnownFolderPath.restype = ctypes.c_long
    shell32.SHGetKnownFolderPath.argtypes = (ctypes.c_void_p, wt.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))
    ole32.CoTaskMemFree.argtypes = (ctypes.c_void_p,)
    if shell32.SHGetKnownFolderPath(ctypes.byref(guid), 0, None, ctypes.byref(result)) != 0:
        return None
    try:
        return ctypes.wstring_at(result.value)
    finally:
        ole32.CoTaskMemFree(result)


def install_warnings(project_root) -> list[str]:
    """Operator warnings about where an installation lives (strings; the GUI may show
    them): onedrive_folder, controlled_folder_access_risk, volume_without_acls,
    project_root_writable_by_others, long_path_budget."""
    warnings: list[str] = []
    root = shim.normalize(os.path.realpath(project_root))
    for variable in ("OneDrive", "OneDriveConsumer", "OneDriveCommercial"):
        value = os.environ.get(variable)
        if value and shim.is_absolute(value) and shim.is_inside(shim.normalize(value), root):
            warnings.append("onedrive_folder")
            break
    for folder_id in _PROTECTED_KNOWN_FOLDERS:
        folder = _known_folder(folder_id)
        if folder and shim.is_inside(shim.normalize(folder), root):
            warnings.append("controlled_folder_access_risk")
            break
    try:
        if not shim.acl_supported(root):
            warnings.append("volume_without_acls")
        elif shim.foreign_access(root, rights=_WRITE_RIGHTS, follow_links=True):
            warnings.append("project_root_writable_by_others")
    except OSError:
        pass
    if not shim.long_paths_enabled() and len(root) > 120:
        warnings.append("long_path_budget")
    return warnings

# ----------------------------------------------------------------------------- test support


class _TRUSTEE_W(ctypes.Structure):
    _fields_ = [("pMultipleTrustee", ctypes.c_void_p), ("MultipleTrusteeOperation", ctypes.c_int),
                ("TrusteeForm", ctypes.c_int), ("TrusteeType", ctypes.c_int), ("ptstrName", ctypes.c_void_p)]


class _EXPLICIT_ACCESS_W(ctypes.Structure):
    _fields_ = [("grfAccessPermissions", wt.DWORD), ("grfAccessMode", ctypes.c_int),
                ("grfInheritance", wt.DWORD), ("Trustee", _TRUSTEE_W)]


def make_public_for_tests(path, *, sid: str = "S-1-5-32-545", rights: int = 0x120089, inheritance: int = 0) -> None:
    """Test support (C radios_platform_test_make_public): add an allow ACE for `sid`
    (default BUILTIN\\Users, FILE_GENERIC_READ) to the DACL of path."""
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    get_info = advapi32.GetNamedSecurityInfoW
    get_info.restype = wt.DWORD
    get_info.argtypes = (wt.LPCWSTR, ctypes.c_int, wt.DWORD, ctypes.c_void_p, ctypes.c_void_p,
                         ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))
    set_entries = advapi32.SetEntriesInAclW
    set_entries.restype = wt.DWORD
    set_entries.argtypes = (wt.ULONG, ctypes.POINTER(_EXPLICIT_ACCESS_W), ctypes.c_void_p,
                            ctypes.POINTER(ctypes.c_void_p))
    set_info = advapi32.SetNamedSecurityInfoW
    set_info.restype = wt.DWORD
    set_info.argtypes = (wt.LPWSTR, ctypes.c_int, wt.DWORD, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                         ctypes.c_void_p)
    kernel32.LocalFree.argtypes = (ctypes.c_void_p,)
    kernel32.LocalFree.restype = ctypes.c_void_p
    target = os.fspath(path)
    old_dacl, descriptor, new_dacl = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
    code = get_info(target, 1, 0x4, None, None, ctypes.byref(old_dacl), None, ctypes.byref(descriptor))
    try:
        if code == 0:
            entry = _EXPLICIT_ACCESS_W(rights, 1, inheritance,                    # GRANT_ACCESS
                                       _TRUSTEE_W(None, 0, 0, 0, shim._sid_pointer(sid)))   # TRUSTEE_IS_SID
            code = set_entries(1, ctypes.byref(entry), old_dacl, ctypes.byref(new_dacl))
        if code == 0:
            code = set_info(target, 1, 0x4, None, None, new_dacl, None)          # DACL_SECURITY_INFORMATION
    finally:
        if new_dacl.value:
            kernel32.LocalFree(new_dacl)
        if descriptor.value:
            kernel32.LocalFree(descriptor)
    if code:
        raise ctypes.WinError(code)


__all__ = [*shim.__all__, "shim", "SHIM_PATH", "FLOCK_OFFSET", "FLOCK_LENGTH", "open_nofollow", "open_log_append",
           "make_private_dir", "private_mkstemp", "ensure_private_tree", "publish_noreplace", "resolve_python",
           "python_argv", "display_command", "boot_time_iso", "no_proxy_opener", "ProcessEntry", "iter_processes",
           "install_warnings", "make_public_for_tests"]
