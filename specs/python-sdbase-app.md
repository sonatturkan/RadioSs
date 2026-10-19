# Area spec: python-sdbase-app (RadioS Windows port)

Analysed copy: `C:\Users\sonatturkan\Desktop\RadioS` (read-only phase). All paths below are relative to
`database/SDBASE/src/sdbase/` unless they start with `tools/`, `tests/` or `src/`.

Binding inputs: `specs/DECISIONS.md` (D1 Windows only, D2 fix defects, D2b remove leftovers, D3 platform
contract, D4 python invocation, D6 cross-language rules) and `specs/PLATFORM_FACTS.md`. Shared C contract:
`specs/core-posix-files.md` §4–§5 (lock region, private SDDL, stop event, digest, path rules). Where this
spec and DECISIONS.md disagree, DECISIONS.md wins.

**Scope (owned by this spec):** `api_server.py`, `http_server.py`, `cli.py`, `workflow_api.py`,
`read_api.py`, `openapi.py`, `runtime_version.py`, every `desktop_*.py`, `media.py` (managed-object
staging used by the desktop flows), `audio_playout.py`, `endpoint.py`, `endpoint_journal.py`,
`endpoint_media.py`, `endpoint_protocol.py`, `transport.py`, `http_pool.py`, `http_limits.py`,
`importer.py`, `settings.py`, `rekordbox.py`, `rekordbox_device.py`, `metadata.py`, `mairlist31_*.py`,
`radioboss_m3u8*.py`, `libretime_adapter.py`, `podcast.py`, `recording_jobs.py`, `listener_*.py`,
`now_playing.py`, `emergency_alerts.py`, `monitoring.py`, `diagnostics.py`, `capacity_monitor.py`, and the
time-zone validators in `schema.py`, `planning.py`, `station_clone.py`. All remaining pure-logic modules
(ads, clocks, schedules, plans, playout, playlists, smart_blocks, carts, catalog, …) were scanned: they only
need the tz-database fix (§12).

**Not owned (engine/backup/replication agent):** `storage.py`, `schema.py` core, `identity.py`,
`integrity.py`, `store_health.py`, `backup.py`, `wal_archive.py`, `pitr.py`, `replication*.py`,
`failover*.py`, `online_maintenance.py`, `disk_index.py`, `migration.py`, `retention.py`,
`portable_*.py`, `ops_archive.py`, `ops_scheduler.py`, `query.py`, `read_snapshot.py`. Findings there that
block this area are listed in §17.

Legend: **[C]** crashes/raises on Windows, **[D]** silent data corruption / wrong result, **[S]** security
or semantic weakening, **[P]** portability of persisted data, **[OK]** portable as is.

---------------------------------------------------------------------------------------------------

## 0. TL;DR — the items that break the product

1. Nothing imports today: `__init__.py:3` imports `storage` (`fcntl`, engine area) and `cli.py:15` imports
   `endpoint_journal` which does `import fcntl` at module level (`endpoint_journal.py:5`). `workflow_api.py:13-28`
   imports every workflow module eagerly, so one Linux-only import (e.g. `desktop_admin.py:19 → integrity.py:12
   fcntl`) takes down **all** HTTP routes. Fix order: engine shim first, then this area.
2. **Raw `os.open()` returns a TEXT-mode CRT fd on Windows** (`_fmode` default `_O_TEXT`; Python adds only
   `O_NOINHERIT`). Bytes written through `os.write`/`os.fdopen(fd,"wb")` get `\n→\r\n`; reads stop at `0x1A`
   and fold `\r\n`. Affected in this area: `desktop_transfers.py:36` (reviewed CSV/M3U8 bytes → sha mismatch),
   `endpoint_journal.py:45` (journal), `audio_playout.py:73` (WAV hashing/decoding), `rekordbox.py:536`
   (XML), `desktop_api.py:1477` (credentials), `desktop_media.py:542` (staged import). Every data fd must be
   opened through the shim (`O_BINARY`).
3. `os.getuid()` (`desktop_api.py:1472`), `os.O_NOFOLLOW`/`os.O_DIRECTORY`/`os.O_NONBLOCK` attribute access
   (`desktop_api.py:1477,1481`, `desktop_media.py:540,542`), `dir_fd=` (`desktop_media.py:542`),
   `/proc/self/fd/N` (`desktop_media.py:624`) and `os.statvfs` (`capacity_monitor.py:182,188`) do not exist →
   AttributeError/NotImplementedError → HTTP 500.
4. Directory fsync via `os.open(dir)` fails with `PermissionError` on Windows: `endpoint_journal.py:24`
   (EndpointJournal cannot be constructed), `endpoint_media.py:87`, `media.py:67` (every managed-object
   publication fails), `audio_playout.py:361` (reports failure **after** publishing), `desktop_api.py:1481`.
   Windows: no directory flush (D3/C spec 4.13); durability via file flush + journaled rename.
5. `ThreadingHTTPServer` sets `SO_REUSEADDR` (`HTTPServer.allow_reuse_address = True`). On Windows that lets a
   second process bind 127.0.0.1:8000 at the same time (port sharing/hijack). Disable it and set
   `SO_EXCLUSIVEADDRUSE` in `BoundedHTTPServer` (it is the base of the API, failover and replication servers).
6. Graceful stop is SIGTERM/SIGINT based (`tools/radios_database.py:613-615`; `http_server.serve` has none).
   SIGTERM is never delivered on Windows (`os.kill`/`Popen.terminate` = `TerminateProcess`). Replace with the
   shared named event `Local\RadioS-stop-<pid>` (D3/D6, C spec 5.3) + console control events; identity =
   pid + process creation time.
7. Time zones: `ZoneInfo(...)` is used in 21 modules and validates every station (`schema.py:873-877`). The
   embeddable CPython has no tz database → even `ZoneInfo("UTC")` fails → `init`/`bootstrap` fails. The
   runtime ships `tzdata` (PLATFORM_FACTS); additionally validate keys **case-exactly** (Windows file lookup is
   case-insensitive: `"Europe/istanbul"`, `"UTC "` would otherwise be accepted and persisted).
8. Persisted-path validators use host `os.path` and `startswith("/")`: `desktop_media.validate_library`
   (runs at every commit `schema.py:826-829` and in the integrity scrub `integrity.py:249-252`) rejects every
   Windows import with provenance and would also reject records migrated from the Linux database.
   Same class: `desktop_api.py:432` (recorder folder), `desktop_transfer_package.py:222-272,644-651`,
   `rekordbox_device.py:126-137`, `settings.py:67`, `mairlist31_export.py:49-51`, `libretime_adapter.py:60-66`.
9. Symlink checks miss **junctions** (creatable without privilege): every `is_symlink()`/`S_ISLNK` check in
   this area must become "name-surrogate reparse point" (`stat_is_link`).
10. 0600/0700/`st_uid` privacy → Windows ACLs per C spec 5.2 (explicit owner SID, protected DACL, owner check,
    no-others check, FAT/exFAT fail closed) for credential keys, transfer temp sources, packages, backups.
11. `desktop_integrity._scan` (`desktop_integrity.py:153-159`) relies on `PYTHONPATH`, which `-E` (D4) ignores →
    the scrub child cannot import `sdbase`; it also must not open a console window (`CREATE_NO_WINDOW`) and must
    decode UTF-8 explicitly.
12. `runtime_version.source_digest` (`runtime_version.py:13`) sorts `Path` objects (case-insensitive on Windows)
    and globs `*.py` case-insensitively → must follow C spec 5.7 (exact lowercase `.py`, byte order of name), or
    the GUI/tools "stale service" detection disagrees with the service.
13. `http_server._run` treats only `BrokenPipeError`/`ConnectionResetError` as client disconnects
    (`http_server.py:258`); Windows mostly raises `ConnectionAbortedError` (WSAECONNABORTED 10053) → tries to send
    a 500 on a dead socket and logs tracebacks. Catch `ConnectionError`.
14. Replace/unlink over files held open by others (C player, FFmpeg, Defender, our own streaming GET) fails with
    `ERROR_SHARING_VIOLATION`/`ERROR_ACCESS_DENIED` → shim `replace()`/`unlink()` with POSIX-semantics rename and
    the 2000 ms retry budget (C spec 4.4), and share-delete readers.
15. Transfer-package import writes user/album names from packages to disk: Windows-invalid names (`:` = NTFS
    alternate data stream, `CON`, `NUL.mp3`, trailing dot/space, `<>|?*"`) must be sanitised per component.

---------------------------------------------------------------------------------------------------

## 1. Context and Windows facts that drive this area

### 1.1 Runtime (binding, D4 + PLATFORM_FACTS)
* Interpreter: official CPython **3.14.8** embeddable, `<root>\runtime\python\python.exe`, `python314._pth`
  **removed**, `tzdata 2026.5` in `<prefix>\Lib\tzdata` (598 zones verified). The app starts Python as
  `python.exe -E -s -B -X utf8 <script|-m module> …` with env `PYTHONUTF8=1 PYTHONDONTWRITEBYTECODE=1
  PYTHONIOENCODING=utf-8` and without `PYTHONHOME/PYTHONPATH`. **`-E` ⇒ PYTHONPATH is ignored**; `sys.path[0]`
  is the script directory (or the CWD for `-m`/`-c`).
* The service outlives the GUI (never in a kill-on-close job); it is spawned DETACHED (no console:
  `DETACHED_PROCESS|CREATE_NEW_PROCESS_GROUP|CREATE_NO_WINDOW|CREATE_BREAKAWAY_FROM_JOB`, D3). A console-less
  process that spawns a console program (python.exe) gets a **visible console window** unless the child is
  created with `CREATE_NO_WINDOW`.
* Use `<root>\runtime\python\python.exe` for every Python test (D7). PATH python resolves to 3.15 without tzdata.
* Python 3.13/3.14 features relied on below (all present in 3.14.8): `os.path.isreserved()`,
  `os.path.isjunction()`, `Path.from_uri()`, `os.fchmod` on Windows, `ntpath.isabs("\\x") is False`,
  `os.mkdir(path, 0o700)` applies `D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)`,
  `time.monotonic()` = QueryPerformanceCounter (1 µs), `time.time()` = GetSystemTimePreciseAsFileTime.

### 1.2 Facts (numbered, referenced below as Wn)
| # | Fact | Consequence for this area |
|---|---|---|
| W1 | `os.open()` fds are CRT **text mode** unless `O_BINARY`; `os.fdopen(fd)`/FileIO never changes the mode; `tempfile.mkstemp`/`NamedTemporaryFile` and `open(...,"rb"/"wb")` are binary. | Every `os.open` that is read/written must go through `_platform.open_fd` (adds `O_BINARY`). |
| W2 | `os.O_NOFOLLOW`, `O_DIRECTORY`, `O_NONBLOCK`, `O_CLOEXEC` do not exist; `os.supports_dir_fd` is empty; `os.getuid/geteuid`, `os.statvfs`, `os.sync`, `os.pidfd_open`, `signal.pidfd_send_signal`, `fcntl`, `/proc` do not exist. | Shim functions (§2). |
| W3 | `os.open(directory)` fails (`PermissionError`, EACCES); `FlushFileBuffers` needs write access (fsync of a read-only fd fails). | No directory fsync; shim `fsync()` is a no-op for read-only fds. |
| W4 | `Path.is_symlink()`/`os.path.islink()`/`S_ISLNK` are False for **junctions** (`IO_REPARSE_TAG_MOUNT_POINT`); junctions need no privilege; symlinks need SeCreateSymbolicLinkPrivilege/Developer Mode. `os.lstat` exposes `st_file_attributes` and `st_reparse_tag`. OneDrive placeholders are non-name-surrogate reparse points (not links). | Link test = `FILE_ATTRIBUTE_REPARSE_POINT` **and** `IsReparseTagNameSurrogate(tag)` (bit `0x20000000`). |
| W5 | Opening with Python `open()`/CRT shares READ\|WRITE but **not DELETE**. Rename-over or delete of a file another process holds without `FILE_SHARE_DELETE` fails with `ERROR_SHARING_VIOLATION`(32)/`ERROR_ACCESS_DENIED`(5) — Defender/backup agents/OneDrive do this transiently. | Shim `replace/unlink` with POSIX semantics + retry 2000 ms; our readers open share-delete. |
| W6 | `os.rename` never replaces (FileExistsError); `os.replace` = `MoveFileExW(REPLACE_EXISTING)`; `os.link` = `CreateHardLinkW`, unsupported on FAT32/exFAT/many SMB shares. | `rename_noreplace` instead of `os.link` publication. |
| W7 | Paths: case-insensitive, `\` and `/` separators, drive letters/UNC, reserved names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`, `CONIN$`, `CONOUT$`, also with extensions), invalid chars `<>:"\|?*` + controls, no trailing dot/space, `:` = alternate data stream, MAX_PATH 260 unless LongPathsEnabled (python.exe is longPathAware). `realpath` expands 8.3 names (`%TEMP%` is `C:\Users\SONATT~1\…` here) and turns mapped drives into UNC. | Path helpers mirror C `radios_path_*` (C spec 4.14, 5.5). |
| W8 | `os.chmod/os.mkdir(mode)/umask` only toggle the read-only attribute (except 3.13+ `mkdir(0o700)`); `st_mode` reports `0o666`/`0o444`/`0o777`; `st_uid` is 0. | Privacy via ACL (C spec 5.2). |
| W9 | `SO_REUSEADDR` on Windows allows binding a port already bound by another socket of the same user; `SO_EXCLUSIVEADDRUSE` forbids it; `SO_REUSEPORT` does not exist; `IPV6_V6ONLY` defaults to 1. | §3.1. |
| W10 | Client-side disconnects raise `ConnectionAbortedError` (10053) / `ConnectionResetError` (10054), rarely `BrokenPipeError`. | §3.2. |
| W11 | SIGTERM is never delivered; `os.kill(pid, sig)` = `TerminateProcess` (except CTRL_C/BREAK events); `Popen.terminate()` = hard kill; `signal.signal` works for SIGINT/SIGBREAK only in console processes and only in the main thread. | Named stop event (§4). |
| W12 | `os.access(dir, W_OK)` ignores ACLs (always True for directories). | Write-probe (C spec 4.8). |
| W13 | `HTTPServer.server_bind` calls `socket.getfqdn(host)` (reverse lookup); on Windows LLMNR/NetBIOS can stall. | Skip it. |
| W14 | Accessing an empty removable drive shows a modal "no disk" dialog unless `SetErrorMode(SEM_FAILCRITICALERRORS)`. The service has no visible console/window → the thread blocks invisibly. | `configure_process()` at startup (usb_root, storage folders, transfer to USB). |
| W15 | Connecting to a closed loopback port takes ~1–2 s on Windows (SYN retries after RST); `localhost` resolves `::1` first. | Clients use `127.0.0.1` (config already enforces it, `tools/radios_database_setup.py:159-163`). |
| W16 | `zoneinfo` finds keys through `importlib.resources` files of `tzdata` → leaf lookup is case-insensitive and Win32 strips trailing dots/spaces. | Exact-case key validation (§12). |
| W17 | Threads get the exe's default stack (python.exe ~2 MB) vs 8 MiB on Linux. | `threading.stack_size(8 MiB)` before starting server threads (§2, `configure_process`). |

