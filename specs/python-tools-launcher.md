# Area spec: python-tools-launcher (Windows 10/11 x64 port)

Analyzed copy: `C:\Users\sonatturkan\Desktop\RadioS` (read-only phase; nothing was modified).
All `file:line` references below point into that copy.

Files in scope: `run-radios.sh`, `run-database.sh`, `run-radios-sdk.sh`, `tools/radios_launch.py`,
`tools/radios_database.py`, `tools/radios_database_setup.py`, `tools/radios_reliability.py`,
`tools/radios_package.py`, `tools/radios_sdk.py`, `tools/radios_hardware_check.py`, `tools/css_to_gtk.py`,
`tools/islev_envanteri.py`, `tools/gui_backend_coverage.py`, `tools/README.md`, `tools/otomasyon_testleri/*`,
`tools/yayin_akisi_724/*`, `packaging/README.md`, `config/radios.example.ini`, `config/README.md`.
C files are referenced only where they spawn or share state with these tools (their port belongs to other areas,
listed in section 9 as cross-area dependencies).

---

## 0. TL;DR (decisions)

1. **Entry point on Windows is `RadioS.exe` itself** (`<root>\bin\RadioS.exe` in the product, `build\radios.exe` in dev).
   No shell script, no `exec`. Before creating its window the exe spawns **one** Python helper:
   `python.exe -B -X utf8 <root>\tools\radios_database.py --project-root <root> --config <cfg> ensure`
   (new subcommand = today's `radios_launch.py` lines 110-125: first-run init + owned service start, under the
   operation lock). All lifecycle logic stays in Python; C only spawns, waits, and shows the error dialog.
2. **New Python shim `tools/radios_platform.py`** (stdlib + ctypes, no third-party) used by every tool. It
   replaces `fcntl`, `/proc`, `os.getuid`, `O_NOFOLLOW/O_DIRECTORY/O_NONBLOCK`, `pidfd_*`, `os.sync`,
   `start_new_session`, directory fsync, mode-bit privacy checks. Its lock byte-range, private-DACL SDDL and
   privacy rule MUST be identical to `sdbase/_platform.py` and `src/core/platform.c` (contract in 4.7/4.8).
3. **Process identity on Windows** = `(boot_id="win32", start_ticks=str(creation FILETIME in 100 ns), argv)` where
   argv comes from `NtQueryInformationProcess(ProcessCommandLineInformation=60)` + `CommandLineToArgvW`.
   `_owned()` stays byte-for-byte the same. GetTickCount64-based boot time is written only as an informational
   `boot_time` field (it drifts with clock changes and must not be compared).
4. **Detached service spawn**: `CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW` (+ `CREATE_BREAKAWAY_FROM_JOB`,
   retried without it on `ERROR_ACCESS_DENIED`), `stdin=DEVNULL`, stdout/stderr = an **append-only**
   (`FILE_APPEND_DATA`) handle on `data\logs\database-service.log`, `close_fds=True`.
5. **Graceful stop** = named manual-reset event `Local\RadioS-SDBASE-<instance>` created by `serve` (polled every
   loop turn, 0.2 s) and set by `stop` after re-verifying identity through a pinned process handle
   (`OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE)`; a held handle prevents PID
   reuse exactly like a pidfd). `os.kill(pid, SIGTERM)` must never be used on Windows (it is `TerminateProcess`).
   `TerminateProcess` only (a) on our own Popen child in the start failure path (as today), (b) with a new explicit
   `stop --force` (installer/uninstaller, operator).
6. **Privacy** = explicit protected DACLs, not "trust the profile ACL". Evidence on this very machine:
   `config\credentials.json` in the copy inherits `Sonat\CodexSandboxUsers: ReadAndExecute` from the Desktop.
   Private SDDL is CPython's own (`os.mkdir(p, 0o700)` since 3.12.4/3.13):
   dirs `D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)`, files `D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)`.
   Check = owner is the current user (or BUILTIN\Administrators when elevated) and no allow-ACE grants data
   rights to any SID outside {current user, SY, BA, OW}. Volumes without persistent ACLs (FAT/exFAT) fail closed
   for `config\` and `data\database-service\` (Linux parity: vfat shows 0644/0755 and is rejected too).
7. **Every `os.open` gets `O_BINARY`** on Windows (without it the CRT fd is text mode: CRLF translation and
   Ctrl-Z EOF would corrupt `.sdb` copies in `_publish`/`_digest`). Every text file write gets `encoding="utf-8",
   newline="\n"`. Every Python helper prints UTF-8 (`-X utf8` + `sys.stdout.reconfigure`), because the GUI reads
   helper output with `g_subprocess_communicate_utf8`.
8. **Atomic replace** = `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)` with a bounded retry on
   ERROR_ACCESS_DENIED/SHARING_VIOLATION/LOCK_VIOLATION; **publish-without-replace** (today `os.link`) =
   `MoveFileExW(WRITE_THROUGH)` without REPLACE (fails with `ERROR_ALREADY_EXISTS`, works on FAT/exFAT too);
   directory fsync = no-op; `os.fsync` needs a write-access handle on Windows (FlushFileBuffers).
9. **Python runtime**: bundled CPython embeddable in `<root>\runtime\python\` (the copy already contains 3.14.8 +
   `Lib\tzdata`, *without* a `._pth`; the lead decided 3.13 - pin one version in `packaging/windows/runtime-lock.json`).
   Ship a `python3XX._pth` = `python3XX.zip`, `.`, `tzdata.zip` (or `Lib`), `..\..\tools` → hermetic interpreter
   (ignores PYTHONHOME/PYTHONPATH/user site; script dir comes from `..\..\tools`). `python_command = python3` in the
   INI maps on Windows to `<root>\runtime\python\python.exe`; never resolve `python3` through `PATH`
   (Microsoft Store alias stub).
10. **Dev launchers**: `run-radios.cmd`/`.ps1` (→ `tools\radios_launch.py`: optional CMake/Ninja build, then run
    `build\radios.exe`, propagate exit code; no execv) and `run-database.cmd`/`.ps1` (same subcommands as Linux
    + `ensure`). `.cmd` is primary (no execution-policy problems), must be CRLF and must strip `%~dp0`'s trailing
    backslash before quoting.
11. **Linux-only, not shipped on Windows**: `run-radios-sdk.sh`, `tools/radios_sdk.py`, flatpak branches of
    `run-radios.sh`, `otomasyon_testleri/build.sh` (→ CMake targets). `radios_hardware_check.py` keeps its report
    schema but on Windows asks `RadioS.exe --audio-inventory` (WASAPI enumeration in the C backend) instead of
    `pactl`.
12. **Packaging**: `radios_package.py` gains kind `windows` → deterministic portable
    `dist\RadioS-<ver>-windows-x86_64.zip` (layout `RadioS\bin\RadioS.exe + DLL closure`, `lib\`, `share\`, `etc\`,
    `runtime\python\`, tools, SDBASE, config, assets, docs, empty `data\*`), verified like the tarballs, plus an
    optional per-user NSIS installer (`$LOCALAPPDATA\Programs\RadioS`, `RequestExecutionLevel user`) built from the
    same verified tree. Per-machine `Program Files` install is incompatible with root-relative writable `data\`/`config\`.

---

## 1. Inventory and disposition

| File | Lines | Linux-specific content | Windows disposition |
|---|---|---|---|
| `run-radios.sh` | 13 | `sh`, `pwd -P`, flatpak-spawn handoff (L8-11), `exec python3` (L12) | Keep for Linux. Add `run-radios.cmd` + `run-radios.ps1`. |
| `run-database.sh` | 6 | `sh`, `exec python3` | Keep. Add `run-database.cmd` + `run-database.ps1`. |
| `run-radios-sdk.sh` | 6 | Flatpak SDK only | Linux-only; exclude from Windows package. |
| `tools/radios_launch.py` | 136 | `fcntl` (L12,L51), cmake/pkg-config/cc + SDK (L29-46), `/usr/bin/cmake` (L46), X_OK (L65,84,86), ELF loader stderr probe (L90-94), `os.execv` (L99,L126), DISPLAY/WAYLAND (L100-102), umask (L113) | Port: build step + exe run on Windows; first-run/start moved to `radios_database.ensure_installation()` (used in-process on Linux, by RadioS.exe on Windows). |
| `tools/radios_database.py` | 682 | `fcntl`, `/proc` identity, `O_NOFOLLOW/O_NONBLOCK/O_DIRECTORY`, uid/mode checks, dir fsync, `os.sync`, `start_new_session`, `pidfd_open/pidfd_send_signal`, SIGTERM, POSIX storage-path rules | Port via shim (section 5.2 has every line). New `ensure` and `stop --force`. |
| `tools/radios_database_setup.py` | 562 | `fcntl`, uid/0600/0700 checks, `O_NOFOLLOW/O_NONBLOCK/O_DIRECTORY`, dir fsync, `os.link(follow_symlinks=False)`, `str(relative_to)` path form, glob ordering of digest | Port via shim (5.3). |
| `tools/radios_reliability.py` | 810 | `fcntl`, `/proc` GUI scan, uid/mode checks, symlink-only checks | Port via shim (5.4). |
| `tools/radios_package.py` | 438 | `dir_fd`/`O_DIRECTORY` walk, ELF/readelf/GLIBC, `.sh` modes, LF-only CMakeCache match | Port + new `windows` kind (zip + NSIS) (5.5, 4.17). |
| `tools/radios_sdk.py` | 42 | Flatpak SDK | Linux-only; never imported on Windows; excluded from Windows package. |
| `tools/radios_hardware_check.py` | 98 | `pactl` | Platform branch: Windows uses `RadioS.exe --audio-inventory` (4.16). |
| `tools/css_to_gtk.py` | 197 | locale-default `read_text`/`write_text` | Encoding/newline fixes only (build-time, run by CMake's Python). |
| `tools/islev_envanteri.py` | 222 | Path-object sort order (case-insensitive on Windows), CRLF compare, sdbase import | Sort/compare fixes. |
| `tools/gui_backend_coverage.py` | 269 | sdbase import (fcntl) | Works once sdbase imports on Windows; stdout UTF-8. |
| `tools/otomasyon_testleri/` | | `build.sh` (cc/pkg-config/static libs), `e2e` without `.exe`, locale `open()`, urllib proxies | CMake target for e2e, `.exe`, UTF-8, no-proxy opener. |
| `tools/yayin_akisi_724/` | | hard-coded `/home/sturkan/...` (rapi.py:3, verify.py:6, sweeper_import.py:4-5), `analiz` without `.exe`, locale `open()` | Derive root from `__file__`, args for folders, CMake target for `analiz.c`. |
| `tools/README.md`, `packaging/README.md`, `config/README.md`, `config/radios.example.ini` | | sh commands, 0600 wording, `python_command = python3` | Windows sections; python_command semantics comment. |

Pre-existing state in the Windows copy that the porting team must know:
- `config\credentials.json` exists (a real Linux credential, `database_file = data/database/RadioS.sdb`), but
  `data\database\` is empty. The first Windows launch will treat the install as *not fresh* and fail in
  `load_credentials` ("Gerekli yerel dosya bulunamadı"). Either delete it (it is git-ignored user data and its tokens
  must be treated as exposed: its ACL grants `CodexSandboxUsers` read) or copy the matching `data\database\*`.
- `config\audio-panel.ini`, `config\ui-preferences.ini` are Linux user state (harmless).
- `runtime\python\` holds an extracted CPython 3.14.8 embeddable + `Lib\tzdata` and **no `python314._pth`**.
  `packaging\windows\cache\` holds `python-3.14.8-embed-amd64.zip` and `tzdata-2026.5-py2.py3-none-any.whl`.
  `.gitignore` ignores `*.exe`/`*.dll` globally, so `runtime\` must be (re)generated by tooling (4.17.2), and
  `packaging/windows/cache/` and `/runtime/` should be git-ignored explicitly.
- `assets\fonts\InterVariable.ttf` exists (needed by Win32 `AddFontResourceEx`, which cannot load WOFF2), but
  `radios_package.COMMON_TREES["assets"]` (radios_package.py:34) does not allow `.ttf` → it would be dropped from
  packages.

---

## 2. Linux lifecycle today (complete description)

### 2.1 Entry points
- `run-radios.sh:3` computes the physical project dir (`cd -P`), exports `PYTHONDONTWRITEBYTECODE=1`;
  inside a Flatpak sandbox (`/.flatpak-info` + `flatpak-spawn`) it re-executes itself on the host with
  `RADIOS_ON_HOST=1` (L8-11); then `exec python3 tools/radios_launch.py --project-root <dir> "$@"` (L12).
- `run-database.sh:5` → `exec python3 tools/radios_database.py --project-root <dir> "$@"`.
- `run-radios-sdk.sh:5` → `tools/radios_sdk.py` → `radios_launch.main(force_sdk=True)` (radios_sdk.py:33-37).

### 2.2 `radios_launch.main` (radios_launch.py:71-131), in order
1. `parse_known_args` for `--project-root` (default `parents[1]` of the script) and `--config`; all original
   arguments (including `--project-root`) are later forwarded to the binary (L89).
2. Passive mode if any of `--version --help -h --help-all --check-config --smoke-test` is present (L79-80).
3. Non-passive: `build_current_sources(root)` (2.3).
4. Binary = `build/radios`, else `build/release/radios`, must be executable (L83-88).
5. `radios --version` probe (timeout 10 s); "error while loading shared libraries" / "version ... not found" in
   stderr → missing runtime → re-run inside the Flatpak SDK (`radios_sdk.sdk_command`, L90-97).
6. Passive → `os.execv` the binary (L98-99). Never touches DB/credentials.
7. Requires `WAYLAND_DISPLAY` or `DISPLAY` (L100-102) - no desktop → exit 1 before any identity exists.
8. `radios <args> --check-config` (timeout 15 s); non-zero → copy stderr, exit with its code (L105-109). The C
   binary is the authoritative option/INI parser.
9. Python config `setup.load_config(root, options.config)` (L112), `os.umask(0o077)` (L113).
10. Under `setup.operation_lock(cfg)` (L114): `recovering` = `data/database-service/setup-pending.json` exists;
    `fresh` = no `config/credentials.json`. If fresh or recovering: when fresh and **the port already answers TCP**
    → refuse ("port kullanımda"), else `setup.initialize(cfg)` (L118-122). Then `service.start_service(cfg)` (L125).
11. `os.execv(native...)` - same PID becomes the GUI (L126). Errors (`RuntimeError, OSError, ValueError,
    SubprocessError`) → `RadioS: <msg>` on stderr, exit 1 (L127-130).

### 2.3 Build-before-launch (radios_launch.py:22-68)
- Only for a source installation (`src/app/main.c` and `CMakeLists.txt` exist, L24). `RADIOS_SKIP_BUILD` skips (L26).
- Native toolchain = `cmake`, `pkg-config`, `cc` on PATH and `pkg-config --exists` for the dependency list
  (L31-41, includes `libpulse >= 15.0`). Otherwise (or `force_sdk`) cmake runs inside the SDK (L45-46).
- Serialised by an exclusive **blocking** `flock` on `<root>/default-binary.lock` (L50-51).
- Configure `cmake -S root -B build` (adds `-DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF` only if there is no
  `CMakeCache.txt`), build target `radios` with `--parallel min(4, cpus)`; cmake stdout goes to stderr (L53-60).
- Build failure: if an executable `build/radios` exists, warn and launch the last good binary; else re-raise
  (L61-68). Tested by `tests/test_radios_launch.py` (subprocess.run and shutil.which are monkeypatched).

### 2.4 Freedesktop SDK fallback (radios_sdk.py:15-30)
`flatpak run --socket=wayland --socket=fallback-x11 --socket=pulseaudio --share=network --filesystem=home
--filesystem=/run/media --filesystem=/media --filesystem=/mnt --talk-name=org.freedesktop.Flatpak
--filesystem=<root> --command=<binary> org.freedesktop.Sdk//25.08 <args>` (or via `flatpak-spawn --host flatpak`).
Only the C GUI enters the SDK; the DB service stays in the host PID namespace, which is why `data.c` and
`reset_ui.c` call `flatpak-spawn --host` when they find `/.flatpak-info`. **Nothing of this exists on Windows.**

