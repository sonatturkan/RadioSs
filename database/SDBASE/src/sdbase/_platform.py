"""RadioS platform layer for Python: the one canonical Windows shim.

Windows 10 1903+ / Windows 11, x64 only.  The SDBASE service imports it as
``from . import _platform``; the tools load this very file by path
(``tools/radios_platform.py``), so it must never import anything else from
``sdbase``.  Standard library + ctypes only.

Cross-language contract with the C layer (include/radios/platform.h,
src/core/platform.c, src/core/platform_win32.c) — keep both sides identical:

* Locks: ``LockFileEx``/``UnlockFileEx`` on 1 byte at offset
  0x4000000000000000 (``RADIOS_LOCK_REGION_OFFSET``/``_LENGTH``).  Every
  SH/EX/UN request first unlocks that region (``ERROR_NOT_LOCKED`` ignored),
  so re-locking is idempotent and SH<->EX converts non-atomically, like Linux
  ``flock``.  Contention with ``LOCK_NB`` raises ``BlockingIOError``.
* Private objects (replaces 0600/0700 and ``st_uid == getuid()``): created
  with ``O:<user>D:P(A;;FA;;;<user>)(A;;FA;;;SY)(A;;FA;;;BA)`` (directories
  with ``OICI`` ACEs).  Owner check: owner == token user, or owner ==
  BUILTIN\\Administrators while the token is an effective administrator.
  No-others check: every effective allow ACE (not inherit-only) that grants a
  data/metadata/control right names the user, the owner, SYSTEM,
  Administrators or OWNER RIGHTS.  Volumes without persistent ACLs
  (FAT/exFAT) fail every check (fail closed).
* Graceful stop: manual-reset event ``Local\\RadioS-stop-<pid>``.
* Process identity: pid + process creation time (``GetProcessTimes``, 100 ns
  ticks since 1601-01-01 UTC).
* Project-relative paths crossing C <-> Python use '/' (``to_portable``);
  comparisons accept both separators and are case-insensitive like NTFS
  (``CompareStringOrdinal``).
* Replace/unlink: POSIX-semantics rename/delete on a no-follow handle,
  classic fallback on FAT/exFAT/SMB, bounded retry (5, 10, 20, 40, 80, 160,
  then 250 ms steps, 2 s total) while other programs hold the file without
  ``FILE_SHARE_DELETE``.

Errors are ``OSError`` subclasses built from the Win32 error (``winerror``
preserved), so ``FileNotFoundError``/``PermissionError``/``FileExistsError``
clauses keep working.  A refused no-follow open raises ``OSError(errno.ELOOP)``;
a path that Win32 would turn into a DOS device ("NUL", "x\\CON.txt") raises
``OSError(errno.EINVAL)``; a closed descriptor raises ``OSError(errno.EBADF)``.
"""
from __future__ import annotations

import atexit
import contextlib
import ctypes
import errno
import faulthandler
import functools
import logging
import msvcrt
import ntpath
import os
import secrets
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
from ctypes import wintypes as wt
from pathlib import Path
from typing import NamedTuple

if sys.platform != "win32":
    raise ImportError("sdbase._platform is the Windows platform layer of RadioS (Windows only)")
if sys.version_info < (3, 12):
    raise ImportError("RadioS needs CPython 3.12 or newer (runtime\\python ships 3.14)")

_log = logging.getLogger("radios.platform")

# ----------------------------------------------------------------------------- contract constants

LOCK_SH, LOCK_EX, LOCK_NB, LOCK_UN = 1, 2, 4, 8          # == RADIOS_LOCK_* == fcntl values
LOCK_REGION_OFFSET = 0x4000000000000000                 # == RADIOS_LOCK_REGION_OFFSET
LOCK_REGION_LENGTH = 1                                  # == RADIOS_LOCK_REGION_LENGTH
WIN_LOCK_OFFSET, WIN_LOCK_LENGTH = LOCK_REGION_OFFSET, LOCK_REGION_LENGTH   # engine-spec names

# Open flags.  O_BINARY/O_NOINHERIT are implied by every shim open.  O_NOFOLLOW and
# O_DIRECTORY are shim-only bits (same values as RADIOS_O_NOFOLLOW/RADIOS_O_DIRECTORY)
# understood by open_fd() and friends; os.open() cannot honour them (open_flags()
# refuses them).  There are no FIFOs in the Windows file system: O_NONBLOCK is 0.
O_BINARY = os.O_BINARY
O_NOINHERIT = os.O_NOINHERIT
O_CLOEXEC = os.O_NOINHERIT                              # == RADIOS_O_CLOEXEC
O_NONBLOCK = 0                                          # == RADIOS_O_NONBLOCK
O_NOFOLLOW = 0x00100000                                 # == RADIOS_O_NOFOLLOW
O_DIRECTORY = 0x00200000                                # == RADIOS_O_DIRECTORY

PRIVATE_FILE_SDDL = "O:{sid}D:P(A;;FA;;;{sid})(A;;FA;;;SY)(A;;FA;;;BA)"
PRIVATE_DIR_SDDL = "O:{sid}D:P(A;OICI;FA;;;{sid})(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)"
# Trusted besides the token user and the object's owner: SYSTEM, Administrators, OWNER RIGHTS.
PRIVATE_TRUSTED_SIDS = frozenset({"S-1-5-18", "S-1-5-32-544", "S-1-3-4"})
# FILE_READ_DATA|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_READ_EA|FILE_WRITE_EA|FILE_EXECUTE|
# FILE_DELETE_CHILD|FILE_WRITE_ATTRIBUTES|DELETE|WRITE_DAC|WRITE_OWNER|GENERIC_*.  Grants of
# READ_CONTROL, SYNCHRONIZE and FILE_READ_ATTRIBUTES to others are tolerated (the analogue of
# being able to stat a 0600 file).
PRIVATE_SENSITIVE_RIGHTS = 0xF00D017F

STOP_EVENT_PREFIX = "Local\\RadioS-stop-"
PYTHON_FLAGS = ("-E", "-s", "-B", "-X", "utf8")
# Console children of a RadioS process never get a console window.  A detached child gets
# its own windowless console (CREATE_NO_WINDOW), its own process group and leaves a
# kill-on-close job when allowed.  DETACHED_PROCESS is deliberately absent (same as the C
# layer): measured on Windows 11 26300, a console grandchild of a DETACHED_PROCESS child
# pops up a visible console window, while children of a CREATE_NO_WINDOW process inherit
# the invisible console.
CHILD_CREATIONFLAGS = subprocess.CREATE_NO_WINDOW
DETACHED_CREATIONFLAGS = (subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP |
                          subprocess.CREATE_BREAKAWAY_FROM_JOB)


def _retry_budget_from_environment() -> float:
    raw = os.environ.get("SDBASE_FS_RETRY_SECONDS")
    try:
        value = float(raw) if raw is not None else 2.0
    except ValueError:
        return 2.0
    return min(max(value, 0.0), 60.0) if value == value else 2.0


# Bounded wait for files other programs hold without FILE_SHARE_DELETE (C: RETRY_BUDGET_MS).
RETRY_BUDGET_SECONDS = _retry_budget_from_environment()
_RETRY_STEPS = (0.005, 0.010, 0.020, 0.040, 0.080, 0.160)
_RETRY_STEP_MAX = 0.250

# ----------------------------------------------------------------------------- Win32 constants

_SYNCHRONIZE, _DELETE, _READ_CONTROL, _WRITE_DAC, _WRITE_OWNER = 0x100000, 0x10000, 0x20000, 0x40000, 0x80000
_FILE_LIST_DIRECTORY = 0x1                         # == FILE_READ_DATA
_FILE_WRITE_DATA = 0x2
_FILE_TRAVERSE = 0x20
_FILE_READ_ATTRIBUTES, _FILE_WRITE_ATTRIBUTES = 0x80, 0x100
_FILE_GENERIC_READ, _FILE_GENERIC_WRITE = 0x120089, 0x120116
_GENERIC_WRITE = 0x40000000
_FILE_SHARE_READ, _FILE_SHARE_WRITE, _FILE_SHARE_DELETE = 0x1, 0x2, 0x4
_SHARE_ALL = 0x7
_FILE_ATTRIBUTE_READONLY, _FILE_ATTRIBUTE_HIDDEN = 0x1, 0x2
_FILE_ATTRIBUTE_DIRECTORY, _FILE_ATTRIBUTE_NORMAL = 0x10, 0x80
_FILE_ATTRIBUTE_TEMPORARY, _FILE_ATTRIBUTE_REPARSE_POINT = 0x100, 0x400
_SETTABLE_ATTRIBUTES = 0x1 | 0x2 | 0x4 | 0x20 | 0x80 | 0x100 | 0x1000 | 0x2000
_NAME_SURROGATE = 0x20000000                       # IsReparseTagNameSurrogate()
_FILE_OPEN, _FILE_CREATE, _FILE_OPEN_IF, _FILE_OVERWRITE, _FILE_OVERWRITE_IF = 1, 2, 3, 4, 5
_FILE_DIRECTORY_FILE, _FILE_SEQUENTIAL_ONLY, _FILE_SYNCHRONOUS_IO_NONALERT = 0x1, 0x4, 0x20
_FILE_NON_DIRECTORY_FILE, _FILE_RANDOM_ACCESS, _FILE_OPEN_REPARSE_POINT = 0x40, 0x800, 0x200000
_OBJ_CASE_INSENSITIVE = 0x40
_CREATE_NEW = 1
_FILE_FLAG_DELETE_ON_CLOSE = 0x04000000
_FILE_TYPE_DISK = 1
_FILE_PERSISTENT_ACLS = 0x8
_LOCKFILE_FAIL_IMMEDIATELY, _LOCKFILE_EXCLUSIVE_LOCK = 0x1, 0x2
_FILE_RENAME_REPLACE_IF_EXISTS, _FILE_RENAME_POSIX_SEMANTICS = 0x1, 0x2
_FILE_RENAME_IGNORE_READONLY_ATTRIBUTE = 0x40
_FILE_DISPOSITION_DELETE, _FILE_DISPOSITION_POSIX_SEMANTICS = 0x1, 0x2
_FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE = 0x10
_FileBasicInfo, _FileRenameInfo, _FileDispositionInfo, _FileEndOfFileInfo, _FileAttributeTagInfo = 0, 3, 4, 6, 9
_FileFullDirectoryInfo, _FileFullDirectoryRestartInfo, _FileIdInfo = 14, 15, 18
_FileDispositionInfoEx, _FileRenameInfoEx = 21, 22
_ERROR_ACCESS_DENIED, _ERROR_INVALID_FUNCTION, _ERROR_NOT_SUPPORTED = 5, 1, 50
_ERROR_FILE_NOT_FOUND, _ERROR_PATH_NOT_FOUND, _ERROR_NO_MORE_FILES = 2, 3, 18
_ERROR_SHARING_VIOLATION, _ERROR_LOCK_VIOLATION, _ERROR_HANDLE_EOF = 32, 33, 38
_ERROR_FILE_EXISTS, _ERROR_INVALID_PARAMETER, _ERROR_DIR_NOT_EMPTY = 80, 87, 145
_ERROR_NOT_LOCKED, _ERROR_ALREADY_EXISTS, _ERROR_IO_PENDING, _ERROR_USER_MAPPED_FILE = 158, 183, 997, 1224
_FALLBACK_INFO_CLASS = frozenset({_ERROR_INVALID_FUNCTION, _ERROR_NOT_SUPPORTED, _ERROR_INVALID_PARAMETER})
_STATUS_INFO_LENGTH_MISMATCH = 0xC0000004
_STATUS_SHARING_VIOLATION, _STATUS_LOCK_NOT_GRANTED, _STATUS_FILE_LOCK_CONFLICT = 0xC0000043, 0xC0000054, 0xC0000055
_STATUS_DELETE_PENDING = 0xC0000056
_STATUS_OBJECT_NAME_NOT_FOUND, _STATUS_OBJECT_PATH_NOT_FOUND = 0xC0000034, 0xC000003A
_STATUS_OBJECT_NAME_COLLISION, _STATUS_NAME_TOO_LONG = 0xC0000035, 0xC0000106
_STATUS_FILE_IS_A_DIRECTORY, _STATUS_NOT_A_DIRECTORY = 0xC00000BA, 0xC0000103
_RETRYABLE_STATUS = frozenset({_STATUS_SHARING_VIOLATION, _STATUS_LOCK_NOT_GRANTED, _STATUS_FILE_LOCK_CONFLICT})
_MISSING_STATUS = frozenset({_STATUS_OBJECT_NAME_NOT_FOUND, _STATUS_OBJECT_PATH_NOT_FOUND, _STATUS_DELETE_PENDING})
_STATUS_ERRNO = {
    _STATUS_FILE_IS_A_DIRECTORY: errno.EISDIR,
    _STATUS_NOT_A_DIRECTORY: errno.ENOTDIR,
    0xC000050B: errno.ELOOP,                       # STATUS_REPARSE_POINT_ENCOUNTERED
    0x8000002D: errno.ELOOP,                       # STATUS_STOPPED_ON_SYMLINK
    _STATUS_OBJECT_NAME_COLLISION: errno.EEXIST,
    0xC0000101: errno.ENOTEMPTY,                   # STATUS_DIRECTORY_NOT_EMPTY
    _STATUS_DELETE_PENDING: errno.ENOENT,
}
_PROCESS_TERMINATE, _PROCESS_QUERY_LIMITED_INFORMATION = 0x1, 0x1000
_EVENT_MODIFY_STATE = 0x2
_WAIT_OBJECT_0, _WAIT_TIMEOUT, _INFINITE = 0, 0x102, 0xFFFFFFFF
_SE_FILE_OBJECT = 1
_OWNER_SECURITY_INFORMATION, _DACL_SECURITY_INFORMATION = 0x1, 0x4
_PROTECTED_DACL_SECURITY_INFORMATION = 0x80000000
_INHERIT_ONLY_ACE = 0x8
_ALLOW_ACE_TYPES = frozenset({0, 5, 9, 11})        # ACCESS_ALLOWED(_OBJECT)(_CALLBACK)(_OBJECT)
_ACCESS_DENIED_ACE_TYPE = 1
_FILE_ALL_ACCESS, _GENERIC_ALL = 0x1F01FF, 0x10000000
_OBJECT_ACE_TYPES = frozenset({5, 6, 7, 8, 11, 12, 15, 16})
_ADMINISTRATORS_SID = "S-1-5-32-544"
_CTRL_CLOSE_EVENT, _CTRL_LOGOFF_EVENT, _CTRL_SHUTDOWN_EVENT = 2, 5, 6

# ----------------------------------------------------------------------------- Win32 bindings

_HANDLE = ctypes.c_void_p
_INVALID_HANDLE = ctypes.c_void_p(-1).value


class _OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                ("Offset", wt.DWORD), ("OffsetHigh", wt.DWORD), ("hEvent", _HANDLE)]


class _TAG_INFO(ctypes.Structure):
    _fields_ = [("FileAttributes", wt.DWORD), ("ReparseTag", wt.DWORD)]


class _FILE_ID_INFO(ctypes.Structure):
    _fields_ = [("VolumeSerialNumber", ctypes.c_ulonglong), ("FileId", ctypes.c_ubyte * 16)]


class _BY_HANDLE_INFO(ctypes.Structure):
    _fields_ = [("dwFileAttributes", wt.DWORD), ("ftCreationTime", wt.FILETIME),
                ("ftLastAccessTime", wt.FILETIME), ("ftLastWriteTime", wt.FILETIME),
                ("dwVolumeSerialNumber", wt.DWORD), ("nFileSizeHigh", wt.DWORD), ("nFileSizeLow", wt.DWORD),
                ("nNumberOfLinks", wt.DWORD), ("nFileIndexHigh", wt.DWORD), ("nFileIndexLow", wt.DWORD)]