---------------------------------------------------------------------------------------------------

## 2. The shared shim `sdbase/_platform.py` (contract this area calls)

### 2.1 Rules
* New file `database/SDBASE/src/sdbase/_platform.py`, Windows-only implementation (D1; no POSIX branches),
  stdlib + `ctypes` (`kernel32`, `advapi32`, `ntdll` via `ctypes.WinDLL(name, use_last_error=True)`), `msvcrt`,
  `_winapi`. Imported **relatively** everywhere (`from . import _platform`) because `tools/radios_database_setup.py:211-227`
  imports the package under the alias `_radios_sdbase_<hash>`; `desktop_integrity`'s child imports it as `sdbase`.
* Shared with the engine area (it will need lock/replace/fsync/private too). If the engine spec defines the
  module, merge; the names/semantics below are what this area calls. `tools/_platform.py` mirrors the same
  functions (C spec §5 title) — keep the two files identical in behaviour (ideally tools imports
  `sdbase._platform` through `backend_import`, else a copy kept in sync by a test comparing their source).
* Errors are `OSError` subclasses with `errno` mapped per C spec 4.2 (`winerror` preserved); callers keep
  catching `OSError`/`BlockingIOError`/`FileExistsError` exactly as today.
* Retry budget for sharing violations: sleeps 5, 10, 20, 40, 80, 160, 250, 250… ms, **2000 ms total**
  (`RETRY_BUDGET_SECONDS = 2.0`, C spec 4.4 step 4).

### 2.2 API (signature → semantics → implementation)
| Function | Semantics | Windows implementation |
|---|---|---|
| `open_fd(path, flags, *, nofollow=False, private=False, share_delete=True, deny_write=False) -> int` | Like `os.open` but always binary and non-inheritable. `flags` are `os.O_*` (`O_RDONLY/WRONLY/RDWR/CREAT/EXCL/TRUNC/APPEND`). `nofollow`: final component must not be a link (ELOOP). `private`: new file gets the 5.2 file SDDL. | `CreateFileW` (ctypes) with the C spec 4.3 mapping: access from RDONLY/WRONLY/RDWR (+`WRITE_DAC` when CREAT); **`O_APPEND` keeps `FILE_WRITE_DATA`** (needs `SetEndOfFile` for `os.ftruncate`, see §7.1) and sets CRT `_O_APPEND`; dispositions CREATE_NEW/CREATE_ALWAYS/OPEN_ALWAYS/TRUNCATE_EXISTING/OPEN_EXISTING; share `READ\|WRITE` (+`DELETE` if `share_delete`; `deny_write` → `READ` only); `FILE_FLAG_OPEN_REPARSE_POINT` when `nofollow`, then post-check `GetFileInformationByHandleEx(FileAttributeTagInfo)` (name-surrogate → close, `ELOOP`; non-surrogate → reopen without the flag and compare `FileIdInfo`, C spec 4.3); `SECURITY_ATTRIBUTES{bInheritHandle=FALSE, lpSecurityDescriptor = private SD}`; then `msvcrt.open_osfhandle(h, crtflags \| os.O_BINARY \| os.O_NOINHERIT \| (os.O_APPEND if append))`. Paths ≥ 240 UTF-16 units → `\\?\` form after `GetFullPathNameW`. |
| `open_read(path, *, nofollow=False) -> BinaryIO` | Share-delete binary reader (so others can replace/delete while we hash/stream). | `os.fdopen(open_fd(path, os.O_RDONLY, nofollow=nofollow), "rb")`. |
| `open_beneath(directory, name, *, deny_write=True) -> tuple[int, os.stat_result]` | Replacement for `openat(dirfd, name, O_RDONLY\|O_NOFOLLOW\|O_NONBLOCK)` after `open(dir, O_DIRECTORY\|O_NOFOLLOW)`: `name` is one valid component; the directory itself and the file must not be links; the file must be a regular disk file; returns a binary read fd and its `os.fstat`. | 1) `hdir = CreateFileW(directory, FILE_LIST_DIRECTORY\|FILE_TRAVERSE\|FILE_READ_ATTRIBUTES\|SYNCHRONIZE, FILE_SHARE_READ\|FILE_SHARE_WRITE /*no DELETE: pins the dir*/, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS\|FILE_FLAG_OPEN_REPARSE_POINT)`; must be DIRECTORY and not a name-surrogate reparse point (`ELOOP`/`ENOTDIR`). 2) `NtCreateFile(RootDirectory=hdir, ObjectName=name, GENERIC_READ\|SYNCHRONIZE, share READ (\|WRITE if not deny_write), FILE_OPEN, FILE_NON_DIRECTORY_FILE\|FILE_SYNCHRONOUS_IO_NONALERT\|FILE_OPEN_REPARSE_POINT\|FILE_SEQUENTIAL_ONLY, OBJ_CASE_INSENSITIVE)` (true openat, C spec 4.3); reparse post-check as above; `GetFileType == FILE_TYPE_DISK`; `ERROR_SHARING_VIOLATION` → `OSError(EBUSY)`. Acceptable simpler fallback: `CreateFileW(directory\name, …, FILE_FLAG_OPEN_REPARSE_POINT)` while holding `hdir`, then require `GetFinalPathNameByHandleW` of the file's parent == that of `hdir` (ordinal ignore-case). |
| `fsync(fd) -> None` | Data durability of a written file. | `os.fsync` (`_commit` → FlushFileBuffers); read-only handle → return (C spec 4.13). |
| `replace(src, dst) -> None` | Atomic replace that works over destinations held open with share-delete; retries transient sharing violations. | Open `src` with `DELETE\|SYNCHRONIZE\|FILE_READ_ATTRIBUTES`, `FILE_FLAG_OPEN_REPARSE_POINT`, share all; `SetFileInformationByHandle(FileRenameInfoEx, Flags=REPLACE_IF_EXISTS\|POSIX_SEMANTICS\|IGNORE_READONLY_ATTRIBUTE, RootDirectory=NULL, FileName="\\??\\"+GetFullPathNameW(dst))`; on `ERROR_INVALID_PARAMETER/NOT_SUPPORTED/INVALID_FUNCTION` (FAT/exFAT/SMB) fall back to `FileRenameInfo{ReplaceIfExists=TRUE}`; retry 5/32/33 within the 2000 ms budget (5 only if `dst` is an existing regular file); `dst` directory → `IsADirectoryError`. Same as C `radios_fs_rename` (C spec 4.4). |
| `rename_noreplace(src, dst) -> None` | Publish a complete temp file under a new name, never overwriting (replaces `os.link` publication). | `MoveFileExW(src, dst, MOVEFILE_WRITE_THROUGH)` (no REPLACE_EXISTING); `ERROR_ALREADY_EXISTS`/`ERROR_FILE_EXISTS` → `FileExistsError`; sharing retries as above. Works on FAT/exFAT/SMB. |
| `unlink(path, *, missing_ok=True) -> None` | Delete a file (never follows links) with POSIX semantics so the name is free immediately. | Open `DELETE\|SYNCHRONIZE\|FILE_READ_ATTRIBUTES`, `FILE_FLAG_OPEN_REPARSE_POINT`, share all; `FileDispositionInfoEx{DELETE\|POSIX_SEMANTICS\|IGNORE_READONLY_ATTRIBUTE}` with fallback `FileDispositionInfo{TRUE}`; retry budget. |
| `unlink_quietly(path) -> bool` / `rmtree_quietly(path) -> bool` | Best-effort cleanup for `finally:` blocks: never raises; returns False if something remains (caller may log). | `unlink` / `shutil.rmtree(path, onexc=…)` with the retry budget per entry; never follows junctions (3.12+ rmtree treats them as links). |
| `stat_is_link(st) -> bool`, `is_link(path) -> bool`, `has_link_component(path) -> bool` | Link = symlink **or junction or any name-surrogate reparse point**. `is_link` uses `os.lstat` (False if missing). `has_link_component` walks every component from the drive/UNC root to the leaf. | `bool(st.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT) and bool(st.st_reparse_tag & 0x20000000)`. |
| `owned_by_current_user(path_or_fd) -> bool` | C spec 5.2 owner check. | `GetSecurityInfo/GetNamedSecurityInfoW(OWNER_SECURITY_INFORMATION)`; owner == `TokenUser` SID, or owner == `S-1-5-32-544` and `CheckTokenMembership(NULL, Administrators)`. Volume without `FILE_PERSISTENT_ACLS` → False. |
| `is_private(path_or_fd) -> bool` | C spec 5.2 no-others check (DACL present; allow ACEs only for user/owner/SYSTEM/Administrators/OWNER RIGHTS unless the mask is harmless). | `GetSecurityInfo(DACL)` + ACE walk. |
| `make_private(path_or_fd) -> None` | Apply the 5.2 SDDL (file or dir variant by type), explicit owner. | `SetSecurityInfo/SetNamedSecurityInfoW(OWNER\|DACL\|PROTECTED_DACL)`; FAT/exFAT → `OSError(ENOTSUP)`. |
| `mkdir_private(path, *, parents=False, exist_ok=False) -> None` | Replaces `mkdir(mode=0o700)` + `chmod(0o700)`: new directories get the dir SDDL `O:<SID>D:P(A;OICI;FA;;;<SID>)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)`; an existing one is verified (`not is_link`, is dir, `owned_by_current_user`) and `make_private`'d. | `CreateDirectoryW(path, &sa)`; parents created with inherited security. |
| `lock(fd, *, shared=False, blocking=True) -> None`, `unlock(fd) -> None` | flock emulation, C spec 5.1: region offset **0x4000000000000000**, length 1; every (re)lock first `UnlockFileEx` ignoring `ERROR_NOT_LOCKED`; NB + held → `BlockingIOError(EWOULDBLOCK)`. | `LockFileEx/UnlockFileEx` on `msvcrt.get_osfhandle(fd)` with an `OVERLAPPED{Offset=0, OffsetHigh=0x40000000}`. |
| `disk_free(path) -> int` | Bytes available to the caller (= POSIX `f_bavail*f_frsize`). | `shutil.disk_usage(path).free` (GetDiskFreeSpaceExW caller-available). |
| `dir_writable(path) -> bool` | Real write permission (ACLs, read-only media, Controlled Folder Access). | C spec 4.8 probe: `CreateFileW(dir\.radios-access-<pid>-<rand>, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN\|FILE_ATTRIBUTE_TEMPORARY, FILE_FLAG_DELETE_ON_CLOSE)`. |
| `long_paths_enabled() -> bool` | Whether paths > 260 work. | `ntdll.RtlAreLongPathsEnabled()`. |
| `is_absolute(p) -> bool` | `X:\…` or `\\server\share\…` (mirror `radios_path_is_absolute`; rejects `\foo`, `C:foo`, `\\?\…`, `/x`). | string rules. |
| `is_root(p) -> bool` | `X:\` or `\\server\share[\]`. | string rules. |
| `normalize(p) -> str` / `is_canonical(p) -> bool` | `os.path.normpath` + upper-case drive letter; canonical ⇔ `p == normalize(p)` and every component `component_valid` (C spec 5.5 rule 3). | `ntpath`. |
| `path_equal(a, b) -> bool`, `is_inside(root, p) -> bool` | Separator-agnostic, case-insensitive (NTFS rules); root may end with a separator. | `kernel32.CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL` on separator-unified strings (C spec 4.14; never `str.lower()`/`casefold()` — Turkish `İ`, `ß`). |
| `component_valid(name) -> bool` | One file/dir name valid on NTFS: non-empty, not `.`/`..`, no `\ /`, `not os.path.isreserved(name)` (covers `<>:"\|?*`, controls, trailing dot/space, device names incl. extensions/superscript COM/LPT), ≤ 255 UTF-16 units. Same rules as C `radios_path_component_valid`. | pure. |
| `sanitize_component(text, fallback, *, limit=150) -> str` | Map an arbitrary title/album/file name to a valid component: controls/`<>:"/\|?*` → `_`; strip trailing dots/spaces repeatedly (also after truncation); reserved base name → prefix `_`; empty → `fallback`. | pure. |
| `stored_path_flavour(value) -> "windows"\|"legacy-posix"\|None` | String-only classification of **persisted** absolute paths (Linux-created databases/packages contain `/home/...`). Never used for new writes. | `windows` = `is_absolute(value)` (either separator accepted for classification); `legacy-posix` = starts with exactly one `/`. |
| `python_child_argv(*args) -> list[str]` | `[sys.executable, "-E", "-s", "-B", "-X", "utf8", *args]` (D4). | — |
| `python_child_env(base=None) -> dict` | Copy of `os.environ` minus `PYTHONHOME`, `PYTHONPATH`; plus `PYTHONUTF8=1`, `PYTHONDONTWRITEBYTECODE=1`, `PYTHONIOENCODING=utf-8`. | — |
| `CHILD_CREATIONFLAGS` | `subprocess.CREATE_NO_WINDOW` for every console child spawned by the service. | — |
| `process_identity(pid) -> tuple[int, int] \| None` | `(pid, creation_ticks)` of a **running** process (100 ns since 1601, `GetProcessTimes`), else None. | `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION\|SYNCHRONIZE)`, `WaitForSingleObject(h,0)==WAIT_TIMEOUT` (do not use `STILL_ACTIVE`), `GetProcessTimes`. |
| `stop_event_name(pid=None) -> str` | `"Local\\RadioS-stop-<decimal pid>"` (default: current pid). | — |
| `class StopRequest` (context manager) | `.event` (threading.Event set on any stop request), `.finished` (owner sets it when shutdown is complete), `install()`, `close()`. | §4.2. |
| `request_stop(pid) -> bool` | Signal another RadioS process to stop gracefully; False if it has no stop event. | `OpenEventW(EVENT_MODIFY_STATE, FALSE, name)`; `ERROR_FILE_NOT_FOUND` → False; `SetEvent`. |
| `canonical_timezone(name) -> str` | Exact-case IANA key from the bundled database (or `"UTC"`); raises `zoneinfo.ZoneInfoNotFoundError`. | `name in _available()` where `_available = functools.cache(zoneinfo.available_timezones)` + `ZoneInfo(name)` load. |
| `check_timezone_database() -> None` | Fail fast at service start: `ZoneInfo("UTC")`, `ZoneInfo("Europe/Istanbul")` must load. Turkish error: «Saat dilimi veritabanı (tzdata) bulunamadı; RadioS Python çalışma ortamı eksik.» | — |
| `configure_process() -> None` | Once per service/CLI process: `SetErrorMode(SEM_FAILCRITICALERRORS\|SEM_NOOPENFILEERRORBOX)` (W14), `threading.stack_size(8 * 1024 * 1024)` (W17), `faulthandler.enable()` (to stderr = service log). | — |