### 2.5 First-run installation (`radios_database_setup.initialize`, L487-561)
1. Refuse if `restore-pending.json` exists (L494-496). `backend_import(cfg)` loads SDBASE from
   `database/SDBASE/src` under an isolated package name `_radios_sdbase_<sha256(src)[:16]>` (L201-239).
2. Private runtime dir `data/database-service` (0700, owner check, L498 → `_directory`).
3. `setup-pending.json` present → `_finish_pending` (resume). `credentials.json` present → validate and return
   `already_initialized`. Any existing `RadioS.sdb{,.meta,.lock,.checkpoint}` without credentials → refuse
   ("yönetilmiyor") (L499-508). Validate username/station/timezone (`ZoneInfo` - needs tzdata on Windows) (L509-515).
4. Stage = `data/database-service/setup-<uuid32>` (mkdir 0700, L516-517). SDBASE store is created in the stage,
   `bootstrap` (user/station/endpoint), admin + 30-day endpoint token, `credentials` dict (L519-536), checkpoint,
   chmod 0600 + fsync every stage file (L537-545), write `stage/credentials.json` (O_EXCL, 0600, fsync, dir fsync)
   (L546), validate credentials against the staged store (L547), write the pending manifest
   `setup-pending.json = {schema_version:1, stage_name, store_id, database_file, files:{name: sha256}}` (L548-552).
5. `_finish_pending` (L438-484): validates manifest, stage dir mode 0700, staged credentials, file digests; then
   publishes `.lock`, `.meta`, `.checkpoint`, journal, and finally `credentials.json` with `_publish` (no-replace:
   private temp in target dir → copy → fsync → digest → `os.link` → dir fsync; an existing identical target is
   accepted, a different one aborts). Then `load_credentials` validates the live install, removes the pending file
   (dir fsync) and best-effort removes the stage. Crash at any point = next `init`/launch resumes with the same
   identity (tests: `tests/test_database_recovery.py`).
6. Initial backup is **not** automatic: `run-database.sh backup` → `initial_backup` (radios_database.py:560-579)
   creates/verifies `data/backups/initial-installation` via `sdbase.backup`.

### 2.6 Service start (`start_service`, radios_database.py:429-506) and `serve` (L582-636)
- `effective_config` applies `config/storage.ini` (database/backups relocation, 2.9); `restore-pending.json` blocks;
  `_prepare_database` migrates the DB into a newly chosen folder; `load_credentials` (opens the store read-only and
  authenticates both tokens); expected digest = `backend_source_digest(cfg)` (setup L185-198).
- `service_status` running → `GET /v1/capabilities` (Bearer api_token): same `source_digest`,
  `desktop_api_version == 1`, not stale DB → `already_running`; else `stop_service` then restart (L439-452).
  Any other state than `stopped` → refuse (port in use / unhealthy) (L453-455).
- Log `data/logs/database-service.log` opened `O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW`, must be a regular file owned
  by uid, fchmod 0600 (L456-463).
- New `instance = uuid4()`; argv = `shlex.split(python_command) + [<root>/tools/radios_database.py, --project-root,
  <root>, --config, <cfg>, serve, --instance, <uuid>]`; env + `PYTHONDONTWRITEBYTECODE=1`;
  `Popen(stdin=DEVNULL, stdout=log, stderr=log, cwd=root, start_new_session=True, close_fds=True)` (L464-476).
- Wait up to `max(connect_timeout+3, 60 s)` (`SERVICE_START_TIMEOUT_SECONDS`, L91): child exit → error pointing at
  the log; `service.json` with our instance and `_owned` → `service_status` running → capabilities digest check →
  `_remember_database` → return `{..., already_running: false, restarted, source_digest}` (L479-496).
  Any exception (incl. KeyboardInterrupt) → terminate/kill **our own Popen child** (L497-506).
- `serve`: re-checks restore journal, loads credentials, imports `<pkg>.http_server`/`<pkg>.api_server`, opens
  `data/database-service/server.lock` (`O_RDWR|O_CREAT|O_NOFOLLOW`, 0600) and takes `flock(LOCK_EX|LOCK_NB)` for the
  whole lifetime (one service per installation), opens the store (can take ~14 s for a 290 MB checkpoint),
  `BoundedHTTPServer((127.0.0.1, port), max_connections=32, request_timeout)`, `daemon_threads=False`,
  `block_on_close=True`, `timeout=0.2`; SIGTERM/SIGINT set a `threading.Event`; writes `service.json`
  (2.7) atomically; prints `{"event":"started",...}` to the log; loops `handle_request()` until stopping;
  `finally` closes the server (joins in-flight requests), removes `service.json` only if it still carries its
  instance, closes the lock fd (L582-636).

### 2.7 Ownership / identity
`service.json` (`data/database-service/service.json`, 0600, written by `_write_json`: mkstemp in the same dir,
fsync, refuse symlink target, `os.replace`, dir fsync):
```json
{"schema_version":1,"pid":1234,"boot_id":"<linux boot_id>","start_ticks":"<field 22 of /proc/pid/stat>",
 "instance":"<uuid4>","project_root":"<abs>","database_file":"<abs>","service_url":"http://127.0.0.1:8000",
 "store_id":"<uuid>","source_digest":"<sha256 hex>"}
```
`_process_identity(pid)` (L76-88): `/proc/<pid>/stat` (zombie → None), `/proc/sys/kernel/random/boot_id`,
`/proc/<pid>/cmdline`. `_owned(cfg, state, same_database)` (L354-373) requires: schema 1; same `project_root`,
`service_url` (and `database_file` unless `same_database=False`); live identity; valid instance UUID; same boot id
and start ticks; argv **suffix** `[script, --project-root, root, --config, cfg, serve, --instance, instance]`.
A port that answers but is not owned is **foreign**: never adopted, never signalled (`_occupied` = TCP connect
within 0.25 s, L400-405). `service_status` (L408-426) → `running` (owned + `/health` ok with our `store_id`),
`foreign`, `unhealthy` (owned, no health), `stopped`, plus `pid`, `stale_database`.

### 2.8 Other commands
- `stop` (L509-532): owned check → `os.pidfd_open(pid)` (pins the process) → re-check `_owned` → `pidfd_send_signal(SIGTERM)`
  → poll `_owned` every 50 ms up to `min(request_timeout+3, 60)` → `stopped:true`; timeout → error "kapanışı sürüyor"
  (**never kills**). Foreign/none → `{"state": foreign|stopped, "stopped": false}`.
- `status` (no token auth), `check` (capabilities + metrics + integrity, schema 4, identity match).
- `main` (L639-677): `os.umask(0o077)`; every command except `serve` runs under `operation_lock` (bounded wait =
  `request_timeout_seconds`, 50 ms polling of `flock(LOCK_EX|LOCK_NB)` on `data/database-service/operation.lock`);
  stdout = one JSON object (`indent=2, ensure_ascii=False`); errors → `RadioS veritabanı: <msg>` on stderr, exit 1;
  Ctrl+C → 130.

### 2.9 storage.ini relocation (radios_database.py:93-347, mirrors `src/core/storage_locations.c`)
`config/storage.ini` `[storage] version=1, database_dir, recording_root, backup_root` (GKeyFile escapes, decoded by
`_keyfile_unescape`). Paths must be absolute POSIX, normalized, not `/`, not a symlink anywhere
(`realpath == path`), writable, outside the protected project subtrees, and contain `.radios-storage.json`
`{"format":"radios-storage-v1","role":...}`. A bad database folder is fatal; a bad backup folder falls back to
`data/backups`. A new database folder receives a one-time copy (`_migrate_database`: objects tree, sidecars, journal
last via stage+replace, dir fsync, `os.sync()`); `data/database-service/storage-last.json` remembers the last used DB.

### 2.10 Runtime files, permissions and locks

| Path (under project root unless noted) | Created by | Linux permission | Lock (holder → mode) |
|---|---|---|---|
| `default-binary.lock` | radios_launch | default | launcher → `LOCK_EX` blocking |
| `data/database-service/` | `_directory(private)` | 0700, owner == uid (repaired by chmod) | - |
| `…/operation.lock` | operation_lock | 0600, owner == uid | every CLI command → `LOCK_EX`, bounded poll |
| `…/server.lock` | serve; reliability | 0600 | serve → `LOCK_EX|LOCK_NB` for lifetime; reliability offline → `LOCK_EX|LOCK_NB` probe |
| `…/gui.lock` | C `radios_recovery_gui_lock` (recovery.c:358-376) | 0600, nlink 1, dir must be 0700 | GUI → `LOCK_SH|LOCK_NB` for lifetime; reliability → `LOCK_EX|LOCK_NB` probe |
| `…/service.json` | serve | 0600 (checked on read) | - |
| `…/storage-last.json` | `_remember_database` | 0600 | - |
| `…/setup-pending.json`, `…/setup-<hex32>/` | initialize | 0600 / 0700 | - |
| `…/restore-pending.json`, `restore-transaction-*`, `restore-preview-*`, `restore-outbox-*`, `media-quarantine-*`, `verify-*` | reliability | 0600 / 0700 | - |
| `data/database/RadioS.sdb.lock` | SDBASE storage | 0600 | SDBASE → SH/EX per operation; reliability offline → `LOCK_EX|LOCK_NB` |
| `data/recovery/<store>/<user>/` + `.lock` + `<op>.json` | C recovery.c | 0700 / 0600, owner == uid | C → `LOCK_EX` per operation; reliability → `LOCK_EX|LOCK_NB` |
| `config/credentials.json` | `_publish` | 0600, owner == uid (checked by Python and C `data_client.c:209-210`) | - |
| `data/logs/database-service.log` | start_service | 0600 | - |
| `data/backups/radios-<ts>-<hex8>/` | reliability | 0700 / files 0600 | - |
Everything is created under `umask 077`, so even plain `mkdir` results are 0700.

### 2.11 How the C app uses these tools (no C code reads `service.json`)
- `src/ui/data.c:376-402 start_local_service`: on a CONNECT probe failure of type transport/timeout/unavailable/
  schema-mismatch (`probe_done`, L403-418) it spawns `python_command <root>/tools/radios_database.py --project-root
  <root> --config <config_file> start` (prefixed with `flatpak-spawn --host` in a sandbox), stdout+stderr piped,
  read with `g_subprocess_communicate_utf8_async`, **15 s** timeout → `g_subprocess_force_exit` (L400, L369-375);
  success → CONNECT again (L353-368). Note: the Python side may legitimately need up to 60 s.
- `src/ui/admin_ui.c:694-710 tool_launch`: `python_command tools/radios_reliability.py --project-root R --config C
  <list|create|verify|restore-stage|restore-plan> [--backup NAME]`, 60 s timeout, parses stdout JSON;
  `restore-plan`'s `command` array is displayed POSIX-quoted with `g_shell_quote` (L655-663).
- `src/ui/reset_ui.c:335-371 radios_reset_ui_restart`: re-reads `/proc/self/cmdline`, drops `--project-root`, runs
  `sh -c "sleep 2; exec setsid run-radios.sh <args>"` (optionally via flatpak-spawn) and closes the window.
- `src/ui/window.c:718-723,783-785`: the presence of `config/credentials.json` decides whether the GUI takes
  `gui.lock` and creates the data context at all → **first-run init must finish before the window is created**.
- `src/core/recovery.c:358-376`: `gui.lock` needs an existing private `data/database-service` and fails if
  `restore-pending.json` exists; outbox dirs/locks (L135-169) are what reliability probes.
- `src/core/data_client.c:200-239,290-377`: reads `config/credentials.json` (0600 + uid), `<db>.meta`; compares the
  JSON `database_file` with the realpath-relative DB path (**POSIX form `data/database/RadioS.sdb`**); honours the
  `token_environment` variable; `installed_source_digest` (L894-918) recomputes the SDBASE digest in C
  (strcmp-sorted `*.py` names, `name\0bytes\0`).
- `src/core/storage_locations.c:61-152` shares `config/storage.ini` + marker rules with the Python service.
- `src/core/config.c:83-164`: root markers `config/radios.example.ini` + `database/SDBASE/pyproject.toml`, root =
  `--project-root` or walk up from `/proc/self/exe` (main.c:138) or cwd.

### 2.12 radios_reliability interplay with the lifecycle
Offline operations (`restore`, `recover`, `media-cleanup`) require `--offline --confirm-store-id`, refuse if the
`service.json` PID is alive, the port answers, or a GUI is detected (`_gui_open`: `/proc/*/exe` named `radios`
inside the project root and root in argv or cwd, L325-338), then hold `LOCK_EX|LOCK_NB` on `server.lock`, `gui.lock`,
the DB `.lock` and every outbox `.lock` while swapping files with a durable journal (`restore-pending.json`).

---

## 3. Contracts that must be preserved (and the tests that pin them)

| # | Contract | Exercised by |
|---|---|---|
| C1 | `radios_database.py` CLI: `[--project-root R] [--config C] init[--username --station-code --station-name --timezone] / start / status / check / stop / backup / serve --instance U`; stdout exactly one JSON object; errors on stderr prefixed `RadioS veritabanı:`; exit 0/1/130 | test_database_setup.py:119-140, test_release_install.py:99-103, all `tests/run_*_ui.py` (`python_command = sys.executable`) |
| C2 | Tokens never appear in argv, stdout or stderr | test_database_setup.py:105-117, test_release_install.py:85-97, test_reliability.py:450-458 |
| C3 | Idempotency: repeated `init`/`start` keep credentials bytes, IDs, PID | test_database_setup.py:247-255, 382-399; test_release_install.py:279-305 |
| C4 | Foreign listener is never adopted/stopped; launch fails before creating identity | test_database_setup.py:445-461; test_release_install.py:319-347 |
| C5 | Digest-driven restart: changed SDBASE sources → `check` reports "eski kaynak kod", `start` restarts (new pid), next `start` is `already_running` | test_database_setup.py:355-380; run_data_connection_ui `outdated-service-restarts-on-host` |
| C6 | `service.json` schema v1 + `_owned` predicate (argv suffix!) | test_storage_locations.py:209-223 (mocks `_process_identity` returning `(boot, ticks, argv)`) |
| C7 | Interrupted first install resumes with the same identity; conflicting target preserved | test_database_recovery.py |
| C8 | `credentials.json` schema v1, `database_file` = project-relative POSIX path; C and Python agree | data_client.c:323-333, setup L348 |
| C9 | Source digest algorithm identical in tools (setup L185-198), SDBASE (`runtime_version.source_digest`) and C (`data_client.c:894-918`) | C5 tests; GUI handshake |
| C10 | Lock topology of 2.10 (file names, SH vs EX, who holds what) shared by Python and C | test_reliability.py:291-318, test_release_install.py:144-154, test_application_shutdown.py:114-127 |
| C11 | Private files/dirs: credentials, runtime dir, outbox, backups | test_database_setup.py:228,292-307; test_database_recovery.py:57; test_reliability.py:121-124 |
| C12 | Passive modes never initialise; invalid options/INI fail before init; no desktop → no identity | test_release_install.py:241-268 |
| C13 | `restore-pending.json` blocks launch/start/init/GUI | test_release_install.py:349-360, test_reliability.py:389-399 |
| C14 | storage.ini syntax/marker/link rules identical to C | test_storage_locations.py:88-135 |
| C15 | Launcher build behaviour (skip flag, last-good fallback, hard failure without binary, lock file) | test_radios_launch.py |
| C16 | Package determinism, allowlist, no private data, verification rejects tampering | test_packaging.py, run_release_acceptance.py |
| C17 | reliability JSON contract consumed by admin_ui.c (`status`, `backups[]`, `name`, `sequence`, `restored_path`, `store_id`, `integrity`, `command[]`, `note`, `error`) | tests/run_admin_ui.py |