class _FILE_BASIC_INFO(ctypes.Structure):
    _fields_ = [("CreationTime", ctypes.c_longlong), ("LastAccessTime", ctypes.c_longlong),
                ("LastWriteTime", ctypes.c_longlong), ("ChangeTime", ctypes.c_longlong),
                ("FileAttributes", wt.DWORD)]


class _UNICODE_STRING(ctypes.Structure):
    _fields_ = [("Length", wt.USHORT), ("MaximumLength", wt.USHORT), ("Buffer", ctypes.c_void_p)]


class _OBJECT_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Length", wt.ULONG), ("RootDirectory", _HANDLE), ("ObjectName", ctypes.POINTER(_UNICODE_STRING)),
                ("Attributes", wt.ULONG), ("SecurityDescriptor", ctypes.c_void_p),
                ("SecurityQualityOfService", ctypes.c_void_p)]


class _IO_STATUS_BLOCK(ctypes.Structure):
    _fields_ = [("Status", ctypes.c_void_p), ("Information", ctypes.c_size_t)]


_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_adv = ctypes.WinDLL("advapi32", use_last_error=True)
_ntdll = ctypes.WinDLL("ntdll")
_shell32 = ctypes.WinDLL("shell32", use_last_error=True)


def _bind(dll, name, restype, *argtypes):
    function = getattr(dll, name)
    function.restype = restype
    function.argtypes = argtypes
    return function


_P = ctypes.POINTER
_CloseHandle = _bind(_k32, "CloseHandle", wt.BOOL, _HANDLE)
_GetFileType = _bind(_k32, "GetFileType", wt.DWORD, _HANDLE)
_GetFileInformationByHandle = _bind(_k32, "GetFileInformationByHandle", wt.BOOL, _HANDLE, _P(_BY_HANDLE_INFO))
_GetFileInformationByHandleEx = _bind(_k32, "GetFileInformationByHandleEx", wt.BOOL, _HANDLE, ctypes.c_int,
                                      ctypes.c_void_p, wt.DWORD)
_SetFileInformationByHandle = _bind(_k32, "SetFileInformationByHandle", wt.BOOL, _HANDLE, ctypes.c_int,
                                    ctypes.c_void_p, wt.DWORD)
_FlushFileBuffers = _bind(_k32, "FlushFileBuffers", wt.BOOL, _HANDLE)
_LockFileEx = _bind(_k32, "LockFileEx", wt.BOOL, _HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, _P(_OVERLAPPED))
_UnlockFileEx = _bind(_k32, "UnlockFileEx", wt.BOOL, _HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, _P(_OVERLAPPED))
_ReadFile = _bind(_k32, "ReadFile", wt.BOOL, _HANDLE, ctypes.c_void_p, wt.DWORD, _P(wt.DWORD), _P(_OVERLAPPED))
_ReOpenFile = _bind(_k32, "ReOpenFile", _HANDLE, _HANDLE, wt.DWORD, wt.DWORD, wt.DWORD)
_CreateFileW = _bind(_k32, "CreateFileW", _HANDLE, wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD,
                     wt.DWORD, _HANDLE)
_GetDiskFreeSpaceExW = _bind(_k32, "GetDiskFreeSpaceExW", wt.BOOL, wt.LPCWSTR, _P(ctypes.c_ulonglong),
                             _P(ctypes.c_ulonglong), _P(ctypes.c_ulonglong))
_GetVolumeInformationByHandleW = _bind(_k32, "GetVolumeInformationByHandleW", wt.BOOL, _HANDLE, wt.LPWSTR, wt.DWORD,
                                       _P(wt.DWORD), _P(wt.DWORD), _P(wt.DWORD), wt.LPWSTR, wt.DWORD)
_LocalFree = _bind(_k32, "LocalFree", ctypes.c_void_p, ctypes.c_void_p)
_CompareStringOrdinal = _bind(_k32, "CompareStringOrdinal", ctypes.c_int, wt.LPCWSTR, ctypes.c_int, wt.LPCWSTR,
                              ctypes.c_int, wt.BOOL)
_GetCurrentProcess = _bind(_k32, "GetCurrentProcess", _HANDLE)
_OpenProcess = _bind(_k32, "OpenProcess", _HANDLE, wt.DWORD, wt.BOOL, wt.DWORD)
_GetProcessTimes = _bind(_k32, "GetProcessTimes", wt.BOOL, _HANDLE, _P(wt.FILETIME), _P(wt.FILETIME),
                         _P(wt.FILETIME), _P(wt.FILETIME))
_GetExitCodeProcess = _bind(_k32, "GetExitCodeProcess", wt.BOOL, _HANDLE, _P(wt.DWORD))
_TerminateProcess = _bind(_k32, "TerminateProcess", wt.BOOL, _HANDLE, wt.UINT)
_QueryFullProcessImageNameW = _bind(_k32, "QueryFullProcessImageNameW", wt.BOOL, _HANDLE, wt.DWORD, wt.LPWSTR,
                                    _P(wt.DWORD))
_WaitForSingleObject = _bind(_k32, "WaitForSingleObject", wt.DWORD, _HANDLE, wt.DWORD)
_WaitForMultipleObjects = _bind(_k32, "WaitForMultipleObjects", wt.DWORD, wt.DWORD, _P(_HANDLE), wt.BOOL, wt.DWORD)
_CreateEventW = _bind(_k32, "CreateEventW", _HANDLE, ctypes.c_void_p, wt.BOOL, wt.BOOL, wt.LPCWSTR)
_OpenEventW = _bind(_k32, "OpenEventW", _HANDLE, wt.DWORD, wt.BOOL, wt.LPCWSTR)
_SetEvent = _bind(_k32, "SetEvent", wt.BOOL, _HANDLE)
_ResetEvent = _bind(_k32, "ResetEvent", wt.BOOL, _HANDLE)
_HANDLER_ROUTINE = ctypes.WINFUNCTYPE(wt.BOOL, wt.DWORD)
_SetConsoleCtrlHandler = _bind(_k32, "SetConsoleCtrlHandler", wt.BOOL, _HANDLER_ROUTINE, wt.BOOL)
_GetTickCount64 = _bind(_k32, "GetTickCount64", ctypes.c_ulonglong)
_SetErrorMode = _bind(_k32, "SetErrorMode", wt.UINT, wt.UINT)
_GetErrorMode = _bind(_k32, "GetErrorMode", wt.UINT)
_OpenProcessToken = _bind(_adv, "OpenProcessToken", wt.BOOL, _HANDLE, wt.DWORD, _P(_HANDLE))
_GetTokenInformation = _bind(_adv, "GetTokenInformation", wt.BOOL, _HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD,
                             _P(wt.DWORD))
_ConvertSidToStringSidW = _bind(_adv, "ConvertSidToStringSidW", wt.BOOL, ctypes.c_void_p, _P(ctypes.c_void_p))
_ConvertStringSidToSidW = _bind(_adv, "ConvertStringSidToSidW", wt.BOOL, wt.LPCWSTR, _P(ctypes.c_void_p))
_ConvertStringSdToSd = _bind(_adv, "ConvertStringSecurityDescriptorToSecurityDescriptorW", wt.BOOL, wt.LPCWSTR,
                             wt.DWORD, _P(ctypes.c_void_p), _P(wt.ULONG))
_ConvertSdToStringSd = _bind(_adv, "ConvertSecurityDescriptorToStringSecurityDescriptorW", wt.BOOL, ctypes.c_void_p,
                             wt.DWORD, wt.DWORD, _P(ctypes.c_void_p), _P(wt.ULONG))
_GetSecurityInfo = _bind(_adv, "GetSecurityInfo", wt.DWORD, _HANDLE, ctypes.c_int, wt.DWORD, _P(ctypes.c_void_p),
                         _P(ctypes.c_void_p), _P(ctypes.c_void_p), _P(ctypes.c_void_p), _P(ctypes.c_void_p))