### 2.3 Shim tests (new `tests/test_sdbase_platform.py`, run with runtime\python)
1. Binary fidelity: `open_fd(p, O_WRONLY|O_CREAT|O_EXCL)` + `os.write(b"a\nb\r\n\x1ac")` then `os.read` via
   `open_fd(p, O_RDONLY)` returns the identical bytes and `os.stat(p).st_size == 8`.
2. `replace()` over a destination held by `open(dst,"rb")` (no share-delete) from another thread that closes
   after 100 ms → succeeds; held for 3 s → `PermissionError` after ~2 s; over a destination held by
   `open_read()` → succeeds immediately (POSIX semantics) and the old reader still reads old bytes.
3. `rename_noreplace` onto an existing name → `FileExistsError`, source untouched.
4. `unlink` of a file held by `open_read()` → name gone immediately; recreating the same name succeeds.
5. Links: `_winapi.CreateJunction(target, link)` → `is_link`, `has_link_component(link\child)`;
   `os.symlink` only when permitted (else skip); OneDrive-like non-surrogate reparse: code review.
6. `open_beneath`: junction as the directory → ELOOP; file symlink → ELOOP; directory named `x.wav` → not
   regular; `name` with `\`, `:`, `..`, `CON` → `EINVAL`; held open for write by another handle with
   `deny_write=True` → `EBUSY`.
7. Privacy: `mkdir_private`/`open_fd(private=True)` → `is_private` and `owned_by_current_user`; after adding a
   `BUILTIN\Users` read ACE (test helper mirroring `radios_platform_test_make_public`) → `is_private` False.
8. Locks: SH+SH from two fds ok; EX|NB on a third fd → `BlockingIOError`; closing releases; interop with the C
   helper (`tests/test_platform_lock_interop.py`, C spec 7.3 #10).
9. StopRequest: `install()` then `request_stop(os.getpid())` → `event` set within 0.5 s; a second process
   (`python_child_argv("-c", …)`) that installs a StopRequest exits 0 within 2 s after `request_stop(pid)`;
   `request_stop` of a pid without event → False.
10. Path vectors: `is_absolute/is_root/normalize/is_canonical/path_equal/is_inside/component_valid/
    sanitize_component` (`"D:\\radyo\\kayıt"` valid; `"", "göreli", "C:\\", "C:\\a\\..\\b", "C:\\a\\\\b",
    "C:\\a\\", "\\a", "C:a", "C:/a", "c:\\a"` not canonical; `CON`, `nul.mp3`, `a:b`, `x. `, `COM¹`, `İ` vs `i̇`).
11. `canonical_timezone("Europe/Istanbul")` ok; `"europe/istanbul"`, `"Europe/Istanbul "`, `"Europe/Istanbul."`,
    `"Europe\\Istanbul"`, `"../x"` rejected; `"UTC"` ok.

---------------------------------------------------------------------------------------------------

## 3. HTTP service layer

### 3.1 `api_server.py` — `BoundedHTTPServer` (base of the API, failover and replication listeners)
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 3, 8 | `ThreadingHTTPServer` subclass; inherits `HTTPServer.allow_reuse_address = True` (and 3.14's `allow_reuse_port`) | [S] W9 | Class attributes `allow_reuse_address = False`, `allow_reuse_port = False`. Override `server_bind()`: `self.socket.setsockopt(socket.SOL_SOCKET, getattr(socket, "SO_EXCLUSIVEADDRUSE", -5), 1)`, then `socketserver.TCPServer.server_bind(self)` (skips `getfqdn`, W13), then `host, port = self.server_address[:2]; self.server_name = host; self.server_port = port` (tests use `server_port`). |
| (new) | `request_queue_size = 5` (listen backlog) | improvement | `request_queue_size = 64`: the GUI opens several connections at once at start-up; 32 handler slots behind a backlog of 5 is too small on Windows. |
| (new) | default `handle_error` prints tracebacks for client disconnects | [S] W10 | Override `handle_error(self, request, client_address)`: if `sys.exc_info()[1]` is `ConnectionError`/`TimeoutError` → return silently; else `super().handle_error(...)`. |
| 12-23 | ctor validation, slots | OK | keep unchanged (contract: `max_connections` 1..1024, `request_timeout` (0, 3600]). |
| 25-45 | capacity rejection writes raw `HTTP/1.1 503 … Retry-After: 1` then `shutdown_request` | OK | keep. |
| 52-56, 58-63 | slot release, `stats()` keys | OK | keep (`/v1/operations/metrics` `http_server` section and tests read them). |
| bind errors | `OSError` from `server_bind` | [C] UX | Callers (tools serve, `cli serve`) map `winerror 10048` (WSAEADDRINUSE) → «127.0.0.1:<port> kullanımda.» and `10013` (WSAEACCES: Hyper-V/WSL excluded port range or another exclusive owner) → «127.0.0.1:<port> Windows tarafından ayrılmış veya başka bir uygulamaya özel; `netsh int ipv4 show excludedportrange protocol=tcp` ile denetleyin ve config\radios.ini [database] service_url portunu değiştirin.» |
| restart | — | risk | With `SO_EXCLUSIVEADDRUSE`, a re-bind right after a stop can fail while accepted connections of the old listener are still closing. Test "stop → start within 1 s after keep-alive traffic" (§16); if it ever fails, retry `server_bind` for up to 10 s (0.25 s steps) before reporting the error. Never fall back to `SO_REUSEADDR`. |

IPv6/dual stack: the class keeps `address_family = AF_INET` and the service binds `127.0.0.1` only (config
rule). If IPv6 is ever added, set `IPV6_V6ONLY=0` explicitly (Windows default 1). Firewall: loopback binds
do not trigger the Defender Firewall prompt; the replication listener on a LAN address does (installer rule,
§17).

### 3.2 `http_server.py`
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 18-24 | imports `storage` (fcntl), `wal_archive`, `identity` | [C] | import chain fixed by the engine port (§17). |
| 34, 37, 216-218 | `runtime_digest = source_digest()` captured once; `/v1/capabilities.source_digest` | [P] | digest algorithm per §11 (must equal `tools/radios_database_setup.backend_source_digest`). |
| 41-46, 93 | per-request `settimeout` | OK | — |
| 102-142 `_upload_media` | 118 `staging = store.root/".uploads"`; 119 `mkdir(mode=0o700)`; 120 `is_symlink()`; 124 `NamedTemporaryFile(delete=False)` (binary); 134 `os.fsync`; 135 `stage_media(..., temporary, [staging])`; 140-142 `finally: temporary.unlink(missing_ok=True)` | [S] W4, W5 | 119-121 → `_platform.mkdir_private(staging, exist_ok=True)` (it rejects links incl. junctions). 134 → `_platform.fsync`. 141-142 → `_platform.unlink_quietly(temporary)` (**defect fix**: today an unlink `PermissionError` raised from `finally` after a successful commit turns into HTTP 500 although the media was staged). Add a start-up sweep that deletes `.uploads/upload-*` older than 1 h (`desktop_integrity`-style helper called from the service start; tools area wires it). |
| 158-189 | `/health` unauthenticated JSON | OK | contract used by `tools/radios_database.py:420` readiness. |
| 169-170 | rate limit key `"ip:" + client_address[0]` | OK | always `127.0.0.1`. |
| 244-261 | exception → status mapping; 258 `except (BrokenPipeError, ConnectionResetError)` | [S] W10 | Replace 258 with `except (ConnectionError, TimeoutError): self.close_connection = True` placed **before** `except Exception` (**defect fix**: `ConnectionAbortedError` mid-stream currently goes to `except Exception` → second write on a dead socket → traceback). Mapping of all other exceptions unchanged (tests assert 403/404/409/422/429/503/500 shapes). |
| 481-493 | media stream: `source.stat().st_size`, `source.open("rb")`, 1 MiB chunks | [S] W5 | `with _platform.open_read(source) as stream:` and take the length from `os.fstat(stream.fileno()).st_size` (no TOCTOU, and a long download no longer blocks object repair/retention from replacing/deleting the file). |
| 526-530 | `dispatch_workflow(...)` | OK | — |
| 541-549 `serve()` | `serve_forever()`; `finally: server_close()`; no signal handling | [C] W11 | §4.3 implementation (StopRequest + watcher → `server.shutdown()`), plus `_platform.configure_process()` and `_platform.check_timezone_database()` before binding. |

Log output: `BaseHTTPRequestHandler.log_message` writes to `sys.stderr`. The service always has stderr
redirected to `data\logs\database-service.log` (tools); never run the service under `pythonw.exe` without
redirection (`sys.stderr is None` → every request raises in `log_message`). Optionally override
`Handler.log_message` to drop per-request lines (the log is opened append-only by tools and grows forever;
Windows cannot rotate a file the service holds without share-delete).

### 3.3 Contract preserved (tests rely on it)
`make_handler(store) -> type[BaseHTTPRequestHandler]` with class attribute `source_digest`;
`BoundedHTTPServer(address, handler, *, max_connections=32, request_timeout=30.0)` with attributes
`max_connections`, `request_timeout`, `server_address`, `server_port`, `daemon_threads`, `block_on_close`,
`timeout`, methods `serve_forever/shutdown/handle_request/server_close/stats/_release_slot/
process_request_thread` (replication subclass `replication_stream.py:480-503` overrides the last one);
HTTP/1.1 keep-alive, `Connection: close` on errors and on GET with a body, `Cache-Control: no-store`,
`Retry-After`. Tests: `tests/test_desktop_*.py` start `BoundedHTTPServer(("127.0.0.1", 0), make_handler(...))`
in a thread (e.g. `test_desktop_admin.py:411`, `test_desktop_media.py:600`, `test_desktop_reset.py:217-223`,
`test_desktop_rekordbox_device.py:188-240`, `test_desktop_playout.py:877`) — port 0 + exclusive bind is fine.

---------------------------------------------------------------------------------------------------

## 4. Graceful shutdown (server side)

### 4.1 Linux behaviour today
* Production service = `tools/radios_database.py serve --instance <uuid>` (`:582-636`): builds
  `http_server.make_handler(store)` + `BoundedHTTPServer(..., max_connections=32)`, sets
  `daemon_threads=False`, `block_on_close=True`, `timeout=0.2`, installs `SIGTERM`/`SIGINT` handlers that set a
  `threading.Event`, loops `server.handle_request()` until set, then `server_close()` (joins request threads;
  each has its deadline), removes `service.json` if it is its own, closes `server.lock` (flock).
* Stop: `stop_service` (`:509-532`) checks ownership through `/proc/<pid>/stat` start ticks + boot id +
  cmdline, pins the process with `pidfd_open`, sends SIGTERM, waits ≤ `min(request_timeout+3, 60)` s.
* `sdbase serve` (`cli.py:359-363` → `http_server.serve`) has no handler: Ctrl+C → KeyboardInterrupt → exit
  130; SIGTERM = immediate death.
* Long-running CLI loops (`failover-watch` `cli.py:273-283`, `maintenance-loop` `:412-422`, `failover-serve`,
  `replication-serve`, `replication-follow`) stop only by KeyboardInterrupt/kill.

### 4.2 Windows mechanism (D3/D6, C spec 5.3) — `_platform.StopRequest`
* At start-up every long-lived RadioS Python process creates the **manual-reset** event
  `Local\RadioS-stop-<GetCurrentProcessId()>` (`CreateEventW(NULL, TRUE, FALSE, name)`, default security =
  creator/SYSTEM/Administrators). If `GetLastError() == ERROR_ALREADY_EXISTS` (stale object pre-created by
  someone in the session) call `ResetEvent` and log a warning.
* A daemon watcher thread waits with `WaitForMultipleObjects([stop_event, wake_event], FALSE, INFINITE)`; index 0
  → `self.event.set()`. `close()` signals `wake_event`, joins the thread, closes both handles.
* Console runs (developer `sdbase serve`, tests): in the main thread `signal.signal(SIGINT/SIGBREAK, …)` set
  `self.event` (no KeyboardInterrupt). A ctypes `SetConsoleCtrlHandler` routine (keep a module-level reference
  to the `WINFUNCTYPE` object) handles `CTRL_CLOSE_EVENT/CTRL_LOGOFF_EVENT/CTRL_SHUTDOWN_EVENT`: set the event,
  wait ≤ 4.5 s on `self.finished`, return TRUE; it returns FALSE for CTRL_C/CTRL_BREAK so the CRT → Python
  signal path runs. The detached service has no console; on logoff/shutdown it is terminated by the OS — the
  journal is crash-safe by design; the GUI may additionally `request_stop` the service from its
  `WM_QUERYENDSESSION` path (UI area, optional).
* Requesters call `request_stop(pid)` **after** verifying identity: the tools `service.json` stores
  `{"pid", "creation_ticks": process_identity(pid)[1], "instance", …}` instead of `boot_id/start_ticks`; the
  stopper opens the process handle first (`OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION)`; the
  handle pins the process object like a pidfd), re-checks creation time (and image path via
  `QueryFullProcessImageNameW` == the runtime python), then `request_stop(pid)`, then
  `WaitForSingleObject(handle, deadline)`. Never `TerminateProcess` the service on a timeout (keep the Linux
  "Servis kapanışı sürüyor" result); `TerminateProcess` stays only for `start_service`'s own failed child
  (`tools/radios_database.py:497-506`, `Popen.kill`).

### 4.3 Server-side wiring (this area)
```python
# http_server.serve (also the pattern for cli failover-serve / replication-serve)
def serve(store, host="127.0.0.1", port=8000, *, max_connections=32, request_timeout=30.0):
    from . import _platform
    from .api_server import BoundedHTTPServer
    _platform.configure_process()
    _platform.check_timezone_database()
    server = BoundedHTTPServer((host, port), make_handler(store),
                               max_connections=max_connections, request_timeout=request_timeout)
    with _platform.StopRequest() as stop:
        threading.Thread(target=lambda: (stop.event.wait(), server.shutdown()),
                         name="sdbase-stop-watch", daemon=True).start()
        try:
            server.serve_forever(poll_interval=0.25)
        finally:
            server.server_close()
            stop.finished.set()
