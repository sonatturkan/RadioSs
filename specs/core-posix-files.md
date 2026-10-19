# Area spec: core-posix-files (RadioS Windows port)

Analysed tree: `C:\Users\sonatturkan\Desktop\RadioS` (read-only). Line numbers refer to that copy.
Target: Windows 10 1903+ (build 18362, needed for `activeCodePage=UTF-8`) / Windows 11, x64, MSYS2 UCRT64 GCC.

This document has two jobs:

1. Inventory every POSIX/Linux assumption in `src/core/*.c` (plus the core headers) with file:line, why
   the code does it, and the exact Windows replacement.
2. Define the shared C portability layer `include/radios/platform.h` + `src/core/platform.c`
   (+ `src/core/platform_win32.c`, `src/core/platform_posix.c`) that **every** other area must use.
   Section 3 is the binding contract. Section 6 maps every POSIX call site in `src/` and `tests/` to it.

---------------------------------------------------------------------------------------------------

## 0. TL;DR

* The core is written around **descriptor-relative, no-follow, private, atomically replaced files**
  (`open(O_DIRECTORY|O_NOFOLLOW)` + `fstatat/openat/renameat/unlinkat/mkdirat` + `fsync(dir)` +
  `flock` + `st_uid==getuid()` + `st_mode & 077`). None of `openat`, `fstatat`, `renameat`,
  `unlinkat`, `mkdirat`, `flock`, `realpath`, `lstat`, `statvfs`, `getuid`, `pread`, `pwrite`,
  `setpriority`, `syscall`, `O_NOFOLLOW`, `O_DIRECTORY`, `O_CLOEXEC`, `O_NONBLOCK`, `S_ISLNK`,
  `st_mtim/st_ctim`, `st_uid` exist in mingw-w64/UCRT. Every one of those lines is a compile error.
* The port keeps the **shape** of the code (plain `gint` CRT descriptors, errno-based error helpers)
  and swaps the calls for `radios_fs_*` / `radios_fd_*` functions. On Windows a "directory fd" is a
  CRT fd wrapping a directory `HANDLE` (`_open_osfhandle`), and `*at` operations are implemented with
  `NtCreateFile(RootDirectory=…)` and `SetFileInformationByHandle(FileRenameInfoEx/FileDispositionInfoEx)`,
  which gives real openat/renameat/unlinkat semantics (no TOCTOU path re-resolution).
* `mode 0600/0700` and `st_uid == getuid()` map to a **Windows "private" ACL definition** (owner =
  current user SID, protected DACL that grants only user/SYSTEM/Administrators). C and Python must
  implement the same check and the same creation SDDL (section 5).
* `flock` maps to `LockFileEx` on **one byte at offset 2^62**; C and Python must use the identical
  region (section 5).
