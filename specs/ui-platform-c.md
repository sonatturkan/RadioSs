# Area spec: ui-platform-c (RadioS Windows port)

Analyzed copy: `C:\Users\sonatturkan\Desktop\RadioS` (read-only). Line numbers refer to that copy.
Toolchain checked on this machine: MSYS2 UCRT64 GLib **2.90.1**, GTK **3.24.52**, Pango **1.58.2**, json-glib 1.10.8, gcc 16.2.

Scope: `src/app/main.c`, `src/ui/window.c`, `src/ui/content_scale.c`, `src/ui/data.c`, `src/ui/reset_ui.c`,
`src/ui/preferences.c`, `src/ui/transfer_ui.c`, `src/ui/playlist.c`, `src/ui/admin_ui.c`, `src/ui/media_archive_ui.c`,
`src/ui/media.c`, `src/ui/technical.c`, plus a full grep of `src/ui/*.c` for platform constructs.

Empirical probes I compiled and ran against the MSYS2 GLib/Pango (scratch only, outside the project) are quoted as **[probe]**.
The probe sources are in `...\scratchpad\probe_ui\probe_glib.c`, `probe_pango*.c`.

---

## 0. Executive summary (what breaks and what replaces it)

| # | Location | Linux construct | Windows effect | Replacement (section) |
|---|----------|-----------------|----------------|-----------------------|
| 1 | main.c:5,7,55-56,85-87 | `glib-unix.h`, `g_unix_signal_add(SIGTERM/SIGINT)` | does not compile | termination layer: console ctrl handler + named event + WM_QUERYENDSESSION/WM_ENDSESSION (§3) |
| 2 | main.c:138-140 | `g_file_read_link("/proc/self/exe")` | NULL, then PATH search | `GetModuleFileNameW` (§2.3) |
| 3 | main.c (GUI subsystem) | prints `--version/--help/--check-config` to terminal | `-mwindows` exe has no stdout | `AttachConsole(ATTACH_PARENT_PROCESS)` when std handles are missing; log file otherwise (§2.1) |
| 4 | main.c:160 | "Wayland/X11 masaüstü oturumu gerekli." | wrong text | Windows text (§2.6) |
| 5 | window.c:748-751 | `FcInit()` + `FcConfigAppFontAddFile(woff2)` | Pango on Windows uses DirectWrite, not Fontconfig: the Inter font is **never used** (and FcInit may scan C:\Windows\Fonts on first run) | `pango_font_map_add_font_file(InterVariable.ttf)`; family name is **"Inter Variable Text"** on DirectWrite **[probe]** (§6.2) |
| 6 | data.c:376-402, admin_ui.c:694-710 | `GSubprocess` running `python3 …` | GLib spawns through `gspawn-win64-helper.exe` which calls `_wspawnv` **[probe: helper imports only `_wspawnv/_wspawnvp`]** → a console window pops up for python.exe and stays for the lifetime of the detached service | own `RadioSProcess` on `CreateProcessW` with `CREATE_NO_WINDOW` (§4) |
| 7 | data.c:388-395 (config `python_command = python3`) | PATH lookup of python3 | `python3.exe` on Windows is usually the Microsoft-Store App-Execution-Alias stub; Python stdout is cp1254 → `g_subprocess_communicate_utf8` fails on Turkish text | absolute interpreter resolution + `-X utf8 -B` + `PYTHONUTF8=1` (§4.4) |
| 8 | data.c:381-387, reset_ui.c:355-360 | flatpak-spawn | n/a | compile out (`#ifndef G_OS_WIN32`) |
| 9 | reset_ui.c:340-367 | `/proc/self/cmdline`, `sh -c "sleep 2; exec setsid run-radios.sh …"` | does not exist | re-exec own exe with same args; inherited parent-process handle wait (§5) |
| 10 | preferences.c:332-359 | `/proc/asound/card*/codec#*` parsing | always 0 ("okunamadı") | audio backend API (WASAPI probing) (§8.1) |
| 11 | preferences.c:517 | label "PulseAudio / PipeWire" | wrong | backend display name (§8.2) |
| 12 | content_scale.c:3-5,380-382,457-490 | X11-only popup re-anchoring | popups mis-placed in zoomed content on Win32 | enable the same synchronous path for `GDK_IS_WIN32_DISPLAY` (GDK Win32 uses the generic `move_to_rect` that emits `moved-to-rect` synchronously) (§7) |
| 13 | media.c:849-857 | loop climbing to `"/"` | **infinite loop / UI hang** on Windows: `g_path_get_dirname("C:\\") == "C:\\"` and the separator test never matches a root that ends in `\` **[probe]** | root-aware common-prefix (§10.3) |
| 14 | media.c:944-957 | skip names starting with `.` | imports from `$RECYCLE.BIN`, `System Volume Information`, hidden files when a drive/folder is dropped | also skip HIDDEN/SYSTEM attributes (§10.4) |
| 15 | playlist.c:1739-1774 | `openat`, `O_DIRECTORY/O_NOFOLLOW/O_CLOEXEC/O_NONBLOCK`, `st_mtim/st_ctim` | does not compile on MinGW | platform no-follow open + file identity (§11.1) |
| 16 | playlist.c:1962-2010,2029,2106-2107 | `GVolumeMonitor` USB list (`g_mount_can_unmount`, `g_mount_get_uuid`) | GWin32VolumeMonitor mounts cannot unmount, no uuid → empty list | Win32 volume enumeration (§11.2) |
| 17 | media_archive_ui.c:553,701 | `realpath()` | not in MinGW CRT | `radios_platform_realpath` (§13) |
| 18 | admin_ui.c:658 | `g_shell_quote` for a command the operator must run | prints POSIX `'…'` quoting **[probe]** | Windows quoting for display (§12.2) |
| 19 | transfer_ui.c:522 | only `/` forbidden in a file name | `\ : * ? " < > |`, `CON`, trailing dot/space accepted → failures or alternate data streams | portable file-name validator (§9.2) |
| 20 | audio_ui.c:918-921 | case-sensitive `g_str_has_prefix(path, db + ".objects/")` | drive-letter/case differences refuse playback (`g_canonicalize_filename` keeps `c:` lower-case **[probe]**) | case-insensitive, separator-aware prefix (§14.1) |
| 21 | many src/ui files | `g_time_zone_new_identifier("Europe/Istanbul")` | **returns NULL on Windows [probe]** → clocks/calendars silently fall back to UTC | cross-area time-zone shim (§14.8) |
| 22 | CMakeLists/tests | `GDK_BACKEND=x11`, Xvfb, XTest, `kill -TERM`, `fcntl.flock`, `WORKING_DIRECTORY /tmp` | cannot run | §15 |

**Service lifetime decision (asked by the lead): the Linux design intends the SDBASE service to OUTLIVE the GUI.**
Evidence: `tools/radios_database.py:472-474` starts `serve` with `start_new_session=True`; the GUI never calls `stop`
(`data.c:105-114` only force-exits the *launcher* if it is still running); `tests/test_application_shutdown.py:226-228,236-239`
fails if the service PID or sequence changes across GUI shutdown/reopen ("Application signal stopped/replaced its service").
**Therefore: never place the service (or the launcher that spawns it) in a kill-on-close Job Object owned by the GUI.**
The Python launcher must spawn `serve` with `CREATE_BREAKAWAY_FROM_JOB` (falls back without it when the job forbids breakaway)
so that a GUI started inside someone else's job (CI runner, terminal host) does not take the service down with it (§4.6).

---

## 1. Internal contracts that must be preserved

* `int main(int argc, char **argv)` option surface (main.c:103-112): `--project-root`, `--config`, `--profile`, `--check-config`,
  `--smoke-test`, `--capture`, `--version`; invalid combinations exit 1 (main.c:123-128); `--version` prints
  `RadioS <ver> — C / GTK 3` exit 0; `--check-config` prints the 5-line summary exit 0; smoke test exit 1 when
  `radios_window_smoke_passed` is false (main.c:171-172). `tools/radios_launch.py:79-80` treats
  `--version --help -h --help-all --check-config --smoke-test` as *passive* (no DB init/start) – keep that set on Windows.
* Orderly teardown order (must be identical on every termination path): `g_application_quit` → GtkApplication `shutdown` →
  `radios_window_free` → (`window_destroyed`: players close, playout close, advert close, broadcast close/prepare, calendar
  prepare_close, dj prepare_close, audio prepare_close, `radios_data_close`, `radios_audio_ui_close` (WAV finalization),
  preferences save) → `radios_runtime_shutdown` (durable pending-write preservation) → drain main context
  (main.c:80-95, window.c:614-639, 969-1011). Exit code 0 after a graceful request (test_application_shutdown.py:121-127).
* `radios_window_new` gating: data/audio/players/GUI lock exist only when `config/credentials.json` exists and not smoke
  test (window.c:718-723, 783-786). GUI lock is an **int fd** closed with `close()` (window.c:725,755,1009); shared lock,
  `G_IO_ERROR_BUSY` when a restore is pending (recovery.c:358-376).
* `RadioSData` lifecycle (data.h): `radios_data_connect` probes the running service first (CONNECT handshake), and only on
  `TRANSPORT/TIMEOUT/UNAVAILABLE/SCHEMA_MISMATCH` (data.c:408-414) runs the lifecycle helper
  `<python> tools/radios_database.py --project-root R --config C start`; auth/identity errors never start anything
  (test `auth_error_does_not_start_service`, tests/test_data_connection_ui.c:118-129). A 5 s `recovery_tick` reconnects/
  restarts (data.c:344-352, 423). Launcher timeout 15 s → force exit (data.c:369-375, 400). Failure notices:
  "Veri servisi çalıştırıcısı açılamadı." (spawn failed, data.c:399) vs "Yerel veri servisi açılamadı; …" (non-zero exit).
  Tests set `python_command = "/RadioS-no-lifecycle-helper"` to prove the helper is not needed when a service is already
  running (test_data_connection_ui.c:69-71): **an explicitly configured interpreter path must fail closed, never fall back.**