```
* `tools/radios_database.py serve` (tools area) keeps its `handle_request()` loop, replacing lines 613-615 with
  `stop = _platform.StopRequest().install()` and `while not stop.event.is_set(): server.handle_request()`,
  then `server_close()`, state cleanup, `stop.finished.set()`, `stop.close()`. Exit code 0.
* Keep `daemon_threads=False, block_on_close=True` in the service so in-flight requests finish. Known
  limitation (both OSes): idle keep-alive connections from the GUI keep their handler thread in
  `readline()` until `request_timeout` → stop can take up to 30 s. Improvement (recommended): track accepted
  sockets in `BoundedHTTPServer.process_request` (a set guarded by `_stats_lock`) and, in a new
  `begin_shutdown()`, call `sock.shutdown(socket.SHUT_RD)` on idle ones before `server_close()`.
* Background jobs on stop: `desktop_transfer_package` and `desktop_integrity` jobs run in daemon threads
  (killed at interpreter exit, leaving `.partial` files). Add `cancel_running(timeout=5)` to both modules
  (sets `job.cancel` / kills the scrub child, §6.8) and call them from the service stop path before
  `server_close()`.
* CLI loops (`cli.py`): wrap `failover-watch`, `maintenance-loop`, `replication-follow` in
  `with _platform.StopRequest() as stop:` and pass `stop.event` as their `stop` argument
  (`supervisor.watch(stop)` and `run_online_maintenance(store, stop, …)` already accept one;
  `replication_stream.follow_forever` needs a `stop=` parameter — engine area). `failover-serve`
  (`failover_server.py:90-115`) already polls `stop` → pass `stop.event` in.

### 4.4 Tests
* Replace `process.send_signal(signal.SIGTERM)` in harnesses with `_platform.request_stop(process.pid)`
  (C spec 5.3); keep the expected graceful exit code 0 and the "service survives GUI stop" checks
  (`tests/test_application_shutdown.py:121-125,205-242`, tools area).
* New: `tests/test_sdbase_service_stop.py` — start `python_child_argv("-m", "sdbase.cli", "--db", <tmp>.sdb,
  "serve", "--port", "0"…)` (needs a `--port 0` friendly readiness: print the bound port JSON line), wait for
  `/health`, open an idle keep-alive connection, `request_stop(pid)`, assert exit 0 within
  `request_timeout + 5` s, assert a new server binds the same port immediately afterwards.

---------------------------------------------------------------------------------------------------

## 5. `cli.py`
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 12-15 | top-level imports `identity`, `schema` (→ storage/fcntl), `storage`, `endpoint_journal` (fcntl) | [C] | fixed by §7.1 + engine shim. |
| 48 `main` start | — | new | `_platform.configure_process()`; if `sys.stdout.encoding.lower() != "utf-8"`: `sys.stdout.reconfigure(encoding="utf-8")` (and stderr, `errors="backslashreplace"`) — protects developer runs without `-X utf8`; JSON on stdout is consumed by `desktop_integrity` and tools. |
| 50 | `--db` default `Path("RadioS.sdb")` (CWD-relative) | OK | keep; `desktop_integrity` passes an absolute path (§6.8). |
| 346-353 | `.sdb` suffix check (case-sensitive), `is_symlink()` on db/meta | [S] W4 | `_platform.is_link(args.db) or _platform.is_link(metadata)`. Suffix stays exact `.sdb` (same rule as tools/C config). |
| 359-363 | `serve` | [C] W11 | §4.3; map bind errors (§3.1). |
| 255-284, 285-294, 412-422 | loops | [C] W11 | §4.3 CLI loops. |
| 308-313 | `audio-render` → `EndpointAgent.open` + `render_active_plan_wav` | see §7 | — |
| 506 | `print(json.dumps(result, ensure_ascii=False, default=str, indent=2))` | [D] | UTF-8 stdout (above). |
| 507-514 | exit codes 0/1/2/130 | OK | keep (desktop_integrity maps 0/2 = report, else last stderr line). |

---------------------------------------------------------------------------------------------------

## 6. Desktop modules

### 6.1 `desktop_api.py`
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 22, 1377 | `ZoneInfo(station["timezone"])` (campaign windows) | [C] | tz database (§12). |
| 431-433 | routing recorder `folder`: `folder.startswith("/")` | [C]/[P] | New saves (`persisted=False`): `folder == "" or _platform.is_absolute(folder)` — must equal C `radios_path_is_absolute` (C spec §2.11 row 97: `routing_graph.c` uses it). Persisted documents (`persisted=True`: integrity scrub, transfer import `desktop_transfer_package.py:1563`): also accept `stored_path_flavour(folder) == "legacy-posix"` (the C side then treats it as missing and falls back). Message unchanged ("recorder folder must be empty or absolute"). |
| 434-436 | recorder `prefix` single visible name part (rejects `/`, `\`, leading `.`) | [S] W7 | add `_platform.component_valid(prefix)` for new saves (`:`/`*`/`CON` would make every recording fail in the C recorder). |
| 446 | stream `mount` must start with `/` | OK | URL path, not a file path. |
| 499-510 | implicit audio cycle detection relies on device naming `radios_virtual_<id>`, `<sink>.monitor`, `radios_virtual_<id>_mic`, `@talk` (PulseAudio conventions) | cross-area | The WASAPI backend must keep these synthetic identifiers for RadioS-internal virtual buses in the stored configuration, otherwise this validator silently stops detecting feedback loops. Real WASAPI endpoint ids (`{0.0.0.00000000}.{guid}`, ~55 chars) fit the 512-byte limits (`_routing_text`). |
| 656-723 | `audio_routing` v1 / `audio_preferences` store Pulse sink/source names (`@DEFAULT_SINK@`, `alsa_output…`) | [P] cross-area | No Python change (opaque ≤ 512-byte strings). The C audio area maps unknown names (Linux databases) and `@DEFAULT_SINK@`/`""` to the default endpoint. |
| 734, 815, 926, 961, 1509 | `file`, `primary_file`, `backup_file`, transition `files`, emergency `file`, `file_reference`: free text ≤ 4096 | OK | Native Windows paths pass. Not validated as paths (unverified references by design). |
| 1437-1502 `create_user` | 1469-1474: `mkdir(mode=0o700)`, `lstat`, `S_ISDIR`, **`info.st_uid != os.getuid()`**, `chmod(0o700)`; 1477 `os.open(..., O_WRONLY|O_CREAT|O_EXCL|`**`os.O_NOFOLLOW`**`, 0o600)`; 1479 `os.fdopen(fd, "w", encoding="utf-8")` (text, `\n`→`\r\n`, on a text-mode fd even `\r\r\n`); 1481-1483 `os.open(directory, `**`O_DIRECTORY`**`)` + fsync; 1484 returns `credential_file` path | [C] W2 W3 W8, [D] W1 | `directory = Path(store.root) / "private-user-keys"`; `_platform.mkdir_private(directory, exist_ok=True)` (verifies not-link, directory, owner, re-applies the private DACL — exact equivalent of 1470-1474); `fd = _platform.open_fd(credential_file, os.O_WRONLY \| os.O_CREAT \| os.O_EXCL, private=True)` (CREATE_NEW never follows an existing link name); `with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:` … `_platform.fsync(stream.fileno())`; delete the directory fsync (1481-1483). Volume without ACLs (database folder moved to exFAT via storage.ini) → fail closed: `StoreError("private user credential directory requires an NTFS/ReFS volume")` (C spec 5.2). Error branch 1494-1501: `_platform.unlink_quietly(credential_file)`. `credential_file` stays a native absolute path in the result (`desktop_admin.user_create` strips only `token`). |

### 6.2 `desktop_media.py`
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 196-233 `validate_library` | provenance: `relative` must use `/`, no `\`, not absolute, no empty/`.`/`..` parts (OK); `path`/`root` must `startswith("/")`, `os.path.normpath(v) == v`, `os.path.normpath(os.path.join(root, relative)) == path` | [C]/[P] (runs at every commit `schema.py:826-829` and in `integrity.py:249-252`) | Host-independent, flavour-aware (no OS calls): `flavour = stored_path_flavour(root)`; require `stored_path_flavour(path) == flavour` and flavour not None; `mod = ntpath if flavour == "windows" else posixpath`; require `mod.normpath(v) == v` for both; `joined = mod.normpath(mod.join(root, *relative.split("/")))`; equality: Windows flavour `_platform.path_equal(joined, path)` (case-insensitive — `PureWindowsPath.relative_to` in `_import_media` is case-insensitive, so `root` may differ in case from `path`), legacy-posix exact. Keeps Linux-created records valid (integrity "ok") and accepts every Windows record. |
| 324-337 `_managed_path` | `object_root(store).is_symlink()`, `path.parent.is_symlink()`, `path.is_symlink()`, `path.is_file()`, rehash, returns `str(path)` | [S] W4 | `_platform.is_link` for all three; hashing via `media._digest` (share-delete reader, §6.3). Returned `managed_path` is a native absolute path (`…\RadioS.sdb.objects\ab\<sha>`; ~140 chars, fine). |
| 531-554 `_source` | name checks (240 chars, no `/`/`\`, `Path(name).name == name`); `os.open(root, O_RDONLY\|`**`O_DIRECTORY\|O_NOFOLLOW`**`)`; `os.open(name, O_RDONLY\|O_NOFOLLOW\|`**`O_NONBLOCK`**`, `**`dir_fd=`**`)`; `fstat` regular + `0 < size <= 2 GiB` | [C] W2 | Add `_platform.component_valid(name)` to the name checks (rejects `a:b`, `CON.wav`, trailing dot). Then `descriptor, info = _platform.open_beneath(root, name, deny_write=True)`; keep the regular/size checks on `info`; `OSError` (incl. `EBUSY` while the GUI still writes the staged file) → `StoreError("staged import file is unavailable or unsafe")` — keep text (tests assert StoreError only). The GUI must close the staged file before calling import (C area). |
| 595-606 provenance build | `Path(source_path).is_absolute()`, `".." in parts`, `source.relative_to(root).as_posix()`, `str(source)`/`str(root)` | [C]/[P] | `_platform.is_absolute()` for both (Windows-only new writes, D1); `PureWindowsPath`; `".." in parts` kept; `relative = source.relative_to(root).as_posix()` (portable `/` form, D6); store `str(source)`, `str(root)` (native). |
| 607-609 | `source_name` safe basename (no `/`, `\`, `.`/`..`) | [S] | add `_platform.component_valid` (it is reused as a file name by transfer export `desktop_transfer_package.py:595`). |
| 618-631 | `media_service._stage_object(store, Path(f"/proc/self/fd/{descriptor}"))` | [C] W2 | `media_service._stage_object(store, None, source_fd=descriptor)` (new parameter, §6.3). Same guarantees: the bytes hashed/copied come from the already validated open file (deny-write share on Windows is stronger than the Linux before/after check, which stays). |
| 677 | `"managed_path": str(target)` | OK | native path. |

### 6.3 `media.py` (managed-object staging used by uploads, desktop imports, transfer imports, podcasts)
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 56-63 `_digest` | `path.open("rb")` | [S] W5 | `_platform.open_read(path)` (does not block concurrent replace/unlink). |
| 66-71 `_fsync_dir` | `os.open(dir, O_RDONLY)` + fsync | [C] W3 | **Delete** the helper and its calls (145-148, 175-176, 186) — no directory flush on Windows (D3). |
| 133-139, 142-189 `_stage_object(_locked)` | `source.open("rb")`; identity tuple `(st_dev, st_ino, st_size, st_mtime_ns)` before/after; `mkstemp` in `.objects/.staging`; `os.fsync`; `os.replace(temp, target)`; `temp.unlink` | [C]/[S] | New signature `_stage_object(store, source: Path \| None, *, source_fd: int \| None = None)`: when `source_fd` is given, `input_stream = os.fdopen(os.dup(source_fd), "rb")` and `seek(0)` (the dup shares the CRT binary mode); else `_platform.open_read(source)`. Keep the identity check (on Windows `st_ino` = 64/128-bit file id, `st_dev` = volume serial; NTFS updates LastWriteTime lazily, so with path sources the check is best effort — documented). `os.fsync` → `_platform.fsync`; 185 `os.replace` → `_platform.replace` (a corrupt object being replaced may be open by the C player/FFmpeg: retried 2 s, then `MediaError("managed object is in use; retry")`); 177-178 `is_symlink()` → `_platform.is_link`; 189 → `_platform.unlink_quietly(temp)`. Return tuple unchanged `(digest, size, target, reused)`. |
| 120-130 `_validate_source` | `expanduser().resolve(strict=True)`, `is_relative_to(root)` | OK | `resolve` expands 8.3 names/junctions/mapped drives consistently for both sides; `PureWindowsPath` comparisons are case-insensitive. |
| 227/253/259 | `"path": str(target)` | OK | stripped by `http_server._upload_media:137`. |
| 329-333, 377-381 | `target.is_symlink() or not target.is_file()` | [S] W4 | `_platform.is_link`. |
| 496-504 `reconcile_media` | `root.iterdir()`, `child.is_symlink()`, orphans as `str(path)` | [S] W4 | `_platform.is_link`; orphans are native paths (UI displays). |

### 6.4 `desktop_transfers.py`
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 34 | `tempfile.TemporaryDirectory(prefix=".native-transfer-", dir=store.root)` | [S] W5/W8 | `tempfile.mkdtemp(...)` (3.14: 0o700 → SY/BA/OW ACL) followed by `_platform.make_private(dir)` (explicit owner, 5.2); cleanup in `finally:` with `_platform.rmtree_quietly(dir)` (a Defender scan of `source.csv` must not turn a computed preview/apply into an error). Start-up sweep of stale `.native-transfer-*` (same helper as `.uploads`). |
| 36-39 | `os.open(source, O_WRONLY\|O_CREAT\|O_EXCL\|getattr(os,"O_NOFOLLOW",0), 0o600)` + `os.fdopen(fd, "wb")` | **[D] W1** | `fd = _platform.open_fd(source, os.O_WRONLY \| os.O_CREAT \| os.O_EXCL, private=True)`. Today on Windows every `\n` of the reviewed mAirList CSV / RadioBOSS M3U8 becomes `\r\n` (CRLF input becomes `\r\r\n` → extra blank CSV rows), so `source_sha256`/`source_bytes` differ from the client's file and the exact-byte contract of this module (docstring 1-6) is broken. |
| 89 | `apply_radioboss_m3u8(..., str(source), ...)` | OK | native path, read by `radioboss_m3u8._read` (`read_bytes`). |
| tests | `test_desktop_transfers.py:175-176` assert `S_IMODE == 0o600/0o700`; 169/185 assert no `.native-transfer-*` left | convert | `_platform.is_private(source)` and `is_private(source.parent)`; the glob assertions stay (cleanup must be reliable). |

### 6.5 `desktop_transfer_package.py` (Settings › Export/Import `.st` packages)
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 82-84 | `_PROTECTED` contains `"database/SDBASE"`; `_PROJECT_ROOT = Path(__file__).resolve().parents[4]` | **[S]** | Join protected names as components (`os.path.join(root, *name.split("/"))`); compare with `_platform.is_inside` (the mixed-separator literal never matches on Windows → protection bypass, same defect as C spec 2.6 row 84-98). `_PROJECT_ROOT` OK (long-name real path). |
| 85, 476-479 | `_FILE_NAME = [^/\\\x00-\x1f\x7f]{1,197}\.st`; default name `RadioS-<slug>-<YYYYmmdd-HHMM>.st` | [S] W7 | `api._text(...)`, `file_name.endswith(".st")`, not starting with `.`, ≤ 200 chars **and** `_platform.component_valid(file_name)` (rejects `:` ADS names, `CON.st`, trailing dots before `.st` are impossible but keep the generic rule). `_slug` output is already ASCII-safe. |
| 218-219 `_inside` | string prefix with `/` | [S] | `_platform.is_inside`. |
| 222-251 `_directory` | `startswith("/")`, `== "/"`, `"//" in value or normpath(value) != value`, `lstat` + `S_ISLNK`, `realpath(value) != value`, `os.access(R\|W\|X)`, protected check | [C]/[S] W4 W7 W12 | Implement the storage-folder rules of C spec 5.5 (identical to `storage_locations.c` and `tools/radios_database.py`): 1) non-empty, ≤ 4000 bytes, no controls, ≤ 240 UTF-16 units unless `long_paths_enabled()`; 2) `_platform.is_absolute`; 3) `_platform.is_canonical` (upper-case drive, `\` only, no `..`/`.`/duplicate/trailing separator, every component valid); 4) not `_platform.is_root`; 5) `lstat` → directory and not `stat_is_link`; 6) `_platform.path_equal(os.path.realpath(value), value)`; 7) `_platform.dir_writable(value)`; 8) not inside protected dirs (above). Messages, Windows wording (D1): «{what} klasör yolu mutlak olmalı (örn. D:\RadioS\Paketler).», «{what} klasör yolu sadeleştirilmiş olmalı (.., \\ ve sondaki \ olmadan).», «Klasör sembolik bağlantı veya bağlantı noktası (junction) olamaz: …», «Klasör yolunda bağlantı var; gerçek yolu seçin: …» (also shown for mapped drives / 8.3 spellings: the GUI must send `radios_fs_realpath` of the chooser result, as `media_archive_ui.c` does — C area). |
| 254-272 `_package` | `startswith("/")`, `normpath`, `.endswith(".st")`, `lstat` + `S_ISLNK` + `S_ISREG`, `os.access(R_OK)` | [C]/[S] | `is_absolute` + `is_canonical`; `.st` suffix compared case-insensitively (`value.lower().endswith(".st")`, Explorer may show `.ST`); `stat_is_link`; readability = try `_platform.open_read(value)` and close (W12). |
| 282-287 `_safe_component` | strips controls, `/`→`_`, `\`→`_`, `.strip().strip(".").strip()`, 150 chars | [S] W7 | `_platform.sanitize_component(text, fallback)` (also fixes the `"a. ."` → `"a."` trailing-dot edge). Used for album folders and song names in the zip **and** on disk. |
| 488, 936-938 | `desktop_reset.require_space(...)` | OK | `os.stat().st_dev` (volume serial) + `shutil.disk_usage` work. |
| 592-599 `_song_name` | `Path(source).name == source` | OK | with `component_valid` on `source_name` at import time (§6.2) this stays safe. |
| 629-634 `_relative_path` | `/`-only relative (rejects `\`, leading `/`, empty/`.`/`..`) | OK | portable form (D6). |
| 637-651 legacy `location` provenance | `path.startswith("/")`, `os.path.commonpath(dirname(...))`, `common != "/"`, `os.path.relpath(path, common)` | **[C]/[D]** | Group stored locations by `stored_path_flavour` (Windows locations written by `_import_playlist:1304` on this host; legacy-posix from Linux packages); per group use `ntpath`/`posixpath` `commonpath` inside `try/except ValueError` (mixed drives / mixed absolute-relative currently raise and kill the export job), skip a root `common` (`is_root` / `"/"`), and convert the relative result to `/` (`ntpath.relpath(...).replace("\\", "/")`) — today `os.path.relpath` yields `\` which `_relative_path` rejects → `StoreError` aborts the whole export. |
| 682-686 `_info` | `external_attr = 0o644 << 16`, `create_system` default 0 on Windows | improvement | set `info.create_system = 3` so Windows- and Linux-made packages carry identical metadata (readers ignore it). |
| 689-699 `_stream_song` | `source.open("rb")` | [S] W5 | `_platform.open_read(source)`. |
| 702-805 `_run_export` | 719 temp name `.<name>.<hex>.partial`; 724-725 `os.open(temp, ..., 0o600)` then close; 726 `ZipFile(temp, "w")` reopens by name; 791 `os.replace`; 792-799 dir fsync in `try/except OSError: pass`; 800-801 `temp.unlink` | [S] W5 W8 | `fd = _platform.open_fd(temp, O_RDWR\|O_CREAT\|O_EXCL, private=True)`; if the volume has no ACLs (FAT/exFAT USB stick — common for packages), create normally and add the warning «Hedef disk dosya izinlerini desteklemiyor; paket gizli değil (kayıtlı akış parolaları içerebilir).» (Linux mode 0600 is equally ignored on FAT). Then `with os.fdopen(fd, "w+b") as raw, zipfile.ZipFile(raw, "w", allowZip64=True) as zf:` (no reopen by name, so the private DACL and the handle stay ours). 791 → `_platform.replace(temp, target)` (Defender scans fresh archives → transient sharing violations); delete 792-799; 801 → `unlink_quietly`. Check total path length (`len(str(temp))` ≤ 240 unless long paths) before writing and fail with a clear message. |
| 819-862 `_open_package` | member names: reject leading `/`, `\`, `..`, NUL, unknown top folder | OK + [S] | keep (structure). Windows-invalid components are handled at placement (next row) so Linux-made packages with `Live: 2020` folders still import. |
| 945-983 `_place_song` | `parts = _relative_path(...).split("/")`; `directory.is_symlink()`; `directory.mkdir(exist_ok=True)`; `os.path.lexists` collision loop; temp `.<candidate>.<hex>.partial`; `temp.open("wb")`; `os.fsync`; `os.replace`; `temp.unlink` | **[C]/[S]** W4 W5 W7 | `parts = [_platform.sanitize_component(p, "_") for p in parts]` (prevents writing to `CON`, `NUL.mp3`, `album:stream` ADS, trailing-dot names); `_platform.is_link(directory)` (junction); length check (240 rule) → `StoreError` per song (caught at 1266 as a warning, unchanged behaviour); `fsync` → `_platform.fsync`; `os.replace` → `_platform.replace`; cleanup → `unlink_quietly`. The collision loop is already case-insensitive on Windows (`lexists`). The actually placed relative path is recorded at 1236 (`target.relative_to(root).as_posix()`) — keep. |
| 1104-1107, 1190-1193 | `source_root=str(placed_root)`, `source_path=str(Path(placed_root) / relative)` | OK | native Windows values; `validate_library` (§6.2) accepts them. |
| 1240 | `media_service._stage_object(store, target)` | OK | path source. |
| 1304 | playlist `location = str(target)[:4096]` | OK | native path. |
| 1606-1611 | `local_settings` (ui-preferences.ini, audio-panel.ini, media-archive.ini, library-analysis.ini, storage.ini) returned to the GUI | [P] cross-area | Linux packages carry POSIX paths in `storage.ini`/`media-archive.ini`; the C side must validate them with the Windows rules and drop/ignore invalid keys with a warning. Python only validates INI syntax (CRLF tolerant). |
| 166-181 jobs | daemon threads | improvement | `cancel_running()` (§4.3). |
| tests | `test_desktop_transfer_package.py:436` package mode 0600; 581, 604-605 POSIX provenance (`/indirilenler/...`, `/old/music/...`); 624 `str(root / paths[i])`; 635-652 invalid destinations (639 `os.symlink`, 642-643 `"/"`, trailing `/`) | convert | `_platform.is_private(package)`; POSIX provenance fixtures stay valid as **legacy** records for `validate_library`/export (that is exactly what they test) but the live-import fixtures (581) become Windows paths (`C:\İndirilenler\tek.wav`); 639 → `_winapi.CreateJunction(self.out, link)`; keep `"/"` (not absolute on Windows → still rejected) and add rejected vectors `"C:\\"` (root), `str(self.out) + "\\"` (trailing separator), `str(self.out).replace("\\", "/")` (forward slashes), the same path with a lower-case drive letter (not canonical), and a path through the junction. |

### 6.6 `desktop_reset.py`
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 147-160 `require_space` | `os.stat(folder).st_dev` grouping + `shutil.disk_usage(folder).free` | OK | — (volume serial grouping; caller-available bytes). |
| 181-197 `_backup_folder` | `root = desktop_backup_root()` or `<store.root>/../backups`; `root.mkdir(mode=0o700, parents=True, exist_ok=True)` | [S] W8 | `_platform.mkdir_private(root, parents=True, exist_ok=True)` when the folder is inside the project (`data\backups`); for a storage.ini backup folder on another disk, create normally (C spec 5.2: external storage folders are not privacy-checked). The backup content itself is engine area (`backup.create_backup`). |
| 200-201, 205, 213 | `strftime("%Y%m%dT%H%M%SZ")` folder names | OK | portable names. |

### 6.7 `desktop_admin.py`
No direct OS use. `:19` imports `integrity` (fcntl, engine) at module import — blocks `workflow_api`.
`user_create` (263-272) relies on `desktop_api.create_user` (§6.1). `overview` (197-207) calls
`inspect_store_health`/`inspect_integrity` (engine). Tests: `test_desktop_admin.py:294-295` assert
`credential_file` mode 0600 / parent 0700 → `_platform.is_private(credential_file)` and
`is_private(credential_file.parent)` + `owned_by_current_user`.

### 6.8 `desktop_integrity.py` (full scrub in a child process)
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 137-139 | `_PACKAGE_DIR = Path(__file__).resolve().parent`, `_SOURCE_ROOT = parent` | OK | — |
| 153 | `env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONPATH=str(_SOURCE_ROOT))` | **[C]** | `-E` (D4) ignores PYTHONPATH → child fails `No module named sdbase`. Use `env=_platform.python_child_env()` **and** `cwd=str(_SOURCE_ROOT)` (with `-m`, `sys.path[0]` is the CWD, so `sdbase` imports from the same tree). |
| 154-157 | `[sys.executable, "-m", "sdbase.cli", "--db", str(store.journal), "integrity", ...]` | [C] | `_platform.python_child_argv("-m", _PACKAGE_DIR.name + ".cli", "--db", str(Path(store.journal).absolute()), "integrity", …)` (absolute because the CWD changes). |
| 158-159 | `subprocess.run(command, capture_output=True, text=True, env=…, timeout=4 h)` | [C]/[D] | add `encoding="utf-8"`, `creationflags=_platform.CHILD_CREATIONFLAGS` (no console window from the detached service). Recommended robustness fix (D2): replace `run` with `Popen` + `communicate(timeout=1)` loop that checks a module stop flag set by `cancel_running()` and then `proc.kill()` (read-only scrub; killing is safe) — today a 4-hour scrub outlives a stopped service and keeps its shared store lock, blocking an offline restore. |
| 161-165 | 0/2 → `json.loads(stdout)`; else last stderr line without `"sdbase: "` | OK | keep (test `test_desktop_integrity.py:150-154`). |
| tests | `test_desktop_integrity.py:147` asserts `calls[0][1:3] == ["-m", "sdbase.cli"]` | convert | assert `command[command.index("-m") + 1] == "sdbase.cli"` and `command[1:6] == ["-E", "-s", "-B", "-X", "utf8"]`; 149 `--db` value equals `str(Path(store.journal).absolute())` (fixture paths are absolute already). |

### 6.9 `desktop_broadcast.py`
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 106-113 | `broadcast_record.filename`: trimmed, not `.`/`..`, no `/`/`\`, ends `.wav`, no leading `.`, ≤ 240 bytes, no controls | [S] W7 | For new saves add `_platform.component_valid(value)` (rejects `kayıt:1.wav`, `CON.wav`, `a*b.wav`, `a?.wav`, `"x".wav`); keep accepting persisted documents (the C recorder reports a clear error). Extend `test_desktop_broadcast.py:113` invalid list with those names. |
| 96-100 | stream URL checks (`\` rejected, path not `/`) | OK | URL rules. |

### 6.10 Rekordbox (USB/SD `export.pdb` and legacy XML)
USB/SD discovery is done by the GUI (`src/ui/playlist.c:1740-1837`: user picks the stick/folder, C opens
`PIONEER\rekordbox\export.pdb` with `openat`, sends `pdb_zlib_base64` + `usb_root` = parent of `PIONEER`, i.e.
`E:\` for a stick). There is **no** `/media`, `/run/media` or mount scanning in Python. C area: enumerate
removable volumes with `GetLogicalDrives` + `GetDriveTypeW == DRIVE_REMOVABLE` (and FAT32/exFAT volumes from
`GetVolumeInformationW`) to offer `X:\` roots; send `radios_fs_realpath`/normalised `usb_root`.

| File:line | Today | Status | Windows replacement |
|---|---|---|---|
| `rekordbox_device.py:4,126-131` `_media_path` | rejects `\`, `..` parts, `:` in the first part; `str(PurePosixPath(root) / path.lstrip("/"))` | [C] | `parts = [p for p in path.split("/") if p]` (DeviceSQL paths are `/Contents/...`); `\`, a `..` part or `:` in the **first** part (drive escape) → `StoreError` (today's rules, unchanged); any other part failing `_platform.component_valid` → `source_path=None` plus a `LossRecord("unsupported", track, "Location", "Device path is not representable on Windows")` instead of failing the whole library; result `str(PureWindowsPath(root).joinpath(*parts))` → `E:\Contents\Örnek DJ\Track 1.wav`. |
| `rekordbox_device.py:134-137` `usb_root` | `startswith("/")`, ≤ 4096, no controls, no `..` | [C] | `_platform.is_absolute(usb_root)` (drive roots `E:\` and UNC allowed — `is_root` is **allowed** here), no controls, no `.`/`..` components (split on both separators), canonical (`is_canonical` except the trailing separator of a root). Message: "usb_root must be an absolute USB/SD folder such as E:\\". |
| `desktop_rekordbox.py:104-106, 133-134, 291` | `location = track.source_path` (≤ 4096), duplicate key by location text, `usb_root` in the replay payload | OK | native strings. |
| `rekordbox.py:299-321` `_source_path` | `file://localhost/C:/x` → `C:/x`; `file://host/share/x` → `//host/share/x`; POSIX kept | [P] | Convert Windows-drive and UNC results to native separators (`str(PureWindowsPath(path))` → `C:\x`, `\\host\share\x`) so `location` values match other native paths (display, duplicate detection). macOS/Linux XML paths stay POSIX text (foreign references). `by_uri`/`by_path` matching stays consistent because both sides use `_source_path`. |
| `rekordbox.py:525-554` `read_rekordbox_xml` | `os.open(path, O_RDONLY\|O_CLOEXEC\|O_NOFOLLOW)` (=0 on Windows) + `os.fdopen(fd,"rb")` | [D] W1 | Unused in the repository (only defined). D2b: delete, or convert with `_platform.open_fd(path, os.O_RDONLY, nofollow=True)`; drop `st_ctime_ns` from the identity tuple (Windows creation time). |
| `rekordbox.py:608-671` `_normalized_source_path`, `map_media_path` | already handles drive/UNC/POSIX, case-insensitive for Windows style; `Path(root).resolve()`, `is_relative_to` | OK + [S] | When `target_root` is Windows and the relative tail contains a component that fails `component_valid` (macOS name with `:`) → `PathMapping("invalid", …, "Mapped name is not valid on Windows")`. |
| `importer.py:244-253, 298-306` | `_media_proposal` → `map_media_path`; request fingerprint contains `str(root)` of `allowed_roots` | OK | host-native strings (fingerprints are compared on the same host). |
| `settings.py:66-69` | `allowed_roots` must satisfy `Path(root).is_absolute()` | [C]/[P] | `_platform.is_absolute(root)` for new profiles; `import_options` (105-116) re-validates stored profiles: a Linux profile (`/srv/music`) now fails with «İçe aktarım profili Linux klasör yolları içeriyor; Windows klasörlerini yeniden seçin.» (no silent use). `path_rules.source_root` stays free text (source side may be macOS/Linux). |
| tests | `test_desktop_rekordbox_device.py:36,45,56,74,90-95,114,125,172` POSIX roots (`/media/DJ USB`); `test_desktop_rekordbox.py:30-32,239` `file://localhost/not-accessible/…` | convert | Roots → `D:\\DJ USB`, `E:\\`; expected `D:\\DJ USB\\Contents\\Örnek DJ\\Track 1.wav`; invalid roots `None, "relative", "D:\\..\\etc", "D:\\USB\x00", "\\media", "C:media"`; traversal vector unchanged; the XML fixtures with POSIX `file://localhost/…` stay (foreign POSIX text). |