_SetSecurityInfo = _bind(_adv, "SetSecurityInfo", wt.DWORD, _HANDLE, ctypes.c_int, wt.DWORD, ctypes.c_void_p,
                         ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
_GetSecurityDescriptorDacl = _bind(_adv, "GetSecurityDescriptorDacl", wt.BOOL, ctypes.c_void_p, _P(wt.BOOL),
                                   _P(ctypes.c_void_p), _P(wt.BOOL))
_GetAce = _bind(_adv, "GetAce", wt.BOOL, ctypes.c_void_p, wt.DWORD, _P(ctypes.c_void_p))
_CheckTokenMembership = _bind(_adv, "CheckTokenMembership", wt.BOOL, _HANDLE, ctypes.c_void_p, _P(wt.BOOL))
_NtCreateFile = _bind(_ntdll, "NtCreateFile", ctypes.c_long, _P(_HANDLE), wt.DWORD, _P(_OBJECT_ATTRIBUTES),
                      _P(_IO_STATUS_BLOCK), ctypes.c_void_p, wt.ULONG, wt.ULONG, wt.ULONG, wt.ULONG,
                      ctypes.c_void_p, wt.ULONG)
_NtQueryInformationProcess = _bind(_ntdll, "NtQueryInformationProcess", ctypes.c_long, _HANDLE, ctypes.c_int,
                                   ctypes.c_void_p, wt.ULONG, _P(wt.ULONG))
_RtlNtStatusToDosError = _bind(_ntdll, "RtlNtStatusToDosError", wt.ULONG, wt.ULONG)
try:            # undocumented export (Windows 10 1607+): its absence must not break the import
    _RtlAreLongPathsEnabled = _bind(_ntdll, "RtlAreLongPathsEnabled", ctypes.c_ubyte)
except AttributeError:
    _RtlAreLongPathsEnabled = None
_CommandLineToArgvW = _bind(_shell32, "CommandLineToArgvW", _P(ctypes.c_wchar_p), wt.LPCWSTR, _P(ctypes.c_int))

# ----------------------------------------------------------------------------- errors and retries


def _winerror(code: int, filename=None, filename2=None) -> OSError:
    """OSError for a Win32 error: Python maps winerror -> errno and the subclass."""
    return OSError(None, ctypes.FormatError(code).strip(), filename, code, filename2)


def _errno_error(code: int, message: str, filename=None, filename2=None) -> OSError:
    return OSError(code, message, filename, None, filename2)


_ERRNO_TEXT = {
    errno.EISDIR: "Is a directory",
    errno.ENOTDIR: "Not a directory",
    errno.ELOOP: "Refusing to follow a symbolic link or junction",
    errno.EEXIST: "File exists",
    errno.ENOTEMPTY: "Directory not empty",
    errno.ENOENT: "No such file or directory",
}


def _status_error(status: int, filename=None, filename2=None) -> OSError:
    status &= 0xFFFFFFFF
    mapped = _STATUS_ERRNO.get(status)
    if mapped is not None:
        return _errno_error(mapped, _ERRNO_TEXT[mapped], filename, filename2)
    return _winerror(_RtlNtStatusToDosError(status), filename, filename2)


def _loop_error(filename) -> OSError:
    return _errno_error(errno.ELOOP, _ERRNO_TEXT[errno.ELOOP], filename)


class _Retry:
    """Bounded backoff: 5, 10, 20, 40, 80, 160, then 250 ms steps until the budget is spent."""

    __slots__ = ("started", "budget", "attempt")

    def __init__(self, budget: float | None = None, started: float | None = None):
        self.budget = RETRY_BUDGET_SECONDS if budget is None else max(0.0, float(budget))
        self.started = time.monotonic() if started is None else started
        self.attempt = 0

    def entry(self) -> "_Retry":
        """Same deadline, fresh step sequence (one per file in a tree)."""
        return _Retry(self.budget, self.started)

    def allowed(self) -> bool:
        return time.monotonic() - self.started < self.budget

    def sleep(self) -> None:
        step = _RETRY_STEPS[self.attempt] if self.attempt < len(_RETRY_STEPS) else _RETRY_STEP_MAX
        remaining = self.budget - (time.monotonic() - self.started)
        self.attempt += 1
        time.sleep(max(0.001, min(step, remaining)))


# ----------------------------------------------------------------------------- path strings (no I/O)

_SEPARATORS = "\\/"
_INVALID_NAME_CHARACTERS = frozenset('\\/:*?"<>|')
_RESERVED_BASE_NAMES = frozenset({"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"})


def _text(path) -> str:
    text = os.fspath(path)
    if not isinstance(text, str):
        raise TypeError("RadioS paths are str (UTF-8 on the wire), not bytes")
    return text


def _root_length(path: str) -> int:
    """Length of the root of an absolute path ("C:\\" = 3, "\\\\srv\\share\\" ...), 0 when the
    path is not absolute.  Identical to root_length() in src/core/platform.c."""
    if len(path) >= 3 and path[0].isascii() and path[0].isalpha() and path[1] == ":" and path[2] in _SEPARATORS:
        return 3
    if len(path) >= 2 and path[0] in _SEPARATORS and path[1] in _SEPARATORS:
        end = len(path)
        p = 2
        while p < end and path[p] not in _SEPARATORS:
            p += 1
        if p == 2 or p == end:
            return 0
        if p - 2 == 1 and path[2] in "?.":
            return 0                                # "\\?\..." and "\\.\..." device paths
        share = q = p + 1
        while q < end and path[q] not in _SEPARATORS:
            q += 1
        if q == share:
            return 0
        return q + (1 if q < end else 0)
    return 0


def is_absolute(path) -> bool:
    """"X:\\..." / "X:/..." or "\\\\server\\share[\\...]" (radios_path_is_absolute); False for
    "\\foo", "C:foo", relative paths and "\\\\?\\" / "\\\\.\\" device paths."""
    try:
        return _root_length(_text(path)) > 0
    except TypeError:
        return False


def is_root(path) -> bool:
    """"X:\\" or "X:/", "\\\\server\\share" with or without a trailing separator."""
    try:
        text = _text(path)
    except TypeError:
        return False
    root = _root_length(text)
    return root > 0 and root == len(text)


def _unify(path: str) -> str:
    return path.replace("/", "\\")


def _ordinal_equal(a: str, b: str) -> bool:
    # -1: NUL-terminated.  len() would count code points, CompareStringOrdinal counts UTF-16 units.
    return _CompareStringOrdinal(a, -1, b, -1, True) == 2


def path_equal(a, b) -> bool:
    """Equality after separator unification, ordinal case-insensitive like NTFS
    (CompareStringOrdinal; never str.lower()/casefold(), which fold Turkish letters
    differently from the file system).  No disk access; trailing separators matter."""
    try:
        first, second = _unify(_text(a)), _unify(_text(b))
    except TypeError:
        return False
    return _ordinal_equal(first, second)


same_path = path_equal


def is_inside(root, path) -> bool:
    """path == root or below root at a component boundary (radios_path_is_inside)."""
    try:
        base, candidate = _unify(_text(root)), _unify(_text(path))
    except TypeError:
        return False
    if not base:
        return False
    if len(base) > 1 and base.endswith("\\") and not is_root(base):
        base = base[:-1]
    if _ordinal_equal(base, candidate):
        return True
    if len(candidate) > len(base) and _ordinal_equal(base, candidate[:len(base)]):
        return base.endswith("\\") or candidate[len(base)] == "\\"
    return False


def normalize(path) -> str:
    """Absolute, '\\' separators, "." / ".." resolved lexically, duplicate separators
    removed, "\\\\?\\" prefixes removed, upper-case drive letter, no trailing separator
    except for roots (radios_path_normalize).  Relative input is resolved against the CWD."""
    text = _unify(_text(path))
    if not text:
        raise ValueError("empty path")
    if text.startswith("\\\\?\\UNC\\"):
        text = "\\\\" + text[8:]
    elif text.startswith("\\\\?\\"):
        text = text[4:]
    if not _root_length(text):
        text = os.path.abspath(text)
    result = ntpath.normpath(text)
    if len(result) >= 2 and result[1] == ":" and result[0].isascii():
        result = result[0].upper() + result[1:]
    root = _root_length(result)
    while len(result) > max(root, 1) and result.endswith("\\"):
        result = result[:-1]
    return result


def _utf16_units(text: str) -> int:
    return len(text) + sum(1 for char in text if ord(char) > 0xFFFF)


def _reserved_device_name(name: str) -> bool:
    stem = name.split(".", 1)[0].rstrip(" ")
    base = "".join(char.upper() if "a" <= char <= "z" else char for char in stem)   # ASCII-only, like C
    if base in _RESERVED_BASE_NAMES:
        return True
    if base[:3] in ("COM", "LPT"):
        suffix = base[3:]
        return (len(suffix) == 1 and suffix.isascii() and suffix.isdigit()) or suffix in ("\u00b9", "\u00b2", "\u00b3")
    return False


def component_valid(name) -> bool:
    """One file/directory name valid on Windows (radios_path_component_valid): non-empty,
    not "." or "..", no control characters (incl. DEL), none of \\ / : * ? " < > |, no
    trailing '.' or ' ', not a reserved device name (CON PRN AUX NUL CONIN$ CONOUT$
    COM0-9 LPT0-9 COM¹²³ LPT¹²³, any case, with or without extension), encodable as
    UTF-8 (no lone surrogates), at most 255 UTF-16 units."""
    if not isinstance(name, str) or name in ("", ".", ".."):
        return False
    for char in name:
        code = ord(char)
        if code < 0x20 or code == 0x7F or char in _INVALID_NAME_CHARACTERS or 0xD800 <= code <= 0xDFFF:
            return False
    if name[-1] in ". ":
        return False
    if _reserved_device_name(name):
        return False
    return _utf16_units(name) <= 255


def valid_new_name(name) -> bool:
    """A name that may be created on Windows (same rule as component_valid)."""
    return component_valid(name)


def sanitize_component(text, fallback: str, *, limit: int = 150) -> str:
    """Map an arbitrary title/file name to a valid component: control characters are
    dropped, < > : " / \\ | ? * become '_', surrounding spaces and dots are stripped
    (repeatedly, also after truncation to `limit` characters and 255 UTF-16 units), a
    reserved device name gets a '_' prefix, an empty result becomes `fallback`."""
    if not component_valid(fallback):
        raise ValueError(f"fallback is not a valid file name: {fallback!r}")
    clean = "".join("_" if char in _INVALID_NAME_CHARACTERS else char for char in str(text)
                    if ord(char) >= 0x20 and ord(char) != 0x7F and not 0xD800 <= ord(char) <= 0xDFFF)

    def strip(value: str) -> str:
        previous = None
        while previous != value:
            previous, value = value, value.strip(" ").strip(".")
        return value

    clean = strip(clean)
    for _attempt in range(2):            # truncation may expose a reserved stem; the prefix may need room
        if 0 < limit < len(clean):
            clean = strip(clean[:limit])
        while _utf16_units(clean) > 255:
            clean = strip(clean[:-1])
        if not clean or not _reserved_device_name(clean):
            break
        clean = "_" + clean
    return clean if component_valid(clean) else fallback


def safe_relative_parts(relative) -> tuple[str, ...]:
    """Parse a stored project/manifest-relative path ("a/b/c", '/' separators only) into
    components; identical on every host.  ValueError for empty strings, NUL, '\\', ':',
    absolute paths and any component that is not component_valid()."""
    if not isinstance(relative, str) or not relative or any(char in relative for char in "\x00\\:"):
        raise ValueError("unsafe relative path")
    if relative.startswith("/"):
        raise ValueError("absolute path where a relative path is required")
    parts = tuple(relative.split("/"))
    for part in parts:
        if not component_valid(part):
            raise ValueError(f"unsafe path component: {part!r}")
    return parts


def is_canonical(path) -> bool:
    """Absolute, equal to normalize(path) and every component valid (core spec 5.5 rule 3).
    Roots are canonical; storage rules reject them separately with is_root()."""
    try:
        text = _text(path)
    except TypeError:
        return False
    if not is_absolute(text):
        return False
    try:
        if normalize(text) != text:
            return False
    except (ValueError, OSError):
        return False
    return all(component_valid(part) for part in text[_root_length(text):].split("\\") if part)


def stored_path_flavour(value) -> str | None:
    """Classify a persisted absolute path: "windows" (drive or UNC, either separator),
    "legacy-posix" (Linux-created data: exactly one leading '/'), else None.  String
    rule only; never used to accept new writes."""
    if not isinstance(value, str):
        return None
    if is_absolute(value):
        return "windows"
    if value.startswith("/") and not value.startswith("//"):
        return "legacy-posix"
    return None


def to_portable(relative) -> str:
    """Wire form of a project-relative path exchanged with C/SDBASE: '\\' -> '/'."""
    return _text(relative).replace("\\", "/")


def from_portable(relative) -> str:
    """Inverse of to_portable(): '/' -> '\\'."""
    return _text(relative).replace("/", "\\")


def long_paths_enabled() -> bool:
    """True when the system and this process allow Win32 paths longer than MAX_PATH
    (LongPathsEnabled + longPathAware manifest).  False when ntdll has no
    RtlAreLongPathsEnabled (Windows before 1607): keep paths short then."""
    return bool(_RtlAreLongPathsEnabled is not None and _RtlAreLongPathsEnabled())


def _full_path(path) -> str:
    text = _text(path)
    if not text:
        raise _errno_error(errno.ENOENT, _ERRNO_TEXT[errno.ENOENT], text)
    if "\x00" in text:
        raise ValueError("embedded null character in path")
    text = _unify(text)
    if text.startswith(("\\\\?\\", "\\??\\")):
        return "\\\\?\\" + text[4:]
    full = os.path.abspath(text)                     # GetFullPathNameW: CWD, '.', '..'
    if full.startswith("\\\\.\\") and not text.startswith("\\\\.\\"):
        # "NUL", "x\\CON.txt": legacy DOS device names must never turn a file path
        # into a device (same rule as nt_path() in src/core/platform.c).
        raise _errno_error(errno.EINVAL, "path names a DOS device", text)
    return full


def _nt_path(path) -> str:
    """Fully qualified NT path ("\\??\\C:\\x", "\\??\\UNC\\srv\\share\\x"); no MAX_PATH limit."""
    full = _full_path(path)
    if full.startswith(("\\\\?\\", "\\??\\", "\\\\.\\")):
        return "\\??\\" + full[4:]
    if full.startswith("\\\\"):
        return "\\??\\UNC\\" + full[2:]
    return "\\??\\" + full


def _win32_path(path) -> str:
    """Path for Win32 APIs without an NT variant: DOS form, "\\\\?\\" form when long."""
    nt = _nt_path(path)
    if len(nt) - 4 < 240:
        if nt.startswith("\\??\\UNC\\"):
            return "\\" + nt[7:]
        return nt[4:]
    return "\\\\?\\" + nt[4:]


# ----------------------------------------------------------------------------- links


def stat_is_link(info: os.stat_result) -> bool:
    """Symlink, junction, mount point or any other name-surrogate reparse point.
    Non-surrogate reparse points (OneDrive placeholders, dedup, WOF) are not links."""
    if stat.S_ISLNK(info.st_mode):
        return True
    attributes = getattr(info, "st_file_attributes", 0)
    return bool(attributes & _FILE_ATTRIBUTE_REPARSE_POINT and getattr(info, "st_reparse_tag", 0) & _NAME_SURROGATE)


def entry_is_link(entry: os.DirEntry) -> bool:
    """DirEntry variant of is_link() (uses the cached directory data)."""
    try:
        return entry.is_junction() or stat_is_link(entry.stat(follow_symlinks=False))
    except OSError:
        return False


def is_link(target) -> bool:
    """Path, os.stat_result or os.DirEntry -> is it a symlink/junction/name-surrogate
    reparse point (lstat semantics; False when the path does not exist)."""
    if isinstance(target, os.stat_result):
        return stat_is_link(target)
    if isinstance(target, os.DirEntry):
        return entry_is_link(target)
    try:
        return stat_is_link(os.lstat(target))
    except (OSError, ValueError):
        return False


is_link_like = is_link
entry_is_link_like = entry_is_link


def has_link_component(path) -> bool:
    """True when any existing component of the absolute form of path, from the drive or
    UNC root to the leaf, is a link (symlink, junction, mount point)."""
    full = _unify(os.path.abspath(_text(path)))
    if full.startswith("\\\\?\\"):
        full = normalize(full)
    root = _root_length(full)
    current = full[:root]
    for part in (item for item in full[root:].split("\\") if item):
        current = current + part if current.endswith("\\") else current + "\\" + part
        try:
            if stat_is_link(os.lstat(current)):
                return True
        except FileNotFoundError:
            return False
    return False


# ----------------------------------------------------------------------------- handles


def _nt_create(root, name: str, access: int, share: int, disposition: int, options: int, *,
               attributes: int = _FILE_ATTRIBUTE_NORMAL, security=None) -> tuple[int, int | None]:
    """NtCreateFile; name is a full NT path (root None) or one component relative to the
    directory handle root (real openat semantics).  Returns (status, handle or None)."""
    buffer = ctypes.create_unicode_buffer(name)
    size = ctypes.sizeof(buffer) - ctypes.sizeof(ctypes.c_wchar)
    if size > 0xFFF0:
        return _STATUS_NAME_TOO_LONG, None
    text = _UNICODE_STRING(size, size + ctypes.sizeof(ctypes.c_wchar), ctypes.addressof(buffer))
    object_attributes = _OBJECT_ATTRIBUTES(ctypes.sizeof(_OBJECT_ATTRIBUTES), root, ctypes.pointer(text),
                                           _OBJ_CASE_INSENSITIVE, security, None)
    handle = _HANDLE()
    io_status = _IO_STATUS_BLOCK()
    status = _NtCreateFile(ctypes.byref(handle), access | _SYNCHRONIZE, ctypes.byref(object_attributes),
                           ctypes.byref(io_status), None, attributes, share, disposition,
                           options | _FILE_SYNCHRONOUS_IO_NONALERT, None, 0) & 0xFFFFFFFF
    if status >= 0x80000000 or not handle.value:
        return status, None
    return status, handle.value


def _attributes_tag(handle) -> tuple[int, int]:
    info = _TAG_INFO()
    if not _GetFileInformationByHandleEx(handle, _FileAttributeTagInfo, ctypes.byref(info), ctypes.sizeof(info)):
        raise _winerror(ctypes.get_last_error())
    attributes = info.FileAttributes
    return attributes, (info.ReparseTag if attributes & _FILE_ATTRIBUTE_REPARSE_POINT else 0)


def _attributes_are_link(attributes: int, tag: int) -> bool:
    return bool(attributes & _FILE_ATTRIBUTE_REPARSE_POINT and tag & _NAME_SURROGATE)


def _file_identity(handle) -> tuple:
    info = _FILE_ID_INFO()
    if _GetFileInformationByHandleEx(handle, _FileIdInfo, ctypes.byref(info), ctypes.sizeof(info)):
        return info.VolumeSerialNumber, bytes(info.FileId)
    basic = _BY_HANDLE_INFO()
    if not _GetFileInformationByHandle(handle, ctypes.byref(basic)):
        raise _winerror(ctypes.get_last_error())
    return basic.dwVolumeSerialNumber, (basic.nFileIndexHigh << 32) | basic.nFileIndexLow


def _names_link(root, name: str) -> bool:
    status, probe = _nt_create(root, name, _FILE_READ_ATTRIBUTES, _SHARE_ALL, _FILE_OPEN, _FILE_OPEN_REPARSE_POINT)
    if probe is None:
        return False
    try:
        return _attributes_are_link(*_attributes_tag(probe))
    except OSError:
        return False
    finally:
        _CloseHandle(probe)


_NO_CONSOLE_HANDLE = ctypes.c_void_p(-2).value       # _NO_CONSOLE_FILENO == GetCurrentThread()


def _handle_of(target) -> int:
    """OS handle of an fd or file object.  The std fds of a process without a console
    map to -2, which is also the GetCurrentThread() pseudo handle: EBADF, like C."""
    fd = target if isinstance(target, int) else target.fileno()
    handle = msvcrt.get_osfhandle(fd)
    if handle in (None, 0, _INVALID_HANDLE, _NO_CONSOLE_HANDLE, -1, -2):
        raise _errno_error(errno.EBADF, "Bad file descriptor", f"<descriptor {fd}>")
    return handle


def _wrap_fd(handle, *, append: bool = False) -> int:
    """CRT descriptor for an owned handle: always binary and non-inheritable."""
    flags = os.O_BINARY | os.O_NOINHERIT | (os.O_APPEND if append else 0)
    try:
        return msvcrt.open_osfhandle(handle, flags)
    except BaseException:
        _CloseHandle(handle)
        raise


@functools.cache
def current_user_sid() -> str:
    """String SID of the token user of this process ("S-1-5-21-..." / Entra "S-1-12-1-...")."""
    token = _HANDLE()
    if not _OpenProcessToken(_GetCurrentProcess(), 0x0008, ctypes.byref(token)):      # TOKEN_QUERY
        raise _winerror(ctypes.get_last_error())
    try:
        size = wt.DWORD(0)
        _GetTokenInformation(token, 1, None, 0, ctypes.byref(size))                     # TokenUser
        buffer = (ctypes.c_ulonglong * ((size.value + 7) // 8 or 1))()
        if not _GetTokenInformation(token, 1, buffer, size, ctypes.byref(size)):
            raise _winerror(ctypes.get_last_error())
        return _sid_string(ctypes.c_void_p.from_buffer(buffer).value)
    finally:
        _CloseHandle(token)


def _sid_string(sid) -> str:
    text = ctypes.c_void_p()
    if not _ConvertSidToStringSidW(sid, ctypes.byref(text)):
        raise _winerror(ctypes.get_last_error())
    try:
        return ctypes.wstring_at(text.value)
    finally:
        _LocalFree(text)


@functools.cache
def _sid_pointer(text: str) -> int:
    sid = ctypes.c_void_p()
    if not _ConvertStringSidToSidW(text, ctypes.byref(sid)):
        raise _winerror(ctypes.get_last_error())
    return sid.value                                   # kept for the process lifetime


@functools.cache
def _effective_administrator() -> bool:
    member = wt.BOOL(False)
    if not _CheckTokenMembership(None, _sid_pointer(_ADMINISTRATORS_SID), ctypes.byref(member)):
        return False
    return bool(member.value)


@functools.cache
def _private_descriptor(directory: bool) -> int:
    sddl = (PRIVATE_DIR_SDDL if directory else PRIVATE_FILE_SDDL).format(sid=current_user_sid())
    descriptor = ctypes.c_void_p()
    if not _ConvertStringSdToSd(sddl, 1, ctypes.byref(descriptor), None):
        raise _winerror(ctypes.get_last_error())
    return descriptor.value                            # kept for the process lifetime (like C)


@functools.cache
def _private_dacl(directory: bool) -> int:
    present, defaulted, dacl = wt.BOOL(), wt.BOOL(), ctypes.c_void_p()
    if not _GetSecurityDescriptorDacl(_private_descriptor(directory), ctypes.byref(present), ctypes.byref(dacl),
                                      ctypes.byref(defaulted)) or not present.value or not dacl.value:
        raise OSError(errno.EINVAL, "private security descriptor has no DACL")
    return dacl.value


# ----------------------------------------------------------------------------- opening files


class _OpenSpec(NamedTuple):
    access: int
    share: int
    disposition: int
    options: int
    private: bool
    nofollow: bool


_OPEN_FLAG_MASK = (os.O_RDONLY | os.O_WRONLY | os.O_RDWR | os.O_APPEND | os.O_CREAT | os.O_EXCL | os.O_TRUNC |
                   os.O_BINARY | os.O_NOINHERIT | os.O_SEQUENTIAL | os.O_RANDOM | O_NOFOLLOW | O_DIRECTORY)


def _open_spec(flags: int, mode: int, *, nofollow: bool, private: bool | None, share_delete: bool,
               deny_write: bool, append_only: bool) -> _OpenSpec:
    if flags & os.O_TEXT:
        raise ValueError("text-mode descriptors are not supported (bytes must stay exact)")
    unknown = flags & ~_OPEN_FLAG_MASK
    if unknown:
        raise ValueError(f"unsupported open flags 0x{unknown:x}")
    accmode = flags & (os.O_WRONLY | os.O_RDWR)
    if accmode == (os.O_WRONLY | os.O_RDWR):
        raise OSError(errno.EINVAL, "invalid access mode")
    write = accmode != os.O_RDONLY
    directory = bool(flags & O_DIRECTORY)
    if directory and (write or flags & (os.O_CREAT | os.O_TRUNC)):
        if write:
            raise _errno_error(errno.EISDIR, "a directory can only be opened for reading")
        raise OSError(errno.EINVAL, "O_DIRECTORY cannot be combined with O_CREAT/O_TRUNC")
    if append_only and not (write and flags & os.O_APPEND):
        raise ValueError("append_only needs O_APPEND with write access")
    if append_only and flags & os.O_TRUNC:
        raise ValueError("append_only cannot truncate (O_TRUNC needs FILE_WRITE_DATA)")
    access = _SYNCHRONIZE | _FILE_READ_ATTRIBUTES | _READ_CONTROL
    if directory:
        access |= _FILE_LIST_DIRECTORY | _FILE_TRAVERSE
    else:
        if accmode in (os.O_RDONLY, os.O_RDWR):
            access |= _FILE_GENERIC_READ
        if write:
            access |= _FILE_GENERIC_WRITE
            if append_only:
                access &= ~_FILE_WRITE_DATA            # appends only, also through inherited handles
    if flags & os.O_CREAT and flags & os.O_EXCL:
        disposition = _FILE_CREATE
    elif flags & os.O_CREAT and flags & os.O_TRUNC:
        disposition = _FILE_OVERWRITE_IF
    elif flags & os.O_CREAT:
        disposition = _FILE_OPEN_IF
    elif flags & os.O_TRUNC:
        disposition = _FILE_OVERWRITE
    else:
        disposition = _FILE_OPEN
    follow = not (nofollow or flags & O_NOFOLLOW)
    options = _FILE_DIRECTORY_FILE if directory else _FILE_NON_DIRECTORY_FILE
    if not follow:
        options |= _FILE_OPEN_REPARSE_POINT
    if flags & os.O_SEQUENTIAL:
        options |= _FILE_SEQUENTIAL_ONLY
    if flags & os.O_RANDOM:
        options |= _FILE_RANDOM_ACCESS
    share = _FILE_SHARE_READ | (0 if deny_write else _FILE_SHARE_WRITE) | (_FILE_SHARE_DELETE if share_delete else 0)
    creating = bool(flags & os.O_CREAT)
    private_object = creating and (bool(private) if private is not None else (mode & 0o077) == 0)
    return _OpenSpec(access, share, disposition, options, private_object, not follow)


def _truncate_handle(handle, display) -> None:
    end = ctypes.c_longlong(0)                      # FILE_END_OF_FILE_INFO
    if not _SetFileInformationByHandle(handle, _FileEndOfFileInfo, ctypes.byref(end), ctypes.sizeof(end)):
        raise _winerror(ctypes.get_last_error(), display)


def _open_handle(root, name: str, spec: _OpenSpec, display) -> int:
    """Open (root, name) per spec with the no-follow post checks of core spec 4.3."""
    security = _private_descriptor(False) if spec.private else None
    # A no-follow O_TRUNC must not overwrite a link or a placeholder itself: open without
    # truncating, check what was opened, then truncate (open_target() in platform.c).
    truncate_after = spec.nofollow and spec.disposition in (_FILE_OVERWRITE, _FILE_OVERWRITE_IF)
    disposition = spec.disposition
    if truncate_after:
        disposition = _FILE_OPEN if spec.disposition == _FILE_OVERWRITE else _FILE_OPEN_IF
    creating = disposition in (_FILE_CREATE, _FILE_OPEN_IF, _FILE_OVERWRITE_IF)
    retry = _Retry()
    while True:
        status, handle = _nt_create(root, name, spec.access, spec.share, disposition, spec.options,
                                    security=security)
        if handle is not None:
            break
        # A name another program deleted the classic way stays "delete pending" until
        # its last handle closes: creating it again succeeds shortly.
        if (status in _RETRYABLE_STATUS or (creating and status == _STATUS_DELETE_PENDING)) and retry.allowed():
            retry.sleep()
            continue
        if spec.nofollow and status in (_STATUS_FILE_IS_A_DIRECTORY, _STATUS_NOT_A_DIRECTORY) and \
                _names_link(root, name):
            raise _loop_error(display)
        raise _status_error(status, display)
    if spec.nofollow:
        try:
            attributes, tag = _attributes_tag(handle)
        except BaseException:
            _CloseHandle(handle)
            raise
        if attributes & _FILE_ATTRIBUTE_REPARSE_POINT:
            if tag & _NAME_SURROGATE:
                _CloseHandle(handle)
                raise _loop_error(display)
            handle = _reopen_placeholder(root, name, spec, handle, display)      # consumes handle
    if truncate_after:
        try:
            _truncate_handle(handle, display)
        except BaseException:
            _CloseHandle(handle)
            raise
    return handle


def _reopen_placeholder(root, name: str, spec: _OpenSpec, handle, display) -> int:
    """OneDrive placeholder, dedup, WOF...: an ordinary file.  Open the file itself and
    insist that both opens see the same object.  Consumes `handle`."""
    try:
        status, again = _nt_create(root, name, spec.access, spec.share, _FILE_OPEN,
                                   spec.options & ~_FILE_OPEN_REPARSE_POINT)
        if again is None:
            raise _status_error(status, display)
    except BaseException:
        _CloseHandle(handle)
        raise
    try:
        same = _file_identity(handle) == _file_identity(again)
    except BaseException:
        _CloseHandle(again)
        raise
    finally:
        _CloseHandle(handle)
    if not same:
        _CloseHandle(again)
        raise _loop_error(display)
    return again


def open_flags(flags: int, *, nofollow: bool = False) -> int:
    """Flags for a direct os.open(): always binary and non-inheritable.  O_NOFOLLOW and
    O_DIRECTORY (and nofollow=True) cannot be honoured by os.open: ValueError, use
    open_fd() instead of silently following a link.  O_NONBLOCK is 0 (no FIFOs)."""
    if nofollow or flags & (O_NOFOLLOW | O_DIRECTORY):
        raise ValueError("O_NOFOLLOW/O_DIRECTORY need open_fd(); os.open() cannot honour them")
    return flags | os.O_BINARY | os.O_NOINHERIT


def open_fd(path, flags: int, mode: int = 0o666, *, nofollow: bool = False, private: bool | None = None,
            share_delete: bool = True, deny_write: bool = False, append_only: bool = False) -> int:
    """os.open replacement: binary, non-inheritable CRT descriptor whose handle shares
    READ|WRITE|DELETE (atomic replace/unlink of open files keeps working).

    flags: os.O_RDONLY/O_WRONLY/O_RDWR, O_CREAT, O_EXCL, O_TRUNC, O_APPEND, plus the shim
    bits O_NOFOLLOW (same as nofollow=True: the last component must not be a link,
    ELOOP) and O_DIRECTORY (read-only directory descriptor).  A directory without
    O_DIRECTORY raises IsADirectoryError.  When the call creates the file it gets the
    private descriptor if private=True, or, with private=None, if (mode & 0o077) == 0
    (the C rule); otherwise it inherits the parent's ACL.  deny_write=True refuses to
    share write access (a writer holding the file -> PermissionError after the retry
    budget).  append_only=True (with O_APPEND) opens a handle that can only append,
    also when it is handed to a child process.  Sharing violations are retried for
    RETRY_BUDGET_SECONDS."""
    spec = _open_spec(flags, mode, nofollow=nofollow, private=private, share_delete=share_delete,
                      deny_write=deny_write, append_only=append_only)
    display = _text(path)
    handle = _open_handle(None, _nt_path(display), spec, display)
    return _wrap_fd(handle, append=bool(flags & os.O_APPEND))


os_open = open_fd


def open_regular(path, *, write: bool = False, create: bool = False, mode: int = 0o600,
                 nofollow: bool = True) -> int:
    """No-follow open of a regular file (descriptor); OSError(EINVAL) for anything else."""
    descriptor = open_fd(path, (os.O_RDWR if write else os.O_RDONLY) | (os.O_CREAT if create else 0), mode,
                         nofollow=nofollow)
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise OSError(errno.EINVAL, "not a regular file", _text(path))
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor


def open_read(path, *, nofollow: bool = False):
    """Binary reader that never blocks others from replacing or deleting the file."""
    return os.fdopen(open_fd(path, os.O_RDONLY, nofollow=nofollow), "rb")


def read_bytes_shared(path, *, limit: int | None = None) -> bytes:
    """Read a (small) regular file without blocking a concurrent atomic replace."""
    with os.fdopen(open_regular(path), "rb") as stream:
        return stream.read() if limit is None else stream.read(limit)


def _open_directory_handle(root, name: str, display, *, pin: bool) -> int:
    share = (_FILE_SHARE_READ | _FILE_SHARE_WRITE) if pin else _SHARE_ALL
    spec = _OpenSpec(_FILE_LIST_DIRECTORY | _FILE_TRAVERSE | _FILE_READ_ATTRIBUTES | _SYNCHRONIZE, share,
                     _FILE_OPEN, _FILE_DIRECTORY_FILE | _FILE_OPEN_REPARSE_POINT, False, True)
    return _open_handle(root, name, spec, display)


def open_beneath(directory, name: str, *, deny_write: bool = True) -> tuple[int, os.stat_result]:
    """Replacement for open(dir, O_DIRECTORY|O_NOFOLLOW) + openat(dirfd, name, O_RDONLY|
    O_NOFOLLOW): `name` must be one valid component (OSError EINVAL), neither the
    directory nor the file may be a link (ELOOP), the file must be a regular disk file.
    The directory is pinned (cannot be renamed or removed) while the file is opened
    relative to its handle.  deny_write=True: nobody may hold or obtain write access
    (OSError EBUSY when a writer still has it open).  Returns (binary read fd, fstat)."""
    folder = _text(directory)
    if not isinstance(name, str) or not component_valid(name):
        raise OSError(errno.EINVAL, "file name must be one valid path component", str(name))
    display = os.path.join(folder, name)
    directory_handle = _open_directory_handle(None, _nt_path(folder), folder, pin=True)
    try:
        share = _FILE_SHARE_READ | _FILE_SHARE_DELETE | (0 if deny_write else _FILE_SHARE_WRITE)
        spec = _OpenSpec(_FILE_GENERIC_READ | _FILE_READ_ATTRIBUTES | _READ_CONTROL | _SYNCHRONIZE, share, _FILE_OPEN,
                         _FILE_NON_DIRECTORY_FILE | _FILE_OPEN_REPARSE_POINT | _FILE_SEQUENTIAL_ONLY, False, True)
        try:
            handle = _open_handle(directory_handle, name, spec, display)
        except PermissionError as exc:
            if exc.winerror == _ERROR_SHARING_VIOLATION:
                raise OSError(errno.EBUSY, "file is open for writing by another program", display) from None
            raise
    finally:
        _CloseHandle(directory_handle)
    if _GetFileType(handle) != _FILE_TYPE_DISK:
        _CloseHandle(handle)
        raise OSError(errno.EINVAL, "not a disk file", display)
    descriptor = _wrap_fd(handle)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode):
            raise OSError(errno.EINVAL, "not a regular file", display)
    except BaseException:
        os.close(descriptor)
        raise
    return descriptor, info


def open_nofollow_path(path, flags: int = os.O_RDONLY, mode: int = 0o666, *, private: bool | None = None,
                       deny_write: bool = False) -> int:
    """Open an absolute path walking every component from the drive/UNC root relative
    to the previous directory handle, never following a link anywhere in the path
    (radios_fs_open_nofollow_path).  ELOOP when a component is a link; EINVAL for
    relative paths, "." / ".." or invalid components."""
    text = _unify(_text(path))
    root_length = _root_length(text)
    if not root_length:
        raise OSError(errno.EINVAL, "absolute path required", _text(path))
    parts = [part for part in text[root_length:].split("\\") if part]
    if any(part in (".", "..") or not component_valid(part) for part in parts):
        raise OSError(errno.EINVAL, "invalid path component", _text(path))
    spec = _open_spec(flags | O_NOFOLLOW, mode, nofollow=True, private=private, share_delete=True,
                      deny_write=deny_write, append_only=False)
    root = text[:root_length] if text[:root_length].endswith("\\") else text[:root_length] + "\\"
    if not parts:
        if not flags & O_DIRECTORY:
            raise _errno_error(errno.EISDIR, _ERRNO_TEXT[errno.EISDIR], _text(path))
        return _wrap_fd(_open_directory_handle(None, _nt_path(root), root, pin=False))
    current = _open_directory_handle(None, _nt_path(root), root, pin=False)
    walked = root
    try:
        for part in parts[:-1]:
            walked = os.path.join(walked, part)
            following = _open_directory_handle(current, part, walked, pin=False)
            _CloseHandle(current)
            current = following
        handle = _open_handle(current, parts[-1], spec, _text(path))
    finally:
        _CloseHandle(current)
    return _wrap_fd(handle, append=bool(flags & os.O_APPEND))


def _temporary_name(prefix: str, suffix: str) -> str:
    name = f"{prefix}{secrets.token_hex(4)}{suffix}"
    if not component_valid(name):
        raise ValueError(f"invalid temporary name prefix/suffix: {prefix!r} {suffix!r}")
    return name


def mkstemp_private(dir=None, prefix: str = "tmp", suffix: str = "") -> tuple[int, str]:
    """tempfile.mkstemp() with the private file descriptor (created atomically private,
    O_EXCL, no-follow, binary, non-inheritable).  Returns (fd, absolute path)."""
    parent = os.path.abspath(_text(dir) if dir is not None else tempfile.gettempdir())
    for _ in range(100):
        candidate = os.path.join(parent, _temporary_name(prefix, suffix))
        try:
            return open_fd(candidate, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600, nofollow=True,
                           private=True), candidate
        except FileExistsError:
            continue
    raise FileExistsError(errno.EEXIST, "no usable temporary file name", parent)


# ----------------------------------------------------------------------------- durability


def fsync(target) -> None:
    """Flush a written file (fd or file object) to stable storage.  Pipes and consoles
    succeed without flushing; a read-only descriptor is flushed through a temporary
    writable handle when possible (POSIX fsync works on read-only fds)."""
    handle = _handle_of(target)
    if _GetFileType(handle) != _FILE_TYPE_DISK:
        return
    if _FlushFileBuffers(handle):
        return
    code = ctypes.get_last_error()
    if code != _ERROR_ACCESS_DENIED:
        raise _winerror(code)
    writable = _ReOpenFile(handle, _GENERIC_WRITE | _SYNCHRONIZE, _SHARE_ALL, 0)
    if writable in (None, _INVALID_HANDLE):
        return                                    # we cannot write it, so nothing of ours is dirty
    try:
        if not _FlushFileBuffers(writable):
            raise _winerror(ctypes.get_last_error())
    finally:
        _CloseHandle(writable)


def fsync_file(path) -> None:
    """Flush a file by path (opens it with write access; FlushFileBuffers needs it)."""
    descriptor = open_fd(path, os.O_RDWR, nofollow=True)
    try:
        fsync(descriptor)
    finally:
        os.close(descriptor)


def fsync_dir(path) -> None:
    """Directory fsync is a successful no-op on Windows (lead decision, C
    radios_fd_sync_dir): NTFS journals renames/creates, and the next file flush forces
    the log.  Like the Linux call it still fails for a missing path (FileNotFoundError)
    or a non-directory (NotADirectoryError)."""
    if not stat.S_ISDIR(os.stat(path).st_mode):
        raise _errno_error(errno.ENOTDIR, _ERRNO_TEXT[errno.ENOTDIR], _text(path))


def fsync_tree(root) -> None:
    """fsync_dir() for a published tree (no-op per directory on Windows)."""
    fsync_dir(root)


def sync_tree(root) -> int:
    """Windows replacement for os.sync() after a bulk copy (shutil.copytree): flush every
    regular file below root (FlushFileBuffers through a write handle, links never
    followed or entered).  root itself must be a real directory (a link -> ELOOP).
    Returns the number of files flushed; errors raise (a read-only file cannot be opened
    for writing: PermissionError — RadioS never creates read-only files)."""
    top = _text(root)
    if is_link(top):
        raise _loop_error(top)
    if not stat.S_ISDIR(os.stat(top).st_mode):
        raise _errno_error(errno.ENOTDIR, _ERRNO_TEXT[errno.ENOTDIR], top)
    flushed = 0
    for directory, subdirectories, files in os.walk(top, followlinks=False, onerror=_raise):
        subdirectories[:] = [name for name in subdirectories if not is_link(os.path.join(directory, name))]
        for name in files:
            path = os.path.join(directory, name)
            if is_link(path):
                continue
            fsync_file(path)
            flushed += 1
    return flushed


def _raise(error: OSError) -> None:
    raise error


# ----------------------------------------------------------------------------- rename / delete


def _rename_buffer(target_nt: str, flags: int):
    name = target_nt.encode("utf-16-le", "surrogatepass")
    size = 20 + len(name) + 2                     # FILE_RENAME_INFO header + name + NUL
    buffer = (ctypes.c_ulonglong * ((size + 7) // 8))()
    raw = memoryview(buffer).cast("B")
    struct.pack_into("<I4xQI", raw, 0, flags, 0, len(name))
    raw[20:20 + len(name)] = name
    return buffer, size


def _rename_handle(handle, target_nt: str, replace_existing: bool) -> int:
    attempts = ([_FILE_RENAME_REPLACE_IF_EXISTS | _FILE_RENAME_POSIX_SEMANTICS | _FILE_RENAME_IGNORE_READONLY_ATTRIBUTE,
                 _FILE_RENAME_REPLACE_IF_EXISTS | _FILE_RENAME_POSIX_SEMANTICS]
                if replace_existing else [_FILE_RENAME_POSIX_SEMANTICS])
    for flags in attempts:
        buffer, size = _rename_buffer(target_nt, flags)
        if _SetFileInformationByHandle(handle, _FileRenameInfoEx, buffer, size):
            return 0
        code = ctypes.get_last_error()
        if code not in _FALLBACK_INFO_CLASS:
            return code
    # FAT/exFAT/SMB/pre-1607: classic rename (BOOLEAN ReplaceIfExists in the first byte).
    buffer, size = _rename_buffer(target_nt, 1 if replace_existing else 0)
    if _SetFileInformationByHandle(handle, _FileRenameInfo, buffer, size):
        return 0
    return ctypes.get_last_error()


def _destination_kind(target_nt: str) -> str:
    status, handle = _nt_create(None, target_nt, _FILE_READ_ATTRIBUTES, _SHARE_ALL, _FILE_OPEN,
                                _FILE_OPEN_REPARSE_POINT)
    if handle is None:
        return "busy" if status == _STATUS_DELETE_PENDING or status in _RETRYABLE_STATUS else "missing"
    try:
        attributes, tag = _attributes_tag(handle)
    finally:
        _CloseHandle(handle)
    if attributes & _FILE_ATTRIBUTE_DIRECTORY and not _attributes_are_link(attributes, tag):
        return "directory"
    return "file"


def _rename(src, dst, *, replace_existing: bool, timeout: float | None) -> None:
    source, target = _text(src), _text(dst)
    source_nt, target_nt = _nt_path(source), _nt_path(target)
    retry = _Retry(timeout)
    while True:
        status, handle = _nt_create(None, source_nt, _DELETE | _FILE_READ_ATTRIBUTES, _SHARE_ALL, _FILE_OPEN,
                                    _FILE_OPEN_REPARSE_POINT)
        if handle is None:
            if status in _RETRYABLE_STATUS and retry.allowed():
                retry.sleep()
                continue
            raise _status_error(status, source, target)
        try:
            attributes, tag = _attributes_tag(handle)
            code = _rename_handle(handle, target_nt, replace_existing)
        finally:
            _CloseHandle(handle)
        if code == 0:
            return
        source_is_directory = bool(attributes & _FILE_ATTRIBUTE_DIRECTORY) and not _attributes_are_link(attributes, tag)
        transient = code in (_ERROR_SHARING_VIOLATION, _ERROR_LOCK_VIOLATION, _ERROR_USER_MAPPED_FILE)
        if code in (_ERROR_ALREADY_EXISTS, _ERROR_FILE_EXISTS) and not replace_existing:
            raise _winerror(code, source, target)
        if code in (_ERROR_ACCESS_DENIED, _ERROR_ALREADY_EXISTS):
            kind = _destination_kind(target_nt)
            if kind == "directory" and replace_existing:
                raise _errno_error(errno.EISDIR, "cannot replace a directory", source, target)
            # a regular target held by a reader without FILE_SHARE_DELETE, a delete-pending
            # target, or a directory source with a child still open (antivirus) is transient
            transient = transient or kind in ("file", "busy") or source_is_directory
        if transient and retry.allowed():
            retry.sleep()
            continue
        raise _winerror(code, source, target)


def replace(src, dst, *, timeout: float | None = None) -> None:
    """os.replace with POSIX semantics: atomically replaces dst even while others read it
    with FILE_SHARE_DELETE (old readers keep the old bytes); renames a link itself, never
    its target.  Sharing violations (antivirus, indexer, plain open() readers) are retried
    until `timeout` (default RETRY_BUDGET_SECONDS); then PermissionError.  A directory
    destination raises IsADirectoryError at once; a different volume raises OSError
    (EXDEV).  FAT/exFAT/SMB fall back to a classic replace."""
    _rename(src, dst, replace_existing=True, timeout=timeout)


def rename_noreplace(src, dst, *, timeout: float | None = None) -> None:
    """Publish src under a new name, never overwriting: FileExistsError when dst exists
    (source untouched).  Files and directories; works on FAT/exFAT/SMB.  Unlike a POSIX
    hard link the source name is consumed."""
    _rename(src, dst, replace_existing=False, timeout=timeout)


link_noreplace = rename_noreplace                   # engine-spec name (was os.link publication)
publish_dir_noreplace = rename_noreplace            # engine-spec name (was renameat2 RENAME_NOREPLACE)


def _delete_handle(handle) -> int:
    flags = struct.pack("<I", _FILE_DISPOSITION_DELETE | _FILE_DISPOSITION_POSIX_SEMANTICS |
                        _FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE)
    buffer = ctypes.create_string_buffer(flags, 4)
    if _SetFileInformationByHandle(handle, _FileDispositionInfoEx, buffer, 4):
        return 0
    code = ctypes.get_last_error()
    if code not in _FALLBACK_INFO_CLASS:
        return code
    classic = ctypes.create_string_buffer(b"\x01", 1)   # FILE_DISPOSITION_INFO{DeleteFile = TRUE}
    if _SetFileInformationByHandle(handle, _FileDispositionInfo, classic, 1):
        return 0
    return ctypes.get_last_error()


def _clear_readonly(handle, attributes: int) -> bool:
    """Classic deletes (FAT/exFAT) refuse read-only files: drop the attribute."""
    status, writable = _nt_create(handle, "", _FILE_WRITE_ATTRIBUTES, _SHARE_ALL, _FILE_OPEN, _FILE_OPEN_REPARSE_POINT)
    if writable is None:
        return False
    try:
        info = _FILE_BASIC_INFO(0, 0, 0, 0, (attributes & _SETTABLE_ATTRIBUTES & ~_FILE_ATTRIBUTE_READONLY) or
                                _FILE_ATTRIBUTE_NORMAL)
        return bool(_SetFileInformationByHandle(writable, _FileBasicInfo, ctypes.byref(info), ctypes.sizeof(info)))
    finally:
        _CloseHandle(writable)


def _delete_with_retry(handle, display, retry: _Retry, attributes: int) -> None:
    cleared = False
    while True:
        code = _delete_handle(handle)
        if code == 0:
            return
        if code == _ERROR_ACCESS_DENIED and attributes & _FILE_ATTRIBUTE_READONLY and not cleared:
            cleared = True
            if _clear_readonly(handle, attributes):
                continue
        # 32/33: held without FILE_SHARE_DELETE; 5: mapped or being scanned; 145: classic
        # deletes of children still pending (FAT/SMB)
        if code in (_ERROR_ACCESS_DENIED, _ERROR_SHARING_VIOLATION, _ERROR_LOCK_VIOLATION,
                    _ERROR_DIR_NOT_EMPTY) and retry.allowed():
            retry.sleep()
            continue
        raise _winerror(code, display)


def unlink(path, *, missing_ok: bool = False, timeout: float | None = None) -> None:
    """Delete a file or a link (never follows) with POSIX semantics: the name is free at
    once even while others still read the file with FILE_SHARE_DELETE.  Sharing
    violations are retried until `timeout`.  A real directory -> IsADirectoryError."""
    display = _text(path)
    target_nt = _nt_path(display)
    retry = _Retry(timeout)
    while True:
        status, handle = _nt_create(None, target_nt, _DELETE | _FILE_READ_ATTRIBUTES, _SHARE_ALL, _FILE_OPEN,
                                    _FILE_OPEN_REPARSE_POINT)
        if handle is not None:
            break
        if status in _RETRYABLE_STATUS and retry.allowed():
            retry.sleep()
            continue
        error = _status_error(status, display)
        if missing_ok and isinstance(error, FileNotFoundError):
            return
        raise error
    try:
        attributes, tag = _attributes_tag(handle)
        if attributes & _FILE_ATTRIBUTE_DIRECTORY and not _attributes_are_link(attributes, tag):
            raise _errno_error(errno.EISDIR, _ERRNO_TEXT[errno.EISDIR], display)
        _delete_with_retry(handle, display, retry, attributes)
    finally:
        _CloseHandle(handle)


def unlink_quietly(path) -> bool:
    """Best-effort cleanup for finally blocks: never raises; False (and a log warning)
    when the file still exists afterwards."""
    try:
        unlink(path, missing_ok=True)
        return True
    except (OSError, ValueError) as exc:
        _log.warning("RadioS: geçici dosya silinemedi: %s (%s)", path, exc)
        return False


def _list_directory(handle) -> list[tuple[str, int]]:
    """(name, attributes) of every entry of a directory handle ("." and ".." skipped)."""
    entries: list[tuple[str, int]] = []
    buffer = (ctypes.c_ulonglong * 8192)()          # 64 KiB, 8-byte aligned
    raw = memoryview(buffer).cast("B")
    info_class = _FileFullDirectoryRestartInfo
    while True:
        if not _GetFileInformationByHandleEx(handle, info_class, buffer, ctypes.sizeof(buffer)):
            code = ctypes.get_last_error()
            if code == _ERROR_NO_MORE_FILES:
                return entries
            raise _winerror(code)
        info_class = _FileFullDirectoryInfo
        offset = 0
        while True:
            next_offset, = struct.unpack_from("<I", raw, offset)
            attributes, name_length = struct.unpack_from("<II", raw, offset + 56)
            name = bytes(raw[offset + 68:offset + 68 + name_length]).decode("utf-16-le", "surrogatepass")
            if name not in (".", ".."):
                entries.append((name, attributes))
            if not next_offset:
                break
            offset += next_offset


_TREE_DIRECTORY_ACCESS = _FILE_LIST_DIRECTORY | _FILE_TRAVERSE | _DELETE | _FILE_READ_ATTRIBUTES


def _open_tree_child(parent, name: str, listed_attributes: int, display, retry: _Retry):
    """Open an entry for deletion relative to its directory handle, never following it.
    Returns (handle, attributes, tag) or None when the entry vanished."""
    directory = bool(listed_attributes & _FILE_ATTRIBUTE_DIRECTORY)
    flips = 0
    while True:
        access = _TREE_DIRECTORY_ACCESS if directory else (_DELETE | _FILE_READ_ATTRIBUTES)
        options = (_FILE_DIRECTORY_FILE if directory else _FILE_NON_DIRECTORY_FILE) | _FILE_OPEN_REPARSE_POINT
        status, handle = _nt_create(parent, name, access, _SHARE_ALL, _FILE_OPEN, options)
        if handle is not None:
            try:
                attributes, tag = _attributes_tag(handle)
            except BaseException:
                _CloseHandle(handle)
                raise
            return handle, attributes, tag
        if status in _MISSING_STATUS:
            return None
        if status in (_STATUS_FILE_IS_A_DIRECTORY, _STATUS_NOT_A_DIRECTORY) and flips < 4:
            directory, flips = not directory, flips + 1     # replaced since it was listed
            continue
        if status in _RETRYABLE_STATUS and retry.allowed():
            retry.sleep()
            continue
        raise _status_error(status, display)


def _remove_tree_contents(top, display, retry: _Retry) -> None:
    """Delete everything below the directory handle `top`, handle-relative all the way
    down: links (incl. junctions) are deleted, never entered; renaming or swapping a
    directory for a junction during the walk cannot redirect it."""
    stack = [[top, display, _list_directory(top), False, _FILE_ATTRIBUTE_DIRECTORY]]
    try:
        while stack:
            frame = stack[-1]
            handle, where, names, owned, directory_attributes = frame
            if not names:
                stack.pop()
                if owned:
                    try:
                        _delete_with_retry(handle, where, retry.entry(), directory_attributes)
                    finally:
                        _CloseHandle(handle)
                continue
            name, listed = names.pop()
            child_display = os.path.join(where, name)
            child_retry = retry.entry()
            opened = _open_tree_child(handle, name, listed, child_display, child_retry)
            if opened is None:
                continue
            child, attributes, tag = opened
            if attributes & _FILE_ATTRIBUTE_DIRECTORY and not _attributes_are_link(attributes, tag):
                try:
                    entries = _list_directory(child)
                except BaseException:
                    _CloseHandle(child)
                    raise
                stack.append([child, child_display, entries, True, attributes])
                continue
            try:
                _delete_with_retry(child, child_display, child_retry, attributes)
            finally:
                _CloseHandle(child)
    finally:
        for frame in stack:
            if frame[3]:
                _CloseHandle(frame[0])


def rmtree(path, *, timeout: float | None = None, ignore_errors: bool = False) -> bool:
    """shutil.rmtree for Windows: POSIX deletes (open share-delete files do not block),
    read-only files handled, links/junctions removed but never entered (a link as
    `path` itself is refused), sharing violations retried until one overall deadline
    (`timeout`, default RETRY_BUDGET_SECONDS).  Returns True when the tree is gone;
    with ignore_errors=True failures return False instead of raising (use it in
    finally blocks so the original exception is not masked)."""
    display = _text(path)
    retry = _Retry(timeout)
    try:
        status, top = _nt_create(None, _nt_path(display), _TREE_DIRECTORY_ACCESS, _SHARE_ALL, _FILE_OPEN,
                                 _FILE_OPEN_REPARSE_POINT)
        if top is None:
            raise _status_error(status, display)
        try:
            attributes, tag = _attributes_tag(top)
            if _attributes_are_link(attributes, tag):
                raise _errno_error(errno.ELOOP, "cannot call rmtree on a symbolic link or junction", display)
            if not attributes & _FILE_ATTRIBUTE_DIRECTORY:
                raise _errno_error(errno.ENOTDIR, _ERRNO_TEXT[errno.ENOTDIR], display)
            _remove_tree_contents(top, display, retry)
            _delete_with_retry(top, display, retry.entry(), attributes)
        finally:
            _CloseHandle(top)
        return True
    except FileNotFoundError:
        if ignore_errors:
            return True
        raise
    except OSError as exc:
        if ignore_errors:
            _log.warning("RadioS: klasör tamamen silinemedi: %s (%s)", display, exc)
            return False
        raise


def rmtree_quietly(path) -> bool:
    """rmtree(path, ignore_errors=True): never raises, False when something remains."""
    try:
        return rmtree(path, ignore_errors=True)
    except (TypeError, ValueError) as exc:
        _log.warning("RadioS: klasör silinemedi: %s (%s)", path, exc)
        return False


# ----------------------------------------------------------------------------- locks


def flock(target, operation: int) -> None:
    """fcntl.flock() on Windows (fd or file object): LockFileEx/UnlockFileEx on the shared
    1-byte region at LOCK_REGION_OFFSET.  Locks belong to the handle: two descriptors of
    one process conflict like two processes; closing the handle or the process exit
    releases them.  Every request first drops this handle's own lock, so re-locking is
    idempotent and SH<->EX converts (not atomically, as on Linux).  LOCK_NB contention
    -> BlockingIOError(EAGAIN).  The region lies far beyond any file data, so these
    (mandatory) locks never block reads or writes of the content."""
    kind = operation & (LOCK_SH | LOCK_EX | LOCK_UN)
    if kind not in (LOCK_SH, LOCK_EX, LOCK_UN) or operation & ~(LOCK_SH | LOCK_EX | LOCK_UN | LOCK_NB):
        raise ValueError(f"invalid flock operation {operation!r}")
    handle = _handle_of(target)
    overlapped = _OVERLAPPED(0, 0, LOCK_REGION_OFFSET & 0xFFFFFFFF, LOCK_REGION_OFFSET >> 32, None)
    if not _UnlockFileEx(handle, 0, LOCK_REGION_LENGTH, 0, ctypes.byref(overlapped)):
        code = ctypes.get_last_error()
        if code != _ERROR_NOT_LOCKED:
            raise _winerror(code)
    if kind == LOCK_UN:
        return
    flags = (_LOCKFILE_EXCLUSIVE_LOCK if kind == LOCK_EX else 0) | (_LOCKFILE_FAIL_IMMEDIATELY if operation & LOCK_NB else 0)
    overlapped = _OVERLAPPED(0, 0, LOCK_REGION_OFFSET & 0xFFFFFFFF, LOCK_REGION_OFFSET >> 32, None)
    if _LockFileEx(handle, flags, 0, LOCK_REGION_LENGTH, 0, ctypes.byref(overlapped)):
        return
    code = ctypes.get_last_error()
    if code in (_ERROR_LOCK_VIOLATION, _ERROR_IO_PENDING):
        raise BlockingIOError(errno.EAGAIN, "the file lock is held by another handle")
    raise _winerror(code)


def lock(target, *, exclusive: bool | None = None, shared: bool | None = None, blocking: bool = True,
         timeout: float | None = None, poll: float = 0.01) -> None:
    """Take the flock() region.  Exclusive unless exclusive=False or shared=True.
    blocking=False: one attempt (BlockingIOError on contention).  timeout: poll with
    LOCK_NB every `poll` seconds, TimeoutError on expiry.  Otherwise a blocking wait."""
    if exclusive is None:
        exclusive = not shared if shared is not None else True
    elif shared is not None and bool(shared) == bool(exclusive):
        raise ValueError("lock(): exclusive and shared contradict each other")
    mode = LOCK_EX if exclusive else LOCK_SH
    if timeout is not None:
        if not blocking:
            raise ValueError("lock(): blocking=False and timeout are mutually exclusive")
        deadline = time.monotonic() + max(0.0, float(timeout))
        while True:
            try:
                flock(target, mode | LOCK_NB)
                return
            except BlockingIOError:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("file lock wait timed out") from None
                time.sleep(min(max(poll, 0.001), remaining))
    flock(target, mode if blocking else mode | LOCK_NB)


def unlock(target) -> None:
    """Release the flock() region (no error when it was not locked)."""
    flock(target, LOCK_UN)


def unlock_and_close(descriptor: int) -> None:
    """Unlock explicitly, then close (Windows releases locks of closed handles lazily)."""
    try:
        flock(descriptor, LOCK_UN)
    finally:
        os.close(descriptor)


@contextlib.contextmanager
def locked_file(path, *, exclusive: bool, timeout: float | None = None, create: bool = False,
                write: bool = False, mode: int = 0o600):
    """Context manager: open a regular lock file (no-follow) and hold the lock region."""
    descriptor = open_regular(path, write=write or create, create=create, mode=mode)
    try:
        lock(descriptor, exclusive=exclusive, timeout=timeout)
    except BaseException:
        os.close(descriptor)
        raise
    try:
        yield descriptor
    finally:
        unlock_and_close(descriptor)


# ----------------------------------------------------------------------------- positional I/O, space, access


def pread(descriptor: int, length: int, offset: int) -> bytes:
    """os.pread(): read up to `length` bytes at `offset` (b"" at EOF); the descriptor's
    file position is preserved."""
    if length < 0 or offset < 0:
        raise ValueError("pread length and offset must not be negative")
    if length == 0:
        return b""
    length = min(length, 0x7FFFF000)
    handle = _handle_of(descriptor)
    position = os.lseek(descriptor, 0, os.SEEK_CUR)
    buffer = ctypes.create_string_buffer(length)
    done = wt.DWORD(0)
    overlapped = _OVERLAPPED(0, 0, offset & 0xFFFFFFFF, offset >> 32, None)
    try:
        if not _ReadFile(handle, buffer, length, ctypes.byref(done), ctypes.byref(overlapped)):
            code = ctypes.get_last_error()
            if code == _ERROR_HANDLE_EOF:
                return b""
            raise _winerror(code)
        return buffer.raw[:done.value]
    finally:
        os.lseek(descriptor, position, os.SEEK_SET)


def disk_usage(path) -> tuple[int, int]:
    """(total bytes, bytes available to this user) of the volume holding path — statvfs
    f_blocks*f_frsize / f_bavail*f_frsize (quota-aware; shutil.disk_usage().free is the
    total free space instead)."""
    text = _text(path)
    folder = text if stat.S_ISDIR(os.stat(text).st_mode) else os.path.dirname(os.path.abspath(text))
    native = _win32_path(folder)
    if not native.endswith("\\"):
        native += "\\"
    available, total = ctypes.c_ulonglong(), ctypes.c_ulonglong()
    if not _GetDiskFreeSpaceExW(native, ctypes.byref(available), ctypes.byref(total), None):
        raise _winerror(ctypes.get_last_error(), text)
    return total.value, available.value


def disk_free(path) -> int:
    """Bytes available to this user on the volume holding path (POSIX f_bavail*f_frsize)."""
    return disk_usage(path)[1]


def dir_writable(path) -> bool:
    """Real read+write access to a directory (ACLs, read-only media, Controlled Folder
    Access): lists it and creates/deletes a hidden probe file (core spec 4.8)."""
    try:
        with os.scandir(path) as entries:
            next(entries, None)
        probe = os.path.join(_text(path), f".radios-access-{os.getpid()}-{secrets.token_hex(4)}")
        handle = _CreateFileW(_win32_path(probe), _GENERIC_WRITE | _DELETE, 0, None, _CREATE_NEW,
                              _FILE_ATTRIBUTE_HIDDEN | _FILE_ATTRIBUTE_TEMPORARY | _FILE_FLAG_DELETE_ON_CLOSE, None)
    except (OSError, ValueError):
        return False
    if handle in (None, _INVALID_HANDLE):
        return False
    _CloseHandle(handle)
    return True


# ----------------------------------------------------------------------------- privacy (core spec 5.2)


@contextlib.contextmanager
def _security_target(target, follow_links: bool, access: int):
    """Handle for security queries: borrowed for fds/file objects, opened for paths."""
    if isinstance(target, int) or hasattr(target, "fileno"):
        yield _handle_of(target), f"<descriptor {target if isinstance(target, int) else target.fileno()}>"
        return
    display = _text(target)
    options = 0 if follow_links else _FILE_OPEN_REPARSE_POINT
    status, handle = _nt_create(None, _nt_path(display), access, _SHARE_ALL, _FILE_OPEN, options)
    if handle is None:
        raise _status_error(status, display)
    try:
        yield handle, display
    finally:
        _CloseHandle(handle)


def _volume_has_acls(handle) -> bool:
    flags = wt.DWORD(0)
    if not _GetVolumeInformationByHandleW(handle, None, 0, None, None, ctypes.byref(flags), None, 0):
        raise _winerror(ctypes.get_last_error())
    return bool(flags.value & _FILE_PERSISTENT_ACLS)


def _query_security(handle) -> tuple[str | None, list[tuple[int, int, int, str]] | None]:
    """(owner SID, [(ace type, ace flags, mask, sid)] or None for a NULL DACL).  Allow ACEs
    of every kind and plain deny ACEs; audit ACEs and other types are skipped."""
    owner, dacl, descriptor = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
    code = _GetSecurityInfo(handle, _SE_FILE_OBJECT, _OWNER_SECURITY_INFORMATION | _DACL_SECURITY_INFORMATION,
                            ctypes.byref(owner), None, ctypes.byref(dacl), None, ctypes.byref(descriptor))
    if code:
        raise _winerror(code)
    try:
        owner_sid = _sid_string(owner.value) if owner.value else None
        if not dacl.value:
            return owner_sid, None
        aces = []
        count = ctypes.c_ushort.from_address(dacl.value + 4).value          # ACL.AceCount
        for index in range(count):
            ace = ctypes.c_void_p()
            if not _GetAce(dacl, index, ctypes.byref(ace)):
                raise _winerror(ctypes.get_last_error())
            address = ace.value
            ace_type = ctypes.c_ubyte.from_address(address).value
            ace_flags = ctypes.c_ubyte.from_address(address + 1).value
            if ace_type not in _ALLOW_ACE_TYPES and ace_type != _ACCESS_DENIED_ACE_TYPE:
                continue
            mask = ctypes.c_uint32.from_address(address + 4).value
            sid_address = address + 8
            if ace_type in _OBJECT_ACE_TYPES:
                object_flags = ctypes.c_uint32.from_address(address + 8).value
                sid_address = address + 12 + (16 if object_flags & 0x1 else 0) + (16 if object_flags & 0x2 else 0)
            aces.append((ace_type, ace_flags, mask, _sid_string(sid_address)))
        return owner_sid, aces
    finally:
        _LocalFree(descriptor)


def _owner_ok(owner_sid: str | None) -> bool:
    if owner_sid is None:
        return False
    return owner_sid == current_user_sid() or (owner_sid == _ADMINISTRATORS_SID and _effective_administrator())


def _foreign_grants(aces, owner_sid: str | None, rights: int) -> list[str]:
    trusted = PRIVATE_TRUSTED_SIDS | {current_user_sid()} | ({owner_sid} if owner_sid else set())
    return [sid for ace_type, flags, mask, sid in aces                # deny ACEs never widen access
            if ace_type in _ALLOW_ACE_TYPES and not flags & _INHERIT_ONLY_ACE and mask & rights and sid not in trusted]


def _user_has_full_control(aces) -> bool:
    """The DACL grants the token user FILE_ALL_ACCESS through plain allow ACEs and denies it
    nothing (dacl_user_full() in platform.c): with the owner and no-others rules this is
    what fchmod(0600/0700) produces."""
    user, granted = current_user_sid(), 0
    for ace_type, flags, mask, sid in aces:
        if flags & _INHERIT_ONLY_ACE or sid != user:
            continue
        if ace_type == _ACCESS_DENIED_ACE_TYPE:
            return False
        if ace_type == 0:
            granted |= _FILE_ALL_ACCESS if mask & _GENERIC_ALL else mask
    return granted & _FILE_ALL_ACCESS == _FILE_ALL_ACCESS


def _canonical_private(owner_sid: str | None, aces) -> bool:
    """handle_is_canonical_private() in platform.c: owner is the token user (not merely
    Administrators), no foreign grants, the user holds full control."""
    return (owner_sid == current_user_sid() and aces is not None and
            not _foreign_grants(aces, owner_sid, PRIVATE_SENSITIVE_RIGHTS) and _user_has_full_control(aces))


def owned_by_current_user(target, *, follow_links: bool = False) -> bool:
    """Owner rule (RADIOS_PRIVATE_OWNER): owner == token user, or owner == Administrators
    while this token is an effective administrator.  False on volumes without ACLs.
    target: path (link not followed by default), fd or file object.  FileNotFoundError
    for a missing path; an object whose security we may not read is not ours (False)."""
    try:
        with _security_target(target, follow_links, _READ_CONTROL | _FILE_READ_ATTRIBUTES) as (handle, _display):
            if not _volume_has_acls(handle):
                return False
            owner, _aces = _query_security(handle)
            return _owner_ok(owner)
    except PermissionError:
        return False


def is_private(target, *, follow_links: bool = False, directory: bool = False,
               exact_file_mode: bool = False) -> bool:
    """Linux "owner == uid and mode & 077 == 0": the owner rule plus the no-others rule
    (RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_NO_OTHERS).  directory=True additionally
    requires a directory, exact_file_mode=True a regular file (RADIOS_PRIVATE_EXACT_FILE_MODE).
    Volumes without persistent ACLs (FAT/exFAT) are never private (fail closed)."""
    try:
        with _security_target(target, follow_links, _READ_CONTROL | _FILE_READ_ATTRIBUTES) as (handle, _display):
            if not _volume_has_acls(handle):
                return False
            owner, aces = _query_security(handle)
            if not _owner_ok(owner) or aces is None or _foreign_grants(aces, owner, PRIVATE_SENSITIVE_RIGHTS):
                return False
            if directory or exact_file_mode:
                attributes, tag = _attributes_tag(handle)
                if _attributes_are_link(attributes, tag):
                    return False
                is_directory = bool(attributes & _FILE_ATTRIBUTE_DIRECTORY)
                if directory and not is_directory:
                    return False
                if exact_file_mode and (is_directory or _GetFileType(handle) != _FILE_TYPE_DISK):
                    return False
            return True
    except PermissionError:
        return False


def private_dir_ok(path) -> bool:
    """A real directory (not a link) that is private (owner and no-others rules)."""
    return not is_link(path) and is_private(path, directory=True)


def foreign_access(target, *, rights: int = PRIVATE_SENSITIVE_RIGHTS, follow_links: bool = False) -> list[str]:
    """SIDs other than user/owner/SYSTEM/Administrators/OWNER RIGHTS that an effective
    allow ACE grants any of `rights` (diagnostics: why is this not private?).  A NULL
    DACL is reported as ["S-1-1-0"] (Everyone)."""
    with _security_target(target, follow_links, _READ_CONTROL | _FILE_READ_ATTRIBUTES) as (handle, _display):
        owner, aces = _query_security(handle)
        if aces is None:
            return ["S-1-1-0"]
        return sorted(set(_foreign_grants(aces, owner, rights)))


def security_sddl(target, *, follow_links: bool = False) -> str:
    """Owner + DACL of a path/fd as an SDDL string (diagnostics and tests)."""
    with _security_target(target, follow_links, _READ_CONTROL | _FILE_READ_ATTRIBUTES) as (handle, _display):
        owner, dacl, descriptor = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
        code = _GetSecurityInfo(handle, _SE_FILE_OBJECT, _OWNER_SECURITY_INFORMATION | _DACL_SECURITY_INFORMATION,
                                ctypes.byref(owner), None, ctypes.byref(dacl), None, ctypes.byref(descriptor))
        if code:
            raise _winerror(code)
        try:
            text = ctypes.c_void_p()
            if not _ConvertSdToStringSd(descriptor, 1, _OWNER_SECURITY_INFORMATION | _DACL_SECURITY_INFORMATION,
                                        ctypes.byref(text), None):
                raise _winerror(ctypes.get_last_error())
            try:
                return ctypes.wstring_at(text.value)
            finally:
                _LocalFree(text)
        finally:
            _LocalFree(descriptor)


def acl_supported(target) -> bool:
    """The volume holding target keeps persistent ACLs (NTFS/ReFS; not FAT/exFAT)."""
    with _security_target(target, True, _FILE_READ_ATTRIBUTES) as (handle, _display):
        return _volume_has_acls(handle)


def _set_security(handle, options: int, rights: int, what: int, owner, dacl, display) -> int:
    """SetSecurityInfo through a relative reopen of `handle` that holds `rights`
    (descriptors opened for data access have no WRITE_DAC/WRITE_OWNER).  Returns the
    Win32 error code (0 on success)."""
    status, writable = _nt_create(handle, "", rights | _READ_CONTROL | _FILE_READ_ATTRIBUTES, _SHARE_ALL, _FILE_OPEN,
                                  options)
    if writable is None:
        return _RtlNtStatusToDosError(status)
    try:
        return _SetSecurityInfo(writable, _SE_FILE_OBJECT, what, owner, None, dacl, None)
    finally:
        _CloseHandle(writable)


def make_private(target, *, follow_links: bool = False, directory: bool | None = None) -> None:
    """fchmod(0600 file / 0700 dir) (radios_fd_make_private): the protected private DACL
    (file or directory variant by type; inheritable ACEs propagate into an existing
    tree), then owner = current user.  No-op when the object already is in that canonical
    form (owner is the user, no foreign grants, the user has full control).  The DACL is
    set first (owners hold WRITE_DAC implicitly) and the owner second (the new DACL grants
    the user WRITE_OWNER), so files an elevated run left owned by Administrators are taken
    over without SeTakeOwnershipPrivilege.  directory, when given, must match the type.
    A link is refused (ELOOP) unless follow_links=True makes its target private.
    OSError(ENOTSUP) on volumes without persistent ACLs; PermissionError when the object
    belongs to someone else and we may not take it over."""
    with _security_target(target, follow_links, _READ_CONTROL | _FILE_READ_ATTRIBUTES) as (handle, display):
        if not _volume_has_acls(handle):
            raise OSError(errno.ENOTSUP, "the volume has no persistent ACLs (FAT/exFAT); use NTFS", display)
        attributes, tag = _attributes_tag(handle)
        if _attributes_are_link(attributes, tag):
            raise _loop_error(display)
        is_directory = bool(attributes & _FILE_ATTRIBUTE_DIRECTORY)
        if directory is not None and bool(directory) != is_directory:
            raise _errno_error(errno.ENOTDIR if directory else errno.EISDIR,
                               _ERRNO_TEXT[errno.ENOTDIR if directory else errno.EISDIR], display)
        owner, aces = _query_security(handle)
        if _canonical_private(owner, aces):
            return
        options = 0 if follow_links or isinstance(target, int) or hasattr(target, "fileno") else _FILE_OPEN_REPARSE_POINT
        code = _set_security(handle, options, _WRITE_DAC, _DACL_SECURITY_INFORMATION |
                             _PROTECTED_DACL_SECURITY_INFORMATION, None, _private_dacl(is_directory), display)
        if code:
            raise _winerror(code, display)
        if owner == current_user_sid():
            return
        code = _set_security(handle, options, _WRITE_OWNER, _OWNER_SECURITY_INFORMATION,
                             _sid_pointer(current_user_sid()), None, display)
        if code and not _owner_ok(owner):
            raise _winerror(code, display)


def mkdir_private(path, *, parents: bool = False, exist_ok: bool = False) -> None:
    """mkdir(path, 0o700): created with owner = current user and the protected private DACL
    (also when elevated: CPython's own 0o700 makes Administrators the owner then).
    parents=True creates missing parents with inherited security (pathlib semantics).
    exist_ok=True accepts an existing real directory (not a link -> ELOOP) owned by the
    current user (else PermissionError) and makes it private if it is not; on volumes
    without ACLs there is nothing to verify or repair."""
    display = _text(path)
    if parents:
        parent = os.path.dirname(os.path.abspath(display))
        if parent and not os.path.isdir(parent):
            os.makedirs(parent, exist_ok=True)
    status, handle = _nt_create(None, _nt_path(display), _FILE_LIST_DIRECTORY, _SHARE_ALL, _FILE_CREATE,
                                _FILE_DIRECTORY_FILE, security=_private_descriptor(True))
    if handle is not None:
        _CloseHandle(handle)
        return
    if status != _STATUS_OBJECT_NAME_COLLISION or not exist_ok:
        raise _status_error(status, display)
    info = os.lstat(display)
    if stat_is_link(info):
        raise _loop_error(display)
    if not stat.S_ISDIR(info.st_mode):
        raise FileExistsError(errno.EEXIST, "exists and is not a directory", display)
    if not acl_supported(display):
        return
    if not owned_by_current_user(display):
        raise PermissionError(errno.EACCES, "the directory is not owned by the current user", display)
    make_private(display, directory=True)               # Linux: chmod(0o700); no-op when canonical


def mkdtemp_private(prefix: str = "tmp", suffix: str = "", dir=None) -> str:
    """tempfile.mkdtemp() with the private directory descriptor; name = prefix + 8 hex
    characters + suffix.  Returns the absolute path."""
    parent = os.path.abspath(_text(dir) if dir is not None else tempfile.gettempdir())
    for _ in range(100):
        candidate = os.path.join(parent, _temporary_name(prefix, suffix))
        try:
            mkdir_private(candidate)
            return candidate
        except FileExistsError:
            continue
    raise FileExistsError(errno.EEXIST, "no usable temporary directory name", parent)


@contextlib.contextmanager
def temporary_directory(prefix: str = "tmp", suffix: str = "", dir=None):
    """tempfile.TemporaryDirectory(): private directory, removed with rmtree() on exit.
    A cleanup failure never masks the body's exception (it is logged; recovery code
    removes leftovers)."""
    path = mkdtemp_private(prefix=prefix, suffix=suffix, dir=dir)
    try:
        yield path
    finally:
        rmtree(path, ignore_errors=True)


# ----------------------------------------------------------------------------- process identity


class ProcessInfo(NamedTuple):
    pid: int
    creation_ticks: int                 # 100 ns ticks since 1601-01-01 UTC
    image_path: str | None              # None when not readable (other user, protected)
    argv: list[str] | None              # None when not readable


def _filetime_ticks(value: wt.FILETIME) -> int:
    return (value.dwHighDateTime << 32) | value.dwLowDateTime


class PinnedProcess:
    """An open handle to a process: while it is held the pid cannot be reused, so
    identity checks and stop requests cannot hit a different process (the Windows
    analogue of a pidfd).  Use open(); close() or use it as a context manager."""

    __slots__ = ("pid", "creation_ticks", "_handle")

    def __init__(self, handle: int, pid: int, creation_ticks: int):
        self._handle = handle
        self.pid = pid
        self.creation_ticks = creation_ticks

    @classmethod
    def open(cls, pid, *, terminate: bool = False) -> "PinnedProcess | None":
        """None when no such process exists or it cannot be opened (another user,
        elevated, protected: then it is not one of ours)."""
        if type(pid) is not int or pid <= 4 or pid > 0xFFFFFFFF:
            return None
        access = _PROCESS_QUERY_LIMITED_INFORMATION | _SYNCHRONIZE | (_PROCESS_TERMINATE if terminate else 0)
        handle = _OpenProcess(access, False, pid)
        if not handle:
            code = ctypes.get_last_error()
            if code in (_ERROR_INVALID_PARAMETER, _ERROR_ACCESS_DENIED):
                return None
            raise _winerror(code)
        times = [wt.FILETIME() for _ in range(4)]
        if not _GetProcessTimes(handle, *(ctypes.byref(item) for item in times)):
            code = ctypes.get_last_error()
            _CloseHandle(handle)
            raise _winerror(code)
        return cls(handle, pid, _filetime_ticks(times[0]))

    @property
    def identity(self) -> tuple[int, int]:
        return self.pid, self.creation_ticks

    def _require(self) -> int:
        if self._handle is None:
            raise ValueError("process handle is closed")
        return self._handle

    def alive(self) -> bool:
        """Still running (an exited process kept alive by handles is not)."""
        return _WaitForSingleObject(self._require(), 0) == _WAIT_TIMEOUT

    def wait(self, timeout: float | None = None) -> bool:
        """True when the process has exited within timeout seconds (None = forever)."""
        return _wait_handle(self._require(), timeout)

    def exit_code(self) -> int | None:
        """Exit code, or None while it runs."""
        if self.alive():
            return None
        code = wt.DWORD(0)
        if not _GetExitCodeProcess(self._require(), ctypes.byref(code)):
            raise _winerror(ctypes.get_last_error())
        return code.value

    def terminate(self, exit_code: int = 1) -> None:
        """TerminateProcess (needs open(pid, terminate=True)); no error if already gone."""
        if not _TerminateProcess(self._require(), exit_code):
            code = ctypes.get_last_error()
            if self.alive():
                raise _winerror(code)

    def image_path(self) -> str | None:
        size = wt.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(size.value)
        if not _QueryFullProcessImageNameW(self._require(), 0, buffer, ctypes.byref(size)):
            return None
        return buffer.value

    def command_line(self) -> list[str] | None:
        """argv of the process (NtQueryInformationProcess(ProcessCommandLineInformation)
        + CommandLineToArgvW); None when it cannot be read."""
        handle = self._require()
        needed = wt.ULONG(0)
        status = _NtQueryInformationProcess(handle, 60, None, 0, ctypes.byref(needed)) & 0xFFFFFFFF
        if status not in (_STATUS_INFO_LENGTH_MISMATCH, 0xC0000023) or not needed.value:
            return None
        buffer = (ctypes.c_ulonglong * ((needed.value + 7) // 8))()
        status = _NtQueryInformationProcess(handle, 60, buffer, needed.value, ctypes.byref(needed)) & 0xFFFFFFFF
        if status >= 0x80000000:
            return None
        text = _UNICODE_STRING.from_buffer(buffer)
        if not text.Buffer or not text.Length:
            return None
        line = ctypes.wstring_at(text.Buffer, text.Length // 2)
        count = ctypes.c_int(0)
        argv = _CommandLineToArgvW(line, ctypes.byref(count))
        if not argv:
            return None
        try:
            return [argv[index] for index in range(count.value)]
        finally:
            _LocalFree(ctypes.cast(argv, ctypes.c_void_p))

    def request_stop(self) -> bool:
        """Signal Local\\RadioS-stop-<pid> of this (pinned, still running) process."""
        return self.alive() and request_stop(self.pid)

    def close(self) -> None:
        handle, self._handle = self._handle, None
        if handle is not None:
            _CloseHandle(handle)

    def __enter__(self) -> "PinnedProcess":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    def __repr__(self) -> str:
        return f"PinnedProcess(pid={self.pid}, creation_ticks={self.creation_ticks})"


def process_identity(pid) -> tuple[int, int] | None:
    """(pid, creation ticks) of a running process, else None (exited, never existed, or
    not inspectable by us).  Compare with a stored identity to detect pid reuse."""
    process = PinnedProcess.open(pid)
    if process is None:
        return None
    with process:
        return process.identity if process.alive() else None


def process_info(pid) -> ProcessInfo | None:
    """Identity, image path and argv of a running process, read through one pinned handle."""
    process = PinnedProcess.open(pid)
    if process is None:
        return None
    with process:
        if not process.alive():
            return None
        return ProcessInfo(process.pid, process.creation_ticks, process.image_path(), process.command_line())


def boot_time() -> float:
    """Approximate boot instant (POSIX seconds) — informational only: it moves with
    clock adjustments, never compare it (process identity is pid + creation time)."""
    return time.time() - _GetTickCount64() / 1000.0


# ----------------------------------------------------------------------------- graceful stop (core spec 5.3)


def stop_event_name(pid=None) -> str:
    """"Local\\RadioS-stop-<decimal pid>" (radios_process_stop_event_name)."""
    value = os.getpid() if pid is None else pid
    if type(value) is not int or value < 0 or value > 0xFFFFFFFF:
        raise ValueError(f"invalid pid {pid!r}")
    return f"{STOP_EVENT_PREFIX}{value}"


def _wait_handle(handle, timeout: float | None) -> bool:
    """WaitForSingleObject in slices so Ctrl+C can still reach the main thread."""
    deadline = None if timeout is None else time.monotonic() + max(0.0, float(timeout))
    while True:
        slice_ms = 250
        if deadline is not None:
            remaining = deadline - time.monotonic()
            slice_ms = int(max(0.0, min(remaining, 0.25)) * 1000)
        result = _WaitForSingleObject(handle, slice_ms)
        if result == _WAIT_OBJECT_0:
            return True
        if result != _WAIT_TIMEOUT:
            raise _winerror(ctypes.get_last_error())
        if deadline is not None and time.monotonic() >= deadline:
            return False


class StopEvent:
    """Owner side of this process's stop channel: the manual-reset event
    Local\\RadioS-stop-<own pid> (default security, created non-signalled; a stale
    pre-existing object is reset like the C side does)."""

    def __init__(self):
        self.name = stop_event_name()
        handle = _CreateEventW(None, True, False, self.name)
        code = ctypes.get_last_error()
        if not handle:
            raise _winerror(code, self.name)
        self._handle = handle
        self.already_existed = code == _ERROR_ALREADY_EXISTS
        if self.already_existed:
            _ResetEvent(handle)
            _log.warning("RadioS: %s önceden vardı; eski durdurma isteği sıfırlandı", self.name)

    @property
    def handle(self) -> int:
        if self._handle is None:
            raise ValueError("stop event is closed")
        return self._handle

    def is_set(self) -> bool:
        return _WaitForSingleObject(self.handle, 0) == _WAIT_OBJECT_0

    def wait(self, timeout: float | None = None) -> bool:
        """True when a stop was requested within timeout seconds (None = forever)."""
        return _wait_handle(self.handle, timeout)

    def reset(self) -> None:
        if not _ResetEvent(self.handle):
            raise _winerror(ctypes.get_last_error(), self.name)

    def close(self) -> None:
        handle, self._handle = self._handle, None
        if handle is not None:
            _CloseHandle(handle)

    def __enter__(self) -> "StopEvent":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()


def create_stop_event() -> StopEvent:
    """Create this process's stop channel (see StopEvent)."""
    return StopEvent()


def request_stop(pid) -> bool:
    """Ask RadioS process `pid` to stop gracefully (SetEvent on Local\\RadioS-stop-<pid>).
    False when that process has no stop channel (gone, or not a RadioS process); other
    failures raise OSError.  Verify the identity (PinnedProcess) first."""
    name = stop_event_name(pid)
    handle = _OpenEventW(_EVENT_MODIFY_STATE, False, name)
    if not handle:
        code = ctypes.get_last_error()
        if code in (_ERROR_FILE_NOT_FOUND, _ERROR_PATH_NOT_FOUND):
            return False
        raise _winerror(code, name)
    try:
        if not _SetEvent(handle):
            raise _winerror(ctypes.get_last_error(), name)
    finally:
        _CloseHandle(handle)
    return True


class StopRequest:
    """SIGTERM/SIGINT replacement for long-lived Python processes.

    install() creates Local\\RadioS-stop-<pid> and a watcher thread that sets
    ``self.event`` (a threading.Event) when another process calls request_stop(pid).
    With console=True it also turns Ctrl+C/Ctrl+Break (main thread only) into the same
    event instead of KeyboardInterrupt and keeps the process alive up to 4.5 s on
    console close/logoff/shutdown until the owner sets ``self.finished``.  Use it as a
    context manager; set ``finished`` at the end of the teardown."""

    def __init__(self, *, console: bool = True):
        self.event = threading.Event()
        self.finished = threading.Event()
        self.name = stop_event_name()
        self._console = console
        self._stop: StopEvent | None = None
        self._wake = None
        self._thread: threading.Thread | None = None
        self._routine = None
        self._previous_signals: dict[int, object] = {}
        self._lock = threading.Lock()

    def install(self) -> "StopRequest":
        with self._lock:
            if self._stop is not None:
                return self
            stop = StopEvent()
            wake = _CreateEventW(None, True, False, None)
            if not wake:
                code = ctypes.get_last_error()
                stop.close()
                raise _winerror(code)
            self._stop, self._wake = stop, wake
            self._thread = threading.Thread(target=self._watch, name="radios-stop-watch", daemon=True)
            self._thread.start()
            if self._console:
                self._install_console()
            atexit.register(self.close)
        return self

    def _watch(self) -> None:
        handles = (_HANDLE * 2)(self._stop.handle, self._wake)
        result = _WaitForMultipleObjects(2, handles, False, _INFINITE)
        if result == _WAIT_OBJECT_0:
            self.event.set()
        elif result != _WAIT_OBJECT_0 + 1:
            _log.error("RadioS: durdurma olayı beklenemedi (Win32 %s)", ctypes.get_last_error())

    def _install_console(self) -> None:
        if threading.current_thread() is threading.main_thread():
            for number in (signal.SIGINT, signal.SIGBREAK):
                self._previous_signals[number] = signal.signal(number, self._on_signal)
        routine = _HANDLER_ROUTINE(self._on_console)
        if _SetConsoleCtrlHandler(routine, True):
            self._routine = routine

    def _on_signal(self, signum, frame) -> None:
        self.event.set()

    def _on_console(self, control: int) -> bool:
        if control in (_CTRL_CLOSE_EVENT, _CTRL_LOGOFF_EVENT, _CTRL_SHUTDOWN_EVENT):
            self.event.set()
            self.finished.wait(4.5)                 # the OS ends the process after ~5 s
            return True
        return False                                # Ctrl+C/Break: CRT -> Python signal handlers

    def wait(self, timeout: float | None = None) -> bool:
        return self.event.wait(timeout)

    def close(self) -> None:
        with self._lock:
            if self._stop is None:
                return
            if self._routine is not None:
                _SetConsoleCtrlHandler(self._routine, False)
                self._routine = None
            if self._previous_signals and threading.current_thread() is threading.main_thread():
                for number, previous in self._previous_signals.items():
                    signal.signal(number, previous)
                self._previous_signals.clear()
            _SetEvent(self._wake)
            if self._thread is not None:
                self._thread.join(5.0)
            self._stop.close()
            _CloseHandle(self._wake)
            self._stop, self._wake, self._thread = None, None, None
            atexit.unregister(self.close)

    def __enter__(self) -> "StopRequest":
        return self.install()

    def __exit__(self, *exc_info) -> None:
        self.finished.set()
        self.close()


# ----------------------------------------------------------------------------- child processes (D4)


def _bundled_interpreters() -> list[Path]:
    root = Path(__file__).resolve().parents[4]          # <root>\database\SDBASE\src\sdbase\_platform.py
    return [root / "runtime" / "python" / "python.exe", root / "python" / "python.exe"]


def _same_file_name(a: str, b: str) -> bool:
    return path_equal(os.path.realpath(a), os.path.realpath(b))


def python_executable() -> str:
    """Interpreter for Python children: sys.executable when it is the bundled runtime
    (<root>\\runtime\\python or the package's <prefix>\\python; pythonw.exe -> python.exe),
    else the bundled runtime, else sys.executable.  Never a PATH lookup."""
    current = sys.executable or ""
    if current and os.path.basename(current).lower() == "pythonw.exe":
        console = os.path.join(os.path.dirname(current), "python.exe")
        if os.path.isfile(console):
            current = console
    candidates = [path for path in _bundled_interpreters() if path.is_file()]
    if current and any(_same_file_name(current, str(path)) for path in candidates):
        return current
    if candidates:
        return str(candidates[0])
    if not current:
        raise FileNotFoundError(errno.ENOENT, "no Python interpreter for child processes")
    return current


def python_child_argv(*args: str) -> list[str]:
    """[interpreter, -E, -s, -B, -X, utf8, *args] (D4)."""
    return [python_executable(), *PYTHON_FLAGS, *(os.fspath(arg) for arg in args)]


_CHILD_ENVIRONMENT = {"PYTHONUTF8": "1", "PYTHONDONTWRITEBYTECODE": "1", "PYTHONIOENCODING": "utf-8"}


def python_child_env(base=None) -> dict[str, str]:
    """Environment for Python children (D4): base (default os.environ) without
    PYTHONHOME/PYTHONPATH, with PYTHONUTF8=1, PYTHONDONTWRITEBYTECODE=1 and
    PYTHONIOENCODING=utf-8 (names compared case-insensitively, like Windows)."""
    environment = dict(os.environ if base is None else base)
    drop = {"PYTHONHOME", "PYTHONPATH", *_CHILD_ENVIRONMENT}
    for key in [key for key in environment if key.upper() in drop]:
        del environment[key]
    environment.update(_CHILD_ENVIRONMENT)
    return environment


def _checked_argv(argv) -> list[str]:
    if isinstance(argv, (str, bytes)) or not argv:
        raise TypeError("argv must be a non-empty sequence of str")
    items = [os.fspath(item) for item in argv]
    if not all(isinstance(item, str) for item in items) or any("\x00" in item for item in items):
        raise TypeError("argv items must be str without NUL characters")
    program = items[0]
    if not is_absolute(program) or '"' in program:
        raise ValueError(f"the program path must be absolute (never searched in PATH): {program}")
    if program.lower().endswith((".bat", ".cmd")):
        raise ValueError(f"batch files are not started directly (cmd.exe re-parses arguments): {program}")
    return items


def popen_no_window(argv, **kwargs) -> subprocess.Popen:
    """subprocess.Popen for an attached child: no console window (CREATE_NO_WINDOW),
    close_fds=True (only the child's std handles are inherited, through
    PROC_THREAD_ATTRIBUTE_HANDLE_LIST), argv[0] an absolute program path (no PATH
    search), no shell, no batch files."""
    items = _checked_argv(argv)
    if kwargs.get("shell"):
        raise ValueError("shell=True is not allowed")
    if kwargs.pop("close_fds", True) is not True:
        raise ValueError("close_fds=False would leak handles (locks!) into the child")
    if "executable" in kwargs:
        raise ValueError("pass the program as argv[0]")
    flags = kwargs.pop("creationflags", 0) | CHILD_CREATIONFLAGS
    if flags & subprocess.DETACHED_PROCESS:
        raise ValueError("DETACHED_PROCESS makes console grandchildren visible; use spawn_detached()")
    return subprocess.Popen(items, executable=items[0], creationflags=flags, close_fds=True, **kwargs)


def spawn_detached(argv, *, cwd=None, env=None, stdout=None, stderr=None, log_fd=None) -> subprocess.Popen:
    """Start a program that outlives this process: windowless console of its own, new
    process group (no Ctrl+C from our console), CREATE_BREAKAWAY_FROM_JOB (retried
    without it when the job forbids breakaway), stdin = NUL, stdout/stderr = NUL unless
    given (an fd/file such as an append-only log; never our own std handles, so a parent
    reading our pipe still sees EOF), no other handle inherited.  log_fd=fd is short for
    stdout=fd, stderr=fd (the service log of the tools)."""
    if log_fd is not None:
        if stdout is not None or stderr is not None:
            raise ValueError("log_fd replaces stdout/stderr; pass one or the other")
        stdout = stderr = log_fd
    items = _checked_argv(argv)
    common = dict(executable=items[0], cwd=cwd, env=env, close_fds=True, stdin=subprocess.DEVNULL,
                  stdout=subprocess.DEVNULL if stdout is None else stdout,
                  stderr=subprocess.DEVNULL if stderr is None else stderr)
    try:
        return subprocess.Popen(items, creationflags=DETACHED_CREATIONFLAGS, **common)
    except PermissionError as exc:
        if exc.winerror != _ERROR_ACCESS_DENIED:
            raise
        # The job we run in forbids breakaway: the child stays in it.
        return subprocess.Popen(items, creationflags=DETACHED_CREATIONFLAGS & ~subprocess.CREATE_BREAKAWAY_FROM_JOB,
                                **common)


# ----------------------------------------------------------------------------- process setup, encodings, time zones

_process_configured = False


def configure_process() -> None:
    """Once per service/CLI process: no "insert a disk"/critical-error dialogs (an empty
    removable drive would block the windowless service), 8 MiB stacks for new threads
    (the code was written for Linux stacks), faulthandler to stderr when usable."""
    global _process_configured
    if _process_configured:
        return
    _process_configured = True
    _SetErrorMode(_GetErrorMode() | 0x0001 | 0x8000)   # SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX
    threading.stack_size(8 * 1024 * 1024)
    stream = sys.stderr
    if stream is not None and not faulthandler.is_enabled():
        try:
            faulthandler.enable(file=stream, all_threads=True)
        except (AttributeError, ValueError, OSError):
            pass                                        # stderr without a descriptor (pythonw)


def configure_stdio_utf8() -> None:
    """stdout/stderr in UTF-8 (backslashreplace) when they are not already: the GUI
    reads helper output as UTF-8, a Turkish-locale default would be cp1254."""
    for name in ("stdout", "stderr"):
        stream = getattr(sys, name, None)
        encoding = (getattr(stream, "encoding", None) or "").lower().replace("_", "-")
        if stream is not None and encoding not in ("utf-8", "utf8"):
            try:
                stream.reconfigure(encoding="utf-8", errors="backslashreplace")
            except (AttributeError, ValueError, OSError):
                pass


@functools.cache
def _available_timezones() -> frozenset[str]:
    import zoneinfo
    return frozenset(zoneinfo.available_timezones())


def canonical_timezone(name) -> str:
    """Exact-case IANA key of the bundled tz database (or "UTC").  Windows file lookups
    are case-insensitive and strip trailing dots/spaces, so "europe/istanbul" or
    "Europe/Istanbul " would otherwise load and be persisted.  Raises
    zoneinfo.ZoneInfoNotFoundError."""
    import zoneinfo
    if not isinstance(name, str) or not (name == "UTC" or name in _available_timezones()):
        raise zoneinfo.ZoneInfoNotFoundError(f"No time zone found with key {name!r}")
    zoneinfo.ZoneInfo(name)
    return name


def check_timezone_database() -> None:
    """Fail fast at service start when the bundled tzdata package is missing."""
    import zoneinfo
    try:
        zoneinfo.ZoneInfo("UTC")
        zoneinfo.ZoneInfo("Europe/Istanbul")
    except (zoneinfo.ZoneInfoNotFoundError, ValueError, OSError) as exc:
        raise RuntimeError("Saat dilimi veritabanı (tzdata) bulunamadı; RadioS Python çalışma ortamı eksik.") from exc


__all__ = [
    # contract constants
    "LOCK_SH", "LOCK_EX", "LOCK_NB", "LOCK_UN", "LOCK_REGION_OFFSET", "LOCK_REGION_LENGTH",
    "WIN_LOCK_OFFSET", "WIN_LOCK_LENGTH", "O_BINARY", "O_NOINHERIT", "O_CLOEXEC", "O_NONBLOCK", "O_NOFOLLOW",
    "O_DIRECTORY", "PRIVATE_FILE_SDDL", "PRIVATE_DIR_SDDL", "PRIVATE_TRUSTED_SIDS", "PRIVATE_SENSITIVE_RIGHTS",
    "RETRY_BUDGET_SECONDS", "STOP_EVENT_PREFIX", "PYTHON_FLAGS", "CHILD_CREATIONFLAGS", "DETACHED_CREATIONFLAGS",
    # path strings
    "is_absolute", "is_root", "path_equal", "same_path", "is_inside", "normalize", "is_canonical",
    "component_valid", "valid_new_name", "sanitize_component", "safe_relative_parts", "stored_path_flavour",
    "to_portable", "from_portable", "long_paths_enabled",
    # links
    "stat_is_link", "is_link", "is_link_like", "entry_is_link", "entry_is_link_like", "has_link_component",
    # opening
    "open_flags", "open_fd", "os_open", "open_regular", "open_read", "read_bytes_shared", "open_beneath",
    "open_nofollow_path", "mkstemp_private",
    # durability, rename, delete
    "fsync", "fsync_file", "fsync_dir", "fsync_tree", "sync_tree", "replace", "rename_noreplace", "link_noreplace",
    "publish_dir_noreplace", "unlink", "unlink_quietly", "rmtree", "rmtree_quietly",
    # locks
    "flock", "lock", "unlock", "unlock_and_close", "locked_file",
    # positional I/O, space, access
    "pread", "disk_usage", "disk_free", "dir_writable", "acl_supported",
    # privacy
    "current_user_sid", "owned_by_current_user", "is_private", "private_dir_ok", "foreign_access",
    "security_sddl", "make_private", "mkdir_private", "mkdtemp_private", "temporary_directory",
    # processes and stop requests
    "ProcessInfo", "PinnedProcess", "process_identity", "process_info", "boot_time", "stop_event_name",
    "StopEvent", "create_stop_event", "request_stop", "StopRequest",
    # children
    "python_executable", "python_child_argv", "python_child_env", "popen_no_window", "spawn_detached",
    # process setup
    "configure_process", "configure_stdio_utf8", "canonical_timezone", "check_timezone_database",
]