* `radios_reset_ui_restart(ctx, error)` (reset_ui.h:11): returns TRUE once a new instance is scheduled, then closes
  `ctx->parent` with `gtk_window_close` (normal teardown). Used by reset_ui.c:160 and media_archive_ui.c:739.
* Window drop contract (media.c:1326-1336): target `text/uri-list`, info 0, format 8, ≤1 MiB, no NUL, ≤256 URIs, only
  `file:` URIs without a host other than `localhost`, absolute paths; folders become playlists (window drop only);
  `gtk_drag_finish(..., success, del=FALSE)` (COPY, never MOVE); a NULL context is valid (programmatic dispatch in tests).
* Content scale API (content_scale.h) unchanged; test seam env `RADIOS_CONTENT_SCALE_POPUP_MAP_ONLY=1` disables the
  synchronous re-anchoring (content_scale.c:235,483) – keep it on Win32 too.

---

## 2. Startup (src/app/main.c)

### 2.1 GUI subsystem, console attachment, logging

The executable is linked `-mwindows` (GUI subsystem) with the lead's manifest (activeCodePage UTF-8, PerMonitorV2 DPI,
`requestedExecutionLevel asInvoker`; also add `longPathAware=true`). Consequences and design:

1. **Before any stdio use** (first statement of `main`, after the relaunch wait of §5.3):
   `radios_platform_attach_parent_console()`:
   * `HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE)`; a handle is *usable* when it is
     non-NULL, not `INVALID_HANDLE_VALUE` and `GetFileType(h) != FILE_TYPE_UNKNOWN`.
   * If both are usable (CTest/Python `subprocess` pipes, `> file` redirection from cmd) → do nothing (the UCRT already
     wired fd 0/1/2 to them).
   * Otherwise `AttachConsole(ATTACH_PARENT_PROCESS)`; on success `freopen("CONOUT$", "w", stdout)` / `stderr` for the
     unusable ones (and `freopen("CONIN$","r",stdin)` is not needed). Return TRUE when attached.
   * `g_print`/`g_printerr` convert UTF-8 to the console's output code page themselves (GLib `g_get_console_charset`),
     Turkish letters exist in CP857; "—" degrades to "?" – acceptable. Do **not** call `SetConsoleOutputCP` (it changes the
     parent shell's console permanently).
   * Started from Explorer/Start menu: `AttachConsole` fails → no console; see logging below.
2. Behavioural caveat to document (README/tests): cmd.exe and PowerShell do not wait for GUI-subsystem programs, so
   `radios.exe --version` prints after the prompt and `%ERRORLEVEL%` is not the program's exit code. Python
   `subprocess.run`, CTest and `Start-Process -Wait` do wait. Optional (not required by the lead's decision): ship a
   console-subsystem twin `radios-cli.exe` built from the same `main.c` without `-mwindows` for scripted use.
3. **GUI mode**: once the main window has been presented, if we attached to a parent console, call `FreeConsole()` unless
   `RADIOS_KEEP_CONSOLE=1`. Rationale: the shell returns to its prompt immediately, so a later Ctrl+C typed at that prompt
   (or closing the terminal) would otherwise deliver CTRL_C/CTRL_CLOSE to the on-air application.
4. **Log sink when there is no usable stderr** (Explorer launch, or after `FreeConsole`): open
   `<log_root>/radios-desktop.log` (config `log_root`, default `data/logs`) in append mode and redirect `stderr` to it
   (`_wfreopen`/`freopen` with a UTF-8 path; share mode deny-none so several GUI instances can append). Rotate at startup
   only (rename to `.1` if > 5 MiB, before opening – Windows cannot rename an open file). This keeps all existing
   `g_printerr` diagnostics (window.c:588,634,666,698,703; main.c:68,176) observable.
5. Binary default file mode (`_fmode=_O_BINARY`, lead decision): all `g_print` output uses LF; consoles render LF fine.

### 2.2 Command line (UTF-8)

* Replace `g_option_context_parse(&argc, &argv)` (main.c:121) with
  `gchar **args = radios_platform_get_argv(argc, argv)` (Windows: `g_win32_get_command_line()`, POSIX: `g_strdupv(argv)`)
  and `g_option_context_parse_strv(context, &args, &error)`; replace the `argc != 1` check (main.c:123) with
  `g_strv_length(args) != 1`. Pass `args` to `g_application_run(app, 1, args)`. This keeps non-ASCII paths correct even if
  the UTF-8 manifest is missing (GLib filenames are always UTF-8 on Windows; `G_OPTION_ARG_FILENAME` returns UTF-8).
* `setlocale(LC_ALL, "")` (main.c:118) stays. With the UTF-8 ACP manifest the UCRT locale becomes `Turkish_Türkiye.utf8`;
  decimal-comma behaviour is the same risk as tr_TR on Linux (code already uses `g_ascii_*`).

### 2.3 Executable path and project root

* Replace main.c:138-140 with `executable = radios_platform_executable_path(NULL)`:
  `GetModuleFileNameW(NULL, buf, n)` in a loop doubling `n` up to 32768 while the return equals `n`
  (`ERROR_INSUFFICIENT_BUFFER`), strip a leading `\\?\` (and convert `\\?\UNC\` to `\\`), convert with `g_utf16_to_utf8`.
  POSIX branch keeps `/proc/self/exe` then `g_find_program_in_path(argv[0])`.
* `radios_project_root_find()` (config.c:118-169, core area) walks up from the exe directory until
  `config/radios.example.ini` and `database/SDBASE/pyproject.toml` exist; it uses `realpath()` (config.c:133,147,157) which
  MinGW lacks → core area must use `radios_platform_realpath` (GetFinalPathNameByHandleW, §13). Any exe depth works; the
  recommended product layout is `<root>\runtime\bin\radios.exe` (+ DLLs), GTK data in `<root>\runtime\{share,lib,etc}`,
  CPython 3.13 embeddable in `<root>\runtime\python\` (packaging area decides; root discovery needs no change).
* Pitfall: `GetFinalPathNameByHandleW` turns a mapped drive (`Z:\RadioS`) into `\\server\share\RadioS` and a `subst`
  drive into its target. The C side and the Python side must canonicalize the root identically (Python `os.path.realpath`
  uses the same API since 3.8) and compare case-insensitively (`os.path.normcase`), because `_owned()` compares
  `state["project_root"] == str(cfg.project_root)` (radios_database.py:357).
* Product install location must be user-writable (root-relative `data/`, `config/`): not `C:\Program Files`. Prefer a
  per-user install (`%LOCALAPPDATA%\Programs\RadioS`) or a dedicated `C:\RadioS`. Controlled Folder Access may block
  writes under Documents/Music.

### 2.4 First-run preparation (launcher duties move into the exe on Windows)

On Linux the user starts `run-radios.sh` → `tools/radios_launch.py`, which (non-passive modes) validates config with
`radios --check-config`, runs `init` on a fresh/recovering installation under `operation_lock`, `start`s the owned service
and finally `execv`s the GUI (radios_launch.py:98-126). On Windows the Start-menu shortcut targets `radios.exe` directly,
and `window.c` only creates the data layer when `credentials.json` already exists. Design:

* In `main` (Windows build; optionally both), after config load and before `gtk_application_new`, for non-passive modes:
  if `config/credentials.json` is missing **or** `<runtime_dir>/setup-pending.json` exists, run synchronously
  `<python> -X utf8 -B tools/radios_database.py --project-root R --config C prepare` through `RadioSProcess` (§4) with
  `CREATE_NO_WINDOW`, timeout 120 s. `prepare` is a **new Python subcommand (tools area)** with exactly
  radios_launch.py:110-125 semantics (port-occupied check for fresh installs, `initialize`, `start_service`, all under
  `operation_lock`). On failure show a `GtkMessageDialog` with the helper's stderr (redacted by the helper) and exit 1
  (parity: the Linux launcher aborts the launch).
* When already installed nothing is run here: `data.c` probes and starts the service exactly as on Linux.
* Optional UX: show a small undecorated "Veritabanı hazırlanıyor…" window while `prepare` runs (only first run).

### 2.5 Other startup items

* `gtk_init_check` failure text (main.c:160): Windows → "Yerel ekran açılamadı. Etkileşimli bir Windows masaüstü oturumu
  gerekli." (keep Linux text under `#ifdef`).
* `SetCurrentProcessExplicitAppUserModelID(L"RadioS.Desktop")` before the first window (taskbar grouping/pinning; the
  installer's shortcut must carry the same `System.AppUserModel.ID`).
* GApplication + D-Bus on Windows: `gtk_application_new(..., G_APPLICATION_NON_UNIQUE)` (main.c:166) still calls
  `g_application_impl_register`, which may autolaunch a session bus by spawning `gdbus.exe _win32_run_session_bus`
  (strings in libgio: "autolaunch:", "gdbus.exe", "g_win32_run_session_bus"). That process outlives RadioS. Do **not**
  ship `gdbus.exe`; verify no lingering `gdbus.exe` and no GLib warning (tests run with `G_DEBUG=fatal-warnings`). If
  needed, set `DBUS_SESSION_BUS_ADDRESS` to an unusable value (e.g. `disabled:`) before creating the application when it is
  unset.
* Exe resources (build area): `.rc` with the application icon (GDK uses the exe's first icon for window classes),
  VERSIONINFO and the manifest.
* `<Primary>q` accelerator (main.c:47) = Ctrl+Q on Windows; Alt+F4/close button arrive as WM_CLOSE → GTK delete-event →
  normal teardown (no `delete-event` handler on the main window; only dj.c:281 on a detached mixer window).

---

## 3. Termination (replacement of `g_unix_signal_add`)

### 3.1 Linux behaviour to preserve

main.c:55-56 installs SIGTERM/SIGINT sources on the GTK main context; `termination_requested` (main.c:25-32) calls
`g_application_quit`, which runs the full teardown of §1 and exits 0. The sources are removed in `shutdown_application`
(main.c:85-87).

### 3.2 Platform API (shared C layer)

```c
/* include/radios/platform.h */
typedef enum {
    RADIOS_TERMINATE_REQUEST,   /* SIGTERM/SIGINT, Ctrl+C/Break, named stop event: ask GTK thread to quit */
    RADIOS_TERMINATE_NOW        /* session end / console close: teardown must finish before returning */
} RadioSTerminateKind;
typedef struct RadioSTermination RadioSTermination;
typedef void (*RadioSTerminateFunc)(RadioSTerminateKind kind, gpointer data);  /* always invoked on the GTK thread */
RadioSTermination *radios_termination_install(RadioSTerminateFunc func, gpointer data);
void radios_termination_teardown_done(RadioSTermination *t);  /* signals waiters of RADIOS_TERMINATE_NOW */
void radios_termination_remove(RadioSTermination *t);         /* idempotent; joins wait threads */
gchar *radios_termination_event_name(guint32 pid);            /* "Local\\RadioS.Desktop.Terminate.<pid>" (Windows) */
```

POSIX implementation: today's two `g_unix_signal_add` sources (callback → `RADIOS_TERMINATE_REQUEST`).

### 3.3 Windows sources (all funnel into the same callback on the GTK thread)

1. **Named stop event** (SIGTERM equivalent for tools/tests): `CreateEventW(NULL /*default DACL: same user, SYSTEM,
   admins*/, TRUE, FALSE, L"Local\\RadioS.Desktop.Terminate.<pid>")`; `RegisterWaitForSingleObject(..., WT_EXECUTEONLYONCE)`;
   the thread-pool callback does `g_idle_add_full(G_PRIORITY_HIGH, request_idle, ...)`. Remove with
   `UnregisterWaitEx(wait, INVALID_HANDLE_VALUE)` (blocks until a running callback finished). A stopper opens the event by
   PID (`OpenEventW(EVENT_MODIFY_STATE, ...)`) and `SetEvent`s it. PID reuse is not a concern: the named object exists only
   while this process (or a stopper holding a handle) lives.
2. **Console control** (only while attached to a console, e.g. `RADIOS_KEEP_CONSOLE=1`, smoke tests from a terminal):
   `SetConsoleCtrlHandler(handler, TRUE)`. `CTRL_C_EVENT/CTRL_BREAK_EVENT` → schedule REQUEST, return TRUE.
   `CTRL_CLOSE_EVENT` (and LOGOFF/SHUTDOWN for completeness) → schedule NOW and `WaitForSingleObject(teardown_done, 4500)`
   (the system kills the process ~5 s after the handler returns), return TRUE. Note: for processes that load user32,
   LOGOFF/SHUTDOWN are *not* delivered to console handlers – item 3 covers them.
3. **Session end / Restart Manager**: `gdk_window_add_filter(NULL, session_filter, app)` installed in `startup` (GDK is
   initialized). The filter receives `MSG*` (GdkXEvent) for every GDK top-level:
   * `WM_QUERYENDSESSION`: first time only, `ShutdownBlockReasonCreate(msg->hwnd, L"RadioS kayıtları ve yayını güvenle
     kapatıyor…")`; return `GDK_FILTER_CONTINUE` (DefWindowProc answers TRUE = allow). Returning `GDK_FILTER_REMOVE` would
     make GDK return 0 = veto (GDK returns `ret_val` initialised to 0 when a filter removes the message).
   * `WM_ENDSESSION` with `wParam == TRUE` (also `lParam & ENDSESSION_CLOSEAPP` from Restart Manager/installers): call the
     callback with `RADIOS_TERMINATE_NOW` **synchronously** (we are on the GTK thread inside GDK's window procedure), then
     `ShutdownBlockReasonDestroy`, return `GDK_FILTER_REMOVE`. After WM_ENDSESSION returns Windows may terminate the
     process at any time, so the teardown cannot be deferred to the main loop.
   * `WM_ENDSESSION` with `wParam == FALSE`: shutdown cancelled → `ShutdownBlockReasonDestroy`, continue running.
4. **WM_CLOSE** (title-bar ×, Alt+F4, `taskkill /PID n` without `/F`): already handled by GTK (window destroy →
   GtkApplication quits). No code needed; tests can use it (§15).

### 3.4 Teardown factoring (main.c)

* Move the body of `shutdown_application` (main.c:80-95) into an idempotent
  `static void application_teardown(RadioSApplication *app)` guarded by `app->torn_down`; `shutdown_application` calls it.
* `RADIOS_TERMINATE_REQUEST` → `g_application_quit(app)` (G_SOURCE_REMOVE for idle sources; the POSIX unix-signal source
  keeps returning `G_SOURCE_CONTINUE` as today).
* `RADIOS_TERMINATE_NOW` → `application_teardown(app)`; `radios_termination_teardown_done()`; `g_application_quit(app)`.
  `radios_window_free` destroys the toplevel inside its own WM_ENDSESSION dispatch – allowed in Win32 and GDK holds a
  reference on the GdkWindow during `gdk_event_translate`; `radios_runtime_shutdown` is already idempotent
  (runtime.c:242-256, `worker == NULL` → return). The later `shutdown` signal (if the process survives) is a no-op.
* Replace `guint termination_source, interrupt_source` (main.c:22) with `RadioSTermination *termination`; remove in
  `application_teardown`.
* Optional: `SetProcessShutdownParameters(0x3FF, 0)` so RadioS is asked before most applications (more time to finalize
  recordings).

---

## 4. Data service spawn / stop / monitor (src/ui/data.c) and other helper processes

### 4.1 Current behaviour (data.c)

* `start_local_service` (data.c:376-402): argv `[flatpak-spawn --host] <python_command> <root>/tools/radios_database.py
  --project-root <root> --config <config_file> start`, `G_SUBPROCESS_FLAGS_STDOUT_PIPE|STDERR_PIPE` (stdin = /dev/null by
  GSubprocess default), inherits the GUI's cwd and environment (on Linux the env includes `PYTHONDONTWRITEBYTECODE=1`
  exported by run-radios.sh:4), 15 s timeout → `g_subprocess_force_exit` (SIGKILL), output read with
  `g_subprocess_communicate_utf8_async` and discarded; success = exit status 0 → CONNECT request (data.c:353-368).
* The helper (`start_service`, radios_database.py:429-506) spawns the detached `serve` process and returns after the
  service answered `/health` and `/v1/capabilities` (deadline `max(connect+3, 60 s)`), so the GUI-visible launcher can take
  up to ~60 s while the store opens.
* Stop: the GUI never stops the service; `radios_data_close` (data.c:105-114) only force-exits a still-running launcher.
* Monitor: `recovery_tick` every 5 s (data.c:344-352): reconnect (and thus restart through the helper) when disconnected;
  replay pending durable operations.

### 4.2 Process API (shared C layer; POSIX wraps GSubprocess, Windows uses CreateProcessW)

```c
typedef enum {
    RADIOS_PROCESS_CAPTURE_STDOUT = 1 << 0,
    RADIOS_PROCESS_CAPTURE_STDERR = 1 << 1,
    RADIOS_PROCESS_NO_WINDOW      = 1 << 2,  /* Windows: CREATE_NO_WINDOW; POSIX: no-op */
    RADIOS_PROCESS_DETACHED       = 1 << 3,  /* new process group (setsid / CREATE_NEW_PROCESS_GROUP),
                                                CREATE_BREAKAWAY_FROM_JOB when allowed, stdio -> NUL */
    RADIOS_PROCESS_PASS_SELF_HANDLE = 1 << 4 /* Windows relaunch: inherit a SYNCHRONIZE handle to this process,
                                                exported as RADIOS_RELAUNCH_WAIT_HANDLE=<decimal> */
} RadioSProcessFlags;
typedef struct RadioSProcess RadioSProcess;            /* ref-counted */
RadioSProcess *radios_process_spawn(const gchar *const *argv, const gchar *working_directory,
                                    const gchar *const *env_overrides /* "NAME=VALUE" or "NAME=" to unset; NULL */,
                                    RadioSProcessFlags flags, GError **error);
void     radios_process_communicate_utf8_async(RadioSProcess *p, gsize max_bytes_per_stream,
                                               GCancellable *c, GAsyncReadyCallback cb, gpointer user);
gboolean radios_process_communicate_utf8_finish(RadioSProcess *p, GAsyncResult *r,
                                                gchar **stdout_text, gchar **stderr_text, GError **error);
gboolean radios_process_get_successful(RadioSProcess *p);   /* exited, exit code 0 */
gint     radios_process_get_exit_status(RadioSProcess *p);
void     radios_process_force_exit(RadioSProcess *p);        /* SIGKILL / TerminateProcess(h, 1); idempotent */
RadioSProcess *radios_process_ref(RadioSProcess *p);
void     radios_process_unref(RadioSProcess *p);
gboolean radios_process_spawn_detached(const gchar *const *argv, const gchar *cwd,
                                       const gchar *const *env_overrides, RadioSProcessFlags flags, GError **error);
```

Windows implementation requirements:
* `lpApplicationName` = the resolved **absolute** program path (never let CreateProcess search the cwd/app dir);
  `lpCommandLine` built from argv with MSVCRT/`CommandLineToArgvW` quoting rules (backslashes before quotes doubled,
  args with spaces/tabs/quotes/empty quoted). Same helper is used for display (§12.2).
* `STARTUPINFOEXW` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` containing only the child's std handles (NUL for stdin,
  pipe write ends) and, with `PASS_SELF_HANDLE`, the duplicated process handle; `bInheritHandles=TRUE`;
  flags `EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW` (+ `CREATE_NEW_PROCESS_GROUP |
  CREATE_BREAKAWAY_FROM_JOB` for DETACHED, retrying without BREAKAWAY on `ERROR_ACCESS_DENIED`).
* Pipes: `CreatePipe` with non-inheritable read ends; wrap read ends in `g_win32_input_stream_new(handle, TRUE)`
  (`gio-windows-2.0`, header `gio/gwin32inputstream.h`) and splice each into a `GMemoryOutputStream` asynchronously
  (GLib does the same for GSubprocess); enforce `max_bytes_per_stream` (admin tool: 1 MiB+1; launcher: 64 KiB). Close the
  parent's copies of the write ends immediately after CreateProcess so EOF is seen when the child exits.
* Exit: wait with `g_child_watch_add((GPid)process_handle, ...)` (GPid is a HANDLE on Windows); exit code via
  `GetExitCodeProcess`. `communicate_finish` succeeds only after both streams hit EOF **and** the child exited.
* Output must be valid UTF-8 (the callers use the `_utf8` variant semantics: invalid UTF-8 → error). Guaranteed by §4.4.
* Environment: `GetEnvironmentStringsW` + overrides, sorted case-insensitively, double-NUL terminated.
* Do not keep the GLib spawn helpers as a dependency of this path (they remain packaged for any other GLib/GTK code that
  spawns: `gspawn-win64-helper.exe`, `gspawn-win64-helper-console.exe`).

### 4.3 data.c changes

* `GSubprocess *launcher` → `RadioSProcess *launcher` (data.c:14, 94, 113, 356-367, 373, 397-401).
* Compile out the flatpak branch (data.c:380-387, 391) on Windows.
* argv on Windows: `[python_abs, "-X", "utf8", "-B", <root>\tools\radios_database.py, "--project-root", root,
  "--config", config_file, "start"]`; cwd = `config->project_root` (Linux inherits the GUI cwd; on Windows a child's cwd
  handle prevents renaming/deleting that directory, so never inherit an arbitrary cwd); env overrides
  `PYTHONUTF8=1`, `PYTHONIOENCODING=utf-8`, `PYTHONDONTWRITEBYTECODE=1`; flags `CAPTURE_STDOUT|CAPTURE_STDERR|NO_WINDOW`.
  Do not put the launcher in a job (parity: Linux lets it finish if the GUI dies).
* Timeout: keep 15 s on Linux; on Windows use `MAX(15, connect_timeout_seconds + 3 + 60)` = ~68-75 s. Reason: the helper
  itself waits up to 60 s (SERVICE_START_TIMEOUT_SECONDS) and a cold embedded CPython + Defender scanning + store open can
  exceed 15 s; killing the launcher *before* it reached `Popen` would make the 5 s recovery tick restart it forever.
* Keep error mapping and notices exactly (data.c:360-365, 399, 408-417).
* Spawn failure for an explicitly configured but missing interpreter must still produce
  "Veri servisi çalıştırıcısı açılamadı." (test semantics, §1).

### 4.4 Interpreter resolution (C side) – `gchar *radios_python_executable(const RadioSConfig *config, GError **error)`

Used by data.c, admin_ui.c and the first-run `prepare` (§2.4). Order:
1. `RADIOS_PYTHON` environment variable if set and non-empty (developer/test override): must be an absolute existing file.
2. `config->python_command`:
   a. absolute (`g_path_is_absolute`; note `"/RadioS-no-lifecycle-helper"` is absolute on Windows too **[probe]**) → use as
      is; if it does not exist → error (fail closed, no fallback);
   b. contains `/` or `\` → resolve against `config->project_root` (not the cwd), canonicalize, must exist;
   c. bare `python3`, `python` or `py` (the Linux default token in `config/radios.example.ini`) → bundled interpreter
      `<root>\runtime\python\python.exe` (path agreed with packaging); if absent → `SearchPathW(NULL, L"python.exe")`
      skipping anything under `%LOCALAPPDATA%\Microsoft\WindowsApps\` or any file whose reparse tag is
      `IO_REPARSE_TAG_APPEXECLINK` (Store alias stub);
   d. other bare names → `SearchPathW(name[.exe])` with the same alias filter.
3. Return an absolute UTF-8 path. Never pass a bare name to CreateProcess.

Why `-X utf8` on the command line and not only `PYTHONUTF8`: the embeddable distribution's `._pth` file puts Python in
isolated-like mode where `PYTHON*` variables can be ignored; Python 3.13 on Windows otherwise writes pipes in the ANSI code
page (cp1254), `radios_database.py:671` prints JSON with `ensure_ascii=False` (Turkish paths, e.g. the test root
"RadioS Türkçe kapanış"), and `communicate_utf8` would reject the bytes – the GUI would report a failed start although the
service started (or Python would crash with UnicodeEncodeError for characters outside cp1254).

Windows packaging note: `config/radios.example.ini` for Windows should say `python_command = runtime/python/python.exe`
(relative, forward slashes: passes the Python validator regex at radios_database_setup.py:165, and C resolves it against the
root).

### 4.5 admin_ui.c reliability tool (same mechanism)

admin_ui.c:694-710 spawns `<python> tools/radios_reliability.py --project-root R --config C <op> [--backup name]`, parses ≤1 MiB
JSON from stdout (admin_ui.c:632), 60 s timeout → force exit (687-693), force exit on page destroy (1072-1080). Windows:
`RadioSProcess` with the same argv prefix as §4.3 (`-X utf8 -B`), cwd = root, `NO_WINDOW`, capture both streams,
`max_bytes_per_stream = 1 MiB + 1`. `TerminateProcess` during `create`/`restore-stage` leaves partial files exactly like
SIGKILL on Linux (the tool's staging/atomic-rename design must tolerate it – tools area).

### 4.6 Service lifetime, jobs and session end (cross-area with tools/SDBASE)

* Do not assign the service to any GUI job object (see summary). Python launcher: `subprocess.Popen(..., creationflags=
  CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB)` with fallback without BREAKAWAY, `stdin=DEVNULL`,
  stdout/stderr = log handle, `close_fds=True` (default handle-list behaviour), cwd = root; `start_new_session` is ignored on
  Windows.
* At logoff the hidden-console `serve` process receives `CTRL_LOGOFF_EVENT`; Python's default ends the process abruptly.
  The service area must install a console control handler that sets its `stopping` event and blocks until
  `server_close()` finished (≤ ~4.5 s). The GUI's own WM_ENDSESSION teardown may run concurrently; durable operations
  are preserved locally by `radios_data_preserve` (recovery store), so ordering is not required.
* Binding `127.0.0.1:<port>` does not trigger a Windows Firewall prompt (loopback only). Port 8000 can fall inside a
  Hyper-V/WSL *excluded port range* (`netsh int ipv4 show excludedportrange protocol=tcp`) → bind fails with WinError 10013;
  the message should point to `service_url` in `config/radios.ini`.

---

## 5. Relaunch after reset (src/ui/reset_ui.c)

### 5.1 Current behaviour (reset_ui.c:333-371)

Reads `/proc/self/cmdline`, drops argv[0] and any `--project-root[=]` pair (the launcher adds its own), then
`g_spawn_async(project_root, ["sh","-c","sleep 2; exec setsid \"$0\" \"$@\" > /dev/null 2>&1 < /dev/null",
<root>/run-radios.sh, args…], SEARCH_PATH | STDOUT/STDERR_TO_DEV_NULL)` (optionally via `flatpak-spawn --host`), and on
success `gtk_window_close(ctx->parent)`. The 2 s sleep lets the old process release the GUI lock/audio devices; the launcher
rebuilds (source installs), (re)starts the service and execs the GUI.

### 5.2 Windows design

```c
#ifdef G_OS_WIN32
gboolean radios_reset_ui_restart(RadioSViewContext *ctx, GError **error)
{
    exe  = radios_platform_executable_path(error);          /* §2.3 */
    argv = radios_platform_get_argv(0, NULL);               /* g_win32_get_command_line(), UTF-8, includes argv[0] */
    new_argv = [exe] + original[1..] minus {"--project-root" + value, "--project-root=*",
                                             "--smoke-test", "--capture" + value, "--capture=*"}
               + ["--project-root", ctx->config->project_root];
    ok = radios_process_spawn_detached(new_argv, ctx->config->project_root,
             (const gchar *[]){"RADIOS_KEEP_CONSOLE=", NULL},
             RADIOS_PROCESS_DETACHED | RADIOS_PROCESS_PASS_SELF_HANDLE, error);
    if (ok && ctx->parent) gtk_window_close(ctx->parent);
    return ok;
}
#endif
```
* The child is a GUI-subsystem exe: no console is created; stdio → NUL.
* `PASS_SELF_HANDLE`: `DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &h, SYNCHRONIZE,
  TRUE, 0)`, put `h` in the handle list, export `RADIOS_RELAUNCH_WAIT_HANDLE=<(guintptr)h>` in the child's environment.
  Close the parent's copy after CreateProcess.
* The new instance runs §2.4 (first-run prepare – normally a no-op) and `data.c` (re)starts the service if needed, which
  replaces what `run-radios.sh` did on Linux. There is no build step on Windows.

### 5.3 Child side (first statements of `main`, Windows only)

`radios_platform_relaunch_wait()`: read and `g_unsetenv("RADIOS_RELAUNCH_WAIT_HANDLE")`; parse the decimal value; accept it
only if `GetProcessId(h) != 0` (i.e. it really is a process handle); `WaitForSingleObject(h, 30000)`; `CloseHandle(h)`.
On timeout continue anyway (the GUI lock is shared; audio device conflicts are reported by the audio layer). This
replaces the fixed `sleep 2` with a deterministic wait for the old instance's full teardown (WAV finalization, WASAPI
exclusive-mode release).

---

## 6. Main window (src/ui/window.c)

### 6.1 GUI lock

`radios_recovery_gui_lock` (core/recovery.c:358-376, cross-area) must keep returning an int CRT fd so window.c:725,755,1009
(`close(fd)`, `<unistd.h>` exists in MinGW-w64) stay unchanged: Windows implementation opens
`data\database-service\gui.lock` with `CreateFileW` (`FILE_SHARE_READ|FILE_SHARE_WRITE`, no-follow via
`FILE_FLAG_OPEN_REPARSE_POINT` + reparse rejection), takes a **shared** `LockFileEx` (no `LOCKFILE_EXCLUSIVE_LOCK`,
`LOCKFILE_FAIL_IMMEDIATELY`) on a high byte offset (lead decision), and returns `_open_osfhandle((intptr_t)h, _O_RDWR)`;
`close(fd)` closes the HANDLE and releases the lock. Ownership/permission checks (`st_uid == getuid()`, `!(st_mode & 077)`)
need the core area's Windows equivalent (owner SID == current user SID, no ACE granting access to other principals).
Note: Windows releases byte-range locks of a terminated process "eventually" – tests must poll (§15).

### 6.2 Fonts (window.c:23, 735, 748-751)

* Keep the Linux branch. Windows branch (or `radios_platform_register_app_font(assets_root, &error)`):
  `pango_font_map_add_font_file(pango_cairo_font_map_get_default(), <assets>\fonts\InterVariable.ttf, &error)`.
  **[probe]** `PangoCairoWin32FontMap` rejects the WOFF2 ("not supported by DirectWrite"), accepts the TTF (already present
  in `assets\fonts\InterVariable.ttf`, 879708 bytes), and exposes it as family **"Inter Variable Text"**; requests for
  "Inter Variable"/"Inter" fall back to Tahoma.
* Therefore prepend `"Inter Variable Text"` to the font-family lists: `assets/themes/radios.css:4`
  (`.radios { font-family: "Inter Variable Text", "Inter Variable", "Inter", sans-serif; ... }`) and
  `src/ui/preferences.c:72` (`'Inter Variable Text', 'Inter Variable', 'Inter', sans-serif`). Unknown families are
  skipped by Fontconfig, so the change is portable. No other CSS references Inter.
* Do not call `FcInit()` on Windows (it would load `etc\fonts\fonts.conf` and may build a cache of C:\Windows\Fonts on
  first run). Keep the error "Tasarımın Inter fontu yüklenemedi." on failure (startup aborts as on Linux).
* Fallback only if DirectWrite registration ever fails: `g_setenv("PANGOCAIRO_BACKEND", "fc", TRUE)` before `gtk_init`
  plus the Fontconfig path (requires bundled `fonts.conf`).

### 6.3 Other window.c items

* Default size 1800×1000 (window.c:774): Windows does not clamp oversized windows like Linux WMs; clamp to the primary
  monitor work area (`gdk_monitor_get_workarea`) at creation.
* Title updates every 250 ms (window.c:537-568) → `SetWindowTextW`; fine.
* `capture_window` (window.c:641-668) renders with `gtk_widget_draw` into an image surface – works without screen pixels;
  `cairo_surface_write_to_png` accepts the UTF-8 path.
* `gtk-application-prefer-dark-theme` (window.c:765) works with built-in Adwaita on Windows.
* Media drop binding on the toplevel (window.c:782) → §10.

---

## 7. Content scale (src/ui/content_scale.c)

The widget is backend-neutral (GDK offscreen windows: `GDK_WINDOW_OFFSCREEN` parented to the root window,
content_scale.c:56-84). Win32-specific work:

1. **Synchronous popup re-anchoring** (content_scale.c:457-490): enable for Win32. GDK Win32 uses the generic
   `move_to_rect` implementation (gdkwindowimpl.c), which positions the popup and emits `moved-to-rect` synchronously
   **[probe: `probe_moverect.c` – on `GdkWin32Display` the signal fired inside `gdk_window_move_to_rect`, before any
   main-loop iteration]**; only Wayland is asynchronous – the X11 assumptions hold.
   ```c
   #ifdef GDK_WINDOWING_X11
   #include <gdk/gdkx.h>
   #endif
   #ifdef GDK_WINDOWING_WIN32
   #include <gdk/gdkwin32.h>
   #endif
   static gboolean synchronous_popups(GdkDisplay *d) {
   #ifdef GDK_WINDOWING_X11
       if (GDK_IS_X11_DISPLAY(d)) return TRUE;
   #endif
   #ifdef GDK_WINDOWING_WIN32
       if (GDK_IS_WIN32_DISPLAY(d)) return TRUE;
   #endif
       return FALSE;
   }
   ```
   Compile `popup_window_moved` when either backend is present; `watch_popup` uses `synchronous_popups()` instead of
   `GDK_IS_X11_DISPLAY`; keep the `RADIOS_CONTENT_SCALE_POPUP_MAP_ONLY` test seam.
2. **Tooltip cursor size** (content_scale.c:378-382): on Win32 `gdk_display_get_default_cursor_size` returns the system
   cursor metric (32 at 100 % **[probe]**, device pixels) and `gtk-cursor-theme-size` is 0 → divide by
   `gtk_widget_get_scale_factor` for Win32 as for X11 (re-verify at 200 %).
3. **Scale-factor changes at runtime** (new on Windows: per-monitor DPI, moving the window between a 100 % and a 200 %
   monitor sends WM_DPICHANGED, which GDK 3.24.52 handles – string present in libgdk). The offscreen window takes its scale
   from the embedder when `gdk_offscreen_window_set_embedder` is called and its surface is not recreated afterwards.
   Connect `notify::scale-factor` on the widget: if realized, unrealize+realize the widget (or re-run `set_embedder` and
   force an offscreen size change so a new surface is allocated), then `gtk_widget_queue_resize`. X11 never exercised this.
4. **GtkMenu private layout probe** (content_scale.c:277-311): pointers/ints/enums have identical sizes on LP64 and LLP64;
   the runtime check (`menu_layout_known`) stays the safety net.
5. **DPI policy** (lead: per-monitor aware): GDK uses an integer scale = `dpi / 96` (125 %, 150 %, 175 % → 1). All RadioS
   CSS lengths are `px`, so on 125-175 % displays the UI renders crisp but at 100 % physical size. Product decision to
   document: users can use the existing workspace zoom (50-150 %, window.c:798-815); header/footer stay at 1×. Do not
   auto-change `content_scale_percent`. `GDK_SCALE=2` can be offered for ≥175 % monitors.

---

## 8. Preferences (src/ui/preferences.c)

### 8.1 Hardware max sample rate (preferences.c:330-359, used at 552-556)

Replace `/proc/asound` parsing with an audio-backend query (audio area provides):
`guint radios_audio_backend_max_sample_rate(void)` → 0 when unknown. WASAPI design: the audio engine computes it on its own
thread while enumerating active render endpoints (`IMMDeviceEnumerator::EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE)`),
probing `IAudioClient::IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, WAVEFORMATEXTENSIBLE{2ch, 24-in-32 and 16-bit PCM,
rate})` for 768000, 705600, 384000, 352800, 192000, 176400, 96000, 88200, 48000, 44100; devices currently opened exclusively
by the engine (`AUDCLNT_E_DEVICE_IN_USE`) keep their previously cached value. The UI reads the cached maximum (no COM work
on the GTK thread). Label texts stay.

### 8.2 Driver dialog strings

`label_new("PulseAudio / PipeWire")` (preferences.c:517) → `radios_audio_backend_display_name()` ("Windows Ses (WASAPI)").
"Ses sunucusu bağlı/bağlı değil" (482) can stay or become "Ses sistemi …". The device ids `"@default"`/`""` semantics
(preferences.c:441-463,536-537) must be preserved by the audio backend (`@default` = default eConsole render endpoint).

### 8.3 Other

* Undecorated, transient, DIALOG-hint settings window (preferences.c:913-925) works on Win32 (WS_POPUP; no resize border –
  same as Linux without decorations). Escape closes (836-841).
* `gtk_image_new_from_icon_name("emblem-system-symbolic")` (1013) and CSS `-gtk-icontheme('media-record-symbolic')` (115)
  need the Adwaita icon theme, hicolor `index.theme`, gdk-pixbuf SVG loader and `loaders.cache` packaged (packaging area);
  missing icons emit warnings → `G_DEBUG=fatal-warnings` test aborts.
* License page reads `assets/fonts/LICENSE.txt` with `G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS` (648-649) – fine.

---

## 9. Transfer (src/ui/transfer_ui.c)

### 9.1 Default folders

* Export destination default: `config->backup_root` if it is a directory else `g_get_home_dir()` (740-742). On Windows
  `g_get_home_dir()` = `HOME` if it is an absolute existing directory (MSYS2 shells set it!) else `USERPROFILE`
  **[probe: C:\Users\sonatturkan]**. Recommend fallback `g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS)` on Windows.
* Import media root: `g_get_user_special_dir(G_USER_DIRECTORY_MUSIC)` (780) works (SHGetKnownFolderPath)
  **[probe: C:\Users\sonatturkan\Music]**. Risk: Music/Documents are often OneDrive-redirected (Files-On-Demand placeholders,
  upload locks causing sharing violations in the service, sync conflicts). Show the resolved path (already visible in the
  chooser) and warn when it is under `%OneDrive%`.

### 9.2 File-name validation (transfer_ui.c:519-524)

Replace the `'/'`-only check with `radios_platform_file_name_check(name, &error)` (portable; Windows rules always applied so
packages are interchangeable): reject empty, leading `.`, > 200 bytes, control chars, any of `< > : " / \ | ? *`, trailing
space or dot, and reserved device names `CON PRN AUX NUL COM1-9 LPT1-9` (case-insensitive, with or without extension, also
`COM¹²³`). Message: "Dosya adı \\ / : * ? \" < > | içeremez, nokta ile başlayamaz, nokta/boşlukla bitemez ve 200 karakteri
aşamaz." playlist.c:1339-1340 already sanitizes `<>:"/\|?*`; workflow_hub.c:644 only `/ \` – use the same helper
(`radios_platform_sanitize_file_name`) in both.

### 9.3 Paths sent to the service

`destination_dir` (529), `package_path` (602, 628), `media_root` (632) are Windows paths (`C:\…`, `\\server\share\…`). The
Python service validators currently require a leading `/` (e.g. `storage_path_check`, radios_database.py:127) – tools/SDBASE
area must accept Windows absolute paths (drive or UNC) and compare case-insensitively.

### 9.4 File chooser

`gtk_file_chooser_button_new` + `set_local_only(TRUE)` (675-681, 760-763) work on Win32 (UNC paths are local `file:`).
GtkFileChooser **aborts the process** if the GSettings schema `org.gtk.Settings.FileChooser` is missing
(`share\glib-2.0\schemas\gschemas.compiled`) – packaging must ship compiled schemas; also Turkish GTK translations
(`share\locale\tr\LC_MESSAGES\gtk30.mo`). Native IFileDialog (`GtkFileChooserNative`) is optional and would change the
widget names tests use – not recommended for the first port.

---

## 10. Media drag-and-drop and import (src/ui/media.c)

### 10.1 How drops arrive on Win32

GTK 3.24.52 uses **WM_DROPFILES** by default (OLE2 DnD only with `GDK_WIN32_USE_EXPERIMENTAL_OLE2_DND`; do not set it).
`gtk_drag_dest_set` on the toplevel/dialog (media.c:1326-1336) makes GDK call `DragAcceptFiles`; a drop from Explorer is
converted to target `text/uri-list` with `g_filename_to_uri()` URIs joined by CRLF (strings `WM_DROPFILES`, `DragQueryFileW`
in libgdk). Consequences:
* No drag-motion/highlight while hovering (only the drop) – acceptable.
* Shortcuts (`.lnk`) may be resolved to their targets by GDK (verify); virtual items (zip folders, MTP phones) are not
  CF_HDROP → not offered as `text/uri-list` → "okunamadı" message.
* Paths longer than MAX_PATH may be truncated by GDK's fixed buffer (verify; long-path files would then be rejected as
  missing).
* If RadioS runs elevated, UIPI blocks WM_DROPFILES from non-elevated Explorer → manifest `asInvoker`; never require admin.
* In-app DnD with `GTK_TARGET_SAME_APP` targets (programme_queue.c:8-13, playlist.c:2344-2348, routing_graph_ui.c:520-539,
  broadcast.c:3216-3219,7174-7175) uses GDK's local protocol – works.

### 10.2 URI → path (media.c:1278-1296) **[probe]**

| URI | `g_filename_from_uri` result | host | app decision |
|-----|------------------------------|------|--------------|
| `file:///C:/Users/x/M%C3%BCzik/a.mp3` | `C:\Users\x\Müzik\a.mp3` | NULL | accepted |
| `file://localhost/C:/a.wav` | `C:\a.wav` | NULL (dropped by GLib) | accepted |
| `file:////server/share/a.wav` (what GDK produces for UNC) | `\\server\share\a.wav` | NULL | accepted (absolute) |
| `file://remote.invalid/music.wav` | `\music.wav` | `remote.invalid` | rejected by host check ✔ |
| `file:///not-found.wav` | `\not-found.wav` (drive-relative root, `g_path_is_absolute` TRUE) | NULL | rejected as missing ✔ |
| `file:///bad%XX.wav` | NULL + error | – | rejected ✔ |