### 6.11 Other `desktop_*.py`
`desktop_advert.py`, `desktop_autodj.py`, `desktop_automation_calendar.py`, `desktop_copyright.py`,
`desktop_local_event.py`, `desktop_player_topology.py`, `desktop_playlist.py` (location ≤ 4096 text),
`desktop_playout.py`, `desktop_probability_policy.py`, `desktop_programme.py`, `desktop_weekly_programme.py`,
`desktop_workflows.py` (action ids split on `/` — not paths; workflow schemas have no path patterns),
`desktop_audio_preferences.py`: **[OK]** except the tz validators in §12. CSV/report exports return
strings built with `io.StringIO(newline="")` + explicit `\r\n` (`desktop_advert.py:1179`) — the GUI writes
them in binary (D6).

---------------------------------------------------------------------------------------------------

## 7. Endpoint agent and offline audio executor

### 7.1 `endpoint_journal.py`
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 5 | `import fcntl` (module level; imported by `cli.py:15`, `endpoint.py`, `endpoint_media.py`, `audio_playout.py`) | **[C]** | delete; use `_platform.lock/unlock`. |
| 23-28 `_fsync_directory` | `os.open(dir)` + fsync | [C] W3 | delete helper and callers (47 here, `endpoint_media.py:87`). |
| 36-42 | `owner.lock`: `os.open(O_CREAT\|O_RDWR, 0o600)` + `flock(LOCK_EX\|LOCK_NB)`; `BlockingIOError` → "endpoint state directory is already open" | [C] | `self.lock_fd = _platform.open_fd(root / "owner.lock", os.O_CREAT \| os.O_RDWR, private=True)`; `_platform.lock(self.lock_fd, blocking=False)` (raises `BlockingIOError`, message unchanged). Region 0x4000000000000000 (C spec 5.1 lists `owner.lock`). |
| 45 | `os.open(Endpoint.sdb, O_CREAT\|O_RDWR\|O_APPEND, 0o600)` | **[D] W1** | `_platform.open_fd(self.path, os.O_CREAT \| os.O_RDWR \| os.O_APPEND, private=True)`; the shim keeps `FILE_WRITE_DATA` so `os.ftruncate(self.fd, offset)` (76, crash-tail repair = `_chsize_s` → `SetEndOfFile`) works; append is the CRT's seek-to-end (single writer under `owner.lock`, so no atomicity needed). Today text mode writes `\r\n` line ends (replay tolerates them, but the file differs from Linux and byte offsets drift from the logical record sizes). |
| 60-67 `close` | `flock(LOCK_UN)` + close | [C] | `_platform.unlock` + `os.close`. |
| 71-94 `_replay` | `self.path.open("rb")` (second handle while `self.fd` is open), line-based, `ftruncate` of an incomplete tail | OK | share modes are compatible (our handle shares READ\|WRITE\|DELETE). |
| 96-111 `append` | `os.write` loop + `os.fsync` | OK after W1 fix | `_platform.fsync`. |
Format contract (unchanged): UTF-8 JSON lines terminated by `\n`, `hash = sha256(prev_hash_bytes + canonical JSON)`;
`Endpoint.sdb` + `owner.lock` names.