* **Time zones are a critical, non-obvious blocker**: GLib on Windows cannot load IANA identifiers
  (`g_time_zone_new_identifier("Europe/Istanbul")` returns NULL). 22 call sites across core/playout/
  audio/ui must go through `radios_time_zone_new()`, backed by a bundled `share/zoneinfo` tree
  (the same tree Python's `zoneinfo` needs on Windows via `PYTHONTZPATH`).
* Path text: Windows paths are case-insensitive, use `\`, have drive roots `C:\` and UNC roots,
  8.3 short names (`%TEMP%` is `C:\Users\SONATT~1\…` on this machine!) and mapped/subst drives.
  All prefix/equality checks (the inside-check exists in 7 private copies: config.c, local_ini.c,
  preferences_storage.c, ui_preferences.c, storage_locations.c, media_archive.c, data_client.c) must use `radios_path_is_inside`
  / `radios_path_equal`. Project-relative paths that cross the C↔Python boundary must be compared in
  portable `/` form.

---------------------------------------------------------------------------------------------------

## 1. Windows facts that drive the design (read before implementing)

| # | Fact | Consequence |
|---|------|-------------|
| W1 | mingw-w64 has `<unistd.h>`, `<fcntl.h>`, `<sys/stat.h>` but **no** `<sys/file.h>`, `<sys/statvfs.h>`, `<sys/resource.h>`, `<sys/syscall.h>`, `<poll.h>`, `<glib-unix.h>` (GLib only installs it on UNIX), `<alsa/…>`. | Includes must be removed/guarded; functionality comes from platform.h. |
| W2 | `GStatBuf` on Win64 mingw is `struct _stat64`; `struct stat` is a different layout (32-bit `st_size` unless `_FILE_OFFSET_BITS=64`). Passing `struct stat *` to `g_stat/g_lstat` is an incompatible-pointer error (GCC 14 default error, and the project builds with `-Werror`). | `config.c:33,217,380` must stop using `struct stat` with `g_*stat`. Security-relevant stats use `RadioSFileInfo`. |
| W3 | `g_lstat` on Windows cannot report symlinks through `st_mode` (MSVCRT has no `S_IFLNK`; mingw has no `S_ISLNK`). `g_file_test(…, G_FILE_TEST_IS_SYMLINK)` is not a reliable junction detector. | Link detection only through `radios_fs_stat(…, follow_links=FALSE)` → `RADIOS_FILE_TYPE_LINK`. |
| W4 | "Links" on Windows are **reparse points**. Only *name-surrogate* tags (`IsReparseTagNameSurrogate(tag)`: `IO_REPARSE_TAG_SYMLINK`, `IO_REPARSE_TAG_MOUNT_POINT` = junction/volume mount, `IO_REPARSE_TAG_AF_UNIX`, WCI/…) behave like symlinks. Non-surrogate tags (OneDrive/Cloud Files `IO_REPARSE_TAG_CLOUD_*`, dedup, WOF/compressed, AppExecLink) are ordinary files/dirs and **must not** be rejected. | `RADIOS_FILE_TYPE_LINK` ⇔ reparse attribute && name-surrogate tag. No-follow opens must re-open non-surrogate reparse points normally (see 4.3). |
| W5 | Creating symlinks requires `SeCreateSymbolicLinkPrivilege` or Developer Mode (`SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`). **Junctions need no privilege** (any user with write access can create one). | Threat model still exists (junctions); tests that create *file* symlinks must skip when unprivileged; directory-link tests use junctions. |
| W6 | Opening a file normally (CRT `_wopen`, GLib `g_open`/`g_file_get_contents`, Python `open()`) does **not** pass `FILE_SHARE_DELETE`. While such a handle is open, renaming over / deleting that file fails with `ERROR_SHARING_VIOLATION`/`ERROR_ACCESS_DENIED`. AV scanners / Search indexer open fresh files briefly. | All platform opens use `FILE_SHARE_READ\|FILE_SHARE_WRITE\|FILE_SHARE_DELETE`. Replace/unlink use POSIX semantics flags **and** a bounded retry loop. |
| W7 | `FileRenameInfoEx` + `FILE_RENAME_FLAG_POSIX_SEMANTICS` (NTFS; Windows 10 RS1-era API, always present on the 1903+ minimum) replaces a target that others have open with `FILE_SHARE_DELETE`. `FileDispositionInfoEx` + `FILE_DISPOSITION_FLAG_POSIX_SEMANTICS` (NTFS, same era) removes the name immediately even if open. FAT/exFAT/older SMB reject these (`ERROR_INVALID_PARAMETER`/`ERROR_NOT_SUPPORTED`) → fall back to `FileRenameInfo(ReplaceIfExists)` / `FileDispositionInfo`. | Implemented inside `radios_fs_renameat/radios_fs_unlinkat/radios_fs_rename/radios_fs_unlink`. |
| W8 | Directories cannot be `fsync`ed portably; NTFS journals metadata. | `radios_fd_sync_dir()` is a successful no-op on Windows (lead decision). |
| W9 | No uid/mode bits. Ownership = owner SID; access = DACL. Elevated admins create objects owned by `BUILTIN\Administrators` unless an explicit owner is given. FAT/exFAT have no ACLs. | Section 5.2 "private" definition; always set explicit owner on creation. |
| W10 | LLP64: `long` and `off_t` are 32-bit. `lseek()`/`ftruncate()` take 32-bit offsets. | `radios_fd_seek` (`_lseeki64`), `radios_fd_truncate` (`_chsize_s`); no `off_t` casts (`ui/playlist.c:1757`). |
| W11 | CRT text mode converts `\n`↔`\r\n` and treats `^Z` as EOF. | Every platform-opened fd gets `_O_BINARY`; `radios_platform_init()` calls `_set_fmode(_O_BINARY)`; `RADIOS_O_BINARY` exists for `g_open`/`g_mkstemp_full`. |
| W12 | Handles/CRT fds are inheritable only if created inheritable. GLib's Windows spawn passes `bInheritHandles=TRUE`. | All platform handles are created **non-inheritable** (lock handles must not leak into the spawned Python service, or `gui.lock` would outlive the GUI). `RADIOS_O_CLOEXEC` = `_O_NOINHERIT`. |
| W13 | GLib on Windows: `g_time_zone_new_identifier()` only understands constant offsets ("UTC", "+03:00"), POSIX TZ rule strings, and **Windows registry zone names**. IANA names → NULL. `zone_info_unix()`/TZDIR is compiled for `G_OS_UNIX` only. | `radios_time_zone_new()` (section 3.11, 4.11) + bundled zoneinfo. |
| W14 | Paths: case-insensitive (ordinal, upper-case table), both `/` and `\` accepted by Win32 but `g_canonicalize_filename` outputs `\`; drive roots `C:\` (length 3, ends with separator); UNC roots `\\server\share`; drive-relative `C:foo` and rooted `\foo` are **not** absolute even though `g_path_is_absolute("\\foo")` returns TRUE on Windows; 8.3 names; reserved device names (`CON`, `NUL`, `COM1`…) and chars `<>:"\|?*`; trailing dots/spaces stripped; MAX_PATH 260 unless long paths are enabled. | `radios_path_*` helpers (3.9). |
| W15 | `GetFinalPathNameByHandleW` (the realpath equivalent) expands 8.3 names, resolves symlinks/junctions, resolves **subst** drives to the real drive and **mapped network drives to UNC** (`Z:\x` → `\\server\share\x`). | `radios_fs_realpath` semantics (4.6); tests must realpath their temp dirs. |
| W16 | Accessing an empty removable drive pops a "There is no disk in the drive" system dialog unless `SetErrorMode(SEM_FAILCRITICALERRORS)` is set. Storage locations on unplugged USB disks are an explicit RadioS use case. | `radios_platform_init()` sets it. |
| W17 | `access(…, X_OK)`: UCRT `_access` rejects mode 1 (`EINVAL`); mingw's wrapper strips it. `W_OK` on a directory only checks the read-only attribute, never the ACL. | `radios_fs_dir_check_access()` (probe-based). |
| W18 | No signals (`SIGTERM` → Python `send_signal` = `TerminateProcess`), no `/proc`. | Named-event stop protocol (section 5.3), `GetModuleFileNameW`, `CommandLineToArgvW`. |
| W19 | `g_build_filename()` on Windows uses the first separator found in its arguments; `RADIOS_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"` is `C:/…` (forward slashes) → all derived test paths use `/`. Literal multi-component arguments (`"database/SDBASE"`, `"data/database/RadioS.sdb"`) produce mixed separators. | Never compare paths with `strcmp`/`g_str_has_prefix`; never pass `"a/b"` as one `g_build_filename` element in code that compares paths. |

---------------------------------------------------------------------------------------------------

## 2. Inventory of `src/core` (file:line → what/why → Windows replacement)

Notation: **[C]** = compile error on mingw today, **[S]** = silent semantic bug on Windows, **[OK]** =
portable as is. Replacement names refer to section 3.

### 2.1 `src/core/config.c` (project root discovery, INI parsing, path confinement)

Contract (`include/radios/config.h`): `radios_project_root_find(explicit, exe, err)` returns the
*physical* root that contains `config/radios.example.ini` and `database/SDBASE/pyproject.toml`;
explicit invalid roots never fall back; else walk up from the physical executable dir, then from the
physical CWD. `radios_config_load()` never creates anything; every returned path is absolute,
resolved against the root and **physically** inside it (an existing prefix is resolved through links,
so a missing destination below a link cannot escape). `config/radios.ini` overrides
`config/radios.example.ini`; a dangling link there is an IO error, a link to outside is INVALID
(`tests/test_config.c:325-344`).

| Line(s) | Code | Status | Windows replacement |
|---|---|---|---|
| 1 | `#define _XOPEN_SOURCE 700` | OK | keep (harmless on mingw). |
| 11 | `#include <sys/stat.h>` | OK | keep for `S_ISDIR/S_ISREG` if GStatBuf retained; preferably drop. |
| 15-22 | `path_is_inside()` byte-prefix + `path[len]==G_DIR_SEPARATOR`, root special-case `length==1` (`/`) | [S] | `radios_path_is_inside(root, path)`. Windows: case-insensitive, accepts both separators, handles roots that end with a separator (`C:\` has length 3 → current code would reject every child of a drive root). |
| 29 | `g_canonicalize_filename(value, root)` | OK | Works on Windows (outputs `\`). Note: INI value `/tmp/x` is "absolute" on Windows → canonical `\tmp\x` → not inside → INVALID (test still passes). |
| 33, 41 | `struct stat info; g_lstat(existing,&info)` loop until exists; `errno != ENOENT` → IO error | [C] (W2) | `RadioSFileInfo info; radios_fs_stat(existing, FALSE, &info)`; same errno contract (ENOENT keeps walking up). A dangling link must *succeed* here (Windows: open with `FILE_FLAG_OPEN_REPARSE_POINT` succeeds on dangling links). |
| 49-55 | `g_path_get_dirname` until fixpoint | OK | `g_path_get_dirname("C:\\")` == `"C:\\"`; UNC reaches a fixpoint too. |
| 60, 133, 147, 157 | `realpath(x, NULL)` + `free()` (79, 138, 142, 152, 161) | [C] | `radios_fs_realpath(x, NULL)` + `g_free`. Windows: `GetFinalPathNameByHandleW` (4.6). Preserve: NULL when missing (ENOENT), relative input resolved against CWD (`test_cwd_independence`). |
| 66 | `path_is_inside(root, physical)` | [S] | `radios_path_is_inside`. |
| 71-74 | splice `physical + (normalized + strlen(existing))` | OK | pure string ops on same-origin strings. |
| 86-95 | root markers via `g_build_filename(root,"config","radios.example.ini")` + `g_file_test` | OK | keep (separate components). |
| 118-169 | root discovery from executable / CWD | OK after realpath swap | Caller (`app/main.c:138`) must pass `radios_process_executable_path()` instead of `/proc/self/exe`. Layout: `<root>\bin\radios.exe` or `<root>\build\…\radios.exe` both work (walk-up). |
| 217, 224-226 | `struct stat info; g_stat(resolved,&info)` + `S_ISDIR/S_ISREG` | [C] (W2) | `radios_fs_stat(resolved, TRUE, &info) == 0 && info.type != wanted` (or `GStatBuf`). |
| 346-368 | `load_ini_file`: `g_file_get_contents` (binary on Windows) + `;`→`#` + CRLF tolerant | OK | — |
| 380, 398 | `g_lstat(local_path,&info)`, `errno != ENOENT` | [C] | `radios_fs_stat(local_path, FALSE, &info)`. |
| 478 | `python_command` used verbatim as argv[0] by `ui/data.c:392`, `ui/admin_ui.c:697` | [S] | **New behaviour (both OSes, harmless on Linux):** after reading, if the value contains a separator and is relative, resolve it against `project_root` (`g_canonicalize_filename(value, root)`). On Windows the shipped `config/radios.example.ini` value `python3` does not exist (Store alias!) → Windows config must use `runtime/python/pythonw.exe` (GUI-launched helpers must use **pythonw.exe** so no console window flashes; GSubprocess pipes still work with pythonw). Recommended: keep `python3` in the shared example INI and have `config.c` map a bare `python3`/`python` on Windows to `<root>\runtime\python\pythonw.exe` when that file exists. Coordinate with the packaging/service area. |
| 252-260, 263-340 | env-name, host, URL validation | OK | — |

### 2.2 `src/core/runtime.c` — **[OK]**
GThread/GMutex/GCond/atomics only. Contract in `runtime.h` unchanged. `tests/test_runtime.c` is portable.

### 2.3 `src/core/local_ini.c` (workstation files in `<project>/config`)

Contract (`local_ini.h`): config dir must exist and be physically inside the physical project root;
file name: non-empty, not starting with `.`, no `/`, < 100 bytes; reads reject links/non-regular,
are bounded (≤ `limit` ≤ 1 MiB), must be NUL-free UTF-8; a missing file reads as `*contents=NULL`
+ TRUE; writes replace atomically with **0600**; remove of a missing file succeeds. Errors:
`G_IO_ERROR_INVALID_DATA` for rule violations, `g_io_error_from_errno(errno)` for I/O.

| Line(s) | Code | Status | Windows replacement |
|---|---|---|---|
| 27-30 | `name_valid`: no `/` | [S] | add `radios_path_component_valid(name)` (rejects `\`, `:`, `<>"\|?*`, control chars, trailing dot/space, reserved device names). Keep the `.`-prefix and `<100` rules. |
| 36, 39, 41 | `realpath(project_root)`, `realpath(<root>/config)`, `free` | [C] | `radios_fs_realpath` / `g_free`. |
| 42-44 | inline inside-check | [S] | `radios_path_is_inside(root, directory)`. |
| 47 | `open(directory, O_RDONLY\|O_DIRECTORY\|O_NOFOLLOW\|O_CLOEXEC)` | [C] | `radios_fs_open(directory, RADIOS_O_RDONLY\|RADIOS_O_DIRECTORY\|RADIOS_O_NOFOLLOW\|RADIOS_O_CLOEXEC, 0)`. Returns a directory fd (Windows: dir HANDLE wrapped by `_open_osfhandle`). |
| 63-67, 102-106, 123 | `fstatat(dir, name, &info, AT_SYMLINK_NOFOLLOW)`, `errno==ENOENT`, `S_ISREG` | [C] | `radios_fs_statat(dir, name, FALSE, &info)`; `info.type == RADIOS_FILE_TYPE_REGULAR`. ENOENT contract preserved. |
| 70 | `openat(dir, name, O_RDONLY\|O_NOFOLLOW\|O_CLOEXEC\|O_NONBLOCK)` | [C] | `radios_fs_openat(dir, name, RADIOS_O_RDONLY\|RADIOS_O_NOFOLLOW\|RADIOS_O_CLOEXEC\|RADIOS_O_NONBLOCK, 0)`. A link swapped in between statat and openat → ELOOP (same as Linux). |
| 73 | `fstat` + `S_ISREG` + `st_size` | [C] | `radios_fd_stat(fd, &info)`. |
| 80, 142 | `read()`/`write()` on the fd | OK | CRT `read`/`write` on binary fds. |
| 66, 71, 86, 105, 111, 137, 148, 153 | `close()` | OK | plain `close()` (not a locked fd). |
| 108 | `unlinkat(dir, name, 0)` | [C] | `radios_fs_unlinkat(dir, name, 0)` (POSIX-semantics delete, retry on sharing violation). |
| 110, 152 | `fsync(directory)` | [C] | `radios_fd_sync_dir(dir)` (Windows no-op). |
| 129-138 | temp `".%s-%08x-%08x.tmp"` via `openat(O_WRONLY\|O_CREAT\|O_EXCL\|O_NOFOLLOW\|O_CLOEXEC, 0600)`, retry on EEXIST | [C] | `radios_fs_openat(dir, temp, RADIOS_O_WRONLY\|RADIOS_O_CREAT\|RADIOS_O_EXCL\|RADIOS_O_NOFOLLOW\|RADIOS_O_CLOEXEC, 0600)` → created with the private security descriptor (5.2). |
| 147 | `fsync(descriptor)` | [C] | `radios_fd_sync(fd)` (`_commit` → `FlushFileBuffers`). |
| 149 | `renameat(dir, temp, dir, file_name)` | [C] | `radios_fs_renameat(dir, temp, dir, file_name)` (atomic replace with POSIX semantics + retry, 4.4). |
| 151 | `unlinkat(dir, temporary, 0)` on failure | [C] | `radios_fs_unlinkat`. |

Consumers: `storage_locations.c` (storage.ini, 16 KiB), `media_archive.c` (media-archive.ini, 64 KiB),
`library_analysis.c` (library-analysis.ini, 4 KiB). Python also reads `storage.ini`
(`tools/radios_database.py:182-206`, unescaping GKeyFile escapes — Windows paths contain `\`, which
`g_key_file_set_string` writes as `\\`; Python's `_keyfile_unescape` already handles it).

### 2.4 `src/core/preferences_storage.c` (config/audio-panel.ini, 8 KiB) and
### 2.5 `src/core/ui_preferences.c` (config/ui-preferences.ini, 8 KiB)

Both contain a private copy of local_ini's algorithm (config_directory + fstatat + openat + bounded
read + temp/EXCL/0600 + fsync + renameat + best-effort dir fsync). Contracts (headers): missing file
→ defaults without writing; invalid file → `out` unchanged; links (file or config dir escaping the
project) rejected with `G_IO_ERROR_INVALID_DATA`; save = atomic replacement with 0600.
Note: `radios_ui_preferences_load()` is also called by `app/main.c:150` for `--check-config`.

| preferences_storage.c | ui_preferences.c | Code | Replacement |
|---|---|---|---|
| 52-80 (54, 60 realpath; 67-69 inside; 76 open) | 33-70 (45, 51 realpath; 58 inside; 64 open) | config_directory | identical to local_ini (2.3). |
| 86 | 77 | fstatat NOFOLLOW | `radios_fs_statat(…, FALSE, …)` |
| 119 | 133 | openat RDONLY\|NOFOLLOW\|CLOEXEC\|NONBLOCK | `radios_fs_openat` |
| 123 | 138 | fstat | `radios_fd_stat` |
| 132 | 149 | read | OK |
| 172-177 | 225-231 | temp create 0600 EXCL NOFOLLOW | `radios_fs_openat(… 0600)` |
| 187 | 246 | write | OK |
| 192 | 255 | fsync(fd) | `radios_fd_sync` |
| 194 | 259 | renameat | `radios_fs_renameat` |
| 196 | 262 | unlinkat | `radios_fs_unlinkat` |
| 199 | 266 | fsync(dir) best effort | `radios_fd_sync_dir` |

Recommendation (optional, reduces 3 copies to 1): reimplement both on top of
`radios_local_ini_read/write` (names `audio-panel.ini`/`ui-preferences.ini` satisfy `name_valid`).
If done, keep the existing Turkish messages for the cases the tests assert (they assert domain/code,
`G_IO_ERROR_INVALID_DATA`, not text — verify in `tests/test_ui_preferences.c` /
`tests/test_preferences_storage.c` before changing messages).

### 2.6 `src/core/storage_locations.c` (Ayarlar › Medya arşivi › Radyo verileri)

Contract (`storage_locations.h`): folders are absolute, *simplified* (canonical text), UTF-8 without
control chars, not the root, exist, are writable, contain **no symbolic link anywhere in the path**,
are not inside the project's source/config folders, and (when required) carry the role marker
`.radios-storage.json` = `{"format":"radios-storage-v1","role":…,"created":…}` (0600, ≤4096 bytes).
A missing database folder is a hard error; recordings/backups fall back with a notice. This is
mirrored **byte-for-byte in Python** (`tools/radios_database.py:93-230`) — the Windows rules below
must be implemented identically there (section 5.5).

| Line(s) | Code | Status | Windows replacement |
|---|---|---|---|
| 18, 64 | `PATH_LIMIT 4000` bytes | [S] | keep 4000 bytes, **and** on Windows reject paths > 240 UTF-16 units unless long paths are enabled (`RtlAreLongPathsEnabled()`), leaving room for file names. |
| 68 | `g_path_is_absolute(directory)`; message "(/ ile başlamalı)" | [S] | `radios_path_is_absolute()` (Windows: `X:\…` or `\\server\share\…`; rejects `\foo`, `C:foo`, `\\?\…`). Message on Windows: «Klasör yolu mutlak olmalı (örn. D:\RadioS\Kayıtlar).» |
| 69-72 | `g_canonicalize_filename(dir,NULL)` must equal input | [S] | on Windows compare against `radios_path_normalize(dir)` (canonicalize + upper-case drive letter + no trailing separator). `C:/a`, `c:\a`, `C:\a\`, `C:\a\\b`, `C:\a\..\b` are all rejected (deterministic with Python `os.path.normpath` + drive-letter rule). Also run `radios_path_component_valid` on every component. |
| 73 | `g_str_equal(directory, "/")` | [S] | `radios_path_is_root(directory)` (also `C:\`, `\\server\share`). |
| 77-82 | `path_inside` | [S] | `radios_path_is_inside`. |
| 84-98 | `protected_location`: `realpath(project_root)`; `g_build_filename(root, "database/SDBASE")` | [C]+[S] | `radios_fs_realpath`; **split the component** (`g_build_filename(root, "database", "SDBASE", NULL)`) — the literal `"database/SDBASE"` produces a mixed-separator path on Windows and the prefix test never matches (protection bypass). Compare with `radios_path_is_inside`. |
| 106-131 | marker: `lstat` + `S_ISREG` + size; `g_file_get_contents` | [C] | `radios_fs_stat(path, FALSE, &info)`. |
| 139-143 | `lstat(directory)`; `S_ISLNK` → "sembolik bağlantı olamaz"; `S_ISDIR` | [C] | `radios_fs_stat(dir, FALSE, &info)`; `info.type == RADIOS_FILE_TYPE_LINK` (symlink **or junction**). |
| 144-147 | `realpath(dir)` must equal `dir` ("Klasör yolunda sembolik bağlantı var") | [C]+[S] | `p = radios_fs_realpath(dir)`; plain ⇔ `p && radios_path_equal(p, dir)`. On Windows this also rejects 8.3 spellings, subst drives and mapped drives (realpath returns the UNC form) — the UI (`ui/media_archive_ui.c:701`) realpaths the chooser result before calling `prepare`, so chosen folders always pass. |
| 148 | `access(dir, R_OK\|W_OK\|X_OK)` | [S] (W17) | `radios_fs_dir_check_access(dir, TRUE) == 0`. |
| 160-163 | `lstat==ENOENT` → `g_mkdir_with_parents(dir, recordings?0755:0700)` | [S] | `radios_fs_mkdir_with_parents(dir, mode)` (0700 ⇒ newly created components get the private DACL; 0755 ⇒ inherit). |
| 166 | `open(marker, O_WRONLY\|O_CREAT\|O_EXCL\|O_NOFOLLOW\|O_CLOEXEC, 0600)` | [C] | `radios_fs_open(marker, RADIOS_O_WRONLY\|RADIOS_O_CREAT\|RADIOS_O_EXCL\|RADIOS_O_NOFOLLOW\|RADIOS_O_CLOEXEC, 0600)`. |
| 181, 186-188 | write, `fsync`, `close`, `unlink` | [C] | write OK; `radios_fd_sync`; close; `radios_fs_unlink`. |
| 236-243 | GKeyFile writes the path (backslashes escaped) | OK | Python unescapes. |
| 298 | `g_canonicalize_filename(value, project_root)` | OK | — |
| 345-357 | `statvfs` → `f_bavail*f_frsize`, `f_blocks*f_frsize` | [C] | `radios_fs_disk_space(dir, &free, &total)` (`GetDiskFreeSpaceExW`: FreeBytesAvailableToCaller (quota-aware, = f_bavail), TotalNumberOfBytes). -1 on error (missing path, drive not ready). |
| 359-385 | `tree_walk`: `lstat`, `S_ISREG` size sum, recurse `S_ISDIR`, links not followed, depth 32, budget 2M | [C] | `radios_fs_stat(path, FALSE, &info)`; LINK (incl. junction) not followed; Cloud placeholders count with their logical size. |
| 396-397 | `%.1f` then `.`→`,` | OK | (locale: with a Turkish Windows locale `%.1f` already yields `,`; replacement is idempotent). |

### 2.7 `src/core/media_archive.c` (archive folders, scan + SHA-256 index)

Contract (`media_archive.h`): settings in `config/media-archive.ini` via local_ini; folders must be
absolute, canonical, distinct, not nested; scan walks without following links, **skips hidden
entries**, depth ≤ 16, ≤ 50000 files, sorted by path; digest reused from the index while size and
mtime (µs) match; index rewritten atomically (`G_FILE_SET_CONTENTS_CONSISTENT`, 0600).

| Line(s) | Code | Status | Windows replacement |
|---|---|---|---|
| 83-88, 101-103 | `path_inside`, `g_str_equal(folder, other)` | [S] | `radios_path_is_inside`, `radios_path_equal` (`C:\Müzik` vs `c:\müzik\x` are nested/duplicate on Windows). |
| 196-199 | `g_stat` + `S_ISDIR` (GStatBuf — correct type) ; `access(folder, R_OK\|X_OK)` | [S] | keep `g_stat`; replace access with `radios_fs_dir_check_access(folder, FALSE) == 0`. |
| 242-243 | `st_mtim.tv_sec/tv_nsec` | [C] | `info->mtime_ns / 1000` (µs) from `RadioSFileInfo` (Windows LastWriteTime, 100 ns resolution). |
| 254 | skip names starting with `.` | [S] | `radios_file_info_is_hidden(name, &info)`: also skips `FILE_ATTRIBUTE_HIDDEN\|FILE_ATTRIBUTE_SYSTEM` (`$RECYCLE.BIN`, `System Volume Information` at the root of every USB archive disk — otherwise each scan reports them as unreadable). Requires stat before the filter; keep the `.` test first for speed. |
| 261-264, 268, 274 | `g_lstat` + `S_ISLNK` (skipped_links++) + `S_ISDIR` + `S_ISREG` + `st_size` | [C] | `radios_fs_stat(path, FALSE, &info)`; `RADIOS_FILE_TYPE_LINK` → `skipped_links` (junctions count as links). |
| 284-285 | `g_stat` index file | OK | — |
| 340 | `g_mkdir_with_parents(dir, 0700)` | [S] | `radios_fs_mkdir_with_parents(dir, 0700)`. |
| 343 | `g_file_set_contents_full(…, CONSISTENT, 0600)` | [S] (W6) | GLib on Windows writes a temp and `MoveFileExW(REPLACE_EXISTING)` without retry and ignores the mode. Acceptable (index is a cache, failure costs a re-hash). Optional: replace with temp + `radios_fs_rename`. |
| 350 | `g_open(path, O_RDONLY\|O_NOFOLLOW\|O_CLOEXEC\|O_NONBLOCK, 0)` | [C] | `radios_fs_open(path, RADIOS_O_RDONLY\|RADIOS_O_NOFOLLOW\|RADIOS_O_CLOEXEC\|RADIOS_O_NONBLOCK, 0)`. |
| 353, 369-370 | `fstat` + `S_ISREG`, size/mtime unchanged after hashing | [C] | `radios_fd_stat`; compare `size` and `mtime_ns/1000`. |
| 362 | `read()` | OK | — |
| 417 | `g_lstat` + `S_ISREG` + size | [C] | `radios_fs_stat(…, FALSE, …)`. |
| 383-445 | `seen` hash keyed by path (case-sensitive) | OK | paths come from directory enumeration (stable case); duplicates across folders are prevented by the case-insensitive validation. |

### 2.8 `src/core/recovery.c` (durable private outbox + GUI lifecycle lock) — most POSIX-dense file

Contracts (`recovery.h` + tests): outbox dir `<root>/data/recovery/<store>/<user>`; every component
from `recovery` downward is **created 0700, opened with O_NOFOLLOW one component at a time from `/`,
must be owned by the current uid and have no group/other bits** ("no path-based reopen"); a per-user
`.lock` (0600, owner, nlink==1, regular) is held with `flock(LOCK_EX)` (blocking) around every
read-modify-write; entries `<uuid>.json` (owner, 0600, nlink==1, 2..6 MiB+16 KiB, strict JSON,
SHA-256 self-checksum) are written via temp `.write-<uuid>` + fsync + renameat + fsync(dir); at
most 512 entries; enumeration through `/proc/self/fd/<dirfd>`. `radios_recovery_gui_lock()` takes a
**shared, non-blocking** `flock` on `data/database-service/gui.lock` (dir must be owner-private) and
refuses if `restore-pending.json` exists; the fd is held for the GUI lifetime and closed by
`ui/window.c:725,755,1009`. The offline restore tool (`tools/radios_reliability.py:365-395`) takes the
**exclusive** lock on the same files (plus each outbox `.lock`) → C and Python lock regions must be
identical (5.1).

| Line(s) | Code | Status | Windows replacement |
|---|---|---|---|
| 1, 7 | `_GNU_SOURCE`, `<sys/file.h>` | [C] | drop `<sys/file.h>`; include `radios/platform.h`. |
| 137-138 | `g_strsplit(r->directory, "/")` + `open("/", O_RDONLY\|O_DIRECTORY\|O_CLOEXEC)` | [C]+[S] | `gchar **parts = NULL; gint dir = radios_fs_open_root(r->directory, &parts);` — returns the fd of the filesystem root (`/`, `C:\`, `\\server\share\`) and the remaining components split on **both** separators, empty components removed. Loop body stays the same. |
| 142-143 | `private` once a component named `recovery` is reached; create when `private` or next is `recovery` (i.e. also creates `data`) | OK | unchanged logic. |
| 144-145 | `mkdirat(dir, part, 0700)`; `fsync(dir)` after a creation | [C] | `radios_fs_mkdirat(dir, part, 0700)` (private SD with inheritable ACEs, explicit owner); `radios_fd_sync_dir(dir)`. |
| 147 | `openat(dir, part, O_RDONLY\|O_DIRECTORY\|O_NOFOLLOW\|O_CLOEXEC)` | [C] | `radios_fs_openat(dir, part, RADIOS_O_RDONLY\|RADIOS_O_DIRECTORY\|RADIOS_O_NOFOLLOW\|RADIOS_O_CLOEXEC, 0)`. |
| 149-151 | `fstat(next)`; `st_uid != getuid() \|\| st_mode & 077` | [C] | `!radios_fd_is_private(next, RADIOS_PRIVATE_OWNER\|RADIOS_PRIVATE_NO_OTHERS)`. |
| 155-156 | `errno` saved; ENOENT without create = "no outbox" (not an error) | OK | platform functions set errno (ENOENT for missing). |
| 161 | `openat(dir, ".lock", O_RDWR\|O_CREAT\|O_NOFOLLOW\|O_NONBLOCK\|O_CLOEXEC, 0600)` | [C] | `radios_fs_openat(dir, ".lock", RADIOS_O_RDWR\|RADIOS_O_CREAT\|RADIOS_O_NOFOLLOW\|RADIOS_O_NONBLOCK\|RADIOS_O_CLOEXEC, 0600)`. |
| 162-164 | `fstat`: regular, owner, `mode&077`, `st_nlink==1`; `flock(fd, LOCK_EX)` | [C] | `radios_fd_stat` (`type`, `nlink`) + `radios_fd_is_private(fd, OWNER\|NO_OTHERS)` + `radios_fd_lock(fd, RADIOS_LOCK_EX)` (blocking). |
| 172-177 | `openat(name, O_RDONLY\|O_NOFOLLOW\|O_NONBLOCK\|O_CLOEXEC)`; fstat checks incl. size bounds, nlink | [C] | same mapping. ENOENT → "absent" (not an error). |
| 181 | `read()` loop | OK | — |
| 222 | temp `.write-<uuid>` `openat(O_WRONLY\|O_CREAT\|O_EXCL\|O_NOFOLLOW\|O_CLOEXEC, 0600)` | [C] | `radios_fs_openat(… 0600)`. |
| 230 | `fsync(fd)` | [C] | `radios_fd_sync`. |
| 232 | `renameat(dir,temp,dir,name) == 0 && fsync(dir) == 0` | [C] | `radios_fs_renameat(...) == 0 && radios_fd_sync_dir(dir) == 0` (dir sync returns 0 on Windows). |
| 233 | `unlinkat(dir, temp, 0)` unconditionally (ENOENT after success ignored) | [C] | `radios_fs_unlinkat`. |
| 238, 316 | `g_dir_open("/proc/self/fd/%d")` | [C] | `GPtrArray *names = radios_fd_list_names(dir, error)` (owned UTF-8 names, no `.`/`..`). Order is irrelevant (entries are sorted by `created_us`, key). |
| 283 | `unlinkat(dir, name, 0) == 0 && fsync(dir) == 0` | [C] | `radios_fs_unlinkat` + `radios_fd_sync_dir`. |
| 360-362 | `open(project_root, O_RDONLY\|O_DIRECTORY\|O_NOFOLLOW\|O_CLOEXEC)` → `openat("data")` → `openat("database-service")` (all NOFOLLOW) | [C] | `radios_fs_open` + 2× `radios_fs_openat` (same flags). |
| 366 | dir `st_uid == getuid() && !(st_mode & 077)` | [C] | `radios_fd_is_private(dir, OWNER\|NO_OTHERS)`. The Python service creates this dir → it must create it private (5.2). |
| 367 | `openat(dir, "gui.lock", O_RDWR\|O_CREAT\|O_NOFOLLOW\|O_NONBLOCK\|O_CLOEXEC, 0600)` | [C] | `radios_fs_openat`. |
| 368-369 | regular, owner, private, nlink==1, `flock(fd, LOCK_SH\|LOCK_NB)` | [C] | `radios_fd_stat` + `radios_fd_is_private` + `radios_fd_lock(fd, RADIOS_LOCK_SH\|RADIOS_LOCK_NB)`. |
| 370 | `fstatat(dir,"restore-pending.json",AT_SYMLINK_NOFOLLOW)==0 \|\| errno != ENOENT` → refuse | [C] | `radios_fs_statat(dir, "restore-pending.json", FALSE, &info) == 0 \|\| errno != ENOENT`. |
| 373 | `G_IO_ERROR_BUSY` message | OK | — |
| return | caller owns a **locked fd** | — | callers must release with `radios_fd_close(fd)` (`ui/window.c:725,755,1009`). |

### 2.9 `src/core/data_client.c` (credentials + libcurl transport)

Contract (`data_client.h`): reads only the project-local **0600, owner-owned, regular, ≤64 KiB**
`config/credentials.json` (no links) and `<database>.meta`; the identity's `database_file` must equal
the database path relative to the root (or, for a storage.ini-moved database, the same basename);
HTTP only to loopback or HTTPS; no proxies/redirects.

| Line(s) | Code | Status | Windows replacement |
|---|---|---|---|
| 113-119 | `inside()` | [S] | `radios_path_is_inside`. |
| 202 | `open(path, O_RDONLY\|O_NOFOLLOW\|O_NONBLOCK\|O_CLOEXEC)` | [C] | `radios_fs_open(path, RADIOS_O_RDONLY\|RADIOS_O_NOFOLLOW\|RADIOS_O_NONBLOCK\|RADIOS_O_CLOEXEC, 0)`. |
| 208-210 | `fstat`; regular; size; private ⇒ `(st_mode & 0777) == 0600 && st_uid == getuid()` | [C] | `radios_fd_stat` + (private) `radios_fd_is_private(fd, RADIOS_PRIVATE_OWNER\|RADIOS_PRIVATE_NO_OTHERS\|RADIOS_PRIVATE_EXACT_FILE_MODE)`. Message "Kimlik dosyası kullanıcıya ait, 0600 izinli…" may stay (it is the user-facing rule). |
| 219 | `read()` | OK | — |
| 290-293 | `realpath` ×3 | [C] | `radios_fs_realpath` (+ `g_free` at 300, 309, 312, 376 instead of `free`). |
| 294-297 | `lstat(database_file)` + `S_ISREG` | [C] | `radios_fs_stat(…, FALSE, &info)`, `type == REGULAR`. |
| 324-325 | `relative = database + strlen(root) + (root == "/" ? 0 : 1)` compared with `stored_path` via `g_strcmp0` | [S] **critical** | Windows `relative` is `data\database\RadioS.sdb`; Python writes `str(Path.relative_to())` (backslashes on Windows, slashes on Linux). Compare separator-insensitively: `radios_path_equal(radios_path_from_portable(stored), relative)` (no extra API needed; `radios_path_equal` also unifies separators). Root special case → `radios_path_is_root(root)`. Coordinate: Python should write `.as_posix()` from now on (5.6). |
| 329-333 | basename comparison | OK | (case-sensitive is fine: both come from the same config). |
| 347 | `g_getenv(token_environment)` | OK | Windows env names are case-insensitive; harmless. |
| 581 | `curl_global_init(CURL_GLOBAL_DEFAULT)` | OK | includes Winsock init. |
| 611-635 | HTTPS options | [S] | when the service URL is `https://`, add `CURLOPT_SSL_OPTIONS = CURLSSLOPT_NATIVE_CA` on Windows (MSYS2 libcurl/OpenSSL otherwise looks for a CA bundle path that does not exist in the bundle). Loopback HTTP unaffected. |
| 896-918 | `installed_source_digest`: names ending exactly in `.py`, `strcmp` order, hash `name\0bytes\0` | OK in C | **Cross-area:** Python `runtime_version.py:13` sorts `Path` objects — on Windows `WindowsPath` ordering is case-insensitive and `glob("*.py")` is case-insensitive. Both sides must order by the exact byte string of the name and match the `.py` suffix case-sensitively; otherwise the digest diverges as soon as any file name contains an upper-case letter → `SCHEMA_MISMATCH` and the GUI refuses the service. Files are read raw on both sides (CRLF checkouts are fine). |

### 2.10 `src/core/library_analysis.c`

| Line(s) | Code | Status | Windows replacement |
|---|---|---|---|
| 1, 7-9 | `_GNU_SOURCE`, `<sys/resource.h>`, `<sys/syscall.h>`, `<unistd.h>` | [C] | remove; include platform.h. |
| 142-143 | `setpriority(PRIO_PROCESS, syscall(SYS_gettid), 10)` — Linux nice is per-thread; "analysis must never delay audio" | [C] | `radios_thread_lower_priority()`: Linux keeps the exact call (under `__linux__`), other POSIX no-op, Windows `SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL)`. Audio threads must be raised by the audio area (`AvSetMmThreadCharacteristicsW(L"Pro Audio")`), not by this function. |
| rest | settings via local_ini; GThread pool; atomics | OK | — |

### 2.11 `src/core/loudness_policy.c` and time zones

`radios_audio_loudness_policy_validate()` (`loudness_policy.c:18-45`) requires `timezone` to be a
**named zoneinfo identifier** "matching the service": not starting with `/`, no `//`, no `.`/`..`
component (split on `/`), `$TZDIR/<id>` (default `/usr/share/zoneinfo`) is a regular file, and
`g_time_zone_new_identifier(id)` succeeds. The engine then evaluates schedule windows in that zone
(`audio/engine.c:1195`, `radios_audio_loudness_policy_time_matches(day, minute)`).
`tests/test_loudness_policy.c:61` uses `"Europe/Istanbul"`.

| Line(s) | Code | Status | Windows replacement |
|---|---|---|---|
| 27-30 | syntactic checks split on `/` only | [S] security | on Windows also reject `\` and `:` anywhere (otherwise `..\..\Windows\win.ini` passes the `/`-split check and escapes TZDIR). Implemented in `radios_time_zone_identifier_exists()`. |
| 31-33 | `TZDIR` / `/usr/share/zoneinfo` + `g_file_test(IS_REGULAR)` | [S] | `radios_time_zone_identifier_exists(id)` (POSIX: identical logic; Windows: bundled tree, regular file without following links, `TZif` magic). |
| 34 | `g_time_zone_new_identifier(p->timezone)` | [S] **critical** (W13) | `radios_time_zone_new(p->timezone)`. |

All other `g_time_zone_new_identifier` call sites (must all migrate, NULL handling unchanged):
`core/weekly_programme_plan.c:145,176,391`, `core/automation_calendar_plan.c:158,1085,1825`,
`playout/automation_policy.c:80,298`, `audio/engine.c:1113,1195`, `ui/audio_ui.c:154`,
`ui/header_status.c:108`, `ui/header_navigation.c:108`, `ui/automation_calendar.c:132,2561`,
`ui/broadcast.c:610`, `ui/local_event_prepare.c:369`, `ui/instruments.c:363`,
`ui/player_rotation.c:256`, `ui/probability_editor.c:203`, `ui/schema_track_picker.c:101`.
(`ui/copyright_settings.c:77` uses `g_time_zone_new_local()` — fine on Windows.)
The station zone comes from the service (`data_client.c:773-774`, interned string, default `"UTC"`).

### 2.12 `src/core/diagnostics.c` — **[OK]** (`g_file_test` only).
### 2.13 `src/core/external_access.c` — curl HTTPS probe. When `ca_file == NULL` on Windows set
`CURLOPT_SSL_OPTIONS = CURLSSLOPT_NATIVE_CA` (line 64 region), otherwise identical. `tests/test_external_access.c` uses `file:///etc/passwd` only as an *invalid URL string* — portable.

### 2.14 Pure-logic files — **[OK]** apart from time zones
`automation_calendar_plan.c` (181 KB; only `<stdio.h>` for `sscanf("%u-%u-%u")`, GDateTime/GTimeZone),
`automation_scope.c`, `programme_model.c`, `player_topology.c`, `silence_detector.c`,
`transition_preset.c`, `weekly_programme_plan.c` (`sscanf`, GTimeZone). No file, process or OS API.
`sscanf`/`g_strdup_printf("%04d…")` are locale-independent for integers. `routing_graph.c`:

| Line(s) | Code | Windows replacement |
|---|---|---|
| 97 | recorder folder must satisfy `g_path_is_absolute` | `radios_path_is_absolute` (rejects `\foo`). |
| 99-101 | prefix: no `/`, no `\`, not starting with `.` | additionally `radios_path_component_valid(prefix)` (rejects `:`, `*`, `?`, `"`, `<`, `>`, `\|`, reserved names) — the prefix becomes a file name in `audio/patchbay.c:380`. |
| 126-150 | virtual devices are named `radios_virtual_<id>`, monitors `<sink>.monitor`, mics `<sink>_mic` (PulseAudio naming persisted in routing documents) | **Audio area contract**: the WASAPI backend must accept these synthetic ids (or migrate stored graphs). Not a platform.h concern. |

### 2.15 `src/core/midi.c` (non-ALSA bits only)
Portable: `radios_midi_event_key`, `radios_midi_key_valid`, `radios_midi_key_label`, the
`Listener`/`Delivery` ref-counting and `queue()` (idle source on the owner's GMainContext),
`notify()`, `lost()`, `translate()` semantics. ALSA-specific (move to `midi_alsa.c`, provide
`midi_winmm.c`): `snd_seq_*`, port ids `"client:port"` (`sscanf` at 211), the self-pipe wake
(`pipe()` 218, `write(wake[1])` 247, `close` 250-251) and `poll()` 184 with 1 s liveness. WinMM needs
no fd plumbing: `midiInOpen(CALLBACK_FUNCTION)` + `CreateEventW` for shutdown. **platform.h provides
nothing for MIDI** (pipe/poll are not part of the contract).

---------------------------------------------------------------------------------------------------

## 3. The contract: `include/radios/platform.h`

Rules for all implementers:

* Include `radios/platform.h` instead of `<unistd.h>/<sys/*.h>` in any file that touches the
  filesystem, processes or time zones. **Never include `<windows.h>` from a public header**; Win32
  types appear only in `src/core/platform_win32.c` (and in backend files such as the WASAPI/WinMM
  backends). `<windows.h>` defines `DELETE`, `ERROR`, `small`, `interface`, `min`, `max`, … .
* Use `G_OS_WIN32` for conditionals (from `glibconfig.h`).
* Error convention:
  * Functions returning `gint`/`gssize`/`gint64` behave like POSIX: `>= 0` success, `-1` failure with
    **`errno` set** (Windows: mapped from `GetLastError()`/NTSTATUS by
    `radios_platform_errno_from_win32`). They never take a `GError`. Existing helpers such as
    `io_error(error, …)` that read `errno` and call `g_io_error_from_errno(errno)`/`g_strerror(errno)`
    keep working unchanged.
  * Functions returning `gboolean`/pointers with a `GError **error` set `G_IO_ERROR` /
    `g_io_error_from_errno(errno)` and a message `"<path>: <g_strerror>"`, and also leave `errno`
    set. `radios_platform_set_io_error()` is the shared builder.
  * Lock contention reports `EWOULDBLOCK` on Windows (UCRT value 140) and `EWOULDBLOCK`/`EAGAIN`
    (same value) on Linux; callers must test `errno == EWOULDBLOCK || errno == EAGAIN`.
  * A no-follow open of a link fails with `ELOOP` (UCRT 114; `g_io_error_from_errno` →
    `G_IO_ERROR_TOO_MANY_LINKS` on both OSes).
* All functions are thread-safe. Descriptors are never shared between threads concurrently except
  where noted.
* All strings are UTF-8. Returned strings are `g_free()`d, string vectors `g_strfreev()`d.

```c
/* include/radios/platform.h — RadioS portability layer (POSIX + Win32).
 * See docs/specs core-posix-files.md for the full semantics. */
#ifndef RADIOS_PLATFORM_H
#define RADIOS_PLATFORM_H

#include <glib.h>
#include <fcntl.h>
#ifndef G_OS_WIN32
# include <unistd.h>   /* read, write, close */
#else
# include <io.h>       /* read, write, close (CRT old names), _get_osfhandle */
#endif

G_BEGIN_DECLS

/* ---------------------------------------------------------------- 3.1 init */

/* Idempotent, thread-safe (g_once). Call first thing in main() of radios.exe and of
 * every test (tests/support does it). Every platform function also calls it lazily.
 * Windows: SetErrorMode(GetErrorMode()|SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX),
 * _set_fmode(_O_BINARY), resolves the zoneinfo directory (3.11), caches the token-user SID,
 * resolves ntdll entry points. POSIX: nothing besides the zoneinfo directory. */
void radios_platform_init(void);

/* ---------------------------------------------------------------- 3.2 errors */

/* Sets *error (if error != NULL) to G_IO_ERROR / g_io_error_from_errno(saved_errno) with
 * message "<formatted prefix>: <g_strerror(saved_errno)>", restores errno = saved_errno,
 * returns FALSE. */
gboolean radios_platform_set_io_error(GError **error, gint saved_errno,
                                      const gchar *format, ...) G_GNUC_PRINTF(3, 4);
#ifdef G_OS_WIN32
/* DWORD Win32 error -> errno. Exported for backend code (WASAPI, WinMM, spawn). */
gint radios_platform_errno_from_win32(guint32 win32_error);
#endif

/* ---------------------------------------------------------------- 3.3 open flags */

#ifdef G_OS_WIN32
# define RADIOS_O_RDONLY    0x0000      /* == _O_RDONLY  */
# define RADIOS_O_WRONLY    0x0001      /* == _O_WRONLY  */
# define RADIOS_O_RDWR      0x0002      /* == _O_RDWR    */
# define RADIOS_O_APPEND    0x0008      /* == _O_APPEND  */
# define RADIOS_O_CLOEXEC   0x0080      /* == _O_NOINHERIT */
# define RADIOS_O_CREAT     0x0100      /* == _O_CREAT   */
# define RADIOS_O_TRUNC     0x0200      /* == _O_TRUNC   */
# define RADIOS_O_EXCL      0x0400      /* == _O_EXCL    */
# define RADIOS_O_BINARY    0x8000      /* == _O_BINARY  */
# define RADIOS_O_NONBLOCK  0           /* no FIFOs on Windows */
# define RADIOS_O_NOFOLLOW  0x00100000  /* radios_fs_* only, never pass to g_open */
# define RADIOS_O_DIRECTORY 0x00200000  /* radios_fs_* only, never pass to g_open */
# define RADIOS_AT_FDCWD    (-100)
# define RADIOS_AT_REMOVEDIR 0x200
#else
/* The project builds with -std=c11 (CMAKE_C_EXTENSIONS OFF); files that include this header
 * without _POSIX_C_SOURCE>=200809L/_XOPEN_SOURCE/_GNU_SOURCE do not see O_CLOEXEC, O_NOFOLLOW,
 * O_DIRECTORY or AT_FDCWD from glibc. glibc always defines the __O_* aliases, and AT_FDCWD /
 * AT_REMOVEDIR have the same value on every Linux architecture. */
# define RADIOS_O_RDONLY    O_RDONLY
# define RADIOS_O_WRONLY    O_WRONLY
# define RADIOS_O_RDWR      O_RDWR
# define RADIOS_O_APPEND    O_APPEND
# define RADIOS_O_CREAT     O_CREAT
# define RADIOS_O_TRUNC     O_TRUNC
# define RADIOS_O_EXCL      O_EXCL
# define RADIOS_O_BINARY    0
# define RADIOS_O_NONBLOCK  O_NONBLOCK
# ifdef O_CLOEXEC
#  define RADIOS_O_CLOEXEC   O_CLOEXEC
#  define RADIOS_O_NOFOLLOW  O_NOFOLLOW
#  define RADIOS_O_DIRECTORY O_DIRECTORY
# else
#  define RADIOS_O_CLOEXEC   __O_CLOEXEC
#  define RADIOS_O_NOFOLLOW  __O_NOFOLLOW
#  define RADIOS_O_DIRECTORY __O_DIRECTORY
# endif
# define RADIOS_AT_FDCWD    (-100)   /* == AT_FDCWD on Linux */
# define RADIOS_AT_REMOVEDIR 0x200   /* == AT_REMOVEDIR on Linux */
#endif
#define RADIOS_O_ACCMODE (RADIOS_O_RDONLY | RADIOS_O_WRONLY | RADIOS_O_RDWR)
/* g_open()/g_mkstemp_full() may receive RDONLY/WRONLY/RDWR/APPEND/CLOEXEC/CREAT/TRUNC/EXCL/
 * BINARY only. radios_fs_* functions accept every RADIOS_O_* flag and ALWAYS open in binary,
 * non-inheritable mode on Windows (RADIOS_O_BINARY/RADIOS_O_CLOEXEC are implied there). */

/* ---------------------------------------------------------------- 3.4 file info */

typedef enum {
    RADIOS_FILE_TYPE_UNKNOWN = 0,
    RADIOS_FILE_TYPE_REGULAR,
    RADIOS_FILE_TYPE_DIRECTORY,
    RADIOS_FILE_TYPE_LINK,   /* POSIX symlink; Windows name-surrogate reparse point
                                (symlink, junction/mount point) — only when not followed */
    RADIOS_FILE_TYPE_OTHER   /* FIFO, socket, device, pipe/console handle */
} RadioSFileType;

typedef struct {
    RadioSFileType type;
    guint64 device;       /* st_dev | volume serial number (FILE_ID_INFO / BY_HANDLE_...) */
    guint64 inode;        /* st_ino | low 64 bits of the file id */
    guint64 inode_high;   /* 0      | high 64 bits of FILE_ID_128 (ReFS), else 0 */
    gint64  size;         /* bytes; 0 for directories on Windows */
    gint64  mtime_ns;     /* ns since 1970-01-01 UTC: st_mtim | LastWriteTime */
    gint64  ctime_ns;     /* st_ctim | FILE_BASIC_INFO.ChangeTime */
    guint32 nlink;        /* st_nlink | nNumberOfLinks */
    guint32 attributes;   /* 0 on POSIX | FILE_ATTRIBUTE_* */
    guint32 reparse_tag;  /* 0 on POSIX | reparse tag when FILE_ATTRIBUTE_REPARSE_POINT */
} RadioSFileInfo;

/* device, inode, inode_high, size, mtime_ns and ctime_ns all equal (identity + content
 * unchanged). Replaces the static same_file() helpers. */
gboolean radios_file_info_same(const RadioSFileInfo *a, const RadioSFileInfo *b);
/* name starts with '.'; on Windows also FILE_ATTRIBUTE_HIDDEN or FILE_ATTRIBUTE_SYSTEM. */
gboolean radios_file_info_is_hidden(const gchar *name, const RadioSFileInfo *info);

/* ---------------------------------------------------------------- 3.5 path-level fs */

/* stat/lstat. follow_links=FALSE reports the link itself (dangling links succeed). */
gint  radios_fs_stat(const gchar *path, gboolean follow_links, RadioSFileInfo *info);
/* open(2). RADIOS_O_NOFOLLOW applies to the last component only (ELOOP on a link).
 * mode: with RADIOS_O_CREAT, (mode & 077) == 0 creates a *private* object (5.2);
 * otherwise the object inherits its parent's ACL (Windows) / umask applies (POSIX). */
gint  radios_fs_open(const gchar *path, gint flags, guint mode);
/* unlink(2) of a file or a link (never follows). Windows: POSIX-semantics delete,
 * works while others hold the file open with FILE_SHARE_DELETE, retried on sharing
 * violations (4.4). Directories -> EISDIR (use radios_fs_unlinkat(.., REMOVEDIR)). */
gint  radios_fs_unlink(const gchar *path);
/* rename(2) with replace semantics; same retry/fallback rules as radios_fs_renameat. */
gint  radios_fs_rename(const gchar *from, const gchar *to);
/* g_mkdir_with_parents() equivalent; components created by this call get a private
 * descriptor when (mode & 077) == 0. Existing components may be links (followed). */
gint  radios_fs_mkdir_with_parents(const gchar *path, guint mode);
/* realpath(3): absolute, all links resolved, no "." "..", no trailing separator except
 * a root. Windows: long names (8.3 expanded), on-disk case, upper-case drive letter,
 * "\\?\" stripped, subst drives resolved, mapped drives returned as UNC. NULL + errno
 * (ENOENT when any component is missing). */
gchar *radios_fs_realpath(const gchar *path, GError **error);
/* access(dir, R_OK|X_OK[|W_OK]) for directories, ACL-accurate on Windows (4.8). 0/-1. */
gint  radios_fs_dir_check_access(const gchar *directory, gboolean need_write);
/* statvfs: free bytes available to this user and total bytes of the volume holding
 * directory. FALSE (+errno) when unknown; outputs untouched then. */
gboolean radios_fs_disk_space(const gchar *directory, gint64 *free_bytes, gint64 *total_bytes);

/* ---------------------------------------------------------------- 3.6 descriptor-relative fs */

/* Opens the filesystem root of an *absolute* path ("/", "C:\", "\\server\share\") as a
 * directory fd and returns the remaining components (split on '/' and, on Windows, '\',
 * empty components removed; "." or ".." component -> EINVAL). -1 + errno on failure. */
gint  radios_fs_open_root(const gchar *absolute_path, gchar ***components);
/* Opens absolute_path walking every component with no-follow semantics from the root
 * (directories with RADIOS_O_DIRECTORY|RADIOS_O_NOFOLLOW, the last with
 * flags|RADIOS_O_NOFOLLOW). Equivalent to decoder.c open_local()/analyzer.c open_local_path(). */
gint  radios_fs_open_nofollow_path(const gchar *absolute_path, gint flags, guint mode);
/* openat(2). dirfd may be RADIOS_AT_FDCWD (then name is any path). With a real dirfd,
 * name MUST be a single component (no separators, not "." or ".."), else EINVAL. */
gint  radios_fs_openat(gint dirfd, const gchar *name, gint flags, guint mode);
/* fstatat(2); follow_links=FALSE == AT_SYMLINK_NOFOLLOW. */
gint  radios_fs_statat(gint dirfd, const gchar *name, gboolean follow_links, RadioSFileInfo *info);
/* mkdirat(2); (mode & 077) == 0 -> private directory whose ACEs are inherited by children. */
gint  radios_fs_mkdirat(gint dirfd, const gchar *name, guint mode);
/* unlinkat(2); flags 0 or RADIOS_AT_REMOVEDIR (empty directory or directory link/junction). */
gint  radios_fs_unlinkat(gint dirfd, const gchar *name, gint flags);
/* renameat(2) with atomic replacement of an existing regular file / link at newname
 * (never replaces a directory: EISDIR / ENOTEMPTY). Windows: FileRenameInfoEx
 * (REPLACE_IF_EXISTS|POSIX_SEMANTICS|IGNORE_READONLY_ATTRIBUTE) relative to newdirfd,
 * fallback FileRenameInfo, bounded retry on sharing violations (4.4). */
gint  radios_fs_renameat(gint olddirfd, const gchar *oldname, gint newdirfd, const gchar *newname);

/* ---------------------------------------------------------------- 3.7 descriptor ops */

gint     radios_fd_stat(gint fd, RadioSFileInfo *info);                 /* fstat(2) */
/* Names in a directory fd (readdir via /proc/self/fd replacement); excludes "." and "..";
 * order unspecified. Must not run concurrently on the same dirfd. */
GPtrArray *radios_fd_list_names(gint dirfd, GError **error);           /* element: gchar* */
gssize   radios_fd_pread(gint fd, gpointer buffer, gsize count, gint64 offset);
gssize   radios_fd_pwrite(gint fd, gconstpointer buffer, gsize count, gint64 offset);
gint64   radios_fd_seek(gint fd, gint64 offset, gint whence);           /* lseek, 64-bit */
gint     radios_fd_truncate(gint fd, gint64 length);                    /* ftruncate, 64-bit */
gint     radios_fd_sync(gint fd);                                       /* fsync / FlushFileBuffers */
gint     radios_fd_sync_dir(gint dirfd);                                /* fsync(dir) / 0 on Windows */
/* close(2); releases radios_fd_lock locks first (best effort). Use for every fd that was
 * passed to radios_fd_lock; plain close() is fine elsewhere. */
gint     radios_fd_close(gint fd);

/* flock(2) emulation. operation: RADIOS_LOCK_SH | RADIOS_LOCK_EX | RADIOS_LOCK_UN,
 * optionally | RADIOS_LOCK_NB. Locks belong to the open file description (POSIX) /
 * handle (Windows): two fds of the same process conflict exactly like two processes.
 * Converting SH<->EX is not atomic (as with Linux flock). Contention with NB: -1, EWOULDBLOCK.
 * Windows: LockFileEx/UnlockFileEx on the byte range [RADIOS_LOCK_REGION_OFFSET, +1). */
#define RADIOS_LOCK_SH 1
#define RADIOS_LOCK_EX 2
#define RADIOS_LOCK_NB 4
#define RADIOS_LOCK_UN 8
#define RADIOS_LOCK_REGION_OFFSET G_GUINT64_CONSTANT(0x4000000000000000)
#define RADIOS_LOCK_REGION_LENGTH 1
gint     radios_fd_lock(gint fd, gint operation);

/* ---------------------------------------------------------------- 3.8 privacy (mode/uid) */

typedef enum {
    RADIOS_PRIVATE_OWNER           = 1 << 0, /* st_uid == getuid()           | owner SID check  */
    RADIOS_PRIVATE_NO_OTHERS       = 1 << 1, /* (st_mode & 077) == 0          | DACL check (5.2) */
    RADIOS_PRIVATE_EXACT_FILE_MODE = 1 << 2  /* (st_mode & 0777) == 0600, regular file | NO_OTHERS + regular */
} RadioSPrivateCheck;
/* TRUE when every requested check holds. FALSE with errno = EACCES when a check fails,
 * ENOTSUP when the volume has no persistent ACLs (FAT/exFAT; fail closed), or the errno
 * of a failed query. */
gboolean radios_fd_is_private(gint fd, guint checks);
gboolean radios_fs_path_is_private(const gchar *path, gboolean follow_links, guint checks);
/* fchmod(fd, 0600 file / 0700 dir) | protected owner-only DACL + explicit owner.
 * No-op success when already private. */
gint     radios_fd_make_private(gint fd);
gboolean radios_fs_path_make_private(const gchar *path, GError **error);

/* ---------------------------------------------------------------- 3.9 path strings (no I/O) */

/* POSIX: starts with '/'. Windows: "X:\..." / "X:/..." or "\\server\share[\...]";
 * FALSE for "\foo", "C:foo", "\\?\..." and "\\.\..." device paths. */
gboolean radios_path_is_absolute(const gchar *path);
/* "/" | "X:\" (or "X:/"), "\\server\share" with or without trailing separator. */
gboolean radios_path_is_root(const gchar *path);
/* Equality after separator unification; Windows ordinal case-insensitive
 * (CompareStringOrdinal(..., TRUE)), POSIX byte equality. Neither touches the disk. */
gboolean radios_path_equal(const gchar *a, const gchar *b);
/* path == root or path is below root (component boundary), same comparison rules;
 * roots ending with a separator ("/", "C:\") handled. Replaces every path_inside(). */
gboolean radios_path_is_inside(const gchar *root, const gchar *path);
/* g_canonicalize_filename(path, NULL) plus, on Windows: '/' -> '\', upper-case drive
 * letter, "\\?\" / "\\?\UNC\" prefixes removed, trailing separator removed except roots. */
gchar   *radios_path_normalize(const gchar *path);
/* A single file/dir name that is valid on this OS: non-empty, not "." or "..", no '/',
 * no NUL/control chars; Windows additionally no '\' ':' '*' '?' '"' '<' '>' '|', no trailing
 * '.' or ' ', not a reserved device name (CON PRN AUX NUL COM0-9 LPT0-9, COM¹²³ LPT¹²³,
 * case-insensitive, with or without extension), <= 255 UTF-16 units. */
gboolean radios_path_component_valid(const gchar *name);
/* Splits a relative path on '/' (and '\' on Windows), dropping empty components. */
gchar  **radios_path_split(const gchar *relative);
/* Wire form for project-relative paths exchanged with SDBASE: separators -> '/'. */
gchar   *radios_path_to_portable(const gchar *relative);
/* Inverse: '/' -> G_DIR_SEPARATOR. */
gchar   *radios_path_from_portable(const gchar *relative);

/* ---------------------------------------------------------------- 3.10 process & threads */

/* /proc/self/exe | GetModuleFileNameW(NULL). Absolute UTF-8; NULL + error on failure. */
gchar   *radios_process_executable_path(GError **error);
/* main() stores the original argv (before g_option_context_parse modifies it). */
void     radios_process_remember_argv(gint argc, gchar **argv);
/* Copy of the remembered argv; fallback /proc/self/cmdline (Linux) or
 * CommandLineToArgvW(GetCommandLineW()) (Windows). */
gchar  **radios_process_argv(void);
guint32  radios_process_id(void);
/* Graceful-termination requests delivered on the default GMainContext (callback returns
 * G_SOURCE_CONTINUE/REMOVE like g_unix_signal_add). POSIX: SIGTERM and SIGINT.
 * Windows: named event "Local\RadioS-stop-<pid>" (5.3) plus console CTRL_C/CTRL_BREAK/
 * CTRL_CLOSE/LOGOFF/SHUTDOWN events. Returns a source id for radios_process_remove_...(). */
guint    radios_process_add_termination_watch(GSourceFunc callback, gpointer data,
                                              GDestroyNotify notify);
void     radios_process_remove_termination_watch(guint id);
/* Ask another RadioS process to terminate gracefully: kill(pid, SIGTERM) | SetEvent on
 * "Local\RadioS-stop-<pid>". FALSE + G_IO_ERROR_NOT_FOUND when no such channel. */
gboolean radios_process_request_stop(guint32 pid, GError **error);
/* Start a program that outlives this process, without console window and without
 * inheriting handles. POSIX: g_spawn_async + setsid() child setup, stdio to /dev/null.
 * Windows: CreateProcessW(DETACHED_PROCESS|CREATE_NEW_PROCESS_GROUP|CREATE_NO_WINDOW|
 * CREATE_UNICODE_ENVIRONMENT[|CREATE_BREAKAWAY_FROM_JOB], bInheritHandles=FALSE). */
gboolean radios_process_spawn_detached(const gchar *const *argv, const gchar *working_directory,
                                       GError **error);
/* Lower the calling thread below the audio engine (Linux: nice +10 for this tid;
 * Windows: THREAD_PRIORITY_BELOW_NORMAL). Never fails visibly. */
void     radios_thread_lower_priority(void);

/* ---------------------------------------------------------------- 3.11 time zones */

/* $TZDIR when set and non-empty; else POSIX "/usr/share/zoneinfo", Windows
 * "<install>\share\zoneinfo" (install = g_win32_get_package_installation_directory_of_module(NULL)),
 * falling back to "<exe dir>\share\zoneinfo". Never NULL; may not exist. */
const gchar *radios_time_zone_directory(void);
/* Named IANA identifier (Area/Location) present in the zoneinfo directory as a regular
 * file (not followed through links): rejects leading '/', "//", "." / ".." components,
 * and on Windows '\' and ':'; "UTC" always TRUE. Exactly loudness_policy.c's rule. */
gboolean radios_time_zone_identifier_exists(const gchar *identifier);
/* Drop-in replacement for g_time_zone_new_identifier(): NULL for unknown identifiers,
 * new reference otherwise. POSIX: g_time_zone_new_identifier(). Windows: "UTC" ->
 * g_time_zone_new_utc(); IANA id -> TZif footer (POSIX TZ string) of the bundled file ->
 * g_time_zone_new_identifier(footer); cached. Windows registry names are NOT accepted. */
GTimeZone *radios_time_zone_new(const gchar *identifier);

/* ---------------------------------------------------------------- 3.12 test support */

/* symlink(2) for tests. Windows: CreateSymbolicLinkW(ALLOW_UNPRIVILEGED_CREATE
 * [|DIRECTORY]); for directories without the privilege a junction is created instead
 * (target made absolute). File links without privilege: -1, *error =
 * G_IO_ERROR_NOT_SUPPORTED -> the test calls g_test_skip(). */
gint     radios_platform_test_symlink(const gchar *target, const gchar *link_path,
                                      gboolean directory, GError **error);
/* chmod(path, 0644) for tests: grants BUILTIN\Users read on Windows. */
gboolean radios_platform_test_make_public(const gchar *path, GError **error);

G_END_DECLS
#endif /* RADIOS_PLATFORM_H */
```

Source layout: `src/core/platform.c` (portable parts: path strings, file-info helpers, error helper,
time-zone validation/cache), `src/core/platform_posix.c` (`#ifndef G_OS_WIN32`),
`src/core/platform_win32.c` (`#ifdef G_OS_WIN32`). All three go into `radios_core`.

---------------------------------------------------------------------------------------------------

## 4. Windows implementation guide (`platform_win32.c`)

Build with `-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN`; call explicit `…W` APIs (do
not define `UNICODE`). Link: `ntdll` (or resolve `NtCreateFile`, `RtlNtStatusToDosError`,
`RtlAreLongPathsEnabled` with `GetProcAddress(GetModuleHandleW(L"ntdll.dll"), …)`), `advapi32`
(security), `shell32` (`CommandLineToArgvW`). If mingw headers lack them, define locally:
`FileRenameInfoEx = 22`, `FileDispositionInfoEx = 21`, `FILE_RENAME_FLAG_REPLACE_IF_EXISTS 0x1`,
`FILE_RENAME_FLAG_POSIX_SEMANTICS 0x2`, `FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE 0x40`,
`FILE_DISPOSITION_FLAG_DELETE 0x1`, `FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x2`,
`FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE 0x10`, NT create options `FILE_DIRECTORY_FILE 0x1`,
`FILE_SYNCHRONOUS_IO_NONALERT 0x20`, `FILE_NON_DIRECTORY_FILE 0x40`, `FILE_OPEN_FOR_BACKUP_INTENT
0x4000`, `FILE_OPEN_REPARSE_POINT 0x200000`, dispositions `FILE_OPEN 1, FILE_CREATE 2, FILE_OPEN_IF 3,
FILE_OVERWRITE 4, FILE_OVERWRITE_IF 5`, `OBJ_CASE_INSENSITIVE 0x40`.

### 4.1 Descriptor model
* Every fd returned by `radios_fs_*` is a CRT fd from `_open_osfhandle((intptr_t)h, crt)` where
  `crt = (access) | _O_BINARY | _O_NOINHERIT | (APPEND ? _O_APPEND : 0)`; directories use
  `_O_RDONLY|_O_BINARY|_O_NOINHERIT`. On `_open_osfhandle` failure: `CloseHandle`, `errno=EMFILE`.
* `HANDLE h = (HANDLE)_get_osfhandle(fd)`; `INVALID_HANDLE_VALUE` → `EBADF`.
* Handles are created non-inheritable (`SECURITY_ATTRIBUTES.bInheritHandle = FALSE`, no `OBJ_INHERIT`).
* `close(fd)` closes the handle (CRT `_close`).
* UCRT allows up to 8192 fds (`_setmaxstdio`); RadioS uses a handful.

### 4.2 errno mapping (`radios_platform_errno_from_win32`)
`ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_INVALID_DRIVE, ERROR_BAD_NETPATH, ERROR_BAD_NET_NAME,
ERROR_NOT_READY, ERROR_DELETE_PENDING (STATUS_DELETE_PENDING)` → `ENOENT`;
`ERROR_ACCESS_DENIED, ERROR_PRIVILEGE_NOT_HELD` → `EACCES`; `ERROR_SHARING_VIOLATION` → `EBUSY`
(only surfaces after retries); `ERROR_LOCK_VIOLATION` → `EWOULDBLOCK`; `ERROR_FILE_EXISTS,
ERROR_ALREADY_EXISTS` → `EEXIST`; `ERROR_DIRECTORY` (STATUS_NOT_A_DIRECTORY) → `ENOTDIR`;
STATUS_FILE_IS_A_DIRECTORY → `EISDIR`; `ERROR_DIR_NOT_EMPTY` → `ENOTEMPTY`;
`ERROR_NOT_SAME_DEVICE` → `EXDEV`; `ERROR_DISK_FULL, ERROR_HANDLE_DISK_FULL` → `ENOSPC`;
`ERROR_WRITE_PROTECT` → `EROFS`; `ERROR_FILENAME_EXCED_RANGE, ERROR_BUFFER_OVERFLOW` → `ENAMETOOLONG`;
`ERROR_INVALID_NAME, ERROR_INVALID_PARAMETER, ERROR_BAD_PATHNAME` → `EINVAL`;
`ERROR_NOT_SUPPORTED, ERROR_INVALID_FUNCTION` → `ENOTSUP`; `ERROR_NOT_ENOUGH_MEMORY, ERROR_OUTOFMEMORY`
→ `ENOMEM`; `ERROR_TOO_MANY_OPEN_FILES` → `EMFILE`; `ERROR_CANT_ACCESS_FILE`,
STATUS_REPARSE_POINT_ENCOUNTERED, STATUS_STOPPED_ON_SYMLINK → `ELOOP`; `ERROR_HANDLE_EOF` → 0-byte read;
`ERROR_INVALID_HANDLE` → `EBADF`; everything else `EIO`. NTSTATUS → `RtlNtStatusToDosError` → table,
except the three statuses named above which are mapped directly.

### 4.3 Opening (`radios_fs_open`, `radios_fs_openat`, `radios_fs_statat`, `radios_fs_open_root`)
Common parameter mapping:

| RADIOS flag | Access mask | Disposition / options |
|---|---|---|
| RDONLY | `FILE_GENERIC_READ` | |
| WRONLY | `FILE_GENERIC_WRITE` (contains `READ_CONTROL`) | |
| RDWR | both | |
| APPEND | `FILE_APPEND_DATA` instead of `FILE_WRITE_DATA` | CRT `_O_APPEND` |
| CREAT | + `WRITE_DAC` (owner always has it) | |
| CREAT\|EXCL | | `CREATE_NEW` / `FILE_CREATE` |
| CREAT\|TRUNC | | `CREATE_ALWAYS` / `FILE_OVERWRITE_IF` |
| CREAT | | `OPEN_ALWAYS` / `FILE_OPEN_IF` |
| TRUNC | | `TRUNCATE_EXISTING` / `FILE_OVERWRITE` |
| (none) | | `OPEN_EXISTING` / `FILE_OPEN` |
| DIRECTORY | `FILE_LIST_DIRECTORY\|FILE_TRAVERSE\|FILE_READ_ATTRIBUTES\|READ_CONTROL\|SYNCHRONIZE` | `FILE_FLAG_BACKUP_SEMANTICS` / `FILE_DIRECTORY_FILE`; non-directory → `ENOTDIR` |
| NOFOLLOW | | `FILE_FLAG_OPEN_REPARSE_POINT` / `FILE_OPEN_REPARSE_POINT` + post-check |
| always | `\|SYNCHRONIZE\|FILE_READ_ATTRIBUTES\|READ_CONTROL` | share `READ\|WRITE\|DELETE`; `FILE_SYNCHRONOUS_IO_NONALERT`; read-only opens also `FILE_FLAG_BACKUP_SEMANTICS`/`FILE_OPEN_FOR_BACKUP_INTENT` so that `open(dir, O_RDONLY)` succeeds like POSIX (callers then see `type == DIRECTORY`) |

* Private creation (`CREAT` and `(mode & 077) == 0`): security descriptor from 5.2
  (`SECURITY_ATTRIBUTES.lpSecurityDescriptor` / `OBJECT_ATTRIBUTES.SecurityDescriptor`).
  Otherwise NULL (inherit). `mode & 0200 == 0` is ignored (no read-only files in RadioS).
* Path-based (`radios_fs_open`, `dirfd == RADIOS_AT_FDCWD`): `CreateFileW` on the UTF-16 path with
  `/`→`\`. Absolute paths ≥ 240 UTF-16 units are converted to `\\?\X:\…` / `\\?\UNC\…` after
  `GetFullPathNameW` (the `\\?\` form disables normalisation, so normalise first).
* Relative (`dirfd >= 0`): `NtCreateFile` with `OBJECT_ATTRIBUTES{RootDirectory = dir HANDLE,
  ObjectName = UTF-16 name, Attributes = OBJ_CASE_INSENSITIVE}`. Name must pass
  `radios_path_component_valid` and contain no separator → otherwise `EINVAL`. This is real
  `openat` semantics (same technique as Rust std's `remove_dir_all` hardening and Cygwin).
* No-follow post-check: query `GetFileInformationByHandleEx(FileAttributeTagInfo)`.
  * reparse + `IsReparseTagNameSurrogate(tag)` → close, `errno = ELOOP`.
  * reparse + non-surrogate (OneDrive placeholder, dedup, WOF…) → reopen the same name relative to
    the same directory (or same path) **without** the reparse flag, then compare
    `FileIdInfo` (volume + 128-bit id) of both handles; mismatch → `ELOOP`; keep the second handle.
  * `RADIOS_O_DIRECTORY` and not `FILE_ATTRIBUTE_DIRECTORY` → `ENOTDIR`.
* Write access to a directory → `EISDIR`.
* `radios_fs_statat`: open with `FILE_READ_ATTRIBUTES|READ_CONTROL|SYNCHRONIZE` only (not subject to
  share-mode conflicts; works on files others hold exclusively), `FILE_OPEN_FOR_BACKUP_INTENT`,
  `FILE_OPEN_REPARSE_POINT` when `!follow_links`; fill info (4.5); close.
* `radios_fs_open_root`: root = `g_path_skip_root()` prefix (`C:\`, `\\server\share\`); open it with
  `CreateFileW(L"\\\\?\\C:\\" | L"\\\\?\\UNC\\server\\share\\", DIRECTORY access, …,
  FILE_FLAG_BACKUP_SEMANTICS)`. Relative or drive-relative input → `EINVAL`.
* `radios_fs_open_nofollow_path`: `open_root` + `openat(DIRECTORY|NOFOLLOW)` per component +
  `openat(last, flags|NOFOLLOW)`; closes intermediate fds; preserves errno of the failing step.

### 4.4 Replace, delete, create directory (`renameat`, `rename`, `unlinkat`, `unlink`, `mkdirat`, `mkdir_with_parents`)
* `radios_fs_renameat(od, old, nd, new)`:
  1. Open `old` relative to `od` with `DELETE|SYNCHRONIZE|FILE_READ_ATTRIBUTES`, `FILE_OPEN`,
     `FILE_OPEN_REPARSE_POINT` (renames a link, never its target), share all.
  2. Build `FILE_RENAME_INFO` (header + UTF-16 name, `FileNameLength` in bytes),
     `Flags = REPLACE_IF_EXISTS|POSIX_SEMANTICS|IGNORE_READONLY_ATTRIBUTE`,
     `RootDirectory = handle(nd)`, `FileName = new` (single component). For
     `nd == RADIOS_AT_FDCWD`: open the parent of `new` as a directory and use its basename.
  3. `SetFileInformationByHandle(h, FileRenameInfoEx, …)`. On `ERROR_INVALID_PARAMETER`,
     `ERROR_NOT_SUPPORTED`, `ERROR_INVALID_FUNCTION` (FAT/exFAT/SMB/pre-1607) retry with
     `FileRenameInfo` and `ReplaceIfExists = TRUE` (no POSIX semantics).
  4. Retry loop for `ERROR_SHARING_VIOLATION`, `ERROR_LOCK_VIOLATION`, and `ERROR_ACCESS_DENIED`
     when the destination exists as a regular file (stat it once): sleep 5, 10, 20, 40, 80, 160,
     250, 250, … ms, give up after **2000 ms total** (`RADIOS_FS_RETRY_BUDGET_MS`). A destination that
     is a directory → `EISDIR`/`ENOTEMPTY` immediately.
  5. Durability: the temp file was flushed (`radios_fd_sync`) by the caller before rename; NTFS
     journals the rename; no directory flush.
* `radios_fs_rename(from, to)`: same, with `RootDirectory = NULL` and `FileName = L"\\??\\" +
  GetFullPathNameW(to)` (fully qualified NT path is required when RootDirectory is NULL).
* `radios_fs_unlinkat(d, name, flags)`: open relative with `DELETE|SYNCHRONIZE|FILE_READ_ATTRIBUTES`,
  `FILE_OPEN_REPARSE_POINT` (never follow), `FILE_NON_DIRECTORY_FILE` (flags 0; a directory → `EISDIR`)
  or `FILE_DIRECTORY_FILE` (REMOVEDIR; junctions/dir-symlinks are directories → only the link is
  removed); `FileDispositionInfoEx{DELETE|POSIX_SEMANTICS|IGNORE_READONLY_ATTRIBUTE}` with fallback
  `FileDispositionInfo{TRUE}`; same retry loop; `ERROR_DIR_NOT_EMPTY` → `ENOTEMPTY`.
* `radios_fs_unlink(path)`: path-based variant (CreateFileW, `FILE_FLAG_OPEN_REPARSE_POINT|
  FILE_FLAG_BACKUP_SEMANTICS`; a real directory → `EISDIR`). Use it instead of `g_unlink` for files
  another RadioS thread may still hold open (transition previews, preview files, job snapshots).
* `radios_fs_mkdirat(d, name, mode)`: `NtCreateFile(FILE_LIST_DIRECTORY|SYNCHRONIZE, FILE_CREATE,
  FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT, SD = private-dir SD if (mode & 077)==0)`,
  close; `STATUS_OBJECT_NAME_COLLISION` → `EEXIST`.
* `radios_fs_mkdir_with_parents(path, mode)`: normalise; walk from the root; `CreateDirectoryW` each
  missing component with the private SD when `(mode & 077)==0`; `ERROR_ALREADY_EXISTS` → verify it is
  a directory (follows links, like GLib) else `ENOTDIR`.

### 4.5 `RadioSFileInfo` from a HANDLE (`radios_fd_stat`, statat)
1. `GetFileType(h) != FILE_TYPE_DISK` → `type = OTHER`, rest 0, return 0.
2. `GetFileInformationByHandle(h, &bhfi)` → attributes, `nlink = nNumberOfLinks`,
   `size = (High<<32)|Low` (0 for directories), `mtime_ns` from `ftLastWriteTime`,
   `device = dwVolumeSerialNumber`, `inode = (nFileIndexHigh<<32)|nFileIndexLow`.
3. `GetFileInformationByHandleEx(h, FileBasicInfo)` → `ctime_ns` from `ChangeTime`.
4. `GetFileInformationByHandleEx(h, FileAttributeTagInfo)` → `reparse_tag`.
5. `GetFileInformationByHandleEx(h, FileIdInfo)` (best effort) → `device = VolumeSerialNumber`
   (64-bit), `inode`/`inode_high` from the 128-bit `FileId`.
6. `type`: reparse && name-surrogate → `LINK` (only possible when opened no-follow);
   `FILE_ATTRIBUTE_DIRECTORY` → `DIRECTORY`; else `REGULAR`.
7. FILETIME→ns: `(ft - 116444736000000000) * 100`.
Pitfall: NTFS updates `LastWriteTime` lazily for some writers (memory-mapped writes, writes by other
handles may only be visible on close). `ChangeTime`+`size`+id still catch replacement; content
change detection by time is best effort on both OSes.

### 4.6 `radios_fs_realpath`
`CreateFileW(path, 0 /*query only*/, share all, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)` (follows
links) → `GetFinalPathNameByHandleW(h, buf, n, FILE_NAME_NORMALIZED|VOLUME_NAME_DOS)` (grow buffer
when the return value ≥ n) → strip `\\?\` → `\\?\UNC\srv\share\x` becomes `\\srv\share\x` → upper-case
drive letter → UTF-8. If `VOLUME_NAME_DOS` fails with `ERROR_PATH_NOT_FOUND` (volume mounted without a
letter) retry with `VOLUME_NAME_GUID` and keep `\\?\Volume{…}\…`. Missing → `ENOENT` (`GError`
`G_IO_ERROR_NOT_FOUND`). Relative input is resolved against the CWD by CreateFileW.

### 4.7 Locks (`radios_fd_lock`)
Stateless flock emulation (no per-fd table → plain `close()` can never leave stale state):
* `OVERLAPPED ov = {0}; ov.Offset = 0; ov.OffsetHigh = 0x40000000;` (= `RADIOS_LOCK_REGION_OFFSET`),
  length 1 (`nNumberOfBytesToLockLow = 1, High = 0`).
* `LOCK_SH|LOCK_EX`: first `UnlockFileEx(h, 0, 1, 0, &ov)` ignoring `ERROR_NOT_LOCKED` (makes
  re-locking idempotent and implements SH↔EX conversion like Linux), then
  `LockFileEx(h, (EX ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (NB ? LOCKFILE_FAIL_IMMEDIATELY : 0), 0, 1, 0, &ov)`.
  Handles are synchronous, so the blocking form waits. `ERROR_LOCK_VIOLATION`/`ERROR_IO_PENDING` with
  NB → `errno = EWOULDBLOCK`.
* `LOCK_UN`: `UnlockFileEx`, `ERROR_NOT_LOCKED` → success.
* `radios_fd_close`: `UnlockFileEx` best effort, then `close`. (Closing the handle also releases the
  lock; Windows releases it at handle cleanup.)
* The region is beyond any real file size so the mandatory byte-range lock never blocks `read`/`write`
  of the file content (gui.lock/.lock files are empty anyway).
* Semantics preserved for the tests: two shared locks from two fds of one process coexist; an
  exclusive NB on a third fd fails; closing both shared fds lets the exclusive succeed; two threads
  each opening `.lock` and taking `LOCK_EX` serialise (`tests/test_recovery.c:198-216`).

### 4.8 `radios_fs_dir_check_access(dir, need_write)`
1. `radios_fs_open(dir, RDONLY|DIRECTORY, 0)` → failure errno (EACCES/ENOENT/ENOTDIR); close.
2. Read/list: `FindFirstFileExW(dir\*)` must succeed (`ERROR_ACCESS_DENIED` → `EACCES`).
3. `need_write`: create `dir\.radios-access-<pid>-<random>` with `CREATE_NEW`,
   `FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_TEMPORARY`, `FILE_FLAG_DELETE_ON_CLOSE`; close (deleted).
   `ERROR_WRITE_PROTECT` → `EROFS`, `ERROR_ACCESS_DENIED` → `EACCES`.
(AccessCheck() with the impersonation token is an alternative; the probe is simpler and also detects
read-only media and Controlled Folder Access.)

### 4.9 Privacy
See 5.2 for the shared definition. Implementation: `GetSecurityInfo(h, SE_FILE_OBJECT,
OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION, …)` (handle needs `READ_CONTROL`: all platform
opens and CRT `GENERIC_READ/WRITE` opens have it). Volume check:
`GetVolumeInformationByHandleW(h, …, &fsflags, …)`; no `FILE_PERSISTENT_ACLS` → `ENOTSUP`, FALSE.
`radios_fd_make_private`: if already private → 0; else `SetSecurityInfo(h, SE_FILE_OBJECT,
OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION, user_sid,
NULL, private_dacl, NULL)` (dir or file variant by `FILE_ATTRIBUTE_DIRECTORY`).
`radios_fs_path_make_private`: `SetNamedSecurityInfoW` with the same arguments.
`radios_platform_test_make_public`: `SetEntriesInAclW` adding `GRANT_ACCESS` `FILE_GENERIC_READ` for
`S-1-5-32-545` (BUILTIN\Users) to the current DACL + `SetNamedSecurityInfoW`.

### 4.10 Directory listing (`radios_fd_list_names`)
`GetFileInformationByHandleEx(h, FileIdBothDirectoryRestartInfo, buf, 64 KiB)` then
`FileIdBothDirectoryInfo` until `ERROR_NO_MORE_FILES`; walk `FILE_ID_BOTH_DIR_INFO` via
`NextEntryOffset`; `FileName`/`FileNameLength` (bytes) → UTF-8; skip `.`/`..`. Buffer 8-byte aligned.
Requires `FILE_LIST_DIRECTORY` on the handle (DIRECTORY opens have it). POSIX implementation:
`fdopendir(dup(dirfd))` + `rewinddir` + `readdir` + `closedir`.

### 4.11 Time zones (`radios_time_zone_*`)
* Directory: `$TZDIR` (non-empty) else `<install>\share\zoneinfo` else `<exe dir>\share\zoneinfo`.
* `identifier_exists`: syntax (components of `[A-Za-z0-9._+-]`, no empty component, not `.`/`..`, no
  leading `/`, no `\` or `:`), then `radios_fs_stat(dir\<id with / → \>, FALSE)` is `REGULAR`, then the
  first 5 bytes are `TZif` + version `'2'|'3'|'4'`. `"UTC"` → TRUE.
* `new`: `"UTC"` → `g_time_zone_new_utc()`. Else validate as above, read the file (≤ 1 MiB), take the
  **footer** = text between the last two `\n` (RFC 8536 §3.3, present in v2+ files; e.g.
  `Europe/Istanbul` → `<+03>-3`, `Europe/Berlin` → `CET-1CEST,M3.5.0,M10.5.0/3`), and return
  `g_time_zone_new_identifier(footer)` (GLib parses POSIX TZ strings including `<…>` quoted names on
  all platforms). Empty footer or GLib rejection → NULL (log once with `g_debug`). Cache
  `identifier → GTimeZone` in a `GHashTable` under a `GMutex`; return `g_time_zone_ref()`.
* Limitation (documented, accepted): only the *current* rule set is applied; historic offsets before
  the zone's last rule change (e.g. Istanbul before 2016) differ from Linux/Python. Broadcast
  scheduling uses present/future instants. `%Z` shows the footer abbreviation (`+03`, `CET/CEST`) —
  same as Linux tzfile output.
* Known GLib risk: RFC 8536 extensions (hours > 24 or negative, e.g. `Asia/Jerusalem`
  `IST-2IDT,M3.4.4/26,M10.5.0`, `America/Nuuk`) may be rejected by older GLib → NULL. Verify against
  the shipped GLib with a unit test over every bundled zone and list failures.
* Alternative considered and rejected: mapping IANA→Windows registry names (ICU
  `ucal_getWindowsTimeZoneID`) — gives history but `%Z` would show long localised names and the
  identifier semantics would differ from the service.

### 4.12 Process functions
* `radios_process_executable_path`: `GetModuleFileNameW(NULL, buf, n)` doubling `n` while the result
  equals `n` (or `ERROR_INSUFFICIENT_BUFFER`); UTF-16→UTF-8. Callers pass it to
  `radios_project_root_find` which realpaths it.
* `radios_process_argv`: remembered copy, else `CommandLineToArgvW(GetCommandLineW())` → UTF-8.
  (`app/main.c` should use `g_win32_get_command_line()` + `g_option_context_parse_strv()` on Windows.)
* Termination watch: create (once) the manual-reset event `Local\RadioS-stop-<GetCurrentProcessId()>`
  (default security); attach a `GSource` with a `GPollFD{ .fd = (gintptr)event, .events = G_IO_IN }`
  to the default main context (GLib's Windows main loop waits on HANDLEs); dispatch → callback; after
  dispatch `ResetEvent`. `SetConsoleCtrlHandler(handler, TRUE)`: the handler calls `SetEvent(event)`
  and returns TRUE; for `CTRL_CLOSE_EVENT/CTRL_LOGOFF_EVENT/CTRL_SHUTDOWN_EVENT` it then waits (≤ 5 s,
  the OS limit) on a "shutdown finished" event signalled by `radios_process_remove_termination_watch`
  so WAV finalisation can complete. GUI-subsystem processes do not get console events: the UI area must
  also route `WM_QUERYENDSESSION/WM_ENDSESSION` (GDK window filter on the toplevel HWND +
  `ShutdownBlockReasonCreate`) to the same termination path.
* `radios_process_request_stop(pid)`: `OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\RadioS-stop-<pid>")`
  → `SetEvent`; `ERROR_FILE_NOT_FOUND` → `G_IO_ERROR_NOT_FOUND`.
* `radios_process_spawn_detached`: build the command line with MSVCRT quoting rules (each arg: if empty
  or contains space/tab/`"` → quote; backslashes before a `"` or the closing quote are doubled; `"` →
  `\"`); `CreateProcessW(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS|CREATE_NEW_PROCESS_GROUP|
  CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|CREATE_BREAKAWAY_FROM_JOB, NULL, cwd, &si, &pi)`; on
  `ERROR_ACCESS_DENIED` retry without `CREATE_BREAKAWAY_FROM_JOB`; close `pi` handles.
* `radios_thread_lower_priority`: `SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL)`.
* `radios_process_id`: `GetCurrentProcessId()`.

### 4.13 `radios_fd_pread/pwrite/seek/truncate/sync`
* `pread`: remember the position (`_lseeki64(fd, 0, SEEK_CUR)`), `ReadFile(h, buf, min(count,
  0x7FFFF000), &n, &ov)` with `ov.Offset/OffsetHigh = offset`, restore the position; `ERROR_HANDLE_EOF`
  → 0. Not atomic against concurrent `read()` on the same fd by another thread (RadioS never does that;
  decoder/analyzer fds are single-threaded).
* `pwrite`: same with `WriteFile`.
* `seek`: `_lseeki64`; `truncate`: `_chsize_s(fd, length)` (returns an errno code → set errno, -1);
  `sync`: `_commit(fd)` (= `FlushFileBuffers`, which needs write access: on a read-only fd return 0,
  matching POSIX `fsync` on a read-only fd); `sync_dir`: return 0.

### 4.14 Paths (`platform.c`, no I/O)
* Separators: `G_IS_DIR_SEPARATOR` (both on Windows).
* Case folding for equality: convert both to UTF-16 and `CompareStringOrdinal(a, -1, b, -1, TRUE) ==
  CSTR_EQUAL` (NTFS-compatible upper-casing; never use locale collation or `g_utf8_casefold`, which
  folds `ß`/Turkish dotless i differently from NTFS).
* `is_inside(root, path)`: unify separators, strip one trailing separator from root unless root is a
  root; equal, or `path` starts with root (case-insensitively) and the next char is a separator (or
  root itself ends with a separator).
* `normalize`: `g_canonicalize_filename(path, NULL)`; strip `\\?\`/`\\?\UNC\`; upper-case `[a-z]:`.
* `component_valid`: as documented in 3.9.

---------------------------------------------------------------------------------------------------

## 5. Cross-language contracts (C ↔ Python shim `sdbase/_platform.py`, `tools/_platform.py`)

These must be byte-for-byte identical in behaviour; the Python area implements the mirror.

### 5.1 Advisory locks
* Region: offset `0x4000000000000000` (`OffsetHigh=0x40000000, Offset=0`), length **1 byte**.
* Shared = `LockFileEx` without `LOCKFILE_EXCLUSIVE_LOCK`; exclusive with it; non-blocking adds
  `LOCKFILE_FAIL_IMMEDIATELY` and raises `BlockingIOError` (errno `EWOULDBLOCK`/`EAGAIN`) on
  `ERROR_LOCK_VIOLATION`. Every (re)lock first does `UnlockFileEx` on the same region ignoring
  `ERROR_NOT_LOCKED`. `LOCK_UN` = `UnlockFileEx`.
* Files using it: `data/database-service/gui.lock` (GUI holds SH for its lifetime; offline restore
  takes EX|NB), `data/recovery/<store>/<user>/.lock` (C EX blocking; Python EX|NB),
  `data/database-service/server.lock`, `<db>.lock`, `operation.lock`, `maintenance.lock`,
  `supervisor.lock`, `owner.lock`, `.build.lock`, `default-binary.lock` (Python only today).
* Lock files are opened with share mode READ|WRITE|DELETE in C; Python `os.open` uses CRT defaults
  (no DELETE share) — fine for locks.

### 5.2 "Private" files and directories (replaces 0600/0700 and `st_uid == getuid()`)
* **Creation SDDL** (user SID from `GetTokenInformation(TokenUser)`, `ConvertSidToStringSidW`):
  * file: `O:<SID>D:P(A;;FA;;;<SID>)(A;;FA;;;SY)(A;;FA;;;BA)`
  * dir:  `O:<SID>D:P(A;OICI;FA;;;<SID>)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)`
  Explicit owner `O:<SID>` so elevated and non-elevated runs agree (elevated defaults would make
  `BUILTIN\Administrators` the owner). `ConvertStringSecurityDescriptorToSecurityDescriptorW`.
* **Owner check** (`RADIOS_PRIVATE_OWNER`, Python `owned_by_current_user`): owner SID == token user
  SID, or owner == `S-1-5-32-544` and `CheckTokenMembership(NULL, Administrators)` is TRUE.
* **No-others check** (`RADIOS_PRIVATE_NO_OTHERS`, Python `is_private`): DACL present (NULL DACL ⇒ not
  private); for every `ACCESS_ALLOWED_ACE_TYPE` / `ACCESS_ALLOWED_CALLBACK_ACE_TYPE` /
  object variants that is **not** `INHERIT_ONLY_ACE`, the SID must be one of {token user, owner,
  `S-1-5-18` SYSTEM, `S-1-5-32-544` Administrators, `S-1-3-4` OWNER RIGHTS}, **or** the ACE mask must
  contain none of `FILE_READ_DATA|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_READ_EA|FILE_WRITE_EA|
  FILE_EXECUTE|FILE_DELETE_CHILD|FILE_WRITE_ATTRIBUTES|DELETE|WRITE_DAC|WRITE_OWNER|GENERIC_ALL|
  GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE` (i.e. `READ_CONTROL`, `SYNCHRONIZE`,
  `FILE_READ_ATTRIBUTES` grants to others are tolerated — the analogue of being able to `stat` a 0600
  file). Deny ACEs are ignored.
* **Exact file mode** (`RADIOS_PRIVATE_EXACT_FILE_MODE`): regular file + no-others (Windows has no x bit).
* Volumes without `FILE_PERSISTENT_ACLS` (FAT32/exFAT) ⇒ every check fails (fail closed, `ENOTSUP`).
  The project (with `config/`, `data/recovery`, `data/database-service`) must live on NTFS/ReFS;
  external storage folders (storage.ini) are not privacy-checked.
* Python creates privately: `credentials.json`, `data/database-service/` (+ `server.lock`,
  `gui.lock` if it creates it), outbox dirs it creates, backups, etc. — wherever Linux uses
  `0o600/0o700` or checks `st_uid`/`st_mode & 0o077`.

### 5.3 Graceful stop
Every long-lived RadioS process (GUI `radios.exe`, Python service) creates the manual-reset event
`Local\RadioS-stop-<decimal pid>` at start-up and treats its signalling exactly like SIGTERM.
Requesters (`radios_process_request_stop`, Python `request_stop(pid)`, the test harness replacing
`process.send_signal(SIGTERM)`) open it with `EVENT_MODIFY_STATE` and `SetEvent`. The **process
identity** check that guards stopping (Python `service._process_identity`) uses pid + process creation
time (`GetProcessTimes` → `lpCreationTime` as 100 ns ticks since 1601) — the C side does not need it
today.

### 5.4 Time-zone data
One bundled IANA tree: `<install>\share\zoneinfo\` (TZif v2+ files, e.g. copied from the
`tzdata` Python package's `tzdata/zoneinfo/**` without `__init__.py`, or compiled with `zic`).
C reads it through `radios_time_zone_directory()` (`TZDIR` overrides). The Python service must be
started with `PYTHONTZPATH=<same dir>` (or call `zoneinfo.reset_tzpath([dir])` at import) because
Windows has no system tz database and the runtime is stdlib-only. CTest sets `TZDIR` for all C tests
(4.11) and `PYTHONTZPATH` for Python tests.

### 5.5 Storage folder rules (storage.ini) on Windows — identical in `storage_locations.c` and
`tools/radios_database.py:storage_path_check/storage_directory_check`
1. non-empty, ≤ 4000 bytes, ≤ 240 UTF-16 units unless long paths enabled, no control chars;
2. absolute: `^[A-Z]:\\` or `^\\\\server\\share` (C: `radios_path_is_absolute`; Python:
   `PureWindowsPath(p).drive` non-empty and `p[1:3] == ":\\"` or UNC);
3. canonical text: `p == radios_path_normalize(p)` (Python: `os.path.normpath(p) == p` **and** drive
   letter upper-case) — rejects `/`, `\\` duplicates, `..`, `.`, trailing `\`;
4. not a root (`C:\`, `\\server\share`); every component `radios_path_component_valid`;
5. `lstat` type must be directory, not LINK (junction or symlink);
6. `realpath(p)` equals `p` case-insensitively (Python: `os.path.normcase(os.path.realpath(p)) ==
   os.path.normcase(p)`);
7. directory access probe (read + write);
8. not inside `<realpath(root)>\{config,src,include,tools,tests,docs,design,assets,vendor,packaging,
   cmake,database\SDBASE}` compared case-insensitively (Python: `normcase` before `_inside`, and join
   `database`,`SDBASE` as two components);
9. marker `.radios-storage.json` regular file, 1..4096 bytes, JSON format/role as today.
Linux rules unchanged.

### 5.6 Project-relative paths on the wire / in identity files
* `credentials.json` `database_file`, backup manifests (`as_posix()` already), media
  `relative_path` (`media/analyzer.c:517`), transfer packages: **portable `/` form**. C converts with
  `radios_path_to_portable()` before sending and compares stored values with
  `radios_path_from_portable()` + `radios_path_equal()`. Python must write `.as_posix()` in
  `radios_database_setup.py:348,441,531` and compare normalised (accept both separators for files
  written by older builds).
* Absolute paths (`managed_path`, storage.ini values, journal `project_root`) are native Windows form.

### 5.7 SDBASE source digest
Both sides: files in `database/SDBASE/src/sdbase/` whose name ends with the exact lowercase `.py`,
sorted by the UTF-8 byte string of the **name** (C `strcmp`; Python `sorted(..., key=lambda p:
p.name.encode())`), hash `name \0 raw-bytes \0`.

---------------------------------------------------------------------------------------------------

## 6. Migration table (every POSIX construct in `src/` → platform.h)

### 6.1 `src/core`
| Call site | Replace with |
|---|---|
| config.c:15-22 `path_is_inside` | `radios_path_is_inside` |
| config.c:33/41, 380/398 `struct stat`+`g_lstat` | `radios_fs_stat(p, FALSE, &info)` |
| config.c:60,133,147,157 `realpath`; 79,138,142,152,161 `free` | `radios_fs_realpath`; `g_free` |
| config.c:217/224 `struct stat`+`g_stat`+`S_ISDIR/S_ISREG` | `radios_fs_stat(p, TRUE, &info)` + `info.type` |
| config.c:478 `python_command` | resolve relative-with-separator against root; Windows default `runtime\python\pythonw.exe` (2.1) |
| local_ini.c:27-30 | `+ radios_path_component_valid` |
| local_ini.c:36,39 / preferences_storage.c:54,60 / ui_preferences.c:45,51 `realpath` | `radios_fs_realpath` |
| local_ini.c:42-44 / preferences_storage.c:67-69 / ui_preferences.c:33-40,58 inside | `radios_path_is_inside` |
| local_ini.c:47 / preferences_storage.c:76 / ui_preferences.c:64 `open(O_DIRECTORY\|O_NOFOLLOW\|O_CLOEXEC)` | `radios_fs_open(…, RADIOS_O_RDONLY\|RADIOS_O_DIRECTORY\|RADIOS_O_NOFOLLOW\|RADIOS_O_CLOEXEC, 0)` |
| local_ini.c:63,102,123 / preferences_storage.c:86 / ui_preferences.c:77 `fstatat(AT_SYMLINK_NOFOLLOW)` | `radios_fs_statat(dir, name, FALSE, &info)` |
| local_ini.c:70 / preferences_storage.c:119 / ui_preferences.c:133 `openat(RDONLY\|NOFOLLOW\|CLOEXEC\|NONBLOCK)` | `radios_fs_openat` |
| local_ini.c:73 / preferences_storage.c:123 / ui_preferences.c:138 `fstat` | `radios_fd_stat` |
| local_ini.c:132 / preferences_storage.c:175 / ui_preferences.c:228 `openat(WRONLY\|CREAT\|EXCL\|NOFOLLOW\|CLOEXEC, 0600)` | `radios_fs_openat(…, 0600)` |
| local_ini.c:147 / preferences_storage.c:192 / ui_preferences.c:255 `fsync(fd)` | `radios_fd_sync` |
| local_ini.c:149 / preferences_storage.c:194 / ui_preferences.c:259 `renameat` | `radios_fs_renameat` |
| local_ini.c:108,151 / preferences_storage.c:196 / ui_preferences.c:262 `unlinkat` | `radios_fs_unlinkat` |
| local_ini.c:110,152 / preferences_storage.c:199 / ui_preferences.c:266 `fsync(dir)` | `radios_fd_sync_dir` |
| storage_locations.c:68 `g_path_is_absolute` | `radios_path_is_absolute` |
| storage_locations.c:69-72 canonical check | `+ radios_path_normalize` equality, component validation |
| storage_locations.c:73 `== "/"` | `radios_path_is_root` |
| storage_locations.c:77-82,93 `path_inside` | `radios_path_is_inside` |
| storage_locations.c:89,144 `realpath` | `radios_fs_realpath` (+ `radios_path_equal` at 145) |
| storage_locations.c:92 `"database/SDBASE"` | two components |
| storage_locations.c:110,140,161,368,382 `lstat`; 142 `S_ISLNK` | `radios_fs_stat(…, FALSE, …)`; `RADIOS_FILE_TYPE_LINK` |
| storage_locations.c:148 `access(R\|W\|X)` | `radios_fs_dir_check_access(dir, TRUE)` |
| storage_locations.c:162 `g_mkdir_with_parents` | `radios_fs_mkdir_with_parents` |
| storage_locations.c:166 `open(marker,…0600)` | `radios_fs_open` |
| storage_locations.c:186 `fsync` | `radios_fd_sync` |
| storage_locations.c:188 `unlink` | `radios_fs_unlink` |
| storage_locations.c:348,355 `statvfs` | `radios_fs_disk_space` |
| media_archive.c:83-88,101-103 | `radios_path_is_inside`, `radios_path_equal` |
| media_archive.c:199 `access(R\|X)` | `radios_fs_dir_check_access(dir, FALSE)` |
| media_archive.c:242-243 `st_mtim` | `info.mtime_ns / 1000` |
| media_archive.c:254 hidden | `radios_file_info_is_hidden` |
| media_archive.c:261-263,417 `g_lstat`+`S_ISLNK` | `radios_fs_stat(…, FALSE, …)` |
| media_archive.c:340 `g_mkdir_with_parents` | `radios_fs_mkdir_with_parents` |
| media_archive.c:350 `g_open(O_NOFOLLOW\|O_CLOEXEC\|O_NONBLOCK)` | `radios_fs_open` |
| media_archive.c:353,369 `fstat` | `radios_fd_stat` |
| recovery.c:137-138 split `/` + `open("/")` | `radios_fs_open_root` |
| recovery.c:144 `mkdirat 0700` | `radios_fs_mkdirat` |
| recovery.c:145,232,283 `fsync(dir)` | `radios_fd_sync_dir` |
| recovery.c:147,161,172,222,361,362,367 `openat` | `radios_fs_openat` |
| recovery.c:150,163-164,175-176,366,368-369 `fstat`+`st_uid`/`getuid`/`st_mode&077`/`st_nlink` | `radios_fd_stat` + `radios_fd_is_private` |
| recovery.c:164 `flock(LOCK_EX)`; 369 `flock(LOCK_SH\|LOCK_NB)` | `radios_fd_lock(fd, RADIOS_LOCK_EX)`; `radios_fd_lock(fd, RADIOS_LOCK_SH\|RADIOS_LOCK_NB)` |
| recovery.c:230 `fsync(fd)` | `radios_fd_sync` |
| recovery.c:232 `renameat` | `radios_fs_renameat` |
| recovery.c:233,283 `unlinkat` | `radios_fs_unlinkat` |
| recovery.c:238,316 `/proc/self/fd/%d` | `radios_fd_list_names` |
| recovery.c:360 `open(project_root, …O_NOFOLLOW)` | `radios_fs_open` |
| recovery.c:370 `fstatat(restore-pending.json)` | `radios_fs_statat` |
| data_client.c:113-119 | `radios_path_is_inside` |
| data_client.c:202 `open(O_NOFOLLOW…)`; 208-210 `fstat`+mode/uid | `radios_fs_open`; `radios_fd_stat` + `radios_fd_is_private(OWNER\|NO_OTHERS\|EXACT_FILE_MODE)` |
| data_client.c:290-293 `realpath`; 297 `lstat` | `radios_fs_realpath`; `radios_fs_stat(…, FALSE, …)` |
| data_client.c:324-333 relative compare | `radios_path_is_root` + portable comparison (5.6) |
| data_client.c:611-635, external_access.c:56-64 | `CURLSSLOPT_NATIVE_CA` for HTTPS on Windows |
| library_analysis.c:143 `setpriority(…syscall(SYS_gettid)…)` | `radios_thread_lower_priority()` |
| loudness_policy.c:31-34 | `radios_time_zone_identifier_exists` + `radios_time_zone_new` |
| weekly_programme_plan.c:145,176,391; automation_calendar_plan.c:158,1085,1825 | `radios_time_zone_new` |
| routing_graph.c:97; 99-101 | `radios_path_is_absolute`; `+ radios_path_component_valid(prefix)` |
| midi.c:184,218,247,250-251 `poll`/`pipe` | MIDI area (WinMM backend), not platform.h |

### 6.2 Other areas (the API must cover them; owners implement)
| Call site | Replace with |
|---|---|
| app/main.c:5,7 `<glib-unix.h>`,`<signal.h>`; 55-56 `g_unix_signal_add(SIGTERM/SIGINT)`; 85-86 `g_source_remove` | `radios_process_add_termination_watch` / `radios_process_remove_termination_watch` (one id) |
| app/main.c:118 `setlocale(LC_ALL,"")` | keep (UCRT honours UTF-8 locale); call `radios_platform_init()` before it; `radios_process_remember_argv(argc, argv)` before option parsing |
| app/main.c:138 `g_file_read_link("/proc/self/exe")` | `radios_process_executable_path` |
| app/main.c:160 "Wayland/X11" message | platform-neutral message on Windows |
| ui/window.c:24 `<unistd.h>`; 725,755,1009 `close(gui_lock)` | `radios_fd_close` |
| ui/media_archive_ui.c:553,701 `realpath`+`free` | `radios_fs_realpath` + `g_free` |
| ui/reset_ui.c:340-352 `/proc/self/cmdline` | `radios_process_argv` |
| ui/reset_ui.c:353-367 `sh -c "sleep 2; exec setsid run-radios.sh"`, `/.flatpak-info` | Windows: `radios_process_spawn_detached({pythonw, tools/radios_launch.py, --wait-for-pid <pid>, …})` (launcher area defines the Windows launcher); flatpak branches `#ifndef G_OS_WIN32` |
| ui/data.c:379-397, ui/admin_ui.c:696-701 GSubprocess with `python_command` | resolved `python_command` (pythonw.exe); `/.flatpak-info` (data.c:381) Linux-only |
| ui/data.c:113,373; admin_ui.c:691,1079 `g_subprocess_force_exit` | OK (TerminateProcess) for helpers; graceful service stop goes through the service's own stop command / `radios_process_request_stop` |
| ui/playlist.c:1739-1749 `g_open(O_DIRECTORY\|O_NOFOLLOW…)` + `openat` walk | `radios_fs_open` + `radios_fs_openat` |
| ui/playlist.c:1754-1772 `fstat` + `st_mtim/st_ctim`, 1757 `(off_t)limit` | `radios_fd_stat` + `mtime_ns/ctime_ns`; `(gint64)limit` |
| ui/preferences.c:334-358 `/proc/asound` | audio backend query (WASAPI mix format); `#ifdef __linux__` |
| ui/broadcast_ui.c:247 `G_FILE_TEST_IS_SYMLINK` + `g_mkdir_with_parents(0700)` | `radios_fs_stat(…, FALSE, …)` + `radios_fs_mkdir_with_parents` |
| ui/audio_ui.c:1260,1426,1836; ui/media_jobs.c:74; audio/transition.c:110 `g_unlink` | `radios_fs_unlink` (file may still be open by the engine/decoder) |
| ui/content_scale.c:3-4,380-381,457-483 X11 | already `#ifdef GDK_WINDOWING_X11` — fine |
| audio/decoder.c:56-61 `same_file` (st_dev/ino/size/mtim/ctim) | `radios_file_info_same` |
| audio/decoder.c:65,346 `lstat`; 65,354 `fstat` | `radios_fs_stat(…, FALSE, …)`; `radios_fd_stat` |
| audio/decoder.c:71-86 `open_local` | `radios_fs_open_nofollow_path(path, RADIOS_O_RDONLY\|RADIOS_O_NONBLOCK\|RADIOS_O_CLOEXEC, 0)` |
| audio/decoder.c:98,115,143-168 `pread` | `radios_fd_pread` |
| media/analyzer.c:57-62 same_file; 64-77 open_local_path; 85,104,501 `lstat`; 94,104,501,506 `fstat`; 117,134,235-262 `pread` | as decoder |
| media/analyzer.c:443 `(*absolute)[length] != '/'` | `radios_path_is_inside(root, absolute) && !radios_path_equal(root, absolute)` |
| media/analyzer.c:449 `g_strsplit(relative, "/")` | `radios_path_split` |
| media/analyzer.c:451 `mkdirat(0700)`; 452,480 `openat`; 520 `unlinkat` | `radios_fs_mkdirat` / `radios_fs_openat` / `radios_fs_unlinkat` |
| media/analyzer.c:497 `fchmod(0600)`+`fsync`+`fsync(dir)` | `radios_fd_make_private` + `radios_fd_sync` + `radios_fd_sync_dir` |
| media/analyzer.c:501-502 dev/ino compare | `radios_fd_stat` vs `radios_fs_stat` (`device`, `inode`, `inode_high`) |
| media/analyzer.c:517 `relative_path = g_build_filename(relative, name)` (sent to SDBASE) | `radios_path_to_portable` |
| audio/output.c:124 `open(O_CREAT\|O_EXCL\|O_WRONLY\|O_CLOEXEC, 0600)`; 142 `lseek` + `fsync` | `radios_fs_open(…, 0600)` (private WAV); `radios_fd_seek` + `radios_fd_sync` |
| audio/transition.c:70 `g_mkdir_with_parents`; 71 `g_mkstemp_full(O_RDWR\|O_CLOEXEC)` | `radios_fs_mkdir_with_parents`; `RADIOS_O_RDWR\|RADIOS_O_CLOEXEC\|RADIOS_O_BINARY` |
| audio/patchbay.c:389-390 | `radios_path_is_absolute`; `radios_fs_mkdir_with_parents` |
| audio/engine.c:1113,1195; playout/automation_policy.c:80,298; ui/*.c (list in 2.11) | `radios_time_zone_new` |

---------------------------------------------------------------------------------------------------

## 7. Tests

### 7.1 What exercises this area
| Test | Exercises | POSIX constructs to replace |
|---|---|---|
| tests/test_config.c | root discovery, confinement, links inside/outside, dangling local link, cwd independence | `symlink` (301, 311 dir links → junction OK; 333, 338 file links → skip unprivileged), `/tmp/...` literals (271, 286: still INVALID on Windows because rooted `\tmp\…` is not inside the root, so they pass unchanged — add a drive-absolute outside path such as the temp dir's parent), fixture root must be `radios_fs_realpath`'d (8.3 `%TEMP%`: `test_defaults:109` and `test_root_discovery:379` compare strings) |
| tests/test_local_ini.c | missing/roundtrip/no temp leftovers/names/links/limits/config escape | `lstat`+`st_mode&0777==0600` (94-95 → `radios_fs_path_is_private`), `symlink` (131 file, 170 dir) |
| tests/test_preferences_storage.c, tests/test_ui_preferences.c | defaults, atomic 0600 save, re-save restores 0600 after chmod 0644, file links, dir links, fifo | `st_mode` checks (83; 105,115), `g_chmod(…,0644)` (ui:109 → `radios_platform_test_make_public`), `symlink` (184, 211; 465, 495, 511), `mkfifo` (196 → `#ifndef G_OS_WIN32`) |
| tests/test_storage_locations.c | path syntax, directory rules, marker 0600, apply/notice, sizes | `realpath` (31), `symlink` (93 dir → junction; 182 file → use a junction to a directory instead so "links are not followed" is still tested), mode checks (104, 130), Windows path-syntax vectors for `test_path_syntax` (`"D:\\radyo\\kayıt"` valid; `"", "göreli", "C:\\", "C:\\a\\..\\b", "C:\\a\\\\b", "C:\\a\\", "C:\\a\tb", "\\a", "C:a", "C:/a", "c:\\a"` invalid), `"/yok/böyle/bir/yer"` → `"Z:\\yok\\böyle"`-style missing path |
| tests/test_media_archive.c | settings, scan, index reuse, links skipped, cancel | `realpath` (34), `symlink` (50), mode check (98) |
| tests/test_recovery.c | outbox create/reopen, exact body, secrets, tree/entry links, identity order, concurrent prepare (two threads), GUI lock lifecycle | `fork`/`waitpid`/`_exit` (52-54, 222-224 → `g_test_trap_subprocess()`), `flock` (215-216, 225 → `radios_fd_lock`), `open(lock_path,O_RDWR)` (214, 225 → `radios_fs_open`), `symlink("/tmp", …)` (173 → junction to `g_get_tmp_dir()`), `symlink("/etc/passwd", …)` (180 → file link, skip unprivileged), `g_chmod(0600)` after `g_file_set_contents` (182, 257 → `radios_fs_path_make_private`), mode checks (59, 125) |
| tests/test_data_client.c | credentials privacy/types/paths, transport | `g_chmod(0600/0644)` (230, 346, 349, 352, 364, 369 → make_private / test_make_public), `symlink` (372 file link → skip), project root from `RADIOS_TEST_PROJECT_ROOT` (forward slashes; fine after realpath) |
| tests/test_library_analysis.c | settings 0600 | mode check (99) |
| tests/test_loudness_policy.c | policy JSON/validation incl. `Europe/Istanbul` | needs `TZDIR` → bundled zoneinfo |
| tests/test_runtime.c, test_external_access.c, test_automation_*.c, test_weekly_*.c | logic | none (time zones via TZDIR) |

### 7.2 Shared test support (new `tests/support/radios_test_support.{h,c}`)
* `void radios_test_init(int *argc, char ***argv)` = `g_test_init` + `radios_platform_init` + default
  `TZDIR` (`<build>/zoneinfo` from a CMake define) when unset.
* `gchar *radios_test_make_tmp_dir(const gchar *tmpl)` = `g_dir_make_tmp` + `radios_fs_realpath`.
* `void radios_test_remove_tree(const gchar *path)` never follows links (stat no-follow; LINK →
  `radios_fs_unlinkat(RADIOS_AT_FDCWD, p, is_dir_link ? RADIOS_AT_REMOVEDIR : 0)`), replaces the
  per-test copies of `remove_tree()` (test_config.c, test_local_ini.c, test_recovery.c,
  test_storage_locations.c, …), which rely on `G_FILE_TEST_IS_SYMLINK`.
* `gboolean radios_test_symlink_or_skip(target, link, directory)` → `radios_platform_test_symlink`,
  `g_test_skip()` on `G_IO_ERROR_NOT_SUPPORTED`.
* `radios_test_assert_private(path)`, `radios_test_make_private(path)`, `radios_test_make_public(path)`.
* CMake: `set_tests_properties(... ENVIRONMENT "TZDIR=…;G_DEBUG=fatal-warnings")`;
  `RADIOS_TEST_PROJECT_ROOT` should be passed through `file(TO_NATIVE_PATH)` or normalised at runtime.

### 7.3 New `tests/test_platform.c` (both OSes)
1. openat/statat no-follow: file link → ELOOP / LINK; junction → LINK; OneDrive-like non-surrogate
   reparse cannot be fabricated in CI → covered by code review.
2. renameat replaces a target that another fd holds open (POSIX-semantics path) and a target held by
   `g_open` (no FILE_SHARE_DELETE) released after 100 ms from another thread (retry path).
3. unlinkat of an open file; recreate same name immediately (no delete-pending).
4. Locks: SH+SH ok; EX|NB fails with EWOULDBLOCK; close releases; EX blocking waits for a thread;
   conversion SH→EX; region does not block `read()`.
5. Privacy: created 0600 file/0700 dir pass all checks; `test_make_public` → NO_OTHERS fails; elevated
   vs non-elevated owner (manual).
6. realpath: 8.3 short path → long; trailing separator; missing → ENOENT; relative → CWD.
7. Path vectors for `is_absolute/is_root/is_inside/equal/normalize/component_valid/split/portable`.
8. Time zones: every bundled zone loads (report the list that GLib rejects); `Europe/Istanbul` offset
   +3 h; `Europe/Berlin` 2026-03-29 01:00 UTC → +2 h; `"../x"`, `"a\\b"`, `"C:x"`, `"/UTC"` rejected.
9. `radios_fd_list_names` on a dir with Turkish names; pread/pwrite at offsets > 2 GiB on a sparse file;
   truncate to 2 GiB + 1.
10. Cross-language: `tests/test_platform_lock_interop.py` — C helper holds SH on gui.lock, Python shim
    EX|NB must fail, and vice versa.

---------------------------------------------------------------------------------------------------

## 8. Build notes for this area
* Add `src/core/platform.c`, `platform_posix.c`, `platform_win32.c` to `radios_core`
  (`CMakeLists.txt:41-42` + `target_sources` lines); on Windows link `ntdll advapi32 shell32`.
* `-Werror` is on by default (`RADIOS_STRICT_WARNINGS`): every [C] item above breaks the build; port
  core first.
* With `CMAKE_C_EXTENSIONS OFF` (`-std=c11`, `__STRICT_ANSI__`) mingw hides `M_PI` unless
  `_USE_MATH_DEFINES`/`_POSIX_C_SOURCE`/`_XOPEN_SOURCE` is defined — files that already define
  `_XOPEN_SOURCE 700`/`_GNU_SOURCE`/`_POSIX_C_SOURCE` are fine; build area should add
  `-D_USE_MATH_DEFINES` globally on Windows.
* Prefer `g_*` over CRT for anything not in platform.h; none of `strptime`, `timegm`, `strndup`,
  `strtok_r`, `setenv`, `getline`, `localtime_r`, `gmtime_r` are used in `src/` or `tests/` today (grep
  verified) — keep it that way (mingw lacks `strptime/timegm/strndup/setenv/getline`;
  `localtime_r/gmtime_r` exist only as inline wrappers under `_POSIX_C_SOURCE`). `strnlen`
  (`ui/reset_ui.c:342`) exists in UCRT.
* Packaging (other area): `share/zoneinfo`, `runtime/python/` (embeddable CPython incl. `pythonw.exe`),
  GLib's `gspawn-win64-helper*.exe` next to `libglib-2.0-0.dll` if the shipped GLib still uses them.
  The RadioS root must be on NTFS and user-writable (not `C:\Program Files`): data/config are
  root-relative by design.

---------------------------------------------------------------------------------------------------

## 9. Risks and open questions
1. **Time zones**: footer approach loses history and may reject RFC 8536 extended rules on older GLib
   (4.11). Must be validated over all bundled zones; Istanbul (the likely station zone) is safe.
2. **Sharing violations**: readers outside our control (Python `open()`, GLib `g_file_get_contents`,
   antivirus, backup agents, OneDrive sync) can hold files without `FILE_SHARE_DELETE` longer than the
   2 s retry budget → atomic replace fails with EBUSY/EACCES. The error paths already exist (Turkish
   "atomik değiştirilemedi" etc.); make sure UI messages are acceptable.
3. **Controlled Folder Access / OneDrive**: a RadioS root under Documents/Desktop can be blocked by
   Defender CFA (writes fail with EACCES) or be a OneDrive-synced folder (placeholder reparse points;
   handled by W4 rule but sync conflicts on `config/*.ini` are possible). Recommend installing to e.g.
   `C:\RadioS` or `%LOCALAPPDATA%\RadioS` (and documenting NTFS requirement).
4. **ACL semantics drift** between C and Python privacy checks would lock users out (credentials
   rejected) — keep 5.2 as the single written definition and add the interop test.
5. **Mapped network drives**: realpath returns UNC; storage.ini then stores UNC paths. Works, but a
   user hand-editing `Z:\…` gets "symbolic link" errors. Document.
6. **Long paths**: without `longPathAware` in the manifest + `LongPathsEnabled`, GLib/CRT calls fail
   beyond 260 chars while relative `NtCreateFile` opens succeed — inconsistent errors deep inside
   archive folders. Recommend adding `<longPathAware>true</longPathAware>` to the manifest (build area).
7. **pread emulation** moves/restores the file pointer; safe only because fds are single-threaded.
8. **Locale**: Windows users typically run with Turkish regional formats (`setlocale(LC_ALL,"")`);
   `%f` formatting yields `,` — same as a Turkish Linux locale, but the Linux developers may have
   tested under C/en locales. JSON uses `g_ascii_*`/json-glib (locale-independent).
9. **Elevation**: running the GUI elevated once creates files owned by the user SID (explicit owner),
   but files created by *other tools* elevated (e.g. an installer) are owned by Administrators —
   accepted by the owner rule only while the user is an administrator.
10. Process-wide `SetErrorMode` affects every library in the process — intended.