All of these keep their meaning on Windows. Platform-specific *values* (`boot_id`, `start_ticks`, absolute paths
with backslashes) are only ever compared with values produced on the same OS by the same code.

---

## 4. Windows design

### 4.1 (a) User double-clicks `RadioS.exe` - what happens, where the logic lives

**Decision: the C app is the entry point and spawns the Python `ensure` helper.** Rejected alternative: a Python
launcher that spawns the GUI (shortcut → `pythonw.exe radios_launch.py`): it cannot `exec`, breaks taskbar
pinning/AppUserModelID/jump lists (the pinned process would be python), doubles process lifetime management, and
puts an interpreter in front of every operator launch. Keeping the lifecycle in Python avoids re-implementing
the init/publish/ownership code in C (SDBASE is Python anyway, and the helpers already exist for the GUI's
reconnect path).

Startup sequence of `RadioS.exe` (Windows build; C area implements, contract defined here):
1. `GetModuleFileNameW` → walk up for root markers (`config\radios.example.ini`, `database\SDBASE\pyproject.toml`)
   or honour `--project-root`. Product: `<root>\bin\RadioS.exe`; dev: `<root>\build\radios.exe`.
2. Option parsing. Passive modes (`--version`, `--help*`, `--check-config`, `--smoke-test`, new `--audio-inventory`):
   `AttachConsole(ATTACH_PARENT_PROCESS)` for console output, never run Python, never touch `data\`.
3. `radios_config_load` (C) - authoritative INI validation (replaces the launcher's `--check-config` probe).
4. `gtk_init_check` - no interactive desktop → error before identity creation (replaces the DISPLAY check;
   keeps test C12).
5. Unless `RADIOS_SKIP_ENSURE=1` (tests/dev): show a small modal "Veritabanı hazırlanıyor…" window and spawn
   `argv = python_argv(python_command) + [<root>\tools\radios_database.py, --project-root, <root>, --config,
   <config_file>, ensure]` with `CREATE_NO_WINDOW`, stdout/stderr pipes (UTF-8), cwd = root, handles not inherited
   except the pipes. Wait asynchronously (main loop keeps running), no hard timeout but a Cancel button
   (→ `TerminateProcess` of the helper only). Budget: operation lock wait (≤ request_timeout) + init + 60 s start.
6. Exit code 0 → continue (create GtkApplication/window exactly as today; `credentials.json` now exists so the GUI
   takes `gui.lock` and connects). Non-zero → `GtkMessageDialog` with the last stderr line (prefix
   `RadioS veritabanı: ` stripped) and exit 1 (Linux parity: the launcher prints and returns 1).
7. Later reconnects keep using `data.c start_local_service` (`start`), unchanged except spawning rules (4.15).

New subcommand (Python, both OSes) - `radios_database.py ensure`:
```python
def ensure_installation(cfg: LocalConfig) -> dict:
    """First launch: finish/perform init, then start or adopt this installation's own service.
    Caller holds operation_lock (main() does, like every other command)."""
    # Windows equivalent of "everything is created under umask 077" (4.8); POSIX: no-op.
    for folder in {cfg.project_root / "data", cfg.credentials_file.parent, cfg.runtime_dir,
                   cfg.database_file.parent, cfg.log_root, cfg.backup_root, cfg.endpoint_root, cfg.import_root}:
        if folder.is_dir():
            radios_platform.ensure_private_tree(folder)   # only if owned by us and not yet private
    pending = cfg.runtime_dir / "setup-pending.json"
    recovering = pending.exists() or radios_platform.is_link(pending)
    fresh = not cfg.credentials_file.exists() and not radios_platform.is_link(cfg.credentials_file)
    installation = None
    if fresh or recovering:
        if fresh and not recovering and _occupied(cfg):
            raise DatabaseError("Yerel servis portu kullanımda; başka servise müdahale edilmedi. "
                                "config/radios.ini içindeki service_url için boş bir port seçin.")
        installation = initialize(cfg)                     # summary contains no tokens
    return {"initialized": installation is not None, "installation": installation,
            "service": start_service(cfg), "warnings": radios_platform.install_warnings(cfg.project_root)}