### 7.2 `endpoint_media.py`
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 12 | imports `_fsync_directory` | [C] | remove. |
| 26-31 `_digest` | `path.open("rb")` | [S] W5 | `_platform.open_read`. |
| 57-59, 131 | `target.is_symlink() or target.parent.is_symlink()` | [S] W4 | `_platform.is_link`. |
| 67-93 `_publish_stream` | `mkstemp` in `staging`, `os.fsync`, `os.replace(temp, target)`, `_fsync_directory(target.parent)`, `temp.unlink` | [C]/[S] | `_platform.fsync`; `_platform.replace` (the external playout process may be playing the object being re-staged); delete dir fsync; `unlink_quietly`. Returned `"path": str(target)` native. |
| 100-115 `stage_media` | `resolve(strict=True)`, `is_relative_to(root)`, identity tuple | OK | — |

### 7.3 `endpoint.py`
`Path(root).expanduser().resolve()` (81, 87-90, 115) OK. `reconcile_startup` (389-417): `path.unlink()` of
`*.partial` → `_platform.unlink_quietly(path)` (count only removed ones) so a scanner holding a partial file
cannot make the agent fail to open. `active_manifest` (433-444) returns `local_media` native paths — consumed
by `audio_playout._media_path` (`Path(supplied) != expected` is case-insensitive on Windows, fine) and by an
external playout process. `ZoneInfo(...)` at 167, 300, 326 → tz database (§12).