No code change needed for parsing. Optional hardening: `g_file_test` on UNC paths runs on the GTK thread and an
unreachable server blocks for seconds per path (up to 256 paths) – move the existence/type checks of dropped paths to a
worker (`g_task_run_in_thread`) on Windows, or check `PathIsNetworkPathW` and test asynchronously.
Dedup key `seen` (1275-1293) should be case-folded on Windows (`C:\A.mp3` vs `c:\a.mp3`).

### 10.3 **Bug: infinite loop computing the common import root (media.c:849-857)**

```c
while (g_strcmp0(library->import_root, "/") &&
       (!g_str_has_prefix(path, library->import_root) || path[strlen(library->import_root)] != G_DIR_SEPARATOR)) {
    gchar *parent = g_path_get_dirname(library->import_root); ...
}
```
On Windows `g_path_get_dirname("C:\\") == "C:\\"` **[probe]**, so files on different drives, or any file directly under a
drive root (root ends with `\`, so `path[len]` is never `\`), loop forever on the GTK thread. Replace with a root-aware
helper (portable):
```c
/* TRUE when @root is a path prefix of @path at a component boundary; case-insensitive on Windows */
gboolean radios_platform_path_has_prefix(const gchar *path, const gchar *root);
gboolean radios_platform_path_is_root(const gchar *path);   /* g_path_skip_root(p) && *g_path_skip_root(p) == '\0' */
...
while (!radios_platform_path_has_prefix(path, root)) {
    if (radios_platform_path_is_root(root)) { g_clear_pointer(&root, g_free); break; }   /* no common root */
    gchar *parent = g_path_get_dirname(root); g_free(root); root = parent;
}
```
`has_prefix` must treat a root ending in a separator (`C:\`, `\\server\share\`, `/`) as matching any descendant. When no
common root exists (different drives) pass `NULL` → `media_jobs.c:131` already falls back to the file's own directory.
The same helper fixes audio_ui.c:918-921 (§14.1).

### 10.4 Folder walking (media.c:944-957)

* Skip hidden/system entries on Windows: `name[0] == '.'` **or** `GetFileAttributesW` has `FILE_ATTRIBUTE_HIDDEN` or
  `FILE_ATTRIBUTE_SYSTEM` (`$RECYCLE.BIN` contains deleted audio files; `System Volume Information` is access-denied;
  `desktop.ini`, `Thumbs.db`). Helper `radios_platform_path_is_hidden(const gchar *path, const gchar *name)`.
* `G_FILE_TEST_IS_SYMLINK` (952) on Windows covers symlinks; junctions/mount points must also be treated as links (avoid
  loops such as `Documents\My Music` legacy junctions) – use the same reparse-point test (`FILE_ATTRIBUTE_REPARSE_POINT`).
* OneDrive placeholders (cloud reparse tags) are regular files; reading them triggers downloads – acceptable, document.
* Extension check is already case-insensitive (937-943). File-chooser patterns `*.wav` etc. (1035-1036): GTK 3 pattern
  matching may be case-sensitive; add upper-case variants or bracket patterns (`*.[wW][aA][vV]`) – same for dj.c:1195-1197,
  editors.c:1245, transfer_ui.c:763 (`*.st`), midi_ui.c:418 (`*.json`), workflow_hub.c:386; `audio/*` MIME filters
  (editors.c:1242, dj.c:1194, playlist.c:2061) are unreliable on Windows (registry content types) – keep the patterns.

---

## 11. Playlist / rekordbox USB import (src/ui/playlist.c)

### 11.1 No-follow reads of `export.pdb` / XML (playlist.c:1729-1793)

Linux opens `PIONEER/rekordbox/export.pdb` beneath the chosen root with `openat` + `O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC`,
`O_NONBLOCK` (no FIFO hang), reads ≤16 MiB (USB) / ≤1 MiB (XML), and rejects the file if size/mtime/ctime changed during the
read (`st_mtim`, `st_ctim`). MinGW has no `openat`, no `O_NOFOLLOW/O_DIRECTORY/O_CLOEXEC/O_NONBLOCK`, and `struct stat`
has no `st_mtim/st_ctim` → compile failure. Platform API:
```c
typedef struct { guint64 size; gint64 mtime_ns, ctime_ns; guint64 volume, file_id; } RadioSFileIdentity;
/* Opens root/components[0]/.../components[n-1] read-only; every component must be a real directory (no symlink/junction),
 * the leaf a regular file (no reparse point). Returns a CRT fd (Windows: _open_osfhandle of the HANDLE). */
gint radios_platform_open_beneath(const gchar *root, const gchar *const *components, GError **error);
gint radios_platform_open_regular_nofollow(const gchar *path, GError **error);
gboolean radios_platform_fd_identity(gint fd, RadioSFileIdentity *out, GError **error);
```
Windows: open each directory with `CreateFileW(FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE /*no DELETE share:
pins the path against rename while held*/, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT)`, reject
`FILE_ATTRIBUTE_REPARSE_POINT` (`GetFileInformationByHandleEx(FileAttributeTagInfo)`), keep the handles open while opening
the next component and the leaf (`GENERIC_READ`, `FILE_FLAG_OPEN_REPARSE_POINT`, reject directories/reparse points);
identity from `FileBasicInfo` (`LastWriteTime`, `ChangeTime` ≈ ctime, 100-ns units) + `FileStandardInfo.EndOfFile` +
`FileIdInfo`. Reading via `read()` on the CRT fd (binary mode). The change check compares size + mtime_ns + ctime_ns.

### 11.2 Removable volume list (playlist.c:1962-2010, 2029, 2105-2108)

`GWin32VolumeMonitor` mounts report `g_mount_can_unmount() == FALSE` and no UUID, so the list stays empty and the
`usb-<uuid>` library identity is never produced. Platform API:
```c
typedef struct { gchar *root /* "E:\\" */; gchar *label; gchar *uuid /* "ABCD-1234" or NULL */; } RadioSVolume;
GPtrArray *radios_platform_removable_volumes(void);   /* of RadioSVolume*, free with radios_volume_free */
```
Windows: `GetLogicalDriveStringsW`; keep `DRIVE_REMOVABLE`, plus `DRIVE_FIXED` whose bus type is USB/SD/MMC
(`IOCTL_STORAGE_QUERY_PROPERTY`/`StorageDeviceProperty` → `BusTypeUsb/BusTypeSd/BusTypeMmc`, open `\\.\E:` with 0 access);
`GetVolumeInformationW` for label and serial; uuid = `"%04X-%04X"` of the 32-bit serial – this equals the Linux
`g_mount_get_uuid()` string for FAT32/exFAT sticks (rekordbox media), so `usb-<uuid>` keys stay compatible across OSes
(NTFS differs: blkid shows 64-bit). Skip drives without media (`GetVolumeInformationW` fails, no error dialog: wrap with
`SetThreadErrorMode(SEM_FAILCRITICALERRORS)`). Combo label `"<label> · E:\"`. Change notification: poll
`GetLogicalDrives()` every 1 s while the dialog is open (simplest) or handle `WM_DEVICECHANGE`
(`DBT_DEVICEARRIVAL/DBT_DEVICEREMOVECOMPLETE`, `DBT_DEVTYP_VOLUME`) through `gdk_window_add_filter`; keep the "refresh"
button (2105). Keep GVolumeMonitor on Linux.
Path-derived identity `usb-path-<sha256(path)>` (1842-1843) differs between `E:\` and `/media/...` – inherent, document.
`PIONEER`/`rekordbox` name checks (1829-1836) are case-sensitive; FAT is case-insensitive → compare with
`g_ascii_strcasecmp` on Windows.

### 11.3 Other playlist.c items

* Alt+click mapped to Ctrl (922-928) and modifier filters (522) – fine on Windows (Alt alone may underline mnemonics).
* Exports (1282-1345): `g_file_set_contents` (temp + `MoveFileEx` replace) fails with a sharing violation if Excel holds the
  CSV open – error is shown (ok). CSV already uses BOM+CRLF; M3U8 uses LF and stores `track->location` as-is (Windows paths
  are fine for Windows players).

---

## 12. Admin UI (src/ui/admin_ui.c)

### 12.1 Reliability tool spawn → §4.5.

### 12.2 Restore command display (admin_ui.c:655-663)

The operator is told to close the app/service and run `command` (radios_reliability.py:269-271). `g_shell_quote` yields
`'C:\Program Files\x y\python.exe'` **[probe]**, unusable in cmd/PowerShell. Add
`gchar *radios_platform_quote_command_for_display(const gchar *const *argv)`: POSIX = today's join of `g_shell_quote`;
Windows = `CommandLineToArgvW`-compatible quoting, prefixed with `& ` when argv[0] is quoted (PowerShell call operator).
Message suffix on Windows: "PowerShell'de çalıştırın". Tools area must emit an absolute interpreter (`sys.executable`)
instead of the possibly relative `cfg.python_command`. Test only checks for `--confirm-store-id` (test_admin_ui.c:261).

### 12.3 Token copy to clipboard (admin_ui.c:345-351)

`gtk_clipboard_set_text` works on Win32, but Windows Clipboard History (Win+V) and Cloud Clipboard keep/sync the API token.
Optional hardening: Windows helper that places `CF_UNICODETEXT` plus registered formats
`ExcludeClipboardContentFromMonitorProcessing`, `CanIncludeInClipboardHistory` (DWORD 0) and `CanUploadToCloudClipboard`
(DWORD 0) using `OpenClipboard(gdk_win32_window_get_handle(toplevel))`.

---

## 13. Media archive UI (src/ui/media_archive_ui.c)

* `realpath(chosen, NULL)` (553, 701) + `free()` → `gchar *radios_platform_realpath(const gchar *path, GError **error)`
  (POSIX realpath; Windows `CreateFileW(FILE_READ_ATTRIBUTES, share all, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)` +
  `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED|VOLUME_NAME_DOS)`, strip `\\?\`, map `\\?\UNC\` → `\\`). Mapped drives
  become UNC – show the user the stored path (already displayed).
* Storage restart button (734-744) → `radios_reset_ui_restart` (§5).
* Size thread (610-619) uses core `radios_storage_free_bytes/total_bytes` (statvfs → `GetDiskFreeSpaceExW`, core area) and
  `radios_storage_tree_bytes` (must not follow junctions).
* Folder list equality (589-590) uses exact strings of stored paths – fine.

---

## 14. Other findings from grepping all of src/ui

1. **audio_ui.c:907-921** managed-path containment: `g_canonicalize_filename(managed)` then
   `g_str_has_prefix(path, database_file + ".objects" + G_DIR_SEPARATOR)`. On Windows the service-returned path may differ
   in drive-letter case/short names → playback refused. Use `radios_platform_path_has_prefix` (case-insensitive, separator
   aware) and make the service return canonical (`GetFinalPathNameByHandle`-style) paths.
2. **broadcast_ui.c:246-249** recording filename check rejects `/` `\` and leading `.`; on Windows also reject `:` (NTFS
   alternate data stream `x:y.wav`) and other invalid characters → `radios_platform_file_name_check`; `.wav` suffix compare
   should be case-insensitive. `g_mkdir_with_parents(..., 0700)` mode is ignored on Windows (ACLs are a core concern).
3. **workflow_hub.c:640-645** save-dialog suggested name: use `radios_platform_sanitize_file_name`.
4. **import_profiles_ui.c:164-187** `g_path_is_absolute(target)`: on Windows `\foo` (drive-relative) counts as absolute
   **[probe]**; require a drive letter or UNC root (`g_path_skip_root` + `X:` or `\\` prefix). `source_root` may be a
   macOS/Linux path from rekordbox – mapping is done by the service.
5. **external_access_ui.c:108** `gtk_show_uri_on_window` → GLib Win32 app-info for `https`. Keep it as the first attempt (the
   test wraps it with `-Wl,--wrap=gtk_show_uri_on_window`, CMakeLists.txt:399) and on failure fall back to
   `ShellExecuteExW(SEE_MASK_NOASYNC, L"open", url)`; the URL is already validated as HTTPS
   (`radios_external_https_url_valid`). TLS probe CA store is core area (`CURLSSLOPT_NATIVE_CA`).
6. **Keyboard**: Ctrl+C/V in the calendar (automation_calendar.c:1475-1477) should ignore events that also carry
   `GDK_MOD1_MASK` (AltGr arrives as Ctrl+Alt on Windows); Shift+F10/Menu key context menus (automation_calendar.c:1473,
   rotation_editor.c:1604) match Windows conventions. No `<Super>`/Meta accelerators exist.
7. **Default folders for save dialogs**: `gtk_file_chooser_set_current_folder(project_root)` (emergency.c:2011,
   header.c:220) fine.
8. **Time zones (cross-area, core/time owner)** – `g_time_zone_new_identifier("Europe/Istanbul")` returns NULL on Windows;
   only Windows registry names ("Turkey Standard Time") and POSIX TZ strings work **[probe]**; `g_time_zone_new_local()`
   identifier is "Turkey Standard Time". Affected src/ui call sites (fall back to UTC or reject): header_status.c:108,
   header_navigation.c:108, instruments.c:363, automation_calendar.c:132/2561, broadcast.c:610, probability_editor.c:203,
   schema_track_picker.c:101, local_event_prepare.c:369, player_rotation.c:256, audio_ui.c:154, plus core/playout files.
   Needed: `GTimeZone *radios_time_zone_new(const gchar *iana)` (Windows: IANA→POSIX TZ string from bundled tzdata TZif
   footer, or IANA→Windows name via CLDR `windowsZones` table / ICU `ucal_getWindowsTimeZoneID`), used everywhere instead
   of `g_time_zone_new_identifier`.
9. **Linux-specific Turkish UI strings** to adapt: main.c:160 (Wayland/X11), preferences.c:517 (PulseAudio/PipeWire),
   data.c:385 (Flatpak, compiled out), transfer_ui.c:523 ('/' only), admin_ui.c:662 (command shell), routing.c:1204-1205
   ("Sanal aygıt ses sunucusunda oluşturuluyor…", Pulse virtual sinks – audio area).
10. **Device inventory**: technical.c:758-833 and preferences.c:423-466 only render what `radios_audio_engine_snapshot`
    and the MIDI layer report (`name`, `description`, `monitor` flag, `@default`). Windows backends must fill the same
    snapshot (WASAPI endpoint ID as `name`, `PKEY_Device_FriendlyName` as `description`, loopback-capable render endpoints as
    `monitor` inputs; WinMM MIDI devices). No UI change besides §8.
11. **Clipboard** – only admin_ui.c:348 (§12.3). **GDK X11/Wayland APIs** – only content_scale.c (§7). **getenv** in src/ui –
    only content_scale.c:483 (test seam). **g_spawn/GSubprocess** – data.c, admin_ui.c, reset_ui.c (§4, §5).
    **unistd/fcntl/sys** includes in src/ui – window.c:24 (`close`, available in MinGW), playlist.c:11-15 (§11.1).

---

## 15. Tests that exercise this area and what changes on Windows

| Test | Linux mechanism | Windows change |
|------|-----------------|----------------|
| `native_window` (CMakeLists.txt:543-546) | `radios --smoke-test --project-root src --capture png`, `WORKING_DIRECTORY /tmp` | use `${CMAKE_BINARY_DIR}`; needs an interactive desktop session |
| `native_application_shutdown` (CMakeLists.txt:389-396, tests/test_application_shutdown.py, tests/application_shutdown_driver.c) | links `src/app/main.c` with `-Wl,--wrap=radios_window_present`; sends SIGTERM and SIGINT; `fcntl.flock` probe of `gui.lock`; Pulse virtual devices | `--wrap` works with GNU ld on PE for symbols defined in our static libs; replace signals with (a) named stop event `Local\RadioS.Desktop.Terminate.<pid>` (SIGTERM role), (b) `WM_CLOSE` to the process's main HWND (EnumWindows by PID), (c) `SendMessageTimeout(WM_QUERYENDSESSION)` + `WM_ENDSESSION(TRUE, ENDSESSION_CLOSEAPP)` (session end); all must exit 0, finalize the WAV, release the GUI lock (poll up to 2 s: Windows releases locks of exited processes asynchronously) and keep the service PID; lock probe via the Python `LockFileEx` shim; device id `routing_microphone.monitor` replaced by the WASAPI test device (audio area) |
| `test_data_connection_ui.c` via run_data_connection_ui.py | `python_command="/RadioS-no-lifecycle-helper"`, starts/stops private service | semantics kept by §4.4 (fail closed); add a check that no console window was created (no `ConsoleWindowClass` window owned by the launcher PID) |
| `native_media_drop` (test_media_drop_ui.c) | builds `GtkSelectionData` through `gtk_clipboard_set_with_data`/`wait_for_contents("text/uri-list")` and emits `drag-data-received` with NULL context | verify the Win32 clipboard round-trip of a custom target; otherwise add a test-only seam that constructs the selection data. Add cases: `file:///C:/…`, `file:////server/share/…`, files on two drives and directly under a drive root (regression for §10.3), hidden/system entries and `$RECYCLE.BIN` skipped |
| content-scale tests (test_content_scale_module.c, test_content_scale_popups.c, test_content_scale_ui.c, header content_scale_test_input.h) | Xvfb + XTest (`XTestFakeMotionEvent/ButtonEvent/KeyEvent`), GNOME Shell Eval for Wayland, `gdk_pixbuf_get_from_window` | add a Win32 branch to content_scale_test_input.h: `cs_motion` = `ClientToScreen` of the toplevel HWND on (x·scale, y·scale) + `SetCursorPos`; `cs_button` = `SendInput` (`MOUSEEVENTF_LEFTDOWN/UP`, buttons 4/5 → `MOUSEEVENTF_WHEEL ±WHEEL_DELTA`); `cs_key` = keyval → VK via `gdk_keymap_get_entries_for_keyval` (hardware_keycode is the VK on Win32) + `SendInput`; window origins via `gdk_window_get_origin`. Requires an unlocked interactive session (not session 0/CI service). Run variants with `GDK_SCALE=2` |
| GUI runners (run_rotation_editor_regressions.py etc.) | start private Xvfb, set `GDK_BACKEND=x11` | on Windows run the test directly on the current desktop; never set `GDK_BACKEND=x11` (the Win32 build would fail to open a display); keep `G_DEBUG=fatal-warnings`, `GTK_IM_MODULE=gtk-im-context-simple`; exit 77 when no interactive desktop |
| All `set_tests_properties(... ENVIRONMENT "...GDK_BACKEND=x11...")` (≈45 places in CMakeLists.txt/cmake/*.cmake) | forces X11 | introduce `RADIOS_GUI_TEST_ENV` (Linux: `GDK_BACKEND=x11`; Windows: empty) |
| `native_external_access` (`--wrap=gtk_show_uri_on_window`) | wraps a GTK DLL import | works if `gtk_show_uri_on_window` is referenced without `dllimport` (GTK headers use plain `extern`); keep calling it first (§14.5) |
| new: relaunch | – | `radios_reset_ui_restart` spawns the same exe with the same args + `--project-root`; old instance exits 0; new instance starts only after the old PID is gone (inherited-handle wait); service PID unchanged |
| new: unit tests | – | `radios_platform_path_has_prefix/is_root`, `file_name_check` (CON.st, a:b.st, "x.", "x "), volume uuid formatting, interpreter resolution order (alias stub skipped, explicit missing path fails) |
| new: font | – | after `radios_platform_register_app_font`, `pango_font_map_load_font("Inter Variable Text 14px")` resolves to family "Inter Variable Text" |

---

## 16. Files touched

src/app/main.c, src/ui/window.c, src/ui/content_scale.c, src/ui/data.c, src/ui/reset_ui.c, src/ui/preferences.c,
src/ui/transfer_ui.c, src/ui/media.c, src/ui/playlist.c, src/ui/admin_ui.c, src/ui/media_archive_ui.c,
src/ui/audio_ui.c (§14.1), src/ui/broadcast_ui.c (§14.2), src/ui/workflow_hub.c (§14.3), src/ui/import_profiles_ui.c
(§14.4), src/ui/external_access_ui.c (§14.5), src/ui/automation_calendar.c (§14.6), file-filter patterns in
src/ui/dj.c, src/ui/editors.c, src/ui/midi_ui.c; assets/themes/radios.css (font family);
new: include/radios/platform.h + src/core/platform.c (process, paths, volumes, console, executable path – shared),
src/app/termination.c (or `src/ui/session_win32.c`: GDK session filter, console handler, named event);
CMakeLists.txt + cmake/*.cmake (GUI test env, WORKING_DIRECTORY, `-mwindows`, `gio-windows-2.0`, `-lshell32 -lole32
-luser32`, .rc), tests/content_scale_test_input.h, tests/test_application_shutdown.py, test runners.

## 17. Implementation checklist (ordered)

1. platform.h additions: executable path, argv, console attach/detach/log sink, realpath, path prefix/root helpers,
   file-name check/sanitize, hidden test, open_beneath/identity, removable volumes, process API, quote-for-display,
   termination API, python resolution (or place resolution in data layer), app font registration.
2. main.c: guard `glib-unix.h`/`signal.h`; relaunch wait; console attach; `parse_strv`; executable path; first-run
   `prepare`; Windows error text; AppUserModelID; termination install; teardown factoring; FreeConsole in GUI mode.
3. Session filter + console handler + named event (Windows) with `RADIOS_TERMINATE_NOW` synchronous teardown.
4. data.c / admin_ui.c on `RadioSProcess`; flatpak compiled out; `-X utf8 -B`, env, cwd, Windows launch timeout.
5. reset_ui.c Windows restart.
6. window.c font registration + CSS family; default-size clamp.
7. content_scale.c Win32 popup path, cursor scale, scale-factor change handling.
8. preferences.c backend max rate/display name.
9. media.c import-root fix, hidden/system skip, case-folded dedup, optional worker-thread stat.
10. playlist.c open_beneath + volumes; transfer/workflow/broadcast file-name checks; audio_ui prefix; import_profiles
    absoluteness; external URI fallback; AltGr guard; file-filter patterns.
11. Tests/CMake changes of §15.

## 18. Cross-area dependencies (summary)

* core/recovery: `radios_recovery_gui_lock` returning a CRT fd with shared `LockFileEx`; Windows ownership/ACL checks.
* core/config: `realpath` replacement in `radios_project_root_find`/`resolve_inside_root`.
* core/storage: `statvfs` → `GetDiskFreeSpaceExW`, junction-safe tree size; Windows path syntax in storage.ini.
* core/time: IANA time-zone shim (§14.8).
* core/external_access: libcurl native CA store.
* audio: `radios_audio_backend_max_sample_rate()`, `radios_audio_backend_display_name()`, snapshot fields for WASAPI
  endpoints, test device for the shutdown test.
* tools (Python): `prepare` subcommand; accept Windows absolute paths/spaces in `python_command` (no `shlex.split` on
  Windows); `serve` spawned with `sys.executable -X utf8 -B` and `CREATE_NO_WINDOW|CREATE_NEW_PROCESS_GROUP|
  CREATE_BREAKAWAY_FROM_JOB`; console-ctrl graceful stop of `serve`; restore-plan with absolute interpreter; Windows path
  validation for `destination_dir/package_path/media_root/source_root/storage dirs`; embedded `._pth` must allow
  `tools\` sibling imports (`radios_database_setup`); test shims for named-event stop and LockFileEx lock probes.
* packaging: layout under `runtime\`; compiled GSettings schemas, Adwaita/hicolor icons, gdk-pixbuf SVG loader cache,
  GTK `tr` translations, InterVariable.ttf, gspawn helpers kept, **no gdbus.exe**, exe icon/VERSIONINFO/manifest
  (`longPathAware`, `asInvoker`, PerMonitorV2, UTF-8 ACP), Start-menu shortcut with AppUserModelID, user-writable
  install location.