```
`radios_launch.main` (Linux) calls it in-process instead of its inline block (L110-125) - no behaviour change on
Linux. `install_warnings` reports e.g. `project_root_writable_by_others`, `onedrive_folder`,
`controlled_folder_access_risk` (strings; the GUI may show them as a notice).

### 4.2 Python runtime and the C↔Python invocation contract

Resolution of `[database] python_command` (implemented identically in C `config.c`/spawners and in
`radios_platform.python_argv`):
1. Windows, value `python3` or `python` (the shipped default) → `<root>\runtime\python\python.exe`; if missing and
   the caller is itself Python → `sys.executable`; C → error "Python çalışma ortamı bulunamadı (runtime\python)".
2. Value containing `\` or `/` or a drive → path, relative to the project root if not absolute; must exist and end
   in `.exe`. Never search `PATH` on Windows (`python3.exe` in `%LOCALAPPDATA%\Microsoft\WindowsApps` is the Store
   alias that opens the Store or prints "Python was not found").
3. POSIX: unchanged (`shlex.split`, PATH lookup).
4. Validation regex at setup.py:165-167 becomes per platform: Windows accepts `[^\x00-\x1f"<>|?*]+` without
   leading/trailing whitespace (spaces inside paths are fine because nothing is shell-split on Windows; do **not**
   apply `shlex.split` to Windows paths - backslashes are escapes in POSIX shlex).

Interpreter flags prepended on Windows, always: `-E -s -B -X utf8`. `-E -s` ignore `PYTHON*` variables and the
user site (a global `PYTHONHOME`/`PYTHONPATH` from another product makes the embedded interpreter fail with
"init_fs_encoding" or import foreign modules; redundant but harmless when a `._pth` already isolates it, and they
also protect dev interpreters). `-B` replaces `PYTHONDONTWRITEBYTECODE` and `-X utf8` replaces `PYTHONUTF8`, both of
which `-E`/isolation ignore. `RADIOS_*` variables are unaffected. Flags precede the script, so `_owned`'s
argv-suffix check is unaffected.

Recommended `runtime\python\python3XX._pth` (generated by packaging, 4.17.2):
```
python3XX.zip
.
tzdata.zip
..\..\tools
```
(`tzdata.zip` = the pinned tzdata wheel's `tzdata/` package zipped; or keep `Lib` if tzdata stays extracted.)
Effects: isolated mode (env vars, registry, user site ignored), script directory NOT on `sys.path` → the
`..\..\tools` line is what lets `radios_database.py` import `radios_database_setup` and `radios_platform`;
SDBASE is loaded by file location (`backend_import`) so it needs no path entry. `zoneinfo` needs tzdata on Windows
(no system zoneinfo) - `initialize()` validates `ZoneInfo(timezone)`, even `"UTC"` fails without tzdata.
Dev interpreters (MSYS2 `ucrt64\bin\python.exe` used by CMake/CTest) need the tzdata package installed too.

Optional perf: replace `-B` by `-X pycache_prefix=<root>\data\cache\pycache` (bytecode cached outside the source
tree, so the source digest and packages stay clean) - startup compiles ~117 SDBASE modules otherwise.

UTF-8 contract: every tool's `main()` starts with `radios_platform.configure_stdio_utf8()`
(`sys.stdout/stderr.reconfigure(encoding="utf-8", errors="backslashreplace")` when not already UTF-8). Without it a
Turkish-locale Python writes cp1254 into the GUI's pipe and `g_subprocess_communicate_utf8` fails → the GUI reports
"Yerel veri servisi açılamadı" although the service started.

### 4.3 `tools/radios_platform.py` - the tools-side shim (API)

Single module, stdlib + `ctypes` + `msvcrt` (Windows) / `fcntl` (POSIX). POSIX branches keep today's exact
behaviour so Linux stays green. Constants and privacy rules must equal `sdbase/_platform.py` (contract test
`tests/test_platform_contract.py` imports both and compares `FLOCK_OFFSET`, `FLOCK_LENGTH`, `PRIVATE_DIR_SDDL`,
`PRIVATE_FILE_SDDL`, `PRIVATE_ALLOWED_SIDS`).

```python
IS_WINDOWS: bool
O_BINARY = getattr(os, "O_BINARY", 0); O_NOINHERIT = getattr(os, "O_NOINHERIT", 0)
O_NOFOLLOW = getattr(os, "O_NOFOLLOW", 0); O_DIRECTORY = getattr(os, "O_DIRECTORY", 0)
O_NONBLOCK = getattr(os, "O_NONBLOCK", 0); O_CLOEXEC = getattr(os, "O_CLOEXEC", 0)
LOCK_SH, LOCK_EX, LOCK_NB, LOCK_UN = 1, 2, 4, 8            # same numbers as fcntl
FLOCK_OFFSET = 0x7FFFFFFF00000000; FLOCK_LENGTH = 1          # cross-language contract (4.7)
PRIVATE_DIR_SDDL  = "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)"
PRIVATE_FILE_SDDL = "D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)"
PRIVATE_ALLOWED_SIDS = {"S-1-5-18", "S-1-5-32-544", "S-1-3-4"}  # + current user SID at runtime

def configure_stdio_utf8() -> None
def flock(fd_or_file, operation: int) -> None                # BlockingIOError(EWOULDBLOCK) on LOCK_NB contention
def open_nofollow(path, flags, mode=0o600, *, private=False) -> int
    # POSIX: os.open(path, flags|O_NOFOLLOW|O_CLOEXEC, mode)
    # Windows: CreateFileW(FILE_SHARE_READ|WRITE|DELETE, FILE_FLAG_OPEN_REPARSE_POINT, SD=PRIVATE_FILE_SDDL when
    #   private and creating) → reparse-tag check → OSError(ELOOP) → msvcrt.open_osfhandle(..., O_BINARY semantics)
    # O_NONBLOCK/O_DIRECTORY ignored on Windows; opening a directory raises PermissionError (callers catch OSError)
def open_log_append(path) -> int                             # Windows: FILE_APPEND_DATA|SYNCHRONIZE|READ_CONTROL only
def is_link(path_or_stat) -> bool                            # S_ISLNK or st_reparse_tag & 0x20000000 (junctions!)
def is_private(path_or_fd, *, directory=False, exact_file_mode=True) -> bool
def owned_by_current_user(path_or_fd) -> bool
def make_private(path_or_fd, *, directory=False) -> None     # POSIX chmod 0600/0700; Windows SetNamedSecurityInfoW
def make_private_dir(path, *, parents=False, exist_ok=True) -> None
def private_mkstemp(dir, prefix="", suffix="") -> tuple[int, str]   # Windows: CREATE_NEW with PRIVATE_FILE_SDDL
def ensure_private_tree(path) -> None                        # Windows: protected inheritable DACL if owned & not private
def acl_supported(path) -> bool                              # GetVolumeInformationW FILE_PERSISTENT_ACLS
def fsync_file(path) -> None                                 # opens with write access (FlushFileBuffers needs it)
def fsync_dir(path) -> None                                  # POSIX dir fsync; Windows no-op
def sync() -> None                                           # POSIX os.sync; Windows no-op
def replace(src, dst, *, timeout=2.0) -> None                # MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)+retry
def publish_noreplace(src, dst) -> None                      # POSIX os.link+keep src; Windows MoveFileExW(WRITE_THROUGH)
def unlink(path, *, missing_ok=False, timeout=2.0) -> None   # retry on sharing violation
def rmtree(path, *, timeout=2.0) -> None
def python_argv(python_command: str, project_root: Path) -> list[str]   # 4.2
def display_command(argv: list[str]) -> str                  # shlex.join / subprocess.list2cmdline
def boot_id() -> str                                         # Linux boot_id; Windows "win32"
def boot_time_iso() -> str                                   # informational only
def process_identity(pid) -> tuple[str, str, list[str]] | None
class PinnedProcess:                                         # pidfd / OpenProcess handle
    @classmethod
    def open(cls, pid) -> "PinnedProcess | None"             # None = no such process
    def start_ticks(self) -> str | None; def alive(self) -> bool
    def wait(self, timeout: float) -> bool; def terminate(self) -> None; def close(self) -> None
    def signal_term(self, instance: str) -> None             # POSIX pidfd SIGTERM; Windows SetEvent(stop event)
def stop_event_name(instance: str) -> str                    # "Local\\RadioS-SDBASE-" + instance
def create_stop_event(instance: str) -> object               # serve side; refuses ERROR_ALREADY_EXISTS
def stop_event_is_set(handle) -> bool                        # WaitForSingleObject(h, 0) == WAIT_OBJECT_0
def spawn_detached(argv, *, cwd, env, log_fd) -> subprocess.Popen
def iter_processes() -> Iterator[ProcessInfo]                # pid, image_path, argv|None (Toolhelp32 / /proc)
def request_graceful_exit(pid: int) -> bool                  # GUI: SetEvent Local\RadioS-GUI-<pid>, else WM_CLOSE
def no_proxy_opener() -> urllib.request.OpenerDirector       # ProxyHandler({}) for 127.0.0.1 scripts
def install_warnings(root) -> list[str]
```

Windows ctypes signatures that the implementation needs (declare `argtypes`/`restype`, `use_last_error=True`):
- kernel32: `CreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) -> HANDLE`,
  `GetFileInformationByHandleEx(HANDLE, FileAttributeTagInfo=9, FILE_ATTRIBUTE_TAG_INFO*, DWORD)`,
  `LockFileEx(HANDLE, DWORD flags, DWORD reserved, DWORD lenLow, DWORD lenHigh, OVERLAPPED*)`,
  `UnlockFileEx(HANDLE, DWORD, DWORD, DWORD, OVERLAPPED*)`, `MoveFileExW(LPCWSTR, LPCWSTR, DWORD)`,
  `FlushFileBuffers(HANDLE)`, `OpenProcess(DWORD, BOOL, DWORD)`, `GetProcessTimes(HANDLE, FILETIME*×4)`,
  `QueryFullProcessImageNameW(HANDLE, DWORD, LPWSTR, PDWORD)`, `WaitForSingleObject(HANDLE, DWORD)`,
  `TerminateProcess(HANDLE, UINT)`, `CreateEventW(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR)`,
  `OpenEventW(DWORD, BOOL, LPCWSTR)`, `SetEvent(HANDLE)`, `CloseHandle(HANDLE)`, `GetTickCount64()`,
  `GetSystemTimeAsFileTime(FILETIME*)`, `CreateToolhelp32Snapshot(DWORD, DWORD)`, `Process32FirstW/NextW`,
  `IsProcessInJob(HANDLE, HANDLE, PBOOL)`, `GetVolumeInformationByHandleW(...)`, `SetErrorMode(UINT)`, `LocalFree`.
- advapi32: `ConvertStringSecurityDescriptorToSecurityDescriptorW(LPCWSTR, DWORD=1, PSECURITY_DESCRIPTOR*, PULONG)`,
  `GetSecurityInfo(HANDLE, SE_FILE_OBJECT=1, SECURITY_INFORMATION, PSID*, PSID*, PACL*, PACL*, PSECURITY_DESCRIPTOR*)`,
  `GetNamedSecurityInfoW`, `SetNamedSecurityInfoW(LPWSTR, 1, DACL|PROTECTED_DACL_SECURITY_INFORMATION, NULL, NULL,
  PACL, NULL)`, `GetSecurityDescriptorDacl`, `GetAclInformation(PACL, ACL_SIZE_INFORMATION*, DWORD, AclSizeInformation=2)`,
  `GetAce(PACL, DWORD, LPVOID*)`, `EqualSid`, `ConvertSidToStringSidW`, `OpenProcessToken`,
  `GetTokenInformation(TokenUser=1 / TokenElevation=20)`.
- ntdll: `NtQueryInformationProcess(HANDLE, 60, PVOID, ULONG, PULONG) -> NTSTATUS` (buffer = UNICODE_STRING
  `{USHORT Length; USHORT MaximumLength; PWSTR Buffer}` followed by the text; first call gets the size,
  `STATUS_INFO_LENGTH_MISMATCH = 0xC0000004`).
- shell32: `CommandLineToArgvW(LPCWSTR, int*) -> LPWSTR*` (free with `LocalFree`).
- user32 (only `request_graceful_exit`): `EnumWindows`, `GetWindowThreadProcessId`, `PostMessageW(WM_CLOSE=0x10)`.

### 4.4 (c) Process identity on Windows
```python
def process_identity(pid):
    if type(pid) is not int or pid <= 4: return None                  # 0 idle, 4 System
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, False, pid)
    if not h: return None                                             # gone (87) or other user (5) → not ours
    try:
        if WaitForSingleObject(h, 0) == WAIT_OBJECT_0: return None    # exited; object kept alive by a handle (Linux "Z")
        creation = FILETIME(); ...; GetProcessTimes(h, creation, exit_, kernel, user)
        ticks = (creation.dwHighDateTime << 32) | creation.dwLowDateTime   # 100 ns since 1601-01-01 UTC
        argv = _command_line_argv(h)                                  # NtQueryInformationProcess(60)+CommandLineToArgvW
        return None if argv is None else ("win32", str(ticks), argv)
    finally: CloseHandle(h)
```
- `start_ticks` keeps its JSON name (value semantics are per-OS). Creation FILETIME is absolute and is not changed by
  later clock adjustments, so `(pid, creation)` is unique across reboots in practice; a boot id adds nothing and the
  GetTickCount64-derived boot time (`now_filetime - tick64*10_000`) shifts with NTP/manual clock changes → comparing
  it would turn our own service into "foreign" and block every launch ("port kullanımda"). Hence the constant
  `boot_id = "win32"` keeps `_owned` unchanged; `service.json` additionally gets `"boot_time"` (ISO, informational).
- `argv` suffix comparison works because the start helper builds `serve` argv from the same `str(Path.resolve())`
  values, Popen serialises with `list2cmdline`, and `CommandLineToArgvW` round-trips it.
- `ProcessCommandLineInformation` needs Windows 8.1+ (OK for 10/11) and only `PROCESS_QUERY_LIMITED_INFORMATION`.
  Mixing elevated and non-elevated runs can deny `OpenProcess` → "foreign" (document: never run RadioS elevated).
- `serve` computes its own identity with the same function (`os.getpid()`), so writer and checker agree.

### 4.5 (d) Detached spawn and log redirection
```python
def spawn_detached(argv, *, cwd, env, log_fd):
    common = dict(stdin=subprocess.DEVNULL, stdout=log_fd, stderr=log_fd, cwd=cwd, env=env, close_fds=True)
    if not IS_WINDOWS:
        return subprocess.Popen(argv, start_new_session=True, **common)
    flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
    try:
        return subprocess.Popen(argv, creationflags=flags | subprocess.CREATE_BREAKAWAY_FROM_JOB, **common)
    except PermissionError:          # job without JOB_OBJECT_LIMIT_BREAKAWAY_OK
        return subprocess.Popen(argv, creationflags=flags, **common)
```
- `start_new_session=True` is **silently ignored** on Windows: the child would share the console of
  `run-database.cmd start` (closing that window or Ctrl+C would hit the service). `CREATE_NO_WINDOW` gives it its own
  hidden console (console children of SDBASE never flash a window); `CREATE_NEW_PROCESS_GROUP` disables Ctrl+C for it.
  Do not combine with `DETACHED_PROCESS` (then `CREATE_NO_WINDOW` is ignored).
- Breakaway: if RadioS.exe runs inside a kill-on-close job (VS Code terminal, some launchers, Task Scheduler), the
  service would die with the GUI; Linux semantics ("C penceresinin kapanması veri servisini kapatmaz") need breakaway.
- `close_fds=True` with explicit std handles makes Popen pass only those via `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`.
  This is essential: if `serve` inherited the GUI→helper stdout pipe, `g_subprocess_communicate_utf8` in data.c would
  never see EOF.
- Log file: open with `open_log_append` = `CreateFileW(path, FILE_APPEND_DATA|SYNCHRONIZE|READ_CONTROL|
  FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, private SA, OPEN_ALWAYS,
  FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT)` + reparse check + owner/privacy check → `open_osfhandle`.
  Reason: CRT `O_APPEND` is emulated per CRT write (seek-to-end) and is **not** a property of the OS handle; the child
  writes through its own CRT fd made from the inherited handle, starting at offset 0 → it would overwrite the log.
  A handle with only `FILE_APPEND_DATA` appends atomically.
- Environment: `dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONUTF8="1")`; interpreter argv from `python_argv`.
- Bind pre-check before spawning (Windows): bind a probe socket to `(127.0.0.1, port)`; `WSAEACCES (10013)` means the
  port is inside a Hyper-V/WSL/Docker *excluded port range* (`netsh int ipv4 show excludedportrange protocol=tcp`)
  although nothing listens - fail early with "Port Windows tarafından ayrılmış; service_url için başka port seçin"
  instead of a child that dies with a traceback in the log. `serve` prints the same message on 10013/10048.

### 4.6 (e) Graceful stop
Server side (`serve`):
```python
stop_event = radios_platform.create_stop_event(instance)    # CreateEventW(NULL, manual=TRUE, initial=FALSE, name)
...write service.json (adds "stop_event": stop_event_name(instance), "platform": sys.platform, "boot_time")...
while not stopping.is_set() and not radios_platform.stop_event_is_set(stop_event):
    server.handle_request()                                  # server.timeout = 0.2 → ≤ 0.2 s reaction
```
- `signal.signal(SIGINT, …)` stays (foreground `serve` in a dev console); add `SIGBREAK`. `SIGTERM` handlers never
  fire on Windows from outside.
- `ERROR_ALREADY_EXISTS` on create → refuse to start (squatting; the instance UUID is random and private).
Client side (`stop_service`):
```python
state = _read_json(_state_path(cfg))
if not _owned(cfg, state, same_database=False): return {...foreign|stopped...}
proc = PinnedProcess.open(state["pid"])         # SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE
if proc is None: return {"state": "stopped", "stopped": False, ...}       # == ProcessLookupError branch
try:
    if not _owned(cfg, state, same_database=False) or proc.start_ticks() != state["start_ticks"]:
        raise DatabaseError("Servis süreç kimliği değişti; kapatma uygulanmadı.")
    proc.signal_term(state["instance"])         # OpenEventW(EVENT_MODIFY_STATE) + SetEvent; missing event → refuse, do not kill
    deadline = time.monotonic() + min(cfg.request_timeout_seconds + 3, 60)
    while time.monotonic() < deadline:
        if proc.wait(0.05) or not _owned(cfg, state, same_database=False):
            return {"state": "stopped", "stopped": True, ...}
    if not force: raise DatabaseError("Servis kapanışı sürüyor; açık isteklerin bitmesi bekleniyor.")
    proc.terminate(); proc.wait(5)              # --force only
    _drop_stale_state(cfg, state["instance"])   # serve could not clean up
    return {"state": "stopped", "stopped": True, "forced": True, ...}
finally: proc.close()
```
- A held process handle keeps the PID from being recycled for exactly as long as a pidfd would.
- `stop --force` (new, both OSes; POSIX uses `pidfd_send_signal(SIGKILL)`): used by the NSIS (un)installer and
  operators. Default `stop` keeps the documented guarantee "zorla işlem kesmez" (docs/05-veritabani-kurulumu.md:29).
  SDBASE is crash-safe by design (fsynced journal + tail repair), which is what makes `--force` acceptable at all.
- Logoff/shutdown (optional, phase 2): console processes do not get `CTRL_LOGOFF/SHUTDOWN` (services only); a hidden
  top-level window (not message-only) in a `serve` thread can handle `WM_QUERYENDSESSION/WM_ENDSESSION` → set
  stopping and finish in-flight requests within the ~5 s budget. Without it the service is terminated like any
  process, which SDBASE survives.

### 4.7 Locks on Windows (flock emulation contract)
- `flock(fd, op)` → handle `msvcrt.get_osfhandle(fd)`; `OVERLAPPED.Offset = FLOCK_OFFSET & 0xFFFFFFFF`,
  `OffsetHigh = FLOCK_OFFSET >> 32`, length 1; `LOCK_EX` → `LOCKFILE_EXCLUSIVE_LOCK`, `LOCK_SH` → shared,
  `LOCK_NB` → `LOCKFILE_FAIL_IMMEDIATELY`; `ERROR_LOCK_VIOLATION (33)` → `BlockingIOError(EWOULDBLOCK)` (existing
  `except BlockingIOError` loops keep working); `LOCK_UN` → `UnlockFileEx` (ignore `ERROR_NOT_LOCKED 158`).
- The byte range must be identical in `src/core/platform.c` (gui.lock, outbox `.lock`), `sdbase/_platform.py`
  (`RadioS.sdb.lock`) and this shim (operation/server/gui/outbox/DB probes) - otherwise the C GUI's `LOCK_SH` on
  `gui.lock` and reliability's `LOCK_EX|LOCK_NB` probe never conflict and restore could run under an open GUI.
  The range lies far beyond any data so locks never block real reads/writes (Windows byte-range locks are mandatory).
  Proposed: `0x7FFFFFFF00000000`, length 1 (below 2^63: `NtLockFile` takes signed LARGE_INTEGER offsets).
- Semantics that differ from flock and must be respected: locks belong to the file object (handle), not the fd
  number - an **inherited** handle keeps the lock alive in the child (C must open lock files non-inheritable); no
  atomic SH→EX upgrade and no re-lock of the same handle (current code never does either); the OS releases locks of
  a terminated process "eventually" → always `UnlockFileEx` before `close` (setup L280-281, reliability L399-401,
  serve L636) and keep the bounded retry loops.
- `LockFileEx` works on read-only handles (SDBASE opens `.lock` with `"rb"`).

### 4.8 (f) Privacy on Windows - decision and algorithm
Options considered: (1) rely on the user-profile ACL; (2) icacls via subprocess; (3) explicit DACLs via ctypes.
**Choice: (3).** (1) fails in real setups: this machine's Desktop grants `CodexSandboxUsers` read on everything
below it (observed on `config\credentials.json`), a portable copy on `D:\` inherits `Authenticated Users: Modify`, a
folder under `C:\` inherits `Users: Read & execute`. (2) is locale-dependent text parsing and an extra process per
check. (3) is what CPython itself does for `os.mkdir(path, 0o700)`/`tempfile.mkdtemp` since 3.12.4/3.13.

- Creation:
  - Private directories: `Path.mkdir(mode=0o700)` already applies `PRIVATE_DIR_SDDL` on Python ≥ 3.12.4/3.13 (only
    for the leaf; parents get default ACLs). `make_private_dir` additionally verifies and, if the dir pre-existed and
    is owned by us, repairs it (Linux `_directory` does `chmod(0o700)` - same "repair if owned" semantics).
  - Private files: `open_nofollow(..., private=True)` / `private_mkstemp` create with `PRIVATE_FILE_SDDL` via
    `SECURITY_ATTRIBUTES` (atomic: no window in which the file is readable). Files created inside a private dir
    inherit SY/BA/OW automatically (mkstemp in `data\database-service` is fine).
  - `ensure_private_tree(root\data)` and `ensure_private_tree(root\config)` in `ensure` (first run): if owned by the
    current user and not private → `SetNamedSecurityInfoW(DACL|PROTECTED_DACL, PRIVATE_DIR_SDDL)` (propagates to
    children). This is the Windows equivalent of running everything under `umask 077`. Same for storage.ini roots
    carrying a RadioS marker (C `radios_storage_directory_prepare` should apply it when creating the marker).
- Check `is_private(target)` (replaces `st_uid == getuid()` and `mode & 0o077 == 0` / `S_IMODE == 0o600|0o700`):
  1. `GetSecurityInfo(handle or GetNamedSecurityInfoW(path), OWNER|DACL)`; NULL DACL → False.
  2. Owner SID == token user SID, or == `S-1-5-32-544` while the current token is elevated.
  3. For each ACE (skip `INHERIT_ONLY_ACE 0x08`): allow types (0, 5, 9, 11) whose SID ∉ {user, SY, BA, OW} and whose
     mask has any of `FILE_READ_DATA|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_EXECUTE|DELETE|WRITE_DAC|WRITE_OWNER|
     GENERIC_*` → False. Deny ACEs are ignored.
  4. Volume without `FILE_PERSISTENT_ACLS` (FAT/exFAT) → False (fail closed) for `config\` and
     `data\database-service\`; storage.ini roles (database/recordings/backups on USB disks) may be accepted with a
     logged warning - product decision, Linux rejects vfat in practice.
  `owned_by_current_user` = step 2 only. `fd` variants use `msvcrt.get_osfhandle(fd)` (GENERIC_READ/WRITE handles
  carry READ_CONTROL).
- Messages: the Turkish texts mention "0600"/"0700" (setup L83, L96, database L48, C data_client.c:213); make them
  platform-neutral ("yalnız kullanıcıya açık olmalı (Linux 0600 / Windows özel ACL)").
- New CLI `radios_database.py repair-permissions` (operator, under operation lock): re-applies private DACLs to
  `config\credentials.json`, `data\` (and storage roots) when owned by the current user, and prints a warning to
  rotate tokens if they were exposed. Readers stay fail-closed (tests C11).
- Copying an installation to another PC/user: files get the copier as owner but inherited ACLs → readers fail closed
  → `repair-permissions` documented in `config/README.md`.

### 4.9 Atomic replace, publish, fsync, delete
- `_write_json`, `_migrate_database` stage, reliability `_move`: `replace()` = `MoveFileExW(src, dst,
  MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`, retry up to ~2 s on 5/32/33 (Defender, Search indexer, our own
  readers, backup agents). Python's plain `open()` does not use `FILE_SHARE_DELETE`, so any reader holding
  `service.json` blocks the replace; all our readers go through `open_nofollow` (share-delete) and C readers through
  the platform layer with `FILE_SHARE_DELETE` (cross-area) - retries cover third parties.
- `_publish` (setup L405-425): `publish_noreplace(temp, target)` - on Windows a rename without REPLACE fails with
  `ERROR_ALREADY_EXISTS`/`ERROR_FILE_EXISTS` → raise `FileExistsError` (same branch as Linux `EEXIST`); works on
  FAT/exFAT where `CreateHardLinkW` does not; the temp is consumed, `finally: unlink(missing_ok=True)` stays.
- `os.fsync(fd)` → `FlushFileBuffers`, which requires a handle with write access: `_copy_fsync` (database.py:288-292)
  and `initialize` (setup L541-545) open `O_RDONLY` → `fsync_file()` must open `O_RDWR|O_BINARY`.
- Directory fsync (database.py:67-71, 322-326; setup L284-289; package L401-405) → `fsync_dir` no-op on Windows (you
  cannot `os.open` a directory there); durability of renames comes from `MOVEFILE_WRITE_THROUGH` + NTFS journaling.
- `os.sync()` (database.py:327) → `sync()` no-op on Windows (copied files were already flushed one by one).
- Deleting `service.json` in `serve`'s finally and `rmtree` of stages → `unlink`/`rmtree` with retry; failures must
  not raise out of `finally` (stale state is harmless because identity is re-checked).
- Directories cannot be renamed while any process has a file open inside or uses them as CWD (the service runs with
  cwd = project root). Restore only moves `data\recovery\<store>` and DB sidecars - all closed when offline - but
  `restore` must drop its own Store objects before `_move` (call `gc.collect()`; ask SDBASE for an explicit
  `Store.close()` if any handle becomes persistent).

### 4.10 Sockets, ports, HTTP
- `ThreadingHTTPServer.allow_reuse_address = 1` → on Windows `SO_REUSEADDR` lets a *second* listener bind the same
  `127.0.0.1:8000` (another RadioS copy, a test server) - connections would be split between two stores. On Windows
  set `allow_reuse_address = False` before `server_bind` (in `sdbase.api_server.BoundedHTTPServer`, or a subclass in
  `serve`). Rebinding a Windows listener while old connections sit in TIME_WAIT is expected to work without
  SO_REUSEADDR (unlike Linux) - prove it with C5 (stop → immediate start on the same port) on Windows.
  `SO_EXCLUSIVEADDRUSE` would also block hijacking, but MS documents that an exclusive socket cannot rebind until
  accepted connections are gone - only enable it if C5 still passes.
- Binding to `127.0.0.1` never triggers the Windows Firewall prompt; keep the `127.0.0.1`-only validation
  (setup L156-163).
- `_occupied` (database.py:400-405): Windows retries SYN after RST, so a refused loopback connect can take ~1 s; the
  0.25 s timeout turns it into `TimeoutError` → `OSError` → "not occupied" (correct, just slower).
- `http.client` (database.py:376-397) ignores proxies - good. Every `urllib.request` use against 127.0.0.1
  (otomasyon_testleri/api.py:24, yayin_akisi_724/rapi.py:21,32, several tests) must use `no_proxy_opener()`: on
  Windows urllib reads the *registry* proxy (`Internet Settings\ProxyServer`) and `<local>` bypass does not match
  `127.0.0.1`.

### 4.11 Paths, case, separators
- JSON that crosses platforms or languages stores **POSIX relative paths**: `credentials.json` `database_file`
  (setup L348, L531), `setup-pending.json` `database_file` (L441, L550) → `.relative_to(root).as_posix()`
  (backup manifests already do this, reliability L143-144, 233-234). On read accept either separator. C side must
  compare `/`-normalised strings (cross-area).
- Absolute paths written and compared by the same Python code on the same OS (`service.json` `project_root`,
  `database_file`; `restore-pending.json`; `storage-last.json`) stay `str(Path.resolve())`. `resolve()` on Windows
  canonicalises case, 8.3 names, subst/mapped drives (GetFinalPathNameByHandle), so different spellings converge.
  Very long roots without LongPathsEnabled may keep a `\\?\` prefix - document "keep the root path short / enable
  long paths"; the exe manifest needs `longPathAware` (C area).
- storage.ini (database.py:119-179) Windows rules, mirrored in `storage_locations.c`:
  absolute = `X:\…` or UNC `\\server\share\…` (`PureWindowsPath(p).drive and .root`); reject `/` anywhere (normpath
  would change it), `\\?\`/`\\.\` prefixes, `:` after the drive (ADS), reserved names (`os.path.isreserved`, 3.13+),
  components with trailing dot/space; normalised = `os.path.normpath(p) == p`; reject drive roots and UNC share roots
  (`p.rstrip("\\") == PureWindowsPath(p).anchor.rstrip("\\")`) - replaces `path == "/"` (L131).
  `_inside` (L135-136) → `os.path.normcase` + `os.sep` (`"C:\\x".startswith(root + "\\")`, root itself excluded
  rules as today). `_STORAGE_PROTECTED` joins `"database/SDBASE"` with `os.path.join(root, *name.split("/"))`.
  Link check (L167) → `is_link(info)` (junctions!). Plain check (L171) → `normcase(realpath(d)) == normcase(d)`.
  Writable check (L173) → `os.access` only looks at the read-only attribute on Windows; probe with
  `tempfile.TemporaryFile(dir=directory)`.
  `_last_database` (L278) → `os.path.isabs(value)` instead of `startswith("/")`.
- Case-insensitive file system: `Path.relative_to`/`==` are already case-insensitive for `WindowsPath`; explicit
  string comparisons of paths must use `normcase`. Never use `sorted(Path objects)` for anything that must be
  reproducible across OSes (WindowsPath sorts case-insensitively) - sort by `p.name` / `as_posix()` strings.
- Source digest (setup L185-198): `sorted(package.glob("*.py"))` → `sorted((p for p in package.iterdir() if
  p.is_file() and p.name.endswith(".py")), key=lambda p: p.name)` - glob is case-insensitive on Windows
  (`X.PY` would match) and WindowsPath ordering is case-insensitive, while C uses `g_str_has_suffix(".py")` and
  `strcmp`. Same fix in `sdbase/runtime_version.py` (cross-area). Today all 117 SDBASE files are lowercase, so this is
  latent - but a mismatch makes the GUI restart the service in a loop (SCHEMA_MISMATCH path).

### 4.12 Text, binary, encodings
- `os.open` without `O_BINARY` = CRT text mode: `os.read` turns CRLF into LF and stops at 0x1A; `os.write` turns LF
  into CRLF. Affected: every `os.open` in section 5 - critical in `_digest`/`_publish` (setup L398, L415: binary
  `.sdb` journal copied through a text-mode fd) and `_read_bytes` (L90). Python `open()`/`Path.read_bytes()` are
  always binary - only raw `os.open` needs the flag (`open_nofollow` adds it).
- Text writes: `os.fdopen(fd, "w", encoding="utf-8")` (database.py:59) → add `newline="\n"`; `checksum_stage.open("w",
  encoding="ascii")` (package L394) → `newline="\n"` (a CRLF `.sha256` breaks `sha256sum -c`); generated sources
  (css_to_gtk L186-192, islev_envanteri L214) → `encoding="utf-8", newline="\n"` for byte-identical outputs.
- INI/JSON reads: decode `utf-8-sig` (tolerate a BOM from Windows editors) in `load_config` (setup L140) and
  `load_storage_settings` (database.py:196); GKeyFile in C must also skip a BOM (cross-area) or both reject it.
- Locale-default `open()`/`read_text()`/`write_text()`/`subprocess.run(text=True)` in dev tools (sections 5.8-5.11) →
  explicit `encoding="utf-8"`. Console output of ✓/✗/→ to a redirected stdout needs UTF-8 stdio.
- `.cmd` files must have CRLF line endings (cmd.exe mis-parses labels/blocks in LF batch files); add
  `.gitattributes`: `* text=auto eol=lf`, `*.cmd text eol=crlf`, `*.bat text eol=crlf`. Sources in the copy are LF.

### 4.13 (b) Dev launchers
`run-database.cmd` (CRLF, ASCII only):
```bat
@echo off
setlocal
set "RADIOS_PROJECT_DIR=%~dp0"
if "%RADIOS_PROJECT_DIR:~-1%"=="\" set "RADIOS_PROJECT_DIR=%RADIOS_PROJECT_DIR:~0,-1%"
set "PYTHONDONTWRITEBYTECODE=1"
set "PYTHONUTF8=1"
set "RADIOS_PY=%RADIOS_PROJECT_DIR%\runtime\python\python.exe"
if defined RADIOS_PYTHON set "RADIOS_PY=%RADIOS_PYTHON%"
if exist "%RADIOS_PY%" goto run
echo RadioS: Python bulunamadi: "%RADIOS_PY%" 1>&2
exit /b 1
:run
"%RADIOS_PY%" -B -X utf8 "%RADIOS_PROJECT_DIR%\tools\radios_database.py" --project-root "%RADIOS_PROJECT_DIR%" %*
exit /b %ERRORLEVEL%
```
Pitfalls: `"%~dp0"` ends in `\` → `"C:\x\RadioS\"` makes the CRT parser treat `\"` as a literal quote and glue the next
argument onto the path - strip it (above) or use `"%~dp0."`; never expand a path variable inside a parenthesised
`if (...)` block (a `)` in `C:\Program Files (x86)\…` ends the block - hence the `goto`); never fall back to
`where python` (Store alias);
subcommands are passed through unchanged: `init [--username … --station-code … --station-name … --timezone …]`,
`start`, `status`, `check`, `stop [--force]`, `backup`, `ensure`, `repair-permissions`.

`run-database.ps1`:
```powershell
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$env:PYTHONDONTWRITEBYTECODE = '1'; $env:PYTHONUTF8 = '1'
$python = if ($env:RADIOS_PYTHON) { $env:RADIOS_PYTHON } else { Join-Path $root 'runtime\python\python.exe' }
if (-not (Test-Path -LiteralPath $python)) { [Console]::Error.WriteLine("RadioS: Python bulunamadı: $python"); exit 1 }
& $python -B -X utf8 (Join-Path $root 'tools\radios_database.py') --project-root $root @args
exit $LASTEXITCODE
```
Pitfalls: no `param()`/`[CmdletBinding()]` (common parameters like `-d`/`-e` would swallow arguments) - use `$args`;
default execution policy (Restricted) blocks `.ps1`, so docs show `run-database.cmd` first or
`powershell -NoProfile -ExecutionPolicy Bypass -File run-database.ps1 …`; Windows PowerShell 5.1 mangles native
arguments containing `"` - acceptable for these subcommands; save `.ps1` as UTF-8 **with BOM** if it ever contains
Turkish characters (5.1 reads BOM-less files as ANSI).

`run-radios.cmd` / `run-radios.ps1`: same skeleton calling `tools\radios_launch.py --project-root <root> %*`, plus
`if not defined RADIOS_MSYS2_ROOT set "RADIOS_MSYS2_ROOT=C:\msys64"` and
`if exist "%RADIOS_MSYS2_ROOT%\ucrt64\bin\gcc.exe" set "PATH=%RADIOS_MSYS2_ROOT%\ucrt64\bin;%PATH%"`
so CMake/Ninja/GCC are found and `build\radios.exe` finds its DLLs and GLib finds `ucrt64\share\glib-2.0\schemas`
(undefined variables expand to an empty string in batch files, hence the explicit default).
`run-radios-sdk` has no Windows counterpart.

### 4.14 `radios_launch.py` on Windows (dev launcher brain)
- Import the shim instead of `fcntl` (L12) - today the module cannot even be imported on Windows (test import fails).
- `build_current_sources`: tools = `cmake`, `pkg-config` (pkgconf), `gcc` (not `cc`); dependency list without
  `libpulse` (WASAPI/WinMM are system APIs; ALSA is not probed today either); no SDK fallback - if tools/deps are
  missing: existing `build\radios.exe` → warn and run it, else `RuntimeError` naming the MSYS2 packages; fresh
  configure adds `-G Ninja` (otherwise CMake on Windows may pick a Visual Studio generator and MSVC);
  binary = `build\radios.exe`; `binary.is_file()` instead of `os.access(X_OK)` (always true on Windows).
  A running `radios.exe` is locked by Windows, so the link step fails while the app runs → the existing
  "last good binary" fallback handles it (message should say so).
- Missing DLLs: call `SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX)` before probing (inherited by the
  child, suppresses the modal "X.dll was not found" box); `--version` exit codes `0xC0000135` (DLL not found),
  `0xC0000139` (entry point), `0xC000007B` (bad image) → prepend the MSYS2 `bin` dir found via `shutil.which("gcc")`
  to `PATH` and retry once; else error. Output decoding `encoding="utf-8", errors="replace"`.
- No `os.execv` (on Windows it spawns a new PID and returns immediately to the caller - tests and shells would see the
  "app" exit at once): `return subprocess.run([binary, *arguments], env=env).returncode`; on KeyboardInterrupt keep
  waiting for the child (Ctrl+C reaches both through the shared console; the GUI exits gracefully via its console
  handler - C area).
- No DISPLAY check, no `--check-config` probe and **no ensure** on Windows: `radios.exe` does those itself (4.1).
  POSIX path unchanged except it calls `radios_database.ensure_installation`.

### 4.15 Required C-side behaviour (cross-area, for the spawners)
- `data.c:376-402`: drop the flatpak branch on Windows; argv = `python_argv(config->python_command)` + script args;
  spawn with `CREATE_NO_WINDOW` (GLib's `g_subprocess` may allocate a console for a console-subsystem child of a GUI
  process - use the platform spawn or verify) and no inheritable handles besides the pipes; raise the 15 s timeout to
  ≥ 75 s (Python allows 60 s for store open; Defender scanning makes Windows slower). Same rules for `admin_ui.c:694-710`.
- `admin_ui.c:655-663`: show `response.command_line` when present (reliability adds it, `subprocess.list2cmdline` on
  Windows) instead of `g_shell_quote` (POSIX quotes are wrong for cmd.exe/PowerShell).
- `reset_ui.c:335-371`: Windows restart = `GetCommandLineW` + `CommandLineToArgvW` (drop `--project-root`), spawn
  `RadioS.exe` itself with `--wait-pid <current pid>` (new option: `OpenProcess(SYNCHRONIZE)` +
  `WaitForSingleObject(≤ 30 s)` before startup) and `CREATE_BREAKAWAY_FROM_JOB` fallback, then close the window.
  Never spawn `.cmd` files (BatBadBut-style argument injection; console flash).
- `window.c`/`recovery.c`: open `gui.lock`/outbox locks non-inheritable (`O_NOINHERIT`/`bInheritHandle=FALSE`),
  same byte range as 4.7, `FILE_SHARE_DELETE`; privacy checks via the 4.8 rule.
- `main.c:55-56`: graceful termination on Windows = console ctrl handler (Ctrl+C/Break/Close) + named event
  `Local\RadioS-GUI-<pid>` → `g_application_quit`; tests and `request_graceful_exit` rely on it
  (`Popen.terminate()` is `TerminateProcess`).
- `main.c`: implement 4.1 steps (ensure, `RADIOS_SKIP_ENSURE`, `--audio-inventory`, console attach for passive modes).

### 4.16 (g) Linux-only tools
- Delete from the Windows product (keep in the Linux tree): `run-radios-sdk.sh`, `tools/radios_sdk.py`,
  flatpak branches (run-radios.sh:5-11; data.c:380-391; reset_ui.c:355-360 - C area), `otomasyon_testleri/build.sh`
  (replaced by CMake targets, below), `run_release_acceptance.host_runtime_acceptance` (flatpak).
- `radios_hardware_check.py` - keep the report schema (`schema 1`, `read_only`, `pcm_played:false`,
  `microphone_captured:false`, `physical_acceptance:"operator-required"`, `server_reachable`, `server_info`,
  `outputs[]`, `inputs[]`, `physical_output_count`, `physical_input_count`, `enumeration_passed`, `operator_checks`).
  Windows branch replaces `pactl` (L28-35, L68-80) by `subprocess.run([<root>\bin\RadioS.exe or build\radios.exe,
  "--audio-inventory"], capture_output=True, timeout=15, encoding="utf-8")`. New passive C mode (WASAPI area) prints:
  `{"schema":1,"backend":"wasapi","default_render":id,"default_capture":id,"devices":[{"id","name","flow":"render|capture",
  "state":"active|disabled|notpresent|unplugged","enumerator","form_factor","physical":bool,"muted":bool,
  "mix_format":{"sample_rate","channels","bits","float"}}]}` (IMMDeviceEnumerator::EnumAudioEndpoints(eAll,
  DEVICE_STATEMASK_ALL), PKEY_Device_FriendlyName, PKEY_Device_EnumeratorName, PKEY_AudioEndpoint_FormFactor,
  IAudioClient::GetMixFormat for active endpoints, IAudioEndpointVolume::GetMute; read-only - never opens a stream).
  Mapping: `name=id`, `description=name`, `state`, `driver=enumerator`, `sample_specification="<f32le|s16le> <ch>ch
  <rate>Hz"`, `muted`, `hardware=physical`, `monitor=false` (Windows has loopback capture, not monitor sources),
  `ports=[]`, `active_port=None`; `server_info={"Backend":"WASAPI","Windows":platform.version()}`; `--server` is
  rejected on Windows. Reusing the C backend keeps one enumeration implementation (pure-ctypes COM vtables would be a
  second, untested one).

### 4.17 (h) Packaging redesign
#### 4.17.1 Kinds and outputs
`python tools\radios_package.py [--project-root R] [--build-dir build\release] package --kind windows|source|both`
- `windows` → `dist\RadioS-<ver>-windows-x86_64.zip` + `.sha256`; with `--installer` and `makensis` on PATH also
  `dist\RadioS-<ver>-windows-x86_64-setup.exe` + `.sha256`, built from the same verified staging tree.
- `source` → unchanged deterministic `.tar.gz` (platform-neutral), but `_read` must work on Windows (5.5).
- Linux `binary` stays for Linux builds. `both` = (`windows`|`binary` by host OS) + `source`.
#### 4.17.2 Windows portable layout (root = GTK prefix = RadioS project root)
```
RadioS\
  bin\RadioS.exe (renamed from radios.exe), gspawn-win64-helper.exe, gspawn-win64-helper-console.exe,
      all non-system DLLs (closure of RadioS.exe, helpers, loaders, FFmpeg …)
  lib\gdk-pixbuf-2.0\2.10.0\loaders\*.dll + loaders.cache (relative module paths)
  share\glib-2.0\schemas\gschemas.compiled, share\icons\Adwaita\ (+ hicolor\index.theme),
  share\locale\tr\LC_MESSAGES\{gtk30,gtk30-properties,glib20}.mo (optional)
  etc\fonts\fonts.conf (+conf.d, only if fontconfig is still used), etc\gtk-3.0\settings.ini (optional)
  runtime\python\  python.exe pythonw.exe python3XX.dll python3XX.zip *.pyd *.dll python3XX._pth tzdata.zip LICENSE.txt
  tools\*.py (+ radios_platform.py), database\SDBASE\{pyproject.toml,README.md,IMPLEMENTATION_STATUS.md,src\**.py,docs}
  config\{radios.example.ini,README.md}, assets\** (incl. .ttf), design\**, docs\**, README.md, CMakeLists.txt
  run-database.cmd, run-database.ps1
  packaging\manifest.json, packaging\runtime.json
  data\{database,endpoints,imports,cache,logs,recordings,backups}\   (empty directory entries)
```
Root markers (`config\radios.example.ini`, `database\SDBASE\pyproject.toml`) are inside; GLib derives its prefix
from `bin\..` so schemas/loaders/icons resolve relative to the exe. Exclude `.sh`, `radios_sdk.py`, `__pycache__`,
`packaging\windows\cache`.
- Runtime preparation (new subcommand `radios_package.py runtime`): extract the pinned embeddable zip and tzdata
  wheel from `packaging\windows\cache\` (verify SHA-256 pinned in `packaging\windows\runtime-lock.json`; no
  downloads - parity with "installs nothing and downloads nothing"), write `python3XX._pth`, produce `tzdata.zip`.
  Dev uses the same command to (re)create `<root>\runtime\python`.
- DLL closure: pure-Python PE import parser in `radios_package` (DOS `e_lfanew` → `PE\0\0` → COFF Machine
  `0x8664` → Optional header magic `0x20B` → Subsystem (2 = GUI for RadioS.exe) → import directory → section RVA
  mapping → DLL names), recursive over `bin\*.exe|*.dll` and loaders, searching `--msys2-prefix`
  (default `C:\msys64\ucrt64\bin`); system DLLs allowlisted (`kernel32 user32 gdi32 advapi32 shell32 ole32 oleaut32
  ws2_32 crypt32 bcrypt secur32 iphlpapi dnsapi winmm avrt comdlg32 comctl32 imm32 dwmapi uxtheme setupapi hid ntdll
  msimg32 usp10 dwrite d2d1 d3d11 dxgi version winhttp wldap32 normaliz rpcrt4 shlwapi userenv cfgmgr32 propsys
  mfplat mfreadwrite mf ksuser opengl32`, `api-ms-win-*`, `ext-ms-*`); anything unresolved → `PackageError`;
  `asan`/`ubsan` DLLs rejected (like L213-215). Replaces `readelf` (L193-204).
- `runtime_info` for Windows: `MZ`/PE checks (replaces the ELF magic L181), Release check with
  `re.search(rb"^CMAKE_BUILD_TYPE:STRING=Release\r?$", cache, re.M)` (a CRLF `CMakeCache.txt` - possible on
  Windows or after a CRLF checkout - makes the exact `b"...Release\n"` test at L184 fail),
  `RadioS.exe --version` decoded as UTF-8 must start with `RadioS <ver> `, the embedded manifest must contain
  `activeCodePage>UTF-8`, `PerMonitorV2`, `longPathAware` (byte search in the PE), `runtime.json`:
  `{"os":"Windows","architecture":"x86_64","windows_minimum":"10.0.17763","python":"3.x.y embeddable",
  "python_sha256":…,"bundled_dlls":[{"name","sha256"}…],"system_libraries_bundled":true,"build_type":"Release",
  "audio_service":"WASAPI","display":"GTK 3 (Win32)"}`.
- Limits per kind: `windows` needs `MAX_FILE ≥ 256 MiB` (MSYS2 `avcodec-61.dll` alone is ~80-100 MB),
  `MAX_TOTAL ≈ 2 GiB`, `MAX_FILES ≈ 30000` (Adwaita icons) - keep today's limits for `source`/`binary`.
- Deterministic zip: entries sorted, `ZipInfo(date_time=(1980,1,1,0,0,0))`, `create_system=3`,
  files `external_attr = 0o100644 << 16`, dirs `0o40755 << 16 | 0x10`, `ZIP_DEFLATED` level 9, no comment/extra.
  `verify()` for `.zip`: safe POSIX relative names (no `\`, no drive, no `..`, no absolute), no encrypted entries
  (`flag_bits & 1`), compression ∈ {stored, deflated}, no symlink modes, no duplicates **including case-folded
  duplicates** (they collide on NTFS - add this check for tar too), no reserved device names / trailing dot or space
  components, bounded decompression, SHA-256/size per manifest, directory set == expected, `user_data_included:false`.
- NSIS (`packaging\windows\RadioS.nsi` template + generated file list): `Unicode true`, `ManifestDPIAware true`,
  `RequestExecutionLevel user`, `InstallDir "$LOCALAPPDATA\Programs\RadioS"` (user-writable: `data\`/`config\` are
  root-relative and must be writable - a per-machine `Program Files` install would break the decision), MUI2 Turkish;
  before copying: `"$INSTDIR\runtime\python\python.exe" -B -X utf8 "$INSTDIR\tools\radios_database.py"
  --project-root "$INSTDIR" stop --force` (if present) and refuse while `RadioS.exe` runs (its files are locked);
  upgrades delete the code trees (`bin lib share etc runtime tools database\SDBASE\src assets design docs packaging`)
  before copying so removed modules do not linger (they would change the source digest), never `data\` or user
  files in `config\` (`credentials.json`, `radios.ini`, `ui-preferences.ini`, `storage.ini`, `audio-panel.ini`);
  Start-menu shortcut to `$INSTDIR\bin\RadioS.exe`; HKCU Uninstall key; uninstaller stops the service the same way
  and keeps user data unless the user opts in. ACL hardening of `data\`/`config\` happens at first run (`ensure`).
  NSIS output is not byte-reproducible; the zip is the canonical verifiable artifact. Signing (signtool) must
  happen before hashing/packaging.
- `_safe_relative`, `_allowed`, `collect`, `_directories`, `MANIFEST`, manifest schema (`format 1`, `kind`, `version`,
  `user_data_included:false`, `root:"RadioS"`, `files{size,mode,sha256}`) are reused; `kind` gains `"windows"`.

### 4.18 Dev tools
- `css_to_gtk.py` (run by CMake's `Python3_EXECUTABLE` at build time): L113, L117 `read_text(encoding="utf-8")`;
  L186, L191, L192 `write_text(..., encoding="utf-8", newline="\n")`. Otherwise cp1254 decoding of the UTF-8 design
  files corrupts/aborts the generated `reference-css.c`. Note for the build area: the DEPENDS path
  `design/reference/tasarrım/index.html` contains `ı`; Ninja must run with UTF-8 paths (manifest) - verify.
- `islev_envanteri.py`: L131, L165-166 sort by `p.name`/`relative_to(root).as_posix()`; L209 compare after
  normalising CRLF (`read_bytes().replace(b"\r\n", b"\n")`) so a CRLF checkout does not fail `--check`; L214
  `newline="\n"`; L141-144 needs an importable `sdbase` on Windows (cross-area); UTF-8 stdout.
- `gui_backend_coverage.py`: reads are already UTF-8; L153-160 `from sdbase import desktop_workflows` silently
  returns `set()` when sdbase cannot import (e.g. `import fcntl` in storage.py) → HTTP read routes become "gap" →
  `tests/test_gui_backend_coverage.py` fails on Windows until sdbase is ported; UTF-8 stdout for the report.
- `otomasyon_testleri`: replace `build.sh` with CMake `add_executable(otomasyon_e2e EXCLUDE_FROM_ALL
  tools/otomasyon_testleri/e2e.c)` linking `radios_playout radios_core radios_audio` + json-glib/gio,
  `RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/otomasyon_testleri`, `OUTPUT_NAME e2e` (gets the UTF-8 manifest like
  every exe; its narrow `argv` paths then are UTF-8); `e2e.py:8` `E2E = os.path.join(H, "e2e.exe" if os.name == "nt"
  else "e2e")`; `e2e.py:17` `subprocess.run(..., encoding="utf-8")`; every `open(...)` (e2e.py:35,43,50,57,63,71,116;
  api.py:5; senaryolar_724.py:273,276; senaryolar_kume.py:472,486-488) gets `encoding="utf-8"`; `api.py:7` read
  `service_url` from the config instead of hard-coding 8000; `api.py:24` `no_proxy_opener()`; README: Windows
  commands (`cmake --build build --target otomasyon_e2e`, `$env:RADIOS_BUILD_DIR='build-67'; python senaryolar_724.py`),
  drop `flock build-4c/.build.lock` (Linux `flock(1)`).
- `yayin_akisi_724`: `rapi.py:3`, `verify.py:6`, `sweeper_import.py:4` → `ROOT = Path(__file__).resolve().parents[2]`;
  `sweeper_import.py:5` FOLDER → `sys.argv[2]`; `analiz`/`e2e` → `.exe`; CMake target for `analiz.c`
  (`EXCLUDE_FROM_ALL`); all `open()` → `encoding="utf-8"`; `subprocess.run(text=True)` → `encoding="utf-8"`;
  urllib → `no_proxy_opener()`.

### 4.19 Docs/config
- `config/radios.example.ini:24`: keep `python_command = python3`; add a comment: "Windows: `python3` paketteki
  `runtime\python\python.exe` anlamına gelir; PATH aranmaz. Başka yorumlayıcı için proje köküne göre göreli veya mutlak
  .exe yolu yazın." (The C parser accepts any string; both resolvers implement 4.2.)
- `config/README.md:7`: privacy wording (0600 / Windows private ACL), `repair-permissions`, Windows commands.
- `tools/README.md`, `packaging/README.md`: Windows sections (dev launchers, `ensure`, packaging commands, NSIS,
  runtime preparation, no Flatpak, OneDrive/Controlled Folder Access/Defender notes, autostart via Startup folder or
  HKCU Run instead of Task Scheduler (default 72 h execution limit kills a 24/7 app)).

---

## 5. File:line migration lists

Notation: **R** = replace with shim call, **W** = Windows-specific branch, **K** = keep.

### 5.1 Launch scripts
| Location | Construct | Windows |
|---|---|---|
| run-radios.sh:1-3 | `#!/bin/sh`, `cd -P` root | `run-radios.cmd`/`.ps1` with `%~dp0` (strip `\`) / `$PSScriptRoot` |
| run-radios.sh:4 | `PYTHONDONTWRITEBYTECODE=1` | `set` + `-B` |
| run-radios.sh:5-11 | flatpak host handoff | delete on Windows |
| run-radios.sh:12 | `exec python3 tools/radios_launch.py --project-root … "$@"` | `"%PY%" -B -X utf8 …\tools\radios_launch.py --project-root "%ROOT%" %*` + `exit /b %ERRORLEVEL%` |
| run-database.sh:5 | `exec python3 tools/radios_database.py …` | `run-database.cmd`/`.ps1` (4.13) |
| run-radios-sdk.sh:5 | SDK | none |

### 5.2 tools/radios_database.py
| Line(s) | Today | Windows replacement |
|---|---|---|
| 13 | `import fcntl` | `import radios_platform as plat` |
| new | - | `plat.configure_stdio_utf8()` first line of `main()` |
| 40 | `os.open(path, O_RDONLY|O_NOFOLLOW|O_NONBLOCK)` | `plat.open_nofollow(path, os.O_RDONLY)`; transient sharing violation → retry; keep `FileNotFoundError → None` |
| 47-48 | `st_uid != os.getuid() or st_mode & 0o077` | `not plat.is_private(stream.fileno())`; neutral message |
| 56 | `tempfile.mkstemp(dir=path.parent)` | K inside private runtime dir; `plat.private_mkstemp` otherwise |
| 59 | `os.fdopen(fd, "w", encoding="utf-8")` | add `newline="\n"` |
| 64 | `path.is_symlink()` | `plat.is_link(path)` |
| 66 | `os.replace` | `plat.replace` |
| 67-71 | dir fsync via `O_DIRECTORY` | `plat.fsync_dir(path.parent)` |
| 73 | `unlink(missing_ok=True)` | `plat.unlink(..., missing_ok=True)` |
| 76-88 | `/proc` identity | `plat.process_identity(pid)` (4.4) |
| 100-101 | `"database/SDBASE"` | split on `/` when joining |
| 119-132 | POSIX syntax (`startswith("/")`, `"//"`, `== "/"`) | W rules 4.11 (mirror C) |
| 135-136 | `_inside` with `/` | `normcase` + `os.sep` |
| 162-172 | `lstat`, `S_ISLNK`, `realpath != path` | `plat.is_link(info)`; `normcase` compare |
| 173 | `os.access(R|W|X)` | W: write probe with `TemporaryFile(dir=…)` |
| 175-177 | protected roots join | split names, `normcase` |
| 186-196 | lstat/S_ISREG, `decode("utf-8")` | K; `decode("utf-8-sig")` (and C) |
| 259 | `Path(stored).is_absolute()`, name compare | K (stored is POSIX; accept `\`) |
| 261-265 | sidecar `is_symlink` | `plat.is_link` |
| 278 | `value.startswith("/")` | `os.path.isabs(value)` |
| 282 | `mkdir(mode=0o700, parents=True)` | `plat.make_private_dir(..., parents=True)` |
| 287 | `shutil.copy2(follow_symlinks=False)` | K (new file inherits target dir ACL) |
| 288-292 | `O_RDONLY|O_NOFOLLOW` + fsync | `plat.fsync_file(target)` (write access) |
| 300-314 | `is_symlink` (5×) | `plat.is_link` |
| 308 | `copytree(symlinks=False)` | K; note it follows junctions inside `.objects` (copy, not escape) |
| 315 | `touch(mode=0o600)` | K |
| 319 | `os.replace(stage, target)` | `plat.replace` |
| 322-326 | dir fsync | `plat.fsync_dir` |
| 327 | `os.sync()` | `plat.sync()` (no-op on Windows; AttributeError today) |
| 354-373 | `_owned` | K (identity tuple semantics per 4.4) |
| 400-405 | `_occupied` | K (see 4.10) |
| 456-463 | log `O_APPEND|O_NOFOLLOW`, `st_uid`, `fchmod` | `plat.open_log_append(log)` (append-only handle + private + owner check) |
| 465 | `shlex.split(cfg.python_command)` | `plat.python_argv(cfg.python_command, cfg.project_root)` |
| 470 | env | + `PYTHONUTF8="1"` |
| 472-474 | `Popen(start_new_session=True, close_fds=True)` | `plat.spawn_detached(...)`; bind pre-check (4.5) |
| 497-506 | terminate/kill own child | K (TerminateProcess) |
| 509-532 | `os.pidfd_open`, `pidfd_send_signal(SIGTERM)`, `ProcessLookupError` | `plat.PinnedProcess` + stop event; `--force` (4.6) |
| 567 | `destination.is_symlink()` | `plat.is_link` |
| 592-593 | server.lock `O_RDWR|O_CREAT|O_NOFOLLOW` | `plat.open_nofollow(..., os.O_RDWR|os.O_CREAT, private=True)` |
| 598 | `fcntl.flock(fd, LOCK_EX|LOCK_NB)` | `plat.flock` |
| 607-612 | `BoundedHTTPServer` (SO_REUSEADDR) | W: `allow_reuse_address=False` (4.10) |
| 613-615 | SIGTERM/SIGINT | K SIGINT (+SIGBREAK W); stop event polling W |
| 616-619 | identity of self; "Linux servis süreç kimliği" | `plat.process_identity(os.getpid())`; neutral message |
| 620-625 | state | + optional `stop_event`, `platform`, `boot_time` |
| 628-629 | loop | `while not stopping.is_set() and not plat.stop_event_is_set(ev)` |
| 630-636 | finally unlink + close | `plat.unlink` (never raise), close event, `plat.flock(fd, LOCK_UN)` then close |
| 639-655 | argparse | + `ensure`, `stop --force`, `repair-permissions` |
| 657 | `os.umask(0o077)` | K (harmless no-op for ACLs on Windows) |
| 668-670 | dispatch | + `ensure` → `ensure_installation` |

### 5.3 tools/radios_database_setup.py
| Line(s) | Today | Windows replacement |
|---|---|---|
| 15 | `import fcntl` | shim |
| 63 | `candidate.is_symlink()` | `plat.is_link(candidate)` |
| 80-83 | `S_ISREG`; `S_IMODE != 0o600 or st_uid != getuid()` | K S_ISREG; `plat.is_private(path)` |
| 90 | `O_RDONLY|O_NOFOLLOW|O_NONBLOCK` | `plat.open_nofollow(path, os.O_RDONLY)` (adds O_BINARY - critical) |
| 95-96 | private check on fd | `plat.is_private(fd)` |
| 134 | `local.is_symlink()` | `plat.is_link` |
| 140 | `.decode("utf-8")` | `.decode("utf-8-sig")` |
| 165-167 | python_command regex | per-platform rule (4.2) |
| 188 | `sorted(package.glob("*.py"))` | name-sorted, case-sensitive `.py` filter (4.11) |
| 245 | `mkdir(mode=0o700 if private else 0o755)` | K (3.13 applies private DACL for 0o700) |
| 249-251 | `st_uid != getuid()`, `chmod(0o700)` | `plat.owned_by_current_user`, `plat.make_private(directory=True)` |
| 262 | lock open flags | `plat.open_nofollow(lock_path, os.O_CREAT|os.O_RDWR, private=True)` |
| 266-269 | `st_uid`, `fchmod(0o600)` | `plat.owned_by_current_user(fd)`; `plat.make_private(fd)` only if not private |
| 273 | `fcntl.flock(..., LOCK_EX|LOCK_NB)` | `plat.flock` |
| 281 | `os.close(fd)` | `plat.flock(fd, LOCK_UN)` then close |
| 284-289 | `_sync_dir` | `plat.fsync_dir` |
| 294 | `O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW, 0o600` | `plat.open_nofollow(..., private=True)` (explicit private SD) |
| 348 | `str(cfg.database_file.relative_to(root))` | `.as_posix()`; accept `\` on read |
| 398 | `_digest` open flags | `plat.open_nofollow(path, os.O_RDONLY)` (O_BINARY - critical) |
| 407-408 | target exists → `_regular(private=True)` | K (works once DACLs are private) |
| 412 | `tempfile.mkstemp(dir=target.parent)` | `plat.private_mkstemp(target.parent, ".radios-setup-")` |
| 415 | source open | `plat.open_nofollow` |
| 422 | `os.link(temporary, target, follow_symlinks=False)` | `plat.publish_noreplace(temporary, target)` |
| 423 | `_sync_dir` | `plat.fsync_dir` |
| 441 | `str(relative_to)` | `.as_posix()` |
| 448 | `S_IMODE(stage.stat().st_mode) != 0o700` | `not plat.is_private(stage, directory=True)` |
| 475-479 | unlink, sync, rmtree | `plat.unlink`, `plat.fsync_dir`, `plat.rmtree` (best effort) |
| 513 | `ZoneInfo(...)` | needs tzdata in runtime / dev python |
| 517 | `stage.mkdir(mode=0o700)` | K (+ verify `is_private` on Python < 3.12.4 → `make_private`) |
| 531 | `str(relative_to)` | `.as_posix()` |
| 538-545 | `chmod(0o600)`, `O_RDONLY|O_NOFOLLOW` + fsync | drop chmod on W (inherits); `plat.fsync_file(path)` |
| 558 | `rmtree(ignore_errors=True)` | K |

### 5.4 tools/radios_reliability.py
| Line(s) | Today | Windows replacement |
|---|---|---|
| 16 | `import fcntl` | shim |
| 65 | `S_ISLNK(info.st_mode)` | `plat.is_link(info)` |
| 72-75 | mkdir 0700, `st_uid`, `chmod(0o700)` | `plat.make_private_dir`, `plat.owned_by_current_user` |
| 110-111 | `open("xb")`, `os.chmod(0o600)` | K binary; chmod no-op (inherits private backup dir) |
| 175 | `TemporaryDirectory(dir=runtime_dir)` | K (`ignore_cleanup_errors=True` on W) |
| 219, 221 | `mkdtemp`, `mkdir(0o700)` | K (private on 3.13) |
| 245, 321, 495, 591 | `shutil.rmtree` | `plat.rmtree` |
| 269-273 | `command=[cfg.python_command, …]` | `plat.python_argv(...) + [...]`; add `"command_line": plat.display_command(command)` |
| 325-338 | `/proc` GUI scan | `plat.iter_processes()`: image basename `radios.exe` (case-insensitive) inside `normcase(root)`; if argv readable and contains `--project-root X`, X must resolve to root |
| 358-360 | `_process_identity(state.pid)` | K (shim-backed) |
| 365-376 | lock files `O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK`, `st_uid`, `flock EX|NB` | `plat.open_nofollow(..., private=True)`, `plat.owned_by_current_user`, `plat.flock` |
| 384-395 | outbox dir `st_uid`/`st_mode & 0o077`; `.lock` open/flock | `plat.is_private(dir, directory=True)`; shim open/flock |
| 399-401 | close | unlock then close |
| 407 | `is_symlink` | `plat.is_link` |
| 431 | `os.replace` (files and dirs) | `plat.replace` (retry); dirs must not be open/CWD |
| 432-434, 476-494 | `_sync_dir` | `plat.fsync_dir` |
| 607-614 | outbox lock open/checks/flock | shim |
| 643 | `"/" in leaf or "\\" in leaf` | K (already both) |
| 690-692 | `st_uid`, `st_mode & 0o077`, `st_ctime_ns` | `plat.is_private(path)`; note `st_ctime` = creation time on Windows |
| 699-701 | `st_dev`, `st_ino`, mtime/ctime | K (volume serial + file index) |
| 742 | `_move(path, quarantine)` | K via shim |
| 773 | `os.umask(0o077)` | K |
| new | - | `plat.configure_stdio_utf8()`; `gc.collect()` before `_move` loops in `restore` |

### 5.5 tools/radios_package.py
| Line(s) | Today | Windows replacement |
|---|---|---|
| 24-46 | `COMMON_FILES` with `.sh`; trees | per-kind lists: windows adds `run-database.cmd/.ps1`, drops `.sh`; assets `+ .ttf`; packaging `+ .nsi .nsh .json .ini .conf` as needed |
| 74-80 | `_safe_relative` | + case-fold duplicate + reserved-name checks (portable) |
| 86-87 | `build/radios` | windows: `bin/RadioS.exe`, `bin/*.dll`, `lib/**`, `share/**`, `etc/**`, `runtime/python/**` rules |
| 98-126 | `dir_fd` + `O_DIRECTORY|O_NOFOLLOW` walk | W: per-component `plat.is_link(os.lstat(...))`, open final via `plat.open_nofollow`, verify `GetFinalPathNameByHandleW` equals the expected path (normcase); keep `st_nlink != 1`, size, mtime/ctime stability checks |
| 122 | mode by `.sh` | K for tar; ignored for zip |
| 155-166 | `is_symlink` checks; `os.walk(followlinks=False)` | `plat.is_link` (os.walk descends into junctions on Windows) |
| 174-216 | ELF/readelf/GLIBC; `LC_ALL=C` | W `runtime_info_windows` (4.17.2) |
| 184 | `b"…Release\n" in cache` | CRLF-tolerant regex |
| 230-243 | tar.gz writer | + deterministic zip writer |
| 261-345 | tar verify | + zip verify |
| 262 | `O_RDONLY|O_NOFOLLOW|O_NONBLOCK` | `plat.open_nofollow` |
| 357-360 | `dist.mkdir`, `TemporaryDirectory(dir=dist)` | K (`ignore_cleanup_errors=True` on W) |
| 364-368, 379 | `"os": "Linux"`, `"linux-" + arch` | per kind |
| 394 | checksum `open("w", encoding="ascii")` | `newline="\n"` |
| 398-399 | `os.replace` | `plat.replace` |
| 401-405 | dist dir fsync | `plat.fsync_dir` |
| 411-427 | CLI | `--kind windows`, `--installer`, `--msys2-prefix`, `runtime` subcommand |

### 5.6 tools/radios_launch.py
See 4.14. Lines: 12 (fcntl), 29-41 (toolchain), 36 (libpulse), 38 (`cc`→`gcc`), 42-46 (SDK), 47-48 (`.exe`),
50-51 (lock), 53-55 (`-G Ninja`), 65/84/86 (X_OK), 90-97 (missing DLL detection), 98-99/126 (no execv),
100-109 (skip on Windows), 110-125 (→ `ensure_installation`, POSIX only), 113 (umask K).

### 5.7 tools/radios_hardware_check.py, tools/radios_sdk.py
hardware_check 28-35, 59, 68-80 → platform branch (4.16); 89-92 K (already UTF-8 file write; UTF-8 stdout).
radios_sdk.py whole file Linux-only; radios_launch must not import it on Windows (L45, L96 are lazy - guard them).

### 5.8 tools/css_to_gtk.py: 113, 117, 186, 191, 192 (4.18).
### 5.9 tools/islev_envanteri.py: 131, 141-144, 165-166, 209, 214-216 (4.18).
### 5.10 tools/gui_backend_coverage.py: 153-160 (sdbase import), 165-166 (sort, cosmetic), 261-263 (stdout).
### 5.11 otomasyon_testleri / yayin_akisi_724: see 4.18 (every listed line).

---

## 6. Tests on Windows

### 6.1 Adaptations of existing tests (all keep their Linux behaviour)
- Shared helpers (new `tests/radios_test_platform.py` or reuse `tools/radios_platform.py`): `assert_private(path)`
  (replaces `S_IMODE == 0o600/0o700`), `make_public(path)` (Windows: add an allow ACE for `S-1-5-32-545` Users via
  `SetNamedSecurityInfoW` merged DACL; POSIX chmod 0o644), `make_dir_link(link, target)` (POSIX symlink; Windows
  `_winapi.CreateJunction` - no privilege needed and it is the realistic Windows attack), `symlink_or_skip`,
  `find_service_processes(root)` (`plat.iter_processes()` with argv), `stop_pid(pid)` (`radios_database stop` first,
  then `TerminateProcess`), `gui_lock_held(root)` via `plat.flock`.
- `test_radios_launch.py`: fake `which` returns `C:\msys64\ucrt64\bin\<name>.exe`; binary `build\radios.exe`;
  `SdkSandbox` skipped on Windows; new test: fresh configure on Windows passes `-G Ninja`.
- `test_database_setup.py`: `cleanup_service` (L187-221) → helpers; `test_public_and_symlinked_credentials_are_rejected`
  (L292-307) → `make_public`; file symlink part skipped without privilege; `test_symlink_database_escape_is_rejected`
  (L319-326) → junction variant; L228 → `assert_private`.
- `test_database_recovery.py:57` → `assert_private`.
- `test_reliability.py`: L66/69/77/497/504-513 chmods → no-op/helpers; L124 → `assert_private`; L199-213 outbox
  dirs via `plat.make_private_dir`; L291-318 `fcntl.flock` → `plat.flock`; L340, L498 symlinks → skip or junction
  (directory) variant.
- `test_storage_locations.py`: Windows variant of `test_path_rule_matches_the_c_reader` (bad: `""`, `relative`,
  `C:\a\..\b`, `C:\a\\b`, `C:\a\`, `C:\`, `C:/a/b`, `C:\a\tb`, `C:\a\.\b`, `\\?\C:\a`, `C:\a:s`; good:
  `D:\Türkçe disk\arşiv`); L105, L124 → junction / skip.
- `test_release_install.py` / `run_release_acceptance.py`: launch `build\radios.exe` (or the extracted
  `bin\RadioS.exe`) directly; `native_start` readiness = image path of the pid (shim) + `gui_lock_held` + `/health`;
  `native_stop` = `request_graceful_exit` then wait; `close()` → helpers; DISPLAY tests skipped on Windows; zip
  extraction instead of tar (L45-80); host-runtime (flatpak) part Linux-only; `G_DEBUG=fatal-warnings` may need
  relaxing for known GTK-on-Windows warnings.
- `test_packaging.py`: source-kind tests unchanged; `.sh` mode test (L121-124) Linux kind only; symlink tests
  (L193-205) skip without privilege + junction variant; hardlink test works on NTFS; new windows-kind tests with a
  fake PE (patch `runtime_info`), zip tamper cases (backslash name, case-fold duplicate, symlink mode, encrypted flag,
  unlisted file, missing data dir entry).
- `test_application_shutdown.py:121-127`: `send_signal(SIGTERM)` = TerminateProcess on Windows → use
  `request_graceful_exit` (C must implement the GUI event).
- `run_data_connection_ui.py:84`: `host_boundary` (flatpak) cases Linux-only.
- All tests that use `urllib.request` against the fixture service: `no_proxy_opener()`.
- CMake (build area): `config_from_other_directory` uses `WORKING_DIRECTORY /tmp` (CMakeLists.txt:264) → a
  Windows temp dir.

### 6.2 New tests
- `tests/test_platform_shim.py`: flock EX/SH/NB between two processes (spawn a child python holding the lock);
  `BlockingIOError` mapping; `is_link` for symlink and junction; `open_nofollow` refuses a junction/symlink final
  component and opens binary (round-trip `b"a\r\nb\x1ac"`); `replace` succeeds while our reader holds the target
  open (share-delete) and retries while a non-share-delete reader holds it; `publish_noreplace` raises
  `FileExistsError`; `private_mkstemp`/`make_private_dir` → `is_private`; inherited "Users" ACE → not private;
  `process_identity` of self, of a live child (argv round-trip with spaces/Turkish characters), of an exited child
  whose handle is still open (→ None); stop event create/set/is_set and `ERROR_ALREADY_EXISTS` refusal;
  `spawn_detached` child survives the parent and its console, log is appended not overwritten; UTF-8 stdout through
  a pipe.
- `tests/test_platform_contract.py`: tools shim and `sdbase._platform` constants equal; a C test binary
  (platform area) holding `gui.lock` SH makes the Python EX|NB probe fail and vice versa (lock range interop).
- `tests/test_database_setup.py` additions: `ensure` on a fresh project (initializes + starts), on an initialized
  one (adopts), with a foreign listener (fails, no identity); `stop` uses the event (the process exits with code 0
  and removes `service.json`); `stop --force`; excluded-port (10013) message (simulate with a pre-bound
  `SO_EXCLUSIVEADDRUSE` socket from another process).

---

## 7. Implementation order (suggested)
1. `tools/radios_platform.py` (POSIX branch first = zero behaviour change, then Windows) + `test_platform_shim.py`.
2. Mechanical replacements in setup/database/reliability (sections 5.2-5.4) - makes the modules importable on Windows.
3. `ensure` + `stop --force` + `repair-permissions`; radios_launch refactor (Linux uses `ensure_installation`).
4. Dev launchers (.cmd/.ps1), `.gitattributes`, runtime preparation (`radios_package.py runtime`, `._pth`).
5. Test adaptations; run the Python lifecycle suites on Windows with MSYS2 python + tzdata.
6. With the C/platform areas: RadioS.exe ensure step, spawn flags, lock interop, privacy rule, GUI stop event.
7. Packaging `windows` kind + NSIS; hardware check branch; dev-tool encoding fixes; docs.

---

## 8. Risks / open questions
- **Lock-range mismatch** between C, sdbase shim and tools shim silently disables mutual exclusion (restore under an
  open GUI, two services). Mitigation: one constant, a cross-language test.
- **Inherited lock handles** (C opens with inheritable handles; children keep the file object → `gui.lock` appears
  held after the GUI exits). Mitigation: non-inheritable opens everywhere; Python `close_fds=True`.
- **Text-mode fds** silently corrupting `.sdb` copies if any `os.open` misses `O_BINARY`. Mitigation: ban raw
  `os.open` in tools (grep/lint test), only `plat.open_nofollow`.
- **Privacy false negatives** from inherited foreign ACEs (observed on this machine) → users blocked on first run.
  Mitigation: `ensure_private_tree` at first run + `repair-permissions`; clear messages.
- **SO_REUSEADDR** on Windows lets two stores share a port; must be disabled in sdbase/serve.
- **Excluded port ranges** (Hyper-V/WSL/Docker) make port 8000 unbindable with WSAEACCES. Mitigation: bind pre-check
  + message; consider a different default port for the Windows example config.
- **Store alias `python3.exe`** if any spawner falls back to PATH. Mitigation: 4.2 resolution, no PATH lookups.
- **Python version**: copy has 3.14.8, lead decided 3.13 - pin one in `runtime-lock.json`; `._pth` name follows it.
  Code relies on ≥ 3.12.4 (private `mkdir(0o700)`), ≥ 3.12 (`isjunction`), ≥ 3.13 (`os.path.isreserved`); dev
  Python (MSYS2) must also satisfy this and have `tzdata`.
- **ProcessCommandLineInformation** is an `ntdll` information class (Windows 8.1+); if blocked (elevation mismatch,
  security software) ownership fails closed ("foreign"). Alternative fallback: identity = pid + creation time +
  image path + open-able stop event (weaker but documented).
- **Service lifetime vs jobs**: without breakaway permission the service dies with a job-wrapped GUI (VS Code
  terminal, Task Scheduler). Task Scheduler's default 72 h execution limit would also kill a 24/7 RadioS.exe.
- **Timeouts**: data.c's 15 s helper timeout is shorter than Python's 60 s store-open budget; Defender scanning and
  no-bytecode imports make Windows slower. Consider `pycache_prefix`.
- **OneDrive / Controlled Folder Access**: the dev copy lives on the Desktop; OneDrive placeholders/sync locks and
  CFA write blocking break DB writes. Recommend `%LOCALAPPDATA%\Programs\RadioS` or a plain NTFS folder.
- **Stale Linux credentials** in the Windows copy block first run (section 1).
- **Graceful logoff/shutdown** of `serve` is best effort (phase 2 hidden window); SDBASE crash safety is the real
  guarantee.
- **Package size/limits**: GTK + FFmpeg + icons exceed today's `MAX_FILE`/`MAX_FILES`; per-kind limits needed.
- **gui_backend_coverage/islev_envanteri tests** fail on Windows until sdbase imports without `fcntl`.

---

## 9. Cross-area dependencies (summary)
- platform.c / sdbase `_platform.py`: same `FLOCK_OFFSET/LENGTH`, same private SDDL + `is_private` rule, share-delete
  + non-inheritable opens, `MoveFileExW` replace with retry, dir-fsync no-op.
- C config/spawners (config.c, data.c, admin_ui.c): python_command resolution + `-B -X utf8` flags,
  `CREATE_NO_WINDOW`, no stray inheritable handles, ≥ 75 s start timeout, `command_line` display.
- C main.c: `ensure` at startup, `RADIOS_SKIP_ENSURE`, `--audio-inventory`, `--wait-pid`, console attach for passive
  modes, GUI stop event `Local\RadioS-GUI-<pid>` + console ctrl handler, root discovery via `GetModuleFileNameW`.
- C data_client.c: POSIX-form `database_file` comparison, privacy rule for credentials, digest sort by byte order.
- C storage_locations.c: identical Windows storage path rules (4.11) and marker-folder hardening.
- sdbase: `runtime_version.source_digest` ordering/filter fix, `BoundedHTTPServer.allow_reuse_address=False` on
  Windows, importable without `fcntl` (gui_backend_coverage/islev_envanteri), `backup._fsync_tree/_publish_absent/
  _copy_file` Windows-safe, no persistent file handles (or `Store.close()`), `desktop_reset.require_space` via
  `shutil.disk_usage`.
- WASAPI area: read-only endpoint enumeration for `--audio-inventory`.
- Build/CMake area: `otomasyon_e2e` and `analiz` targets, application manifest on every exe (UTF-8 ACP,
  PerMonitorV2, longPathAware), Ninja + UTF-8 paths, CTest working dirs, Python3_EXECUTABLE with tzdata.

---

## Appendix A. Windows implementation sketches for the shim (ctypes)

Common declarations: `kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)`, same for `advapi32`, `shell32`,
`user32`; `ntdll = ctypes.WinDLL("ntdll")`. Always set `argtypes`/`restype` (64-bit HANDLE truncation otherwise).
Raise errors as `OSError(0, ctypes.FormatError(code), str(path), code)` - Python maps `winerror` to `errno` and to
the right subclass (`FileNotFoundError`, `PermissionError`, `FileExistsError`), so existing `except` clauses keep
working.

A.1 flock
```python
class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                ("Offset", wt.DWORD), ("OffsetHigh", wt.DWORD), ("hEvent", wt.HANDLE)]

def flock(target, operation):
    fd = target if isinstance(target, int) else target.fileno()
    if not IS_WINDOWS:
        return fcntl.flock(fd, operation)
    handle = msvcrt.get_osfhandle(fd)
    ov = OVERLAPPED(0, 0, FLOCK_OFFSET & 0xFFFFFFFF, FLOCK_OFFSET >> 32, None)
    if operation & LOCK_UN:
        if not UnlockFileEx(handle, 0, FLOCK_LENGTH, 0, ctypes.byref(ov)):
            code = ctypes.get_last_error()
            if code != 158:                      # ERROR_NOT_LOCKED
                raise OSError(0, ctypes.FormatError(code), None, code)
        return
    flags = (0x2 if operation & LOCK_EX else 0) | (0x1 if operation & LOCK_NB else 0)
    if not LockFileEx(handle, flags, 0, FLOCK_LENGTH, 0, ctypes.byref(ov)):
        code = ctypes.get_last_error()
        if code == 33:                           # ERROR_LOCK_VIOLATION
            raise BlockingIOError(errno.EWOULDBLOCK, "Kilit kullanımda")
        raise OSError(0, ctypes.FormatError(code), None, code)
```

A.2 open_nofollow (Windows branch)
```python
GENERIC_READ, GENERIC_WRITE, READ_CONTROL = 0x80000000, 0x40000000, 0x00020000
SHARE_ALL = 0x7                                   # READ|WRITE|DELETE
CREATE_NEW, OPEN_EXISTING, OPEN_ALWAYS, TRUNCATE_EXISTING = 1, 3, 4, 5
FILE_FLAG_OPEN_REPARSE_POINT, FILE_ATTRIBUTE_REPARSE_POINT = 0x00200000, 0x400

def open_nofollow(path, flags, mode=0o600, *, private=False):
    access_mode = flags & (os.O_RDONLY | os.O_WRONLY | os.O_RDWR)
    access = {os.O_RDONLY: GENERIC_READ, os.O_WRONLY: GENERIC_WRITE,
              os.O_RDWR: GENERIC_READ | GENERIC_WRITE}[access_mode] | READ_CONTROL
    if flags & os.O_CREAT:
        disposition = CREATE_NEW if flags & os.O_EXCL else OPEN_ALWAYS   # O_TRUNC: SetEndOfFile after open
    else:
        disposition = TRUNCATE_EXISTING if flags & os.O_TRUNC else OPEN_EXISTING
    sa = private_security_attributes(directory=False) if (private and flags & os.O_CREAT) else None
    try:
        handle = CreateFileW(str(path), access, SHARE_ALL, sa, disposition,
                             0x80 | FILE_FLAG_OPEN_REPARSE_POINT, None)
    finally:
        if sa is not None: LocalFree(sa.lpSecurityDescriptor)
    if handle == INVALID_HANDLE_VALUE:
        code = ctypes.get_last_error(); raise OSError(0, ctypes.FormatError(code), str(path), code)
    info = FILE_ATTRIBUTE_TAG_INFO()             # {DWORD FileAttributes; DWORD ReparseTag}
    GetFileInformationByHandleEx(handle, 9, ctypes.byref(info), ctypes.sizeof(info))
    if info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT and info.ReparseTag & 0x20000000:
        CloseHandle(handle); raise OSError(errno.ELOOP, "Bağlantı izlenmez", str(path))
    return msvcrt.open_osfhandle(handle, os.O_RDONLY if access_mode == os.O_RDONLY else 0)  # binary fd
```
Only the last component is not followed (same as POSIX `O_NOFOLLOW`); intermediate junctions are covered by the
existing `_contained`/`_safe` resolve + per-component checks.

A.3 private security attributes and is_private
```python
class SECURITY_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("nLength", wt.DWORD), ("lpSecurityDescriptor", wt.LPVOID), ("bInheritHandle", wt.BOOL)]

def private_security_attributes(*, directory):
    sd = wt.LPVOID()
    sddl = PRIVATE_DIR_SDDL if directory else PRIVATE_FILE_SDDL
    if not ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, 1, ctypes.byref(sd), None):
        raise ctypes.WinError(ctypes.get_last_error())
    return SECURITY_ATTRIBUTES(ctypes.sizeof(SECURITY_ATTRIBUTES), sd, False)   # caller LocalFree()s sd

DATA_RIGHTS = 0x1 | 0x2 | 0x4 | 0x20 | 0x40 | 0x10000 | 0x40000 | 0x80000 | 0xF0000000
# READ_DATA WRITE_DATA APPEND_DATA EXECUTE DELETE_CHILD DELETE WRITE_DAC WRITE_OWNER GENERIC_*

def _is_private_handle(handle):
    owner, dacl, sd = wt.LPVOID(), wt.LPVOID(), wt.LPVOID()
    if GetSecurityInfo(handle, 1, 0x1 | 0x4, ctypes.byref(owner), None, ctypes.byref(dacl), None,
                       ctypes.byref(sd)) != 0:            # OWNER | DACL
        return False
    try:
        if not dacl.value:
            return False                                  # NULL DACL = full access for everyone
        user = current_user_sid_string()
        owner_sid = sid_to_string(owner)
        if owner_sid != user and not (token_is_elevated() and owner_sid == "S-1-5-32-544"):
            return False
        allowed = PRIVATE_ALLOWED_SIDS | {user}
        size = ACL_SIZE_INFORMATION()                     # {DWORD AceCount, AclBytesInUse, AclBytesFree}
        GetAclInformation(dacl, ctypes.byref(size), ctypes.sizeof(size), 2)
        for index in range(size.AceCount):
            ace = wt.LPVOID(); GetAce(dacl, index, ctypes.byref(ace))
            ace_type, ace_flags = ctypes.c_ubyte.from_address(ace.value).value, ctypes.c_ubyte.from_address(ace.value + 1).value
            if ace_flags & 0x08:                          # INHERIT_ONLY_ACE: does not apply to this object
                continue
            if ace_type in (0, 9):                        # ACCESS_ALLOWED / ACCESS_ALLOWED_CALLBACK
                mask = ctypes.c_uint32.from_address(ace.value + 4).value
                if mask & DATA_RIGHTS and sid_to_string(ace.value + 8) not in allowed:
                    return False
            elif ace_type in (5, 11):                     # object ACEs: unusual on files, fail closed
                return False
        return True
    finally:
        LocalFree(sd)
```
Path variants open a handle with `READ_CONTROL`, `FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS`
(directories need backup semantics) instead of `GetNamedSecurityInfoW`, so a link is inspected, not followed.
`make_private`/`ensure_private_tree` = `SetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
PROTECTED_DACL_SECURITY_INFORMATION (0x4 | 0x80000000), None, None, dacl_from(sddl), None)` after checking ownership.

A.4 command line of another process
```python
class UNICODE_STRING(ctypes.Structure):
    _fields_ = [("Length", wt.USHORT), ("MaximumLength", wt.USHORT), ("Buffer", wt.LPVOID)]

def _command_line_argv(handle):
    needed = ctypes.c_ulong(0)
    ntdll.NtQueryInformationProcess(handle, 60, None, 0, ctypes.byref(needed))   # 0xC0000004 expected
    if not needed.value:
        return None
    buffer = ctypes.create_string_buffer(needed.value)
    if ntdll.NtQueryInformationProcess(handle, 60, buffer, needed, ctypes.byref(needed)) < 0:   # NTSTATUS
        return None
    text = UNICODE_STRING.from_buffer(buffer)
    line = ctypes.wstring_at(text.Buffer, text.Length // 2)
    if not line:
        return None
    count = ctypes.c_int(0)
    argv = shell32.CommandLineToArgvW(line, ctypes.byref(count))
    try:
        return [argv[index] for index in range(count.value)]
    finally:
        kernel32.LocalFree(argv)
```

A.5 PinnedProcess (Windows)
```python
class PinnedProcess:
    ACCESS = 0x00100000 | 0x1000 | 0x0001                  # SYNCHRONIZE | QUERY_LIMITED | TERMINATE
    @classmethod
    def open(cls, pid):
        handle = OpenProcess(cls.ACCESS, False, pid)
        return cls(handle) if handle else None             # ERROR_INVALID_PARAMETER: no such process
    def start_ticks(self):                                 # same value process_identity() reports
        c, e, k, u = (wt.FILETIME() for _ in range(4))
        return str((c.dwHighDateTime << 32) | c.dwLowDateTime) if GetProcessTimes(self.h, c, e, k, u) else None
    def wait(self, seconds):  return WaitForSingleObject(self.h, int(seconds * 1000)) == 0
    def signal_term(self, instance):
        event = OpenEventW(0x0002, False, stop_event_name(instance))   # EVENT_MODIFY_STATE
        if not event:
            raise OSError(errno.ESRCH, "Servis kapanış olayı bulunamadı; kapatma uygulanmadı.")
        try: SetEvent(event)
        finally: CloseHandle(event)
    def terminate(self):      TerminateProcess(self.h, 1)
    def close(self):          CloseHandle(self.h)
```