### 7.4 `audio_playout.py` (`sdbase audio-render`)
| Line(s) | Today | Status | Windows replacement |
|---|---|---|---|
| 67-82 `_open_local_file` | `path.is_symlink() or path.parent.is_symlink()`; `os.open(path, O_RDONLY\|getattr(O_NOFOLLOW,0))`; `os.fdopen(fd,"rb")`; `S_ISREG` | **[D] W1**, [S] W4 | `fd = _platform.open_fd(path, os.O_RDONLY, nofollow=True)` (+ `_platform.is_link(path.parent)` → `AudioRenderError`). Today WAV bytes are read through a text-mode fd: every `0x1A` byte ends the read and `\r\n` pairs are folded → hash mismatch ("local media differs…") or truncated audio. |
| 99-100 `_file_identity` | `(st_dev, st_ino, st_size, st_mtime_ns, st_ctime_ns)` | [S] | drop `st_ctime_ns` (Windows creation time, deprecated meaning) — use `(st_dev, st_ino, st_size, st_mtime_ns)`. |
| 181, 282 | `wave.open(str(path))` (binary), `wave.open(stream)` | OK | — |
| 319-332 | output path: `.wav` suffix (case-insensitive), `parent.resolve(strict=True)`, `target.exists() or is_symlink()` | OK | `_platform.is_link(target)`. |
| 335-349 | `mkstemp(prefix=".sdb-audio-", suffix=".wav", dir=parent)`; `os.fsync` | OK | `_platform.fsync`. |
| 354 | `_hash_file(temp)` | OK after W1 fix | — |
| 357-360 | publish with `os.link(temp, target)`; `FileExistsError` → "output file appeared while rendering" | [C] W6 | `_platform.rename_noreplace(temp, target)` (works on FAT/exFAT/SMB; same no-overwrite guarantee). |
| 361-365 | `os.open(parent, O_RDONLY)` + fsync **inside the try** | **[C]** defect | delete (today on Windows the render is published and then reported as "PCM WAV could not be written"). |
| 369 | `output_path: str(target)` | OK | native. |
| 387-389 | `temp.unlink(missing_ok=True)` | OK | `unlink_quietly`. |

### 7.5 `endpoint_protocol.py`, `transport.py`, `http_pool.py`, `http_limits.py`
**[OK]**. `http.client` does not consult the Windows proxy registry (unlike `urllib`). Base URLs must use
`127.0.0.1` (W15). Socket errors on Windows are `OSError` subclasses already caught (`transport.py:52`).
`time.monotonic()` is high-resolution in 3.14 (rate buckets, pool deadlines).

---------------------------------------------------------------------------------------------------

## 8. Importers/exporters for other playout systems (paths inside playlists)
| File:line | Today | Status | Windows replacement |
|---|---|---|---|
| `radioboss_m3u8.py:24,32-39` `absolute_media_path` | string rule: POSIX `/x`, `X:\`/`X:/`, UNC | OK | format rule of a Windows product; no filesystem access. Keep. |
| `radioboss_m3u8.py:42-54` `_read` | `.m3u8` suffix (lower), `read_bytes()` | OK | binary; exact-byte sha256 now correct once `desktop_transfers` writes binary (§6.4). |
| `radioboss_m3u8_import.py:114` | `ZoneInfo(station tz)` | [C] | §12. |
| `radioboss_m3u8_export.py:121` | `"\r\n".join(lines)` returned as a string | OK | the GUI must write `content` in binary (D6) or RadioBOSS gets `\r\r\n`. |
| `mairlist31_csv.py:95-107` | `read_bytes()` + `decode(encoding)` (`utf-8-sig` default; caller may pass `cp1254`/`cp1252`: all codecs exist in the embeddable stdlib zip) | OK | — |
| `mairlist31_export.py:15,49-51` `_absolute_path` | `Path(value).is_absolute() or ntpath.isabs(value)` (host-dependent) | [P] | `isinstance(value, str) and "\x00" not in value and ntpath.isabs(value)` — destination paths are for the mAirList (Windows) machine; 3.13+ `ntpath.isabs` rejects `\foo` and `C:foo`, accepts `C:/x` and UNC. Tests with POSIX destinations become Windows paths. |
| `mairlist31_export.py:138-139` | `csv.writer(..., lineterminator="\r\n")` into `StringIO(newline="")` | OK | GUI writes binary. |
| `mairlist31_import.py` | path strings used as identity evidence only | OK | — |
| `libretime_adapter.py:56-81` `_verified_uri` | `/x` → `Path(x).as_uri()`; for `file:` URIs `Path(unquote(parsed.path))`; netloc must be `""`/`localhost` | **[C]** | `if _platform.is_absolute(source): source = PureWindowsPath(source).as_uri()` (`file:///C:/…`, UNC → `file://host/share/…`); for `file:` URIs `path = Path.from_uri(source)` (3.13+: drive letters, UNC, %-escapes; `ValueError` → `StoreError`); allow netloc `""`, `localhost` or the UNC host produced by `from_uri`; `_platform.is_link(path)` instead of `is_symlink`; read with `_platform.open_read`. Today `WindowsPath("/srv/x").as_uri()` raises `ValueError` and `Path("/C:/x")` is `\C:\x`. |

---------------------------------------------------------------------------------------------------

## 9. Network fetchers
* `podcast.py:115-147` — `socket.getaddrinfo`, pinned numeric connections, `ssl.create_default_context()`
  (loads the Windows ROOT/CA stores; corporate TLS-inspection roots work), `http.client` (no proxy support —
  same as Linux; document). **[OK]**. `782-794`: `mkstemp(prefix=".podcast-", dir=store.root)` +
  `stage_media(..., [store.root])` **[OK]**; its cleanup (after 812) should use `unlink_quietly`.
* `recording_jobs.py` — source URIs are network schemes only (95-102); object verification 400-423 uses
  `media._digest` (share-delete after §6.3) and `is_symlink` → `_platform.is_link`; tz at 295, 356.

---------------------------------------------------------------------------------------------------

## 10. Monitoring, capacity, diagnostics
| File:line | Today | Status | Windows replacement |
|---|---|---|---|
| `capacity_monitor.py:182-191` | `os.statvfs(store.root)`/`os.statvfs(media_root or root)`; `f_bavail * f_frsize`; only `OSError` caught | **[C]** | `database_free_bytes = _platform.disk_free(store.root)`; `media_free_bytes = _platform.disk_free(media_root if media_root.exists() else store.root)` in the same `try/except OSError` (AttributeError today → HTTP 500 for `capacity/inspect`, `capacity/evaluate`, and `ops_scheduler` delivery checks). |
| `capacity_monitor.py:109-116` | `os.stat(path).st_size` | OK | — |
| `capacity_monitor.py:562` | `ZoneInfo` | [C] | §12. |
| `monitoring.py:34-35` | `shutil.disk_usage(store.root)` | OK | same numbers semantics (`free` = caller-available). |
| `diagnostics.py` | pure logic | OK | — |

---------------------------------------------------------------------------------------------------

## 11. Source digest — `runtime_version.py:10-18`
Today: `for source in sorted(Path(__file__).parent.glob("*.py"))` — on Windows `glob` is case-insensitive
(would include `X.PY`) and `WindowsPath` ordering is case-insensitive, so the order can differ from the C/tools
implementation (C spec 5.7). Replace with:
```python
def source_digest() -> str:
    package = Path(__file__).parent
    names = sorted((entry.name for entry in os.scandir(package)
                    if entry.name.endswith(".py") and entry.is_file(follow_symlinks=False)),
                   key=lambda name: name.encode("utf-8"))
    digest = hashlib.sha256()
    for name in names:
        digest.update(name.encode("utf-8")); digest.update(b"\0")
        digest.update((package / name).read_bytes()); digest.update(b"\0")
    return digest.hexdigest()
```
The tools side (`tools/radios_database_setup.py:185-198`) must use the identical selection/order (tools area).
Note: adding `_platform.py` changes the digest on both sides automatically. Git checkouts with
`core.autocrlf=true` change bytes but both sides read the same files.

---------------------------------------------------------------------------------------------------

## 12. Time zones (all call sites)
* Data: bundled `tzdata` package (D3: `runtime\python\Lib\tzdata`, no `PYTHONTZPATH`). `zoneinfo` falls back
  to it automatically. Service/CLI start: `_platform.check_timezone_database()` (fail fast, Turkish message).
* Validation call sites → `_platform.canonical_timezone(name)` (exact-case membership in
  `available_timezones()`, W16): `schema.py:873-877` (every `stations` put — also guards `identity.bootstrap`
  and `init` with `--timezone`), `station_clone.py:93-99`, `desktop_audio_preferences.py:62-68`,
  `desktop_automation_calendar.py:318-319`, `desktop_probability_policy.py:77-78`, `planning.py:73-77`
  (`_zone` returns None for non-canonical keys). Persisted Linux keys are canonical already (Linux lookups are
  case-sensitive), so no existing data is rejected.
* Computation call sites (keys already validated): `auto_schedule.py:159`, `clocks.py:421`,
  `asrun_reports.py:161`, `capacity_monitor.py:562`, `desktop_api.py:1377`, `desktop_advert.py:336,412,1087`,
  `desktop_programme.py:175`, `endpoint.py:167,300,326`, `plans.py:399`, `playlists.py:450,569`,
  `playout.py:168,392`, `radioboss_m3u8_import.py:114`, `recording_jobs.py:295,356`, `schema.py:714`,
  `smart_blocks.py:497`, `schedules.py:194,299,367,467` — no change beyond the database being present.
* Cross-area risk: the C side derives offsets from Windows registry rules via ICU mapping (D3) while Python
  uses tzdata 2026.5. Present/future instants agree for normal zones; add a cross-language test comparing
  `ZoneInfo` offsets with `radios_time_zone_new` for Europe/Istanbul, Europe/Berlin, America/New_York,
  Australia/Lord_Howe over 2026-2028 transitions. Keep the tzdata wheel updated with every release.

---------------------------------------------------------------------------------------------------

## 13. Paths crossing the C ↔ Python boundary (HTTP JSON)
Rule (D6, C spec 5.6): absolute paths are **native Windows form** (`C:\…`, `\\server\share\…`, upper-case drive
letter as produced by `radios_fs_realpath`/`radios_path_normalize`); relative/provenance paths stored in
records use portable `/` form; all comparisons are separator-agnostic and case-insensitive.

| Direction | Field (route) | Producer → validator | Notes |
|---|---|---|---|
| C→Py | `import_path` (desktop-media/import, import-staged) | basename in `data\imports` | `component_valid`; file closed by the GUI before the call. |
| C→Py | `source_path`, `source_root` (same) | provenance; `is_absolute`, `PureWindowsPath` | stored native; `source_relative_path` stored with `/`. |
| C→Py | `destination_dir`, `file_name` (desktop-transfer/export-start) | `_directory` (C spec 5.5 rules) | GUI sends realpath'd folder. |
| C→Py | `package_path`, `media_root` (desktop-transfer/inspect, import-start) | `_package`, `_directory` | — |
| C→Py | `usb_root` (desktop-rekordbox/preview, apply) | `rekordbox_device` root rules | drive root `E:\` allowed. |
| C→Py | `items[].path`, `paths[].path` (transfers/mairlist-export, radioboss-export) | export destination text | `ntpath.isabs` (mAirList) / `absolute_media_path` (RadioBOSS). |
| C→Py | desktop configs: routing recorder `folder` | `is_absolute` (= `radios_path_is_absolute`) | other file fields are free text. |
| C→Py | audio device ids (`*_sink`, `*_source`, `device`) | opaque ≤ 512 bytes | WASAPI ids / synthetic `radios_virtual_*` names (§6.1). |
| C→Py | `local_settings` INI texts | INI syntax | C writes the files back in binary with `\n` (D6). |
| Py→C | `managed_path` (desktop-media get/import/analysis), `credential_file` (desktop-admin/user-create), `package_path`, `media_root`, `path` (desktop-reset/backup), job `package_path`, `orphans` (media/reconcile), library `source_path`/`source_root`, playlist `location`, `local_media` (endpoint manifest), `output_path` (audio-render) | native paths | The C side must not split on `/` only (use `g_path_*` / `radios_path_*`). |
| Py→C | warnings/messages containing paths | Turkish text | backslashes are JSON-escaped; nothing to do. |

Legacy data (Linux-created databases/packages): POSIX absolute strings remain **valid** in persisted-record
validators (string-only, `stored_path_flavour == "legacy-posix"`) so integrity stays "ok" and metadata saves
of those records keep working; new writes require Windows form. File names with unpaired UTF-16 surrogates
(possible on NTFS) cannot be encoded as UTF-8 JSON: `http_server._send` raises `UnicodeEncodeError`, which is
a `ValueError` and is already mapped to 422 — no change needed (only directory listings such as
`reconcile_media` orphans could hit it, and those are hex names).

---------------------------------------------------------------------------------------------------

## 14. Text vs binary and encodings (all sites in this area)
| Site | Kind | Fix |
|---|---|---|
| `desktop_transfers.py:36-38` | raw bytes via `os.open` fd | `open_fd` (binary) — **data corruption today**. |
| `endpoint_journal.py:45,104` | journal via `os.write` | `open_fd` (binary). |
| `audio_playout.py:73-77` | WAV reads via `os.open` fd | `open_fd` (binary). |
| `rekordbox.py:536-544` | XML reads via `os.open` fd | `open_fd` or delete (unused). |
| `desktop_media.py:542` (+ `/proc` reopen) | import source | `open_beneath` (binary). |
| `desktop_api.py:1479` | credential JSON (text) | `encoding="utf-8", newline="\n"` on a binary fd. |
| `desktop_transfer_package.py:724-726` | zip container | binary fd + `ZipFile(fileobj)`. |
| `cli.py:506` | JSON on stdout | UTF-8 stdout (`-X utf8` + reconfigure guard). |
| `desktop_integrity.py:158` | child stdout/stderr | `encoding="utf-8"`. |
| `cli.py:30` | `read_text(encoding="utf-8")` | OK. |
| every `read_bytes()` / `open("rb"/"wb")` / `mkstemp` / `NamedTemporaryFile` | binary | OK (readers that must not block replace → `open_read`). |
| CSV/M3U8/report exports returned as strings with `\r\n` | — | GUI writes binary. |

---------------------------------------------------------------------------------------------------

## 15. Rename / delete / flush / lock sites (summary)
| Site | Operation | Shim |
|---|---|---|
| `media.py:185` | publish content-addressed object | `replace` |
| `endpoint_media.py:86` | publish endpoint object | `replace` |
| `desktop_transfer_package.py:791, 980` | package / placed song | `replace` |
| `audio_playout.py:358` | WAV publish (no overwrite) | `rename_noreplace` |
| `http_server.py:142`, `media.py:189`, `endpoint_media.py:93`, `desktop_transfer_package.py:801,982`, `audio_playout.py:389`, `endpoint.py:393`, `desktop_api.py:1499`, podcast cleanup | temp cleanup | `unlink_quietly` |
| `desktop_transfers.py:34` | temp dir cleanup | `rmtree_quietly` |
| `media.py:66-71,148,176,186`, `endpoint_journal.py:23-28,47`, `endpoint_media.py:87`, `audio_playout.py:361-365`, `desktop_api.py:1481-1483`, `desktop_transfer_package.py:792-799` | directory fsync | **delete** |
| `http_server.py:134`, `media.py:166`, `endpoint_journal.py:77,108`, `endpoint_media.py:83`, `audio_playout.py:349`, `desktop_api.py:1480`, `desktop_transfer_package.py:977`, `podcast.py:788` | file fsync | `fsync` |
| `endpoint_journal.py:39,65` | `owner.lock` | `lock(blocking=False)` / `unlock` |

---------------------------------------------------------------------------------------------------

## 16. Tests: how they exercise this area and what changes
Runner: CTest calls `${Python3_EXECUTABLE} tests/test_desktop_*.py` or `-m unittest` (e.g.
`CMakeLists.txt:639-653, 915-916, 1090-1092`); the test files insert `ROOT/database/SDBASE/src` into
`sys.path` themselves (`test_desktop_api.py:19-21`) and create temp dirs under `ROOT/build`
(`test_desktop_media.py:43`). On Windows: `Python3_EXECUTABLE` = `<root>\runtime\python\python.exe` (D7);
the `PYTHONPATH=…:…` entries (`CMakeLists.txt:916,1092`) use `:` and must become `;`-separated — escaped as
`\;` inside CMake `ENVIRONMENT` or replaced by `ENVIRONMENT_MODIFICATION PYTHONPATH=path_list_append:…`
(build area); with `-E` they are ignored anyway, which is why the tests' own `sys.path.insert` matters.
Fixtures must `os.path.realpath()` any `%TEMP%` directory (8.3 `SONATT~1`) before comparing paths.

| Test | Exercises | Windows conversion |
|---|---|---|
| `test_desktop_media.py` | import/staging/provenance (101-127 POSIX provenance), confinement 302-318 (`symlink_to` 304, `os.mkfifo` 306), managed-path link rejection 534-543 (file symlink 535, directory link 542), HTTP 600 | provenance → Windows paths (`C:\\Müzik\\Albüm\\Disc 1\\song.wav`, root `C:\\Müzik`, expected relative `Albüm/Disc 1/song.wav`); 304/535 file symlinks → skip if `os.symlink` raises `OSError` (unprivileged); 306 FIFO → a directory named `pipe.wav` (non-regular); 542 → `_winapi.CreateJunction(moved, prefix)`; add a staged file held open for writing → `StoreError`. |
| `test_desktop_api.py` | configs, CAS, HTTP, `create_user` 869 (mode 0600) | `is_private` + `owned_by_current_user`; recorder folder vectors `C:\\Kayıt` valid, `/tmp/x` invalid for new saves. |
| `test_desktop_admin.py:294-295,411` | credential file/dir privacy, HTTP | `is_private` both. |
| `test_desktop_transfers.py:171-185` | exact private bytes, cleanup | `is_private`; add a CRLF + `\x1a` source and assert `preview["source_sha256"] == sha256(raw)`. |
| `test_desktop_transfer_package.py` | export/import round trips, privacy 436, provenance 581-624, destination validation 635-652 | §6.5 row "tests"; add: Linux-made package with `medya/Live: 2020/CON.mp3` imports as `Live_ 2020\_CON.mp3`; export to a folder on a mapped/junction path rejected. |
| `test_desktop_rekordbox_device.py`, `test_desktop_rekordbox.py` | DeviceSQL parsing, usb_root, HTTP | §6.10. |
| `test_desktop_broadcast.py:113,310` | record filename rules | add Windows-invalid names (§6.9); `chmod(0o600)` harmless. |
| `test_desktop_integrity.py:135-154,205` | child scrub process | §6.8. |
| `test_desktop_playout.py:635` | link rejection | junction or skip. |
| `test_desktop_reset.py`, `test_desktop_routing.py` (160 `/tmp/capture.wav` is only a forbidden-key value), `test_desktop_autodj.py`, `test_desktop_advert.py`, others | logic + HTTP | unchanged (`chmod(0o600)` only clears read-only). |
| new `tests/test_sdbase_platform.py` | shim | §2.3. |
| new `tests/test_sdbase_service_stop.py` | stop event, exclusive bind, keep-alive drain | §4.4. |
| new HTTP tests | `ConnectionAbortedError` mid media stream → no 500/traceback; second `BoundedHTTPServer` on the same port → `OSError` winerror 10048 | add to `test_desktop_media.py` or a new file. |

---------------------------------------------------------------------------------------------------

## 17. Boundary findings outside this area (for the owners)
1. **Engine:** `storage.py:17` `import fcntl` (blocks `import sdbase`), `integrity.py:12`, `disk_index.py:13,18`
   (`mmap`: a mapped file cannot be truncated/replaced/deleted on Windows), `failover.py:10`,
   `migration.py:12`, `online_maintenance.py:11`, `replication.py:15`, `replication_stream.py:14`; directory
   fsyncs `backup.py:52`, `disk_index.py:86`, `portable_export.py:56`, `storage.py:129`; `renameat2` via ctypes
   (`backup.py:11`, `docs/portable_export.md:31`) → `MoveFileExW` without REPLACE (= `os.rename` on Windows);
   `replication_stream.py:565` `build_opener(...)` keeps the default `ProxyHandler` → on Windows it reads the
   **registry proxy** (Linux: env only) → pass `ProxyHandler({})`; `replication_stream.py:795` text-mode
   hash file (`open("w", encoding="ascii")`) → `newline="\n"`; `follow_forever` needs a `stop` parameter (§4.3).
2. **Backup ops (ownership unclear — please claim):** `ops_scheduler.py:14` fcntl, `:139-140` `mode & 0o077`
   on `output_dir` (always fails on Windows: dirs report 0o777) → `is_private`, `:150-164` flock → `lock`,
   `:207` `os.fchmod`, `:213-217` dir fsync; `ops_archive.py:56-57` `O_NOFOLLOW/O_CLOEXEC`, chmods 205-266;
   `database/SDBASE/tools/ops_scheduler.py:95-105` SIGINT/SIGTERM loop → StopRequest.
3. **Tools (`tools/radios_database.py`, `tools/radios_database_setup.py`):** `/proc` identity (76-88), boot_id,
   `pidfd_open`/`pidfd_send_signal` (515-532), SIGTERM/SIGINT (613-615), `fcntl.flock(server.lock)` (598),
   `_read_json` `O_NOFOLLOW|O_NONBLOCK` + `st_uid != getuid() or st_mode & 0o077` (40-48), `_write_json`
   text mode + dir fsync (55-73), `_copy_fsync` fsync on an `O_RDONLY` fd (286-292: FlushFileBuffers needs
   write access), `os.sync()` (327), `start_new_session=True` (472-474: ignored on Windows → use the D3
   detached flags), log fd `st_uid`/`fchmod` (458-463), `storage_path_check`/`_inside` with `/` (119-136),
   `_last_database` `startswith("/")` (278), `radios_database_setup.py:165-167` `python_command` regex forbids
   `\` and `:` (Windows paths), `:249` `getuid`, `backend_source_digest` (185-198) must match §11.
4. **SDBASE dev tools:** `database/SDBASE/tools/engine_acceptance.py:4` fcntl, `stream_acceptance.py:37`
   SIGTERM, `:290` `openssl` CLI (not on a plain Windows PATH → commit a pre-generated short-lived test
   certificate/key fixture or call `C:\msys64\usr\bin\openssl.exe` explicitly in dev-only runs),
   `benchmark.py:60` `/proc/self/statm` (→ `GetProcessMemoryInfo`), `disk_index_acceptance.py:205` SIGKILL
   (→ `Popen.kill`).
5. **C GUI/data client:** send native, realpath'd absolute paths (§13); close staged import files before
   `desktop-media/import*`; enumerate removable drives for Rekordbox (§6.10); write returned CSV/M3U8/INI
   contents in binary; keep synthetic virtual-device ids (§6.1); `routing_graph.c` recorder folder rule ==
   §6.1; WASAPI mapping of `@DEFAULT_SINK@`/Linux device names.
6. **Build/packaging:** CTest `PYTHONPATH` separator (§16); tests on runtime\python; installer: Defender
   Firewall rule only if the replication listener binds a LAN address; recommend a Defender exclusion for
   `<root>\data` (performance + fewer sharing violations) and documenting NTFS + not-under-OneDrive for the root.

---------------------------------------------------------------------------------------------------

## 18. Defects fixed during the port (D2) and deletions (D2b)
Defects (fix and list in the implementation report):
1. `http_server._upload_media` cleanup exception masks a committed upload (500) — `unlink_quietly`.
2. `http_server._run` misclassifies `ConnectionAbortedError` → 500 attempt on a dead socket — catch `ConnectionError`.
3. `audio_playout` reports failure after publishing the WAV (dir fsync inside the try) — removed.
4. `desktop_transfer_package._export_media` `commonpath` `ValueError` and `\`-relative paths abort exports.
5. `desktop_transfer_package` protected-folder bypass (`database/SDBASE` mixed separators).
6. `desktop_transfer_package._safe_component` trailing-dot edge and Windows-invalid names written to disk.
7. `desktop_transfers` exact-byte contract broken by text-mode fds (sha mismatch / extra CSV rows).
8. `capacity_monitor` AttributeError (`os.statvfs`) → 500.
9. `libretime_adapter._verified_uri` broken path↔URI conversion.
10. Host-dependent absolute-path checks in persisted validators (`validate_library`, `settings`, `mairlist31_export`).
11. Non-canonical time-zone keys accepted on Windows (case-insensitive lookups).
12. `desktop_integrity` child import failure under `-E`; orphaned 4-hour scrub holding the store lock after stop.
13. `runtime_version.source_digest` order/selection differs from the C/tools rule on Windows.
14. `BoundedHTTPServer` allows a duplicate listener on the same port (SO_REUSEADDR) and does a reverse DNS
    lookup at start-up.
15. Credential files written with platform newlines (`\r\r\n` on text fds).
Deletions / candidates:
* Delete: `import fcntl` + `_fsync_directory` (`endpoint_journal.py`), `media._fsync_dir` and all directory
  fsync blocks listed in §15, every `getattr(os, "O_NOFOLLOW"/"O_CLOEXEC", 0)` (replaced by shim parameters),
  the `/proc/self/fd` reopen (`desktop_media.py:624`), `desktop_api.py:1474` `chmod(0o700)`,
  `PurePosixPath` import in `rekordbox_device.py:4`, `st_ctime_ns` identity fields.
* Candidate (verify, then delete or convert): `rekordbox.read_rekordbox_xml` (no caller in the repository).
* Keep (data compatibility, not Linux code): `LEGACY_KINDS`, `ONLINE_EVENT_KEYS`, legacy-posix acceptance in
  persisted validators, `radioboss_m3u8.absolute_media_path` POSIX branch (format rule).

---------------------------------------------------------------------------------------------------

## 19. Implementation order
1. (Engine) `_platform.py` core + `storage.py`/`integrity.py` without `fcntl` → `import sdbase` works.
2. This area: `_platform` additions (§2), `endpoint_journal.py`, `media.py`, `api_server.py`, `http_server.py`,
   `runtime_version.py`, tz validators — then the whole HTTP surface imports and serves.
3. `desktop_api.create_user`, `desktop_media`, `desktop_transfers`, `desktop_transfer_package`,
   `desktop_integrity`, `desktop_reset`, `desktop_broadcast`.
4. Rekordbox, `audio_playout`, `endpoint_media`, `endpoint`, `capacity_monitor`, `libretime_adapter`,
   `mairlist31_export`, `settings`.
5. Graceful stop (`StopRequest`, `serve`, CLI loops) + tools wiring (tools area) + tests (§16).

---------------------------------------------------------------------------------------------------

## 20. Risks / open questions
1. **Sharing violations beyond 2 s** (Defender full scans, backup agents, OneDrive) still surface as errors on
   replace/unlink; messages must be clear ("dosya başka bir işlem tarafından kullanılıyor; tekrar deneyin").
2. **`SO_EXCLUSIVEADDRUSE` re-bind window** after stop (§3.1) — covered by a test and a bounded bind retry.
3. **ACL semantics drift** between C and Python privacy checks would reject credentials/keys — the shim must
   implement C spec 5.2 literally; add the cross-language interop test.
4. **External/removable volumes** (FAT/exFAT) cannot hold private files: credential keys fail closed; packages
   proceed with a warning — confirm this UX with the lead.
5. **Legacy Linux data**: POSIX provenance/recorder folders/import profiles remain readable but unusable as
   paths; import profiles need re-selection (clear message). Linux `storage.ini` inside transfer packages must
   be rejected by the C side.
6. **Long paths**: deep album trees from transfer packages can exceed 260 chars when LongPathsEnabled is off;
   handled per song with a warning; recommend the installer enables LongPathsEnabled (admin) and the C exe
   manifest stays `longPathAware` (D4).
7. **tzdata vs Windows registry** differences between the Python scheduler and the C player (§12).
8. **Keep-alive drain** makes stop take up to `request_timeout` unless the socket-tracking improvement (§4.3)
   is implemented.
