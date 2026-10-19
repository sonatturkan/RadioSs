# RadioS Windows port — area `tests-c` (C test sources)

Scope: every C test source in `C:\Users\sonatturkan\Desktop\RadioS\tests`: 147 `*.c`, 1 `*.h`
(`content_scale_test_input.h`), 1 `*.inc` (`routing_patchbay_ui_test.inc`) = **149 files**, plus the CMake
registration (`CMakeLists.txt`, `cmake/*.cmake`) and the C-side contract with the Python runners
(`tests/run_*.py`). Nothing in the project was modified. Facts marked **[probe]** were verified on this
machine (Windows 11 26300, MSYS2 UCRT64: GCC 16.2, GLib 2.90.1, GTK 3.24.52, FFmpeg libavformat 63)
with small programs in `C:\Users\SONATT~1\AppData\Local\Temp\claude\c--Users-sonatturkan-Desktop-radyo-projesi-kaynak-kodlar-RadioS\0ead7b3c-7fb3-425d-a304-a7629cf56b77\scratchpad\tests-c-probe\`
(`probe.c`, `gtkprobe.c`, `keyprobe.c`, `clipprobe.c`, `sizeprobe.c`, `sleepprobe.c`, `desk.c`).

---------------------------------------------------------------------------------------------------

## 1. Summary

| Category | Meaning | Files |
|---|---|---|
| **A** | test source compiles and runs unchanged (after the global build/CTest changes of §4 and the runner/audio prerequisites in its row) | **99** |
| **B** | small portability edits in the test source, mostly mechanical calls into the shared helper layer of §5 | **36** |
| **C** | a part needs a Windows-specific rewrite (native input, sockets, fork/flock, /proc metrics, ALSA/Pulse-only cases, X11 helper header) | **11** |
| **D** | Linux-only as written; exclude on Windows and replace with the Windows test described | **3** |

Main conclusions:

1. **Port 13 shared bases once and 38 more test files follow.** `test_playout_ui.c` is `#include`d by 22 other tests,
   `routing_patchbay_ui_test.inc` by 2, `content_scale_test_input.h` by 2, `test_audio_ui.c`, `test_automation_calendar.c`,
   `test_playout.c` by 2 each, `test_dj_ui.c`, `test_broadcast_ui.c`, `test_programme_queue.c`, `test_playlist_redesign_ui.c`,
   `test_automation_calendar_plan.c`, `test_player_workspace_ui.c` by 1 each (§3.2). Most of their Linux code is the PCM
   capture reader (`open`/`fstat`/`pread`, `struct stat` + `g_stat`) and the fabricated drag-and-drop selection.
2. **A new shared header `tests/radios_test_support.h` (+ `.c`)** absorbs almost every B edit: no-follow tree removal,
   symlink creation with privilege/junction fallback, "is private file" (0600 ⇒ owner-only DACL), file identity
   (st_ino ⇒ FileId), `pread`/`pwrite`, sparse/large files, retrying unlink/replace (sharing violations), path equality,
   long temp dirs, IANA time zones, numeric locale switching, process RSS/handle counts, selection-data fabrication,
   and the X11/XTest replacement (§5, §6).
3. **GUI tests run on a private Win32 desktop instead of Xvfb.** [probe] GTK 3 win32 works on a `CreateDesktop`
   desktop: windows map, have allocations/origins, `gtk_widget_draw` renders, the window is active and focusable, and
   **posted** `WM_MOUSEMOVE/WM_LBUTTONDOWN/UP` and `WM_KEYDOWN/UP` (+`SetKeyboardState` for modifiers) produce real GTK
   clicks, motion and text input. This replaces XTest (§6). `SendInput` is only for the interactive desktop.
4. **The audio-device tests need a virtual WASAPI-backend mode** (cross-area): in-process virtual sinks named by the
   runner, each continuously writing its rendered PCM to a float32 capture file (the parec replacement), `<sink>.monitor`
   loopback sources, process-wide mixing, real-time pacing, and a control API for server loss/hotplug (§7). The C tests
   keep their env contract (`RADIOS_TEST_{MAIN,CUE,MONITOR}_SINK/_CAPTURE`, `RADIOS_TEST_MIC_SOURCE`, …).
5. **Several GLib behaviours differ on Windows and break tests silently** [probe]: `g_file_test(…IS_SYMLINK)` is always
   FALSE and `g_lstat` reports a symlink as a regular file (all 15 private `remove_tree` helpers would *follow
   directory links and delete the target's contents*); `g_stat` gives `st_ino == 0` and permission bits `0666`/`0444`;
   `g_file_set_contents` fails when the destination is open in another process (it unlinks first); `g_unlink` of an open
   file fails with `EACCES`; `g_time_zone_new_identifier("Europe/Istanbul")` returns NULL; `--wrap=g_*` is ineffective
   (GLib/json-glib are `dllimport`); custom GDK selection atoms emit a `Gdk-WARNING` (fatal under
   `G_DEBUG=fatal-warnings`) and `CLIPBOARD` writes the real system clipboard; `g_usleep(1000)` sleeps ~15.5 ms unless
   `timeBeginPeriod(1)`; windows larger than the monitor are clamped.
6. **D items**: `test_pulse_hotplug.c` (fake libpulse), `test_midi_hotplug.c` (fake ALSA sequencer + `src/core/midi.c`),
   `test_audio_recovery.c` (private PulseAudio daemon + pactl/parec). Each gets a Windows twin with the same assertions
   against a fake WASAPI enumerator / fake WinMM / the virtual backend's control API (§7.3, §8).

---------------------------------------------------------------------------------------------------

## 2. Windows facts the tests depend on (evidence)

| # | Fact on Windows/MSYS2 UCRT64 | Evidence | Consequence for tests |
|---|---|---|---|
| F1 | `GetACP()==1254` (Turkish ANSI) without manifest; `_fmode==0` (text default) | [probe] `probe.c` | every test exe needs the activeCodePage=UTF-8 manifest (argv roots such as `RadioS Türkçe ses kabul` arrive as UTF-8) and binary default mode; raw `open()` must add `O_BINARY` |
| F2 | `g_get_tmp_dir()` = `C:\Users\SONATT~1\AppData\Local\Temp` (8.3 short name) | [probe] | temp fixtures must be resolved to the long final path before comparing with production output |
| F3 | `g_canonicalize_filename("C:/a/b/../c")` = `C:\a\c` (forward slashes become `\`); `g_path_is_absolute("/tmp/x")` = TRUE, `("C:x")` = FALSE | [probe] | mixed-separator strings built from `RADIOS_TEST_PROJECT_ROOT` (CMake gives `C:/…`) or `"%s/%s"` will not string-compare with canonicalized production paths |
| F4 | `g_stat`: regular file `st_mode=100666`, after `g_chmod(0600)` still `100666`; `g_chmod(0444)` gives `100444`; `st_ino` is a 16-bit field and always 0 | [probe] | all `st_mode & 0777 == 0600` and `st_ino !=` assertions are meaningless; need ACL/FileId helpers |
| F5 | `GStatBuf` is `struct _stat64`; `struct stat` is a different struct (`stat64i32`) | `C:\msys64\ucrt64\include\glib-2.0\glib\gstdio.h:50-52`, `sys/stat.h:95-126` | `struct stat x; g_stat(p,&x)` is an incompatible-pointer **compile error** (GCC ≥14 errors by default) |
| F6 | `CreateSymbolicLinkW(…, ALLOW_UNPRIVILEGED_CREATE)` works here (Developer Mode) for files and directories | [probe] | may fail on CI (no privilege) ⇒ fallback/skip needed |
| F7 | `g_file_test(link, G_FILE_TEST_IS_SYMLINK)` = **FALSE** for real file and directory symlinks; `IS_DIR` follows; `g_lstat` returns `S_IFREG` for a file symlink; `GFileInfo` with `G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS` reports `is_symlink=1`; `g_rmdir(dirlink)` removes only the link | [probe] | every `remove_tree` in the tests recurses through directory links (danger: deletes the link target's contents); `g_assert_true(g_file_test(link, IS_SYMLINK))` fails |
| F8 | `g_unlink` of a file opened with `g_open` (no `FILE_SHARE_DELETE`) fails `errno=13`; succeeds after close | [probe] | unlink-must-succeed assertions after async closes, and gate files read concurrently by the Python service, need retry |
| F9 | `g_file_set_contents` over a file opened by another handle fails: "Existing file … could not be removed: g_unlink() failed: Permission denied" (GLib unlinks then renames on Windows — not atomic) | [probe] | gate/hold files rewritten while Python reads them flake; production durable writes must not use it (cross-area) |
| F10 | `g_time_zone_new_identifier`: `Europe/Istanbul`, `Europe/Berlin`, `Asia/Istanbul` → NULL; `Turkey Standard Time`, `UTC`, `+03:00` → OK; local tz id = `Turkey Standard Time` | [probe] | tests asserting IANA zones (3 direct calls) and every production validation of station/loudness zones fail ⇒ platform IANA tz function required |
| F11 | `setlocale(LC_ALL,"")` = `Turkish_Türkiye.1254`, decimal `,`; after `gtk_init` LC_NUMERIC is `tr_TR`, decimal `,`; `setlocale(LC_NUMERIC,"tr_TR.UTF-8")` accepted by UCRT; no `newlocale/uselocale` | [probe] | `test_tempo.c` locale case needs `_configthreadlocale`; GUI tests run with a comma locale on Turkish machines |
| F12 | `-Wl,--wrap=g_get_real_time` is **not** applied (calls go through `__imp_g_get_real_time`: GLib 2.90 headers are `__declspec(dllimport)`, also json-glib, cairo); `-Wl,--wrap=gtk_drag_finish` **is** applied (GTK3/GDK headers use plain `extern`) | [probe] `probe.c`, `gtkprobe.c`; `glib-visibility.h:3-22`, `gdkversionmacros.h:31-32`, `json-version-macros.h:32-47` | `test_header_ui.c` clock seam must be rebuilt; all `radios_*`/`gtk_*` wraps keep working |
| F13 | PE `ld`: `--gc-sections` does **not** suppress undefined references from discarded sections | [probe] (`gc_a.c`) | `test_rotation_editor_regressions` will not link without extra stubs |
| F14 | `gdk/gdkx.h`, X11/XTest, `alsa/`, `pulse/` headers and pkg-config modules `xtst x11 libpulse alsa` do not exist; GDK backends: WIN32, BROADWAY | MSYS2 tree, `gdkconfig.h:13-17` | X11/ALSA/Pulse code must be `#ifdef`-ed out or rewritten; `pkg_check_modules(… REQUIRED)` for them fails configure |
| F15 | `GDK_BACKEND=x11` ⇒ `gtk_init_check()==FALSE` | [probe] | CTest `ENVIRONMENT … GDK_BACKEND=x11` (64 `set_tests_properties` entries, ≈100 expanded tests) makes them skip (77) or exit(1) on Windows |
| F16 | GTK works on a private `WinSta0\RadioSTest-<pid>` desktop; window active and focusable; posted mouse/keyboard messages produce `clicked`, motion and text (`'q'`, Ctrl+A then `z` ⇒ `"z"`); `SendInput` is impossible there | [probe] `gtkprobe.c`, `keyprobe.c`, `desk.c` | Xvfb + XTest replacement (§6) |
| F17 | `gtk_clipboard_get(GDK_SELECTION_PRIMARY)` round-trip works in-process, no warning, system clipboard untouched; a custom atom works but prints `Gdk-WARNING: Unsupported generic selection …`; `CLIPBOARD` works but changes the system clipboard | [probe] `clipprobe.c` | fabricated `GtkSelectionData` must use PRIMARY on Windows |
| F18 | `g_usleep(1000)` ≈ 15.5 ms, `g_usleep(100)` ≈ 15.6 ms; with `timeBeginPeriod(1)` ≈ 1.9 ms | [probe] `sleepprobe.c` | timing-sensitive tests (flush bursts 3.3 ms apart, 5 ms heartbeats, <20 ms snapshot bounds) need `timeBeginPeriod(1)` |
| F19 | Window requests above the monitor are clamped: 2000×1200 and 2560×1600 → 1904×1161 on a 1920×1200 monitor | [probe] `sizeprobe.c` | exact-size layout tests need a large enough desktop, else skip |
| F20 | `ffmpeg.exe`, `gspawn-win64-helper(-console).exe`, `broadwayd.exe`, `python.exe` exist in `C:\msys64\ucrt64\bin` | file system | `g_spawn` of ffmpeg/python works when `ucrt64\bin` is on PATH |

---------------------------------------------------------------------------------------------------

## 3. How the C tests are structured (contracts to preserve)

### 3.1 Kinds of C tests

1. **Pure/offline unit tests** (`g_test_init`, no display, no devices): planners, models, DSP, offline audio engine
   (`radios_audio_engine_new(FALSE, NULL)` + `radios_audio_engine_render`), JSON/INI stores, HTTP client against an
   in-process `GSocketListener` mock. Run directly by CTest.
2. **Realtime-audio engine tests** (`radios_audio_engine_new(TRUE, server)`): `test_audio`, `test_runner`,
   `test_runner_pool_preload`, `test_channel_input`, `test_header_navigation_backend`, `test_preferences_output`,
   `test_patchbay`, `test_audio_soak`, `test_audio_recovery`. Cases skip (`g_test_skip`) unless the runner exported the
   owned sinks; some CTest entries run them through `run_audio_ui.py --engine-only`.
3. **Direct GUI tests** (`gtk_init_check` → `return 77`, or `gtk_test_init`): widget/component tests with stubs or
   `--wrap` seams, no service. Run directly, or through `run_rotation_editor_regressions.py` (private Xvfb).
4. **GUI acceptance against a private SDBASE store** (+ optionally owned audio outputs): `main()` takes the private
   project root as `argv[1]`, shifts argv, `g_test_init`, `gtk_init`, `radios_config_load(root)`,
   `radios_data_client_new`; runner = `run_data_ui.py`, `run_admin_ui.py`, `run_audio_ui.py`, `run_playout_ui.py`, … .
   The runner copies `database/SDBASE/src` into `build/<tmp>/RadioS Türkçe …`, patches `http_server.py` with test gates,
   starts the service with `tools/radios_database.py start`, and runs the executable with `cwd=root`.
5. **Drivers**: `header_layout_driver.c` (CLI: width height text-size png [root [view|header]], no CTest entry),
   `application_shutdown_driver.c` (linked into `src/app/main.c`, used by `test_application_shutdown.py`),
   `test_rhythm_upkeep.c` (CLI: `analyze FILE | scan ROOT DB URL [WORKERS]`, used by `run_rhythm_upkeep.py`),
   `test_audio_soak.c` (CLI: `DIR SECONDS`, used by `run_audio_soak.py`).

### 3.2 Shared bases (port once)

| Base (port once) | Included by (`#include "…c"` / sources) | Linux constructs inside the base |
|---|---|---|
| `test_playout_ui.c` (4702 lines) | `test_autodj_settings_ui`, `test_autoplay_events_ui`, `test_autoplay_header_ui`, `test_autoplay_pending_ui`, `test_cluster_pool_cards_ui`, `test_copyright_playback_ui`, `test_event_assignment_ui`, `test_event_block_queue_ui`, `test_event_playout_ui`, `test_header_next_playlist_ui`, `test_header_playout_ui`, `test_live_row_ui`, `test_live_step_wait_ui`, `test_local_event_prepare_ui`, `test_local_event_ui`, `test_loudness_origin_playout_ui`, `test_player_content_status_ui`, `test_playout_jingle_ui`, `test_playout_vocal_jingle_ui`, `test_schema_track_picker_ui`, `test_stream_metadata`, `test_window_now_playing` (22) | capture readers `:330-378`, `:637-651` (`struct stat`+`g_stat`, `open(O_RDONLY)`, `fstat`, `pread`, `close`); custom selection atom `:835`; `#define _POSIX_C_SOURCE` `:1`, `<fcntl.h> <sys/stat.h> <unistd.h>` `:21-26` |
| `routing_patchbay_ui_test.inc` | `test_routing_graph_ui.c` (`RADIOS_ROUTING_NATIVE_INPUT 1`), `test_routing_ui.c` (`0`) | X11/XTest `:13-16,98-120,543-549,564-566`; 0600 WAV check `:175` (`struct stat`); Pulse device ids `"routing_original.monitor"` `:136`, `"routing_microphone.monitor"` `:146` |
| `content_scale_test_input.h` | `test_content_scale_popups.c`, `test_content_scale_ui.c` | whole file: X11 XTest backend `:12-19,59-81,91,106,130` and a Wayland backend that evals JS in a private GNOME Shell over the session bus `:20-56,83-154` |
| `autoplay_execution_clock.c` | compiled with `test_autoplay_pending_ui.c` (CMake 858) | none; macro-renames `g_get_monotonic_time` and `#include "../src/ui/player_scope_execution.c"` — this is the pattern to copy for `test_header_ui` (F12) |
| `test_audio_ui.c` | `test_audio_loudness_policy_ui.c`, `test_mixer_auto_volume_ui.c` | capture readers `:227-259`; symlink `:586` |
| `test_automation_calendar.c` | `test_automation_probability_ui.c`, `test_player_schema_weekly_ui.c` | `g_time_zone_new_identifier` `:1437`; `FcInit/FcConfigAppFontAddFile` `:3046` |
| `test_playout.c` | `test_automation_probability_playout.c`, `test_playout_event_blocks.c` | managed object path `"%s.objects/%s.wav"` `:17` from a non-canonical `database` (`:1810`) |
| `test_dj_ui.c` | `test_effect_return_ui.c` | capture readers `:218-288` |
| `test_broadcast_ui.c` | `test_broadcast_metadata_ui.c` | none |
| `test_programme_queue.c` | `test_programme_jingle_hint.c` | custom selection atom `:185` |
| `test_playlist_redesign_ui.c` | `test_playlist_export_ui.c` | custom selection atom `:239` |
| `test_automation_calendar_plan.c` | `test_automation_pdf_plan.c` | `database = "/tmp/radios-calendar-plan.sdb"` `:8`, object paths `:35` |
| `test_player_workspace_ui.c` | `test_automation_workspace_acceptance.c` | none |

Mechanism used by includers (must keep working with MinGW): `#define main radios_X_fixture_main`,
`#define g_test_run X_run`, `#include "test_playout_ui.c"`, `#undef …`, then `int main(){ return radios_X_fixture_main(); }`.
Also direct inclusion of production sources: `test_playlist_browser_ui.c:1` (`../src/ui/playlist_browser.c`),
`test_midi_hotplug.c:287` (`../src/core/midi.c`), `test_rhythm_upkeep.c:10` (`ui/rhythm_jobs.c` via `-I src`).
Static-archive members with the same symbols are not pulled — works identically on PE.

### 3.3 Runner ⇄ C-test contract (must be reproduced by the Windows runners)

| Item | Linux producer | Consumers (C) | Windows equivalent |
|---|---|---|---|
| `argv[1]` = private project root (UTF-8, spaces, Turkish letters), `cwd` = root | all `run_*.py` | ~70 GUI mains | same; needs UTF-8 argv (manifest, F1) |
| `RADIOS_TEST_{MAIN,CUE,MONITOR}_SINK` = `radios_gui_<uuid>_{main,cue,monitor}` (tests assert prefix `radios_gui_`) | `run_audio_ui.py:129-137` (pactl null sinks) | `test_audio.c:207-215…`, every playout/audio GUI main, `test_general_queue_ui.c:56` | virtual sinks created by the backend from env (§7) |
| `RADIOS_TEST_{BUS}_CAPTURE` = float32le stereo 48 kHz raw file growing in real time (parec of `<sink>.monitor`) | `run_audio_ui.py:138-145` | capture readers in `test_playout_ui.c`, `test_audio_ui.c`, `test_dj_ui.c`, `test_advert_ui.c`, `test_runner.c:449`, `test_audio.c:646-658`, `test_player_mapping_ui.c:188-202`, `test_programme_ui.c:232` | virtual sink writes `<dir>/<sink>.f32`; same format; must keep writing silence when idle |
| `RADIOS_TEST_MIC_SOURCE` = `<monitor sink>.monitor` | `run_audio_ui.py:154` | `test_audio.c`, `test_channel_input.c:325`, `test_dj_ui.c:1300` | virtual loopback source |
| audio preferences document with these sinks saved before the window opens | `run_audio_ui.py:148-158` (HTTP put) | `window_ready()` checks combo ids == sink names (`test_playout_ui.c:102-111`) | unchanged (device ids == names) |
| `RADIOS_TEST_OUTPUT_SINK/_CAPTURE/_PROOF` (44.1 kHz sink) | `run_preferences_output.py:51-66` | `test_preferences_output.c:65-139` | virtual sink at the requested rate |
| `RADIOS_TEST_PULSE_SERVER` (private daemon), marker files `stop-N/stopped-N/restart-N/restarted-N` | `run_audio_recovery.py`, `run_audio_soak.py` | `test_audio_recovery.c:16-26,58`, `test_audio_soak.c:63,87`, `test_patchbay.c:254` | virtual-backend control API (§7.3) |
| `RADIOS_TEST_STREAM_URL`, `RADIOS_TEST_STREAM_PASSWORD`, `RADIOS_TEST_METADATA_CAPTURE`, `RADIOS_TEST_STREAM_CAPTURE` | `run_audio_soak.py:196`, `run_broadcast_ui.py` | `test_audio_soak.c:99`, `test_broadcast_ui.c:299-339`, `test_broadcast_metadata_ui.c:32-49` | unchanged (Python HTTP receiver, stdlib) |
| `RADIOS_TEST_ACCESS_URL`, `RADIOS_TEST_ACCESS_CA` | `run_external_access.py:55-56` | `test_external_access.c:51` | unchanged (curl must accept a PEM CAINFO) |
| `RADIOS_TEST_VIEW_TOKEN`, `RADIOS_ROUTING_VIEWER_TOKEN`, `RADIOS_TEST_CONNECTION_*` | `run_admin_ui.py:103`, `run_routing_ui.py:123`, `run_data_connection_ui.py:38,131` | `test_admin_ui.c:221`, `.inc:220,258`, `test_data_connection_ui.c:125` | unchanged |
| gate files in the store dir: `.acceptance-list-hold/-ready`, `.acceptance-document-read-hold/-ready`, `.acceptance-playout-prepare-*`, `.acceptance-media-get-hold`, `.acceptance-import-drop`, `.acceptance-playlist-drop`, `.routing-context-hold/-ready/-only-user`, `.catalogue-hold/-ready`, `.acceptance-tool-hold/-ready` | patched `http_server.py` (Python) | `test_data_ui.c`, `test_playout_ui.c`, `test_broadcast_catalogue_ui.c:160-167`, `.inc:230-265,357-365`, `test_admin_ui.c:233-275`, … (105 references in 21 files) | unchanged names; C side must write with retrying atomic replace and delete with retrying unlink (F8/F9); Python side must poll with `stat` only and release handles promptly |
| `RADIOS_SHUTDOWN_READY` (JSON marker path), `RADIOS_SHUTDOWN_RECORD` (WAV leaf) | `test_application_shutdown.py:96-97` | `application_shutdown_driver.c:34,66` | unchanged; the Python side replaces SIGTERM/SIGINT by a Windows graceful-stop request (app area) |
| capture/screenshot dirs: `RADIOS_SHOT_DIR`, `RADIOS_CONTENT_SCALE_CAPTURE_DIR`, `RADIOS_*_CAPTURE` (≈25 names) | optional | many | unchanged (optional) |
| `GSETTINGS_BACKEND=memory` set by tests | `test_playlist_redesign_ui.c:355`, `test_playlist_export_ui.c:68`, `test_workflow_hub_ui.c:211` | — | keep (avoids registry backend `HKCU\Software\GSettings`) |
| exit code **77** = skip (CTest `SKIP_RETURN_CODE 77`) | many mains and runners | — | keep |

### 3.4 Link-time seams (`-Wl,--wrap=`) — all `radios_*`/`gtk_*` seams remain valid on PE

`test_player_tree_ui` (4 symbols), `test_automation_workspace_ui` (17), `test_automation_scope_ui` (6),
`test_programme_queue` / `test_broadcast_catalogue_ui` / `test_programme_jingle_hint` (`gtk_drag_finish`),
`test_application_shutdown` (`radios_window_present`), `test_external_access_ui` (`gtk_show_uri_on_window`,
`radios_external_probe_sync`), `test_playlist_redesign_ui` / `test_playlist_export_ui` (`radios_media_import_open`),
`test_player_waveform_markers` (12), `test_broadcast_header` (7), `test_live_players_ui` (10), `test_player_loader_ui` (2),
`test_programme_view_settings_ui` (2), `test_programme_view_header_ui` (1), `header_layout_driver` (1).
**Only `test_header_ui` (`--wrap=g_get_real_time`) breaks** (F12) — see its note in §10.
Rule for implementers: never add `--wrap` for a GLib/GIO/json-glib/cairo/pango/WinAPI symbol; use a wrapper TU with a
macro rename (pattern of `autoplay_execution_clock.c`) or an internal function-pointer seam.

---------------------------------------------------------------------------------------------------

## 4. Global build / CTest changes the C tests need on Windows (owner: build area, listed here for completeness)

1. **Manifest + runtime init for every test executable**: add a CMake function `radios_test_executable(name sources…)`
   (or a loop over all test targets) that (a) links the same application manifest as the app
   (activeCodePage=UTF-8, per-monitor DPI awareness), (b) links `tests/support/radios_test_runtime_win32.c`, whose
   `__attribute__((constructor))` calls `timeBeginPeriod(1)` (F18), `SetErrorMode(SEM_FAILCRITICALERRORS |
   SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)`, `WerSetFlags(WER_FAULT_REPORTING_NO_UI)` and
   `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)` (a fatal `g_warning` executes `int3`/`abort()` —
   without this a Windows Error Reporting dialog can block CI until the timeout), and sets `_fmode = _O_BINARY`
   (or link MinGW's `binmode.o`);
   (c) links `ws2_32 winmm psapi` where needed. This makes argv UTF-8 without touching 149 mains.
2. **Remove X11/Pulse/ALSA requirements on WIN32**: `pkg_check_modules(PULSE|ALSA REQUIRED)` (CMake 18-19),
   `pkg_check_modules(CONTENT_XTEST|XTST REQUIRED IMPORTED_TARGET xtst x11)` (CMake 118, 405; `Preferences.cmake`) must be
   `if(NOT WIN32)`; `test_pulse_hotplug` (246-250), `test_midi_hotplug` (198-207), `audio_recovery` (238-243) are not built
   on WIN32 and are replaced by their Windows twins (§7.3, §8).
3. **Environment**: on WIN32 do **not** put `GDK_BACKEND=x11` in any `ENVIRONMENT` (64 entries; F15); use
   `GDK_SCALE=1` and (optional) `GDK_BACKEND=win32`. Keep `G_DEBUG=fatal-warnings`, `NO_AT_BRIDGE`, `GTK_IM_MODULE`
   (harmless). Add `PATH=path_list_prepend:<ucrt64/bin>` via `ENVIRONMENT_MODIFICATION` (CMake ≥3.22) when not run from an
   MSYS2 shell, plus `RADIOS_AUDIO_BACKEND=none` for every GUI test that does not use owned sinks (Linux relies on the
   developer's PulseAudio or `PULSE_SERVER=unix:/…/unavailable-audio`; Windows must never open the real speakers in tests).
4. **`WORKING_DIRECTORY /tmp`** (`config_from_other_directory` 262-264, `native_controls` 441-443, `native_window` 546)
   does not exist on Windows; use a configure-time dir, e.g. `${CMAKE_BINARY_DIR}/cwd-elsewhere`.
5. **`RADIOS_TEST_PROJECT_ROOT`**: keep `"${CMAKE_SOURCE_DIR}"` (forward slashes, no escaping issues) but tests must
   canonicalize it (`rt_project_root()`, §5). Several tests already do (`test_audio.c:28`, `test_media.c:52`, …).
6. **`test_rotation_editor_regressions`** (CMake 610-615): `-ffunction-sections -Wl,--gc-sections` does not drop
   unresolved references on PE (F13). On WIN32 add a stub TU (`tests/rotation_editor_win32_stubs.c`) defining every
   symbol reported undefined at the first link (they are only reached from code the test never calls), or link the real
   `radios_ui` objects that provide them. Same check for the other "subset" targets: `test_schema_settings_ui` (604),
   `test_view_presets_ui` (135), `test_loudness_settings`, `test_settings_sync`, `test_header_ui`, `test_neon_progress`,
   `test_header_progress`, `test_content_scale_*`, `test_rhythm_upkeep`, `test_pulse_hotplug`.
7. **`-Wl,--as-needed`** (`test_midi_hotplug`, 200) is accepted by PE ld but meaningless; drop on WIN32.
8. **Serialisation**: tests that touch process-global Windows state must not run in parallel: `RESOURCE_LOCK
   system_clipboard` for `native_admin` (production copies the API token to CLIPBOARD, `test_admin_ui.c:336`), and
   `RESOURCE_LOCK native_input` for every test using `SendInput` (§6). Tests using posted input on private desktops can run
   in parallel.
9. **Runners**: every `run_*.py` runner that starts Xvfb/dbus/pactl/parec needs a Windows path (tests-py area); the C
   side needs only: private desktop for GUI (§6.3), virtual audio env (§7), same argv/cwd/env otherwise. Two runner bugs
   to avoid on Windows: `run_audio_ui.py:221` compares `Path(argv[1]).name != "test_audio_ui"` — the Windows name is
   `test_audio_ui.exe` (use `.stem`); TAP parsing is unaffected.
10. **Timeouts**: Windows process start, Defender scanning and 15.6 ms timers make the suite slower; keep timeouts but
    exclude the build dir from Defender real-time scanning on CI (it also opens files and causes sharing violations).

---------------------------------------------------------------------------------------------------

## 5. Shared test support layer (`tests/radios_test_support.h`, `tests/radios_test_support.c`)

One header (static inline where trivial) + one `.c` linked into every test target. POSIX branches keep today's
behaviour exactly; Windows branches call `include/radios/platform.h` where a production function exists. Every B edit in
§10 is a mechanical replacement by one of these.

```c
/* ---- process / init ---------------------------------------------------- */
const gchar *rt_project_root(void);          /* g_canonicalize_filename(RADIOS_TEST_PROJECT_ROOT) cached */
gchar *rt_build_dir(const gchar *leaf);      /* <root>/build/<leaf>, created 0700 */

/* ---- temp dirs & paths ------------------------------------------------- */
gchar *rt_make_tmp_dir(const gchar *tmpl, GError **error);
  /* g_dir_make_tmp + rt_real_path: on Windows returns the long, final path (no 8.3 names, F2). */
gchar *rt_real_path(const gchar *path);      /* POSIX realpath(); Win: radios_platform_real_path() */
gboolean rt_path_equal(const gchar *a, const gchar *b);
  /* canonicalize both (g_canonicalize_filename), on Windows also case-insensitive compare. Use instead of
     g_assert_cmpstr on paths produced by production code. */
#define rt_assert_path_equal(a, b)  /* g_assert_true(rt_path_equal(a,b)) with a message showing both */

/* ---- links, special files ---------------------------------------------- */
gboolean rt_symlink(const gchar *target, const gchar *link, gboolean is_directory, GError **error);
  /* POSIX symlink(). Win: CreateSymbolicLinkW(link, target, (dir?SYMBOLIC_LINK_FLAG_DIRECTORY:0) |
     SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE); keep relative targets relative (test_ui_preferences:511,
     test_config:338 rely on it). On ERROR_PRIVILEGE_NOT_HELD: directories fall back to a junction
     (FSCTL_SET_REPARSE_POINT, absolute target, rt_symlink returns TRUE and sets a flag readable by
     rt_last_link_is_junction()); files return FALSE with G_IO_ERROR_NOT_SUPPORTED. */
#define RT_SYMLINK_OR_SKIP(target, link, is_dir) \
  /* if (!rt_symlink(...)) { g_test_skip("symbolic links unavailable"); return; } */
gboolean rt_is_link(const gchar *path);
  /* never follows: POSIX lstat S_ISLNK; Win: GetFileAttributesW & FILE_ATTRIBUTE_REPARSE_POINT (symlink or
     junction). Replaces g_file_test(path, G_FILE_TEST_IS_SYMLINK) which is always FALSE on Windows (F7). */
void rt_remove_tree(const gchar *path);
  /* depth-first delete that NEVER descends into a link: links/junctions are removed with RemoveDirectoryW (directory
     links) or DeleteFileW (file links); read-only attribute cleared before delete; each delete retried like
     rt_unlink(). Replaces the 15 private remove_tree() helpers (test_config:29, test_recovery:19,
     test_data_client:50, test_audio_decoder:15, test_media:16, test_ui_preferences:16, test_preferences_storage:13,
     test_local_ini:16, test_library_analysis:15, test_library_analysis_ui:17, test_media_archive:11,
     test_storage_locations:11, test_content_scale_ui:52 (remove_private_tree), test_rotation_editor_regressions:116,
     test_player_waveform_markers:97). */
gboolean rt_mkfifo(const gchar *path);       /* POSIX mkfifo(0600); Win: always FALSE (no FIFOs on NTFS) */
#define RT_HAVE_FIFO  /* 1 on POSIX, 0 on Windows: guard the FIFO sub-cases */

/* ---- file content & metadata ------------------------------------------- */
int rt_open(const gchar *path, int flags, int mode);  /* g_open + O_BINARY on Windows */
gssize rt_pread(int fd, void *buf, gsize count, gint64 offset);   /* POSIX pread; Win: ReadFile with OVERLAPPED offset on _get_osfhandle(fd) */
gssize rt_pwrite(int fd, const void *buf, gsize count, gint64 offset);
gboolean rt_create_sized_file(const gchar *path, gint64 size, GError **error);
  /* replacement for open(O_CREAT|O_EXCL)+ftruncate(): POSIX ftruncate; Win: CreateFileW(CREATE_NEW) +
     FSCTL_SET_SPARSE + SetFilePointerEx + SetEndOfFile (fast, no 2 GiB of zeros; do NOT use _chsize_s, which
     writes zeros, nor ftruncate(off_t) whose off_t is 32-bit — test_audio_decoder:314, test_media:216). */
gint64 rt_file_size(const gchar *path);      /* GStatBuf-based; replaces struct stat + g_stat */
gint64 rt_fd_size(int fd);                   /* POSIX fstat; Win: _fstat64 / GetFileSizeEx(_get_osfhandle(fd)) */
void rt_assert_private_file(const gchar *path);
  /* POSIX: (st_mode & 0777) == 0600 and regular. Win: radios_platform_path_is_private(path) — owner is the current
     user SID, DACL protected (no inherited ACEs) and grants access only to the current user (+ optionally SYSTEM and
     Administrators), no Everyone/Users/Authenticated Users ACEs. */
void rt_make_world_readable(const gchar *path);
  /* POSIX chmod 0644; Win: add an ACE granting Everyone (WinWorldSid) FILE_GENERIC_READ (SetNamedSecurityInfoW).
     Used where the tests prove that production refuses non-private credential files (test_data_client:346,
     test_ui_preferences:109). */
void rt_make_private(const gchar *path);     /* POSIX chmod 0600; Win: radios_platform_make_private() */
gboolean rt_same_file_identity(const gchar *a_path_or_null, ...);
typedef struct { guint64 volume; guint8 id[16]; } RtFileId;
RtFileId rt_file_id(const gchar *path);
  /* POSIX (st_dev, st_ino); Win: GetFileInformationByHandleEx(FileIdInfo) (fallback nFileIndexHigh/Low).
     Replaces st_ino comparisons that prove "replaced atomically, not rewritten in place"
     (test_ui_preferences:116, test_preferences_storage:84). */

/* ---- sharing-violation tolerant mutations (F8/F9) ------------------------ */
gboolean rt_set_contents(const gchar *path, const gchar *data, gssize length, GError **error);
  /* radios_platform_replace_file(): temp file in the same dir, FlushFileBuffers, MoveFileExW(REPLACE_EXISTING |
     WRITE_THROUGH), retry ERROR_SHARING_VIOLATION/ERROR_ACCESS_DENIED/ERROR_LOCK_VIOLATION for up to 2 s.
     Use for every gate/hold file and every file another process may read. */
int rt_unlink(const gchar *path);            /* g_unlink retried up to 2 s on EACCES/EBUSY; returns 0 or -1 */
#define rt_assert_unlinked(path)  /* g_assert_cmpint(rt_unlink(path), ==, 0) */

/* ---- time zones & locale (F10/F11) -------------------------------------- */
GTimeZone *rt_time_zone(const gchar *iana);  /* radios_platform_time_zone_new(); never g_time_zone_new_identifier */
typedef struct _RtNumericLocale RtNumericLocale;
RtNumericLocale *rt_numeric_locale_push(const gchar *posix_name);
  /* POSIX newlocale(LC_NUMERIC_MASK)+uselocale; Win: _configthreadlocale(_ENABLE_PER_THREAD_LOCALE) +
     setlocale(LC_NUMERIC, name) where "tr_TR.UTF-8" → "tr-TR" (UCRT also accepts "tr_TR.UTF-8", F11).
     Returns NULL if unavailable (caller must fail, not skip — test_tempo.c:78-79 requires a real comma locale). */
void rt_numeric_locale_pop(RtNumericLocale *previous);

/* ---- process metrics (test_audio_soak) ---------------------------------- */
guint64 rt_process_resident_bytes(void);     /* /proc/self/statm × page | GetProcessMemoryInfo().WorkingSetSize */
guint rt_process_handle_count(void);         /* /proc/self/fd entries | GetProcessHandleCount() */

/* ---- GTK helpers ---------------------------------------------------------- */
GtkSelectionData *rt_selection_data_new(const gchar *target, const guchar *data, gint length);
  /* the "set_with_data + wait_for_contents" trick used to fabricate drag data, always on
     GDK_SELECTION_PRIMARY on Windows (in-process, no warning, system clipboard untouched — F17); keeps the current
     selection on Linux. Replaces test_media_drop_ui:176-191, test_prompt12_drop_ui:~110-123,
     test_broadcast_catalogue_ui:235,547, test_playout_ui:835, test_programme_queue:185,
     test_playlist_redesign_ui:239. */
gint rt_gtk_init_or_skip(int *argc, char ***argv);   /* gtk_init_check else 77 */
gboolean rt_screen_fits(gint width, gint height);    /* monitor workarea check; caller g_test_skip()s (F19) */

/* ---- native input (§6) ---------------------------------------------------- */
gboolean rt_input_available(void);
void rt_input_motion(gint screen_x, gint screen_y);
void rt_input_button(guint button, gboolean press);  /* 1..3 buttons, 4/5 wheel up/down, X11 numbering */
void rt_input_key(guint keyval, gboolean press);     /* GDK keyval (XK_* == GDK_KEY_* values) */
void rt_input_pointer(gint *screen_x, gint *screen_y);
void rt_input_restore_pointer(void);
```

Two extra rules for all ported tests:

* Never build a path for **comparison** from a literal with an embedded `/` (`g_build_filename(root, "data/database/x")`):
  use separate components or `rt_path_equal` (affected: `test_media_archive.c:40,46,47,91,166,169`,
  `test_storage_locations.c:50-52,101,153,181`, `test_data_client.c:212`, `test_data_ui.c:629`, `test_transfer_ui.c:538` —
  only the first two compare).
* Every raw `open()` of a capture or fixture file goes through `rt_open` (binary mode, UTF-8 path).

---------------------------------------------------------------------------------------------------

## 6. Native input (XTest replacement) and the private desktop (Xvfb replacement)

### 6.1 What the Linux tests do

`content_scale_test_input.h:57-85` and the private copies in `test_content_scale_module.c:14-32`,
`test_content_scale_ui.c:98-135`, `routing_patchbay_ui_test.inc:96-128,543-566` send **real** pointer/keyboard events
through the X server (`XTestFakeMotionEvent/ButtonEvent/KeyEvent` + `XFlush`), at **screen** coordinates computed from
`gdk_window_get_origin`/`gdk_window_get_root_coords` and the content-scale ratio; wheel = buttons 4/5; keys = X keysyms
(`XK_Control_L`, `XK_a`, `XK_Escape`, …); they assert real GTK behaviour (focus, popups opened at the anchor, DnD,
scroll, offscreen-window coordinate translation in `src/ui/content_scale.c`). They skip (77) without XTest
(`.inc:544-546`, `test_content_scale_ui.c:651`). The Wayland branch of the header drives a private GNOME Shell through
`org.gnome.Shell.Eval` (Linux only; delete on Windows).

### 6.2 Windows design (`rt_input_*`, implemented in `radios_test_support.c`)

Backend P (default, works on private desktops, verified F16):
* `rt_input_motion(x,y)`: remember the screen point; find the target `GdkWindow` = the topmost mapped toplevel of this
  process containing the point (walk `gdk_screen_get_toplevel_windows()`/`gtk_window_list_toplevels()`, z-order from
  `GetWindow(GW_HWNDNEXT)`), `HWND = gdk_win32_window_get_handle(toplevel)`, `ScreenToClient`, then
  `PostMessageW(hwnd, WM_MOUSEMOVE, buttons_state, MAKELPARAM(cx,cy))`. While a button is held, keep targeting the
  window that received the press (implicit grab) — GDK’s own grab/capture logic then routes it.
* `rt_input_button(b, press)`: 1/2/3 → `WM_[LMR]BUTTONDOWN/UP` with the `MK_*` state; 4/5 →
  `WM_MOUSEWHEEL` with `GET_WHEEL_DELTA` ±120 and **screen** coordinates in lParam.
* `rt_input_key(keyval, press)`: keyval → VK with `gdk_keymap_get_entries_for_keyval` (falls back to
  `VkKeyScanW`); maintain a thread keyboard state with `GetKeyboardState/SetKeyboardState` for Shift/Ctrl/Alt
  (verified: Ctrl+A works); post `WM_KEYDOWN/WM_KEYUP` (`WM_SYSKEYDOWN` for Alt) with scan code from `MapVirtualKeyW`
  to the focused toplevel’s HWND. GDK’s `TranslateMessage` generates `WM_CHAR`.
* After each call pump the main context for the same delays the tests already use (`pump(25)`, `run_for(15)` …).
* Popups (menus, combo lists, tooltips) are separate popup HWNDs; the target lookup must include
  `GTK_WINDOW_POPUP` toplevels — this is what the "opened at the pointer" assertions need.

Backend S (`RADIOS_TEST_INPUT=sendinput`): `SendInput` with absolute coordinates normalised to the virtual screen,
real OS path (needed only if a case relies on OLE drag-and-drop modal loops — `test_content_scale_module.c`
"native-pixels-popup-and-real-DnD" uses GTK same-app DnD; try backend P first, fall back to S). Backend S requires the
interactive input desktop, moves the user’s cursor, and must hold `RESOURCE_LOCK native_input`. Restore the pointer
afterwards (`rt_input_restore_pointer`, replaces `.inc:547-548,565`).

`rt_input_available()`: X11 → `GDK_IS_X11_DISPLAY && XTestQueryExtension`; Windows → `GDK_IS_WIN32_DISPLAY` (P) or
input desktop present (S). Keep the existing `return 77` paths.

Coordinate helpers stay GDK-portable: `gdk_window_get_origin`, `gdk_window_get_root_coords`,
`gdk_window_coords_to_parent`, `gdk_device_get_position` all work on win32. `cs_popup_frame` on Windows =
`gdk_window_get_origin(popup)` + allocation (same as the X11 branch).

### 6.3 Private desktop launcher (Xvfb/`-displayfd` replacement)

`tools/win32/radios-test-desktop.exe CMDLINE` (C, ~60 lines; prototype `desk.c` in the probe dir):
`CreateDesktopW(L"RadioSTest-<pid>", …, GENERIC_ALL)` → `CreateProcessW` with
`STARTUPINFOW.lpDesktop = L"WinSta0\\RadioSTest-<pid>"`, `bInheritHandles=TRUE` (stdout/stderr TAP), wait with the
runner’s timeout, `TerminateProcess` on timeout, propagate the exit code, `CloseDesktop`. Python runners
(`run_rotation_editor_regressions.py`, `run_content_scale_ui.py`, `run_*_ui.py`) wrap the test command with it instead of
starting Xvfb; CTest tests registered *directly* (e.g. `native_programme_queue`, `playlist_browser_ui`) can be wrapped in
CMake on WIN32 (`COMMAND radios-test-desktop $<TARGET_FILE:…>`). Limits: the desktop shares the monitor resolution and
DPI of the session (F19; content-scale UI asked Xvfb for 2560×1600), shares the window station clipboard (F17), and
needs an interactive session (a service session without a desktop cannot create windows).

---------------------------------------------------------------------------------------------------

## 7. Audio devices for tests (contract the audio area must implement)

### 7.1 Engine-facing contract kept by the tests

* `radios_audio_engine_new(gboolean realtime, const gchar *server)`, `radios_audio_patchbay_new(const gchar *server)`,
  backend `radios_pulse_backend_new(rings[3], capture_ring, server)` / `_apply/_reconnect/_snapshot/_refresh/_free`
  (`test_pulse_hotplug.c:252-286`) and `RadioSAudioRing` with atomic `read/write`/`tags` (`:14-37`).
* Snapshot fields the tests read: `connected`, `buses[i].{ready,error,device,peak[],consumed_frames,output_sample_rate,
  output_buffer_frames}`, `microphone_{ready,error,gate,latch,ptt,peak[]}`, `loss_epoch`, `outputs[]/output_count`
  (names), `inputs[]` with `monitor` flag, `radios_audio_engine_output_flushes()`; device ids are the sink/source
  **names** (`"radios-nonexistent-device"` must be "not ready" without error storms, `test_audio.c:257-260`).
* `server` strings used by tests: `NULL` (default), `"unix:/nonexistent/radios-patchbay-test"` (= *no devices at all*,
  `test_patchbay.c:149,214`), `RADIOS_TEST_PULSE_SERVER` (private daemon). On Windows define `"none:"` and
  `"virtual:"` and map the `unix:/nonexistent…` literal to `none:` (or change the two test literals to a macro
  `RT_AUDIO_SERVER_NONE`).

### 7.2 Virtual backend (`RADIOS_AUDIO_BACKEND=virtual`)

* Sinks from `RADIOS_AUDIO_VIRTUAL_SINKS=name[,name…]`; each sink also exposes a capture source `<name>.monitor`
  (loopback of everything mixed into the sink); optional `RADIOS_AUDIO_VIRTUAL_SOURCES`.
* A process-wide device registry: several engines in one process may open the same sink (mix = sum) or capture its
  monitor (`test_channel_input.c:333-388` uses one realtime engine playing to MONITOR and a second engine capturing
  `MONITOR.monitor`).
* Real-time pacing from a high-resolution waitable timer in 10 ms periods (WASAPI-like); `consumed_frames` advances with
  wall time (soak asserts ±10 %, `test_audio_soak.c:145-146`). Any requested rate (44.1 kHz in
  `test_preferences_output.c:74`); report it in `output_sample_rate`.
* Capture: with `RADIOS_AUDIO_VIRTUAL_CAPTURE_DIR=dir` every sink appends interleaved float32le stereo at its rate to
  `dir/<name>.f32` **from process start to exit, writing silence when nothing plays** (parec semantics: the tests take
  `capture_offset()` then measure ≥4800 frames after it; "silent" assertions need written zeros). Open the file with
  `FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE`; the runner sets `RADIOS_TEST_<BUS>_CAPTURE` to these paths.
* `RADIOS_AUDIO_BACKEND=none`: no devices; all buses not ready, stable error text.
* The Windows `run_audio_ui.py` equivalent: generate `radios_gui_<uuid>_{main,cue,monitor}`, export the three
  `RADIOS_TEST_*_SINK`, `RADIOS_AUDIO_VIRTUAL_SINKS`, capture dir/paths, `RADIOS_TEST_MIC_SOURCE=<monitor>.monitor`,
  post the audio-preferences document exactly as today.

### 7.3 Control API for recovery/hotplug tests (test-only exported functions of the virtual backend)

```c
void     radios_audio_virtual_server_stop(void);              /* all streams fail, connected=FALSE, loss_epoch++ */
void     radios_audio_virtual_server_restart(gboolean keep_sinks); /* keep_sinks=FALSE == "restart-missing" */
gboolean radios_audio_virtual_sink_add(const gchar *name, guint rate, const gchar *description);
gboolean radios_audio_virtual_sink_remove(const gchar *name);
```

They replace the pactl/daemon choreography of `test_audio_recovery.c` (marker files `:16-26`, `pactl load/unload
module-null-sink` `:116-138,180,228,268,303`, `parec` + `kill(SIGTERM)` + `waitpid` `:270-300`) and
`test_stream_metadata.c:787-870` (window stays responsive while devices are plugged). Real WASAPI behaviours the
backend must map to the same snapshot transitions: `AUDCLNT_E_DEVICE_INVALIDATED`, `AUDCLNT_E_SERVICE_NOT_RUNNING`,
`IMMNotificationClient::OnDeviceAdded/Removed/StateChanged/DefaultDeviceChanged`.

Windows twin of `test_pulse_hotplug.c` → `tests/test_wasapi_hotplug.c`: compile the WASAPI backend TU(s) with a fake
enumeration seam (the backend obtains `IMMDeviceEnumerator` through an internal `radios_wasapi_create_enumerator()`
in its own TU; the test links a fake COM implementation of `IMMDeviceEnumerator/IMMDeviceCollection/IMMDevice/
IPropertyStore` with per-item delay and an `IMMNotificationClient` event pump). Same three assertions: lists publish
atomically (never a partial count) incl. overlapping rescans; hotplug followed without "Yenile" with debounce
(≥200 ms, <2 s, one rescan per burst, unrelated events ignored); snapshot <20 ms with 64+64 devices.
(`ole32`/`mmdevapi` calls are `dllimport`: do not try `--wrap=CoCreateInstance`.)

---------------------------------------------------------------------------------------------------

## 8. MIDI (WinMM) test layer

Public contract kept (from `test_midi.c`, `test_midi_hotplug.c`): `radios_midi_list_inputs(GError**)` →
`RadioSMidiPort{id,name}`; `radios_midi_input_open(id, cb, user, error)`, `radios_midi_input_open_full(id, NULL,
event_cb, notice_cb, user, error)`, `radios_midi_input_alive`, `_port`, `_close`; `radios_midi_event_key` keys
`note:<ch>:<n>`, `cc:<ch>:<n>`, `pitch:<ch>`, values (note 0/1, cc/127, pitch ±1); `radios_midi_key_valid/label`;
notices `RADIOS_MIDI_NOTICE_LOST` / port changes; UI widgets `preferences-midi-{status,device,keys,refresh}`, status
texts "Bağlı: …", "… takılı değil. Denetleyiciyi takın; bağlantı kendiliğinden kurulur."; timing: loss <300 ms with
device notifications, ≤~1 s without, UI retry every 5 s in the background, no sequencer call on the GTK thread
(<100 ms), main-loop gap <100 ms.

Windows design for the twin `tests/test_midi_hotplug_win32.c`: the WinMM backend (`src/core/midi_winmm.c`) is included
into the test after `#include <windows.h>/<mmsystem.h>` and `#define midiInGetNumDevs fake_midiInGetNumDevs`,
`midiInGetDevCapsW`, `midiInOpen`, `midiInStart`, `midiInStop`, `midiInReset`, `midiInClose`,
`RegisterDeviceNotificationW`/`CM_Register_Notification` (whatever the backend uses for hotplug) — the macro-rename
pattern of `autoplay_execution_clock.c`; WinMM is `dllimport`, so link-time interposition is impossible. The fake
delivers `MIM_DATA` short messages on a worker thread (like a real driver callback), shifts device indices on
unplug/replug (WinMM indices are not stable — the port id must be name-based, e.g. `"<name>#<k>"`), and can delay
`midiInOpen` by 200–300 ms. Port the 5 cases 1:1.
`test_midi.c` virtual controller: on Windows open a loopback output (loopMIDI/Windows MIDI Services port whose name is
given by `RADIOS_TEST_MIDI_LOOPBACK`) with `midiOutOpen/midiOutShortMsg`; skip when unset. `/midi/keys` is portable.

---------------------------------------------------------------------------------------------------

## 9. Per-file table

Legend — Display: N = none; D = direct GTK display (`gtk_init*`, 77 if missing); X = private display via
`run_rotation_editor_regressions.py`/`run_content_scale_ui.py`; R = GUI under a service runner (`run_*_ui.py`).
Audio: N = none/offline engine; V = owned virtual sinks + capture files (§7); P = private sound-server control (§7.3);
E = env-gated realtime cases that skip without V. Runner = CTest command wrapper. "dep" lists Linux-only items with
`file:line`; details for every B/C/D row are in §10.

| # | File | CTest name(s) | Runner | Linux-only dependencies | Disp | Audio | Cat | Windows action |
|---|---|---|---|---|---|---|---|---|
| 1 | application_shutdown_driver.c | native_application_shutdown | test_application_shutdown.py | Pulse source id `"routing_microphone.monitor"` :81; relies on app SIGTERM/SIGINT (`src/app/main.c:55-56`) via the Python side | R | V | B | read the source id from env (`RADIOS_TEST_MIC_SOURCE`/virtual monitor); keep `--wrap=radios_window_present`; graceful stop handled by app/py areas |
| 2 | autoplay_execution_clock.c | native_autoplay_pending (source) | run_playout_ui.py | none | R | V | A | none — template for clock seams |
| 3 | content_scale_test_input.h | (header of native_content_scale_popup_*, content_scale_ui) | — | `gdk/gdkx.h`, XTest, keysym :12-14; XTest :59-81; GNOME-Shell D-Bus Eval :20-56,83-154 | X | N | C | rewrite as thin wrapper over `rt_input_*`/GDK coords (§6); keep `cs_*` names; drop Wayland path on WIN32; `XK_*` → `GDK_KEY_*` |
| 4 | header_layout_driver.c | (none; exe `header_layout_driver`) | manual | `symlink(assets)` :233; `setlocale(LC_ALL,"")` :200; `<unistd.h>` :12 | D | N | B | `rt_symlink(…, TRUE)` or copy assets; keep setlocale; exact-size check needs `rt_screen_fits` |
| 5 | routing_patchbay_ui_test.inc | native_routing_graph, native_routing | run_routing_graph_ui.py, run_routing_ui.py | X11/XTest :13-16,98-120,543-549,564-566; `struct stat`+`g_stat`+0600 :175; Pulse ids :136,146 | R | V | C | native input → `rt_input_*` (§6); `rt_assert_private_file`; device ids from env/virtual names; gate files via `rt_set_contents`/`rt_unlink` :232-265,357-365 |
| 6 | test_admin_ui.c | native_admin | run_admin_ui.py | `g_chmod(hold,0600)` :235,270; 0600 key file :548; system CLIPBOARD read :336 | R | N | B | `rt_assert_private_file` :548; gate writes `rt_set_contents`; `RESOURCE_LOCK system_clipboard` |
| 7 | test_advert_ui.c | native_advert | run_advert_ui.py | `struct stat`+`g_stat` :360; `open/fstat/pread/close` :365-371 | R | V | B | capture readers → `rt_file_size`/`rt_open`/`rt_pread` |
| 8 | test_audio.c | audio, audio_devices | direct; run_audio_ui.py --engine-only | none in code (env-gated realtime cases :205-262,520-556,599-645,662-700) | N | E/V | A | none; realtime cases need §7 + `timeBeginPeriod(1)` (3.3 ms bursts :616,628) |
| 9 | test_audio_decoder.c | audio_decoder | direct | `lstat`/`S_ISDIR` :17-18; ffmpeg via `g_spawn_sync` :88; `open`+`ftruncate` 64 MiB :203-204; `symlink` :250,253; `mkfifo` :257; `open(O_WRONLY)`+`pwrite` :269-270; `ftruncate((off_t)2GiB+1)` :313-314 | N | N | B | `rt_remove_tree`; `rt_symlink`; FIFO case `#if RT_HAVE_FIFO`; `rt_create_sized_file`; `rt_open`+`rt_pwrite`; ffmpeg.exe on PATH |
| 10 | test_audio_loudness_policy_ui.c | native_loudness_policy_{roundtrip-close-recovery, first-read-independent-merge, invalid-edit-preserves-revision, production-catalogue-names-selection-persistence, preview-applies-stored-measurement} | run_audio_ui.py | via base test_audio_ui.c; IANA "Europe/Istanbul" :41,219 (production tz) | R | V | A | none (base #13; platform tz) |
| 11 | test_audio_recovery.c | audio_recovery | run_audio_recovery.py | `<signal.h>`/`<sys/wait.h>` :6,8; pactl :116-138; `getpid` :129,178,202,266; parec+`kill`+`waitpid` :270-300; private daemon + marker files :16-26,58,177,265 | N | P | D | exclude on WIN32; new `test_wasapi_recovery.c` with the same 3 cases on §7.3 API (server stop/restart/missing sink; hotplug ×6 while MAIN plays; flush-gap via capture file) |
| 12 | test_audio_soak.c | audio_soak | run_audio_soak.py | `/proc/self/statm`+`sysconf` :31-33; `/proc/self/fd` :40-41; Pulse server env :63,87 | N | P | C | `rt_process_resident_bytes/handle_count` (re-tune `fd_max<=start+12` → handle allowance); server → `virtual:` |
| 13 | test_audio_ui.c | native_audio | run_audio_ui.py | `struct stat`+`g_stat` :228; `open/fstat/pread` :233-259; `symlink` :586; gate `g_chmod` :717 | R | V | B | capture readers via helpers; `rt_symlink` (skip case if unsupported); `rt_set_contents` |
| 14 | test_autodj_settings_ui.c | native_autodj_settings, native_autodj_settings_dense_final_section | run_playout_ui.py | via base | R | V | A | none |
| 15 | test_automation_adjunct.c | automation_midtrack_jingle | direct (`RADIOS_AUDIO_NULL=1`, unused by src) | none | N | N | A | none |
| 16 | test_automation_calendar.c | native_automation_calendar | run_automation_calendar_ui.py | `g_time_zone_new_identifier` :1437; `FcInit/FcConfigAppFontAddFile` :3046 (works but not used by Pango-win32) | R | N | A | use `rt_time_zone` at :1437 (1-line, counted A); font registration follows the UI area's Windows font loader |
| 17 | test_automation_calendar_plan.c | automation_calendar_plan | direct | `database = "/tmp/radios-calendar-plan.sdb"` :8 with `"%s.objects/%s.wav"` :35 | N | N | B | build `database` with `g_build_filename(g_get_tmp_dir(),…)` canonicalized and object paths with `g_build_filename` (planner prefix check, see §10) |
| 18 | test_automation_pdf_plan.c | automation_pdf_plan | direct | via base #17 | N | N | A | none |
| 19 | test_automation_policy.c | automation_probability_policy | direct | none ("Europe/Istanbul" through production :113) | N | N | A | none (platform tz) |
| 20 | test_automation_probability_playout.c | automation_probability_playout | direct | via base test_playout.c; raw `RADIOS_TEST_PROJECT_ROOT` database :249 | N | N | B | `database = g_build_filename(rt_project_root(),"build",…)` |
| 21 | test_automation_probability_ui.c | native_automation_probability | run_automation_probability_ui.py | via base #16 | R | N | A | none |
| 22 | test_automation_scope.c | automation_scope | direct | none | N | N | A | none |
| 23 | test_automation_scope_ui.c | automation_scope_fetch | run_rotation_editor_regressions.py | none (`--wrap` radios_* ok) | X | N | A | none |
| 24 | test_automation_workspace_acceptance.c | native_automation_workspace_acceptance | run_audio_ui.py | via base test_player_workspace_ui.c | R | V | A | none |
| 25 | test_automation_workspace_ui.c | native_automation_workspace | run_rotation_editor_regressions.py | none (17 radios_* wraps) | X | N | A | none |
| 26 | test_autoplay_events_ui.c | native_autoplay_events_{actual-next-replacement-stop-scope-readonly, aggregate-retains-independent-queue, general-owned-pool-filter-readonly, future-schema-wait-cancel-clears-events, distinct-block-identity-and-planned-jingle} | run_playout_ui.py | via base | R | V | A | none |
| 27 | test_autoplay_header_ui.c | native_autoplay_header_{scoped-playlist-name-end-advert-pause-stop-PCM, schema-current-future-status-clears-preparing-PCM} | run_playout_ui.py | `g_time_zone_new_identifier(station tz)` :37 | R | V | B | `rt_time_zone` |
| 28 | test_autoplay_pending_ui.c | native_autoplay_pending | run_playout_ui.py | none (`stdatomic.h` ok) | R | V | A | none |
| 29 | test_broadcast_catalogue_ui.c | native_broadcast_catalogue | run_broadcast_catalogue_ui.py | custom selection atoms :235,547 (fatal Gdk-WARNING, F17); gate `g_unlink`==0 :163 | R | N | B | `rt_selection_data_new`; `flag()` via `rt_set_contents`/`rt_unlink` |
| 30 | test_broadcast_header.c | broadcast_header | direct (GUI block) | none (7 radios_* wraps) | N | N | A | none |
| 31 | test_broadcast_metadata_ui.c | native_broadcast_automatic_metadata_filter | run_broadcast_ui.py | via base test_broadcast_ui.c | R | V | A | none |
| 32 | test_broadcast_ui.c | native_broadcast | run_broadcast_ui.py | none in code (stream receiver/metadata capture from runner) | R | V | A | none |
| 33 | test_calendar_regressions.c | native_calendar_regressions | direct | none | D | N | A | none |
| 34 | test_catalogue_playlist_ui.c | native_catalogue_playlist | run_catalogue_playlist_ui.py | none | R | N | A | none |
| 35 | test_channel_input.c | channel_input | direct | none in code; `file:///etc/passwd` & `/home/…` are rejected-URL literals :64-65; capture case env-gated :323-394 | N | E/V | A | none; optional Windows-specific rejected inputs (`C:\a.mp3`, `\\srv\s\a.mp3`, `file:///C:/a.mp3`) |
| 36 | test_cluster_pool_cards_ui.c | native_cluster_pool_cards | run_playout_ui.py | via base | R | V | A | none |
| 37 | test_config.c | config | direct | `symlink` :301,311,333,338; `/tmp/...` literals :271,286; `remove_tree` IS_SYMLINK :31 | N | N | B | `rt_symlink`(+skip), `rt_remove_tree`; add Windows escape cases (§10) |
| 38 | test_content_scale_module.c | native_content_scale_module | run_rotation_editor_regressions.py | `gdkx.h`/XTest/keysym :2-4; XTest motion/button/key :14,24-32; `XK_*` | X | N | C | replace `motion/mouse/key` with `rt_input_*`; `XK_*`→`GDK_KEY_*`; DnD case may need backend S |
| 39 | test_content_scale_popups.c | native_content_scale_popup_{context-menus-at-pointer-and-widget, context-menus-before-map, combo-lists-and-menu-button, popover-and-tooltip, tree-search-entry} | run_rotation_editor_regressions.py | via header #3; `XK_Escape` :20 | X | N | C | follows header rewrite; `RADIOS_CONTENT_SCALE_POPUP_MAP_ONLY` env (:181,223) is read by `src/ui/content_scale.c:483` only on X11 |
| 40 | test_content_scale_ui.c | content_scale_ui | run_content_scale_ui.py | `gdkx.h`/XTest :10-12; XTest :105-109,128-133; `XTestQueryExtension` :651; `symlink(assets)` :303,425,524; IS_SYMLINK :54; `gdk_pixbuf_get_from_window` :76 | X | N | C | `rt_input_*`; `rt_symlink` or copy assets; `rt_remove_tree`; screenshot via `gtk_widget_draw` if window read-back is blank on private desktops; size via `rt_screen_fits` |
| 41 | test_copyright_playback_ui.c | native_copyright_playback | run_playout_ui.py | via base | R | V | A | none |
| 42 | test_data_client.c | data_client | direct | `chmod 0644` refusal :346; `chmod 0600` :230,349,352,364,369; `symlink` :372; IS_SYMLINK :52; `g_socket_listener_add_any_inet_port` :236 (firewall prompt) | N | N | B | `rt_make_world_readable`/`rt_make_private`; `rt_symlink`(+skip); `rt_remove_tree`; bind loopback only |
| 43 | test_data_connection_ui.c | native_data_connection | run_data_connection_ui.py | none (runner starts/stops the service on the host) | R | N | A | none (py area: service lifecycle) |
| 44 | test_data_ui.c | native_data, native_media | run_native_data_ui.py, run_data_ui.py | 0600 key file :452; `g_spawn_sync(python …stop/start)` :626-636 | R | N | B | `rt_assert_private_file`; spawn stays (python_command absolute, gspawn helper on PATH) |
| 45 | test_dj_ui.c | native_dj | run_dj_ui.py | `struct stat`+`g_stat` :219; `open/fstat/pread` :226-288 | R | V | B | capture readers via helpers |
| 46 | test_dsp.c | dsp | direct | none | N | N | A | none |
| 47 | test_effect_return.c | effect_return | direct | none | N | N | A | none |
| 48 | test_effect_return_ui.c | native_effect_return | run_dj_ui.py | via base #45 | R | V | A | none |
| 49 | test_emergency_transfer_ui.c | native_emergency_transfer | run_emergency_transfer_ui.py | none | R | N | A | none |
| 50 | test_event_assignment_ui.c | native_event_assignment_{save-reopen-SEMI-row-PCM, stale-target-fence, general-assignment-isolated-SEMI-row-PCM} | run_playout_ui.py | via base | R | V | A | none |
| 51 | test_event_block_queue_ui.c | native_event_block_queue | run_playout_ui.py | via base | R | V | A | none |
| 52 | test_event_playout_ui.c | native_event_playout_{local-event-unclustered-SEMI-preserves-queues-PCM, event-complete-then-cluster-continuation-PCM, event-stop-newload-target-change-fences-PCM, event-assigned-schema-continuation-future-stop-PCM, event-SEMI-pending-unrelated-peer-replacement-PCM} | run_playout_ui.py | via base | R | V | A | none |
| 53 | test_external_access.c | external_access | run_external_access.py | none (TLS server + CA from runner) | N | N | A | none (curl must honour CAINFO with the MSYS2 TLS backend) |
| 54 | test_external_access_ui.c | native_external_access | run_admin_ui.py | none (`--wrap=gtk_show_uri_on_window` works, F12) | R | N | A | none |
| 55 | test_general_queue_ui.c | native_general_queue, native_general_schema_entry, native_general_schema_apply, native_general_queue_responsive | run_audio_ui.py / run_playout_ui.py | none | R | V | A | none |
| 56 | test_header_navigation_backend.c | header_navigation_backend, header_navigation_realtime | direct; run_audio_ui.py --engine-only | none (realtime case env :197-199) | N | E/V | A | none |
| 57 | test_header_navigation_ui.c | native_header_navigation | run_header_navigation_ui.py | none | R | V | A | none |
| 58 | test_header_next_playlist_ui.c | native_header_next_playlist_{local-queue, general-owner, general-not-owner, scope-flow} | run_playout_ui.py | via base | R | V | A | none |
| 59 | test_header_playout_ui.c | native_header_private_{programme, wait, playlist, transition}, native_header_live_strip | run_playout_ui.py | `g_time_zone_new_identifier` :113 | R | V | B | `rt_time_zone` |
| 60 | test_header_progress.c | native_header_progress | direct | none | D | N | A | none |
| 61 | test_header_ui.c | native_header_status | direct | `-Wl,--wrap=g_get_real_time` (Pdf2HeaderStatus.cmake) + `__wrap_g_get_real_time` :95 — ineffective on Windows (F12); tz "Europe/Istanbul" :513 via production | D | N | B | wrapper TU for `src/ui/header_status.c` with `#define g_get_real_time radios_test_header_real_time`; rename :95; CMake: drop the wrap on WIN32 |
| 62 | test_import_profiles_ui.c | native_import_profiles | run_admin_ui.py | none (macOS `file://localhost/Volumes/…` rules are data) | R | N | A | none; optional Windows rekordbox `file://localhost/C:/…` case |
| 63 | test_library_analysis.c | library_analysis | direct | 0600 :98-99; IS_SYMLINK :17 | N | N | B | `rt_assert_private_file`, `rt_remove_tree` |
| 64 | test_library_analysis_ui.c | native_library_analysis | direct | IS_SYMLINK :19 (no links created) | D | N | A | none (optionally `rt_remove_tree`) |
| 65 | test_live_control.c | live_control | direct | none | N | N | A | none |
| 66 | test_live_layout.c | native_live_layout, native_live_layout_connected | direct; run_audio_ui.py | none (widths ≤1600) | D/R | N/V | A | needs ≥1600 px wide desktop |
| 67 | test_live_players_ui.c | native_live_players | direct | none (10 radios_* wraps) | D | N | A | none |
| 68 | test_live_row_ui.c | native_live_row_{talk_manual, talk_duration, release, music_schema, talk_schema, header_general_queue, ending_warning, remove_unheard} | run_playout_ui.py | via base | R | V | A | none |
| 69 | test_live_step_wait_ui.c | native_live_step_{duration-counts-from-actual-end-PCM, next-ends-duration-step-early-PCM, manual-step-waits-for-next-PCM, next-during-row-enters-step-PCM, next-during-row-starts-duration-PCM} | run_playout_ui.py | via base | R | V | A | none |
| 70 | test_local_event_prepare_ui.c | native_local_event_{materialize-readonly-and-cancel-PCM, insert-fences-live-queue-PCM, mixed-sources-station-breaks-PCM, mixed-flow-manual-wait-stable-PCM, schema-from-now-readonly-PCM} | run_playout_ui.py | via base | R | V | A | none |
| 71 | test_local_event_ui.c | native_local_event_builder | run_playout_ui.py | via base | R | V | A | none |
| 72 | test_local_ini.c | local_ini | direct | `lstat`+`S_ISREG`+0600 :93-95; `symlink` :131,170; IS_SYMLINK :18 and as assertion :137 | N | N | B | `rt_assert_private_file`; `rt_symlink`; `rt_is_link`; `rt_remove_tree`; add Windows bad names (§10) |
| 73 | test_loudness.c | loudness | direct | none | N | N | A | none |
| 74 | test_loudness_engine.c | loudness_engine | direct | none (canonical root :66) | N | N | A | none |
| 75 | test_loudness_origin_playout_ui.c | native_loudness_execution_origin, native_loudness_scheduled_origin, native_loudness_scope_execution_origin | run_playout_ui.py | via base | R | V | A | none |
| 76 | test_loudness_policy.c | loudness_policy | direct | none (IANA zones validated by `src/core/loudness_policy.c:31` = `/usr/share/zoneinfo`) | N | N | A | none (platform tz) |
| 77 | test_loudness_settings.c | loudness_settings | run_rotation_editor_regressions.py | none ("Europe/Istanbul" :104,146) | X | N | A | none (platform tz) |
| 78 | test_media.c | media | direct | `lstat`/`S_ISDIR` :19-20; ffmpeg spawn :125; `lstat`+0600 :183-184; path string compare :181,185; `symlink` :210,228,232; `mkfifo` :212; `O_EXCL`+`ftruncate` :215-216; mutator `open`+`pwrite` :284-287 | N | N | B | as test_audio_decoder + `rt_assert_path_equal` :185 |
| 79 | test_media_archive.c | media_archive | direct | `realpath` :34; `symlink` :50; 0600 :98; IS_SYMLINK :13; "a/b" components compared :40,46,47,91,166,169 | N | N | B | `rt_make_tmp_dir`; `rt_symlink` (adjust `skipped_links`); `rt_assert_private_file`; per-component paths; add hidden-attribute case |
| 80 | test_media_archive_ui.c | native_media_archive | run_data_ui.py | none | R | N | A | none |
| 81 | test_media_drop_ui.c | native_media_drop | run_data_ui.py | system CLIPBOARD selection trick :176-191; `FcInit` :776; `file://` URIs :296-748 | R | N | B | `rt_selection_data_new`; URIs via `g_filename_to_uri` stay; see UNC note §10 |
| 82 | test_media_rights_ui.c | native_media_rights, native_copyright_settings | run_data_ui.py | none | R | N | A | none |
| 83 | test_midi.c | midi | direct | `<alsa/asoundlib.h>` :2; ALSA sequencer client :10-56 | N | N | C | `#ifdef` ALSA case; WinMM loopback variant (§8); `/midi/keys` portable |
| 84 | test_midi_hotplug.c | native_midi_hotplug_{input-announcements, input-liveness-without-announcements, ui-unplug-replug-refresh, ui-slow-sequencer, ui-teardown-pending-job} | run_rotation_editor_regressions.py | fake ALSA sequencer :22-286 (`pipe2` :77, `read/write` :51,216); `#include "../src/core/midi.c"` :287 | X | N | D | exclude; new `test_midi_hotplug_win32.c` (fake WinMM by macro rename, same 5 cases, §8) |
| 85 | test_mixer_auto_volume_ui.c | native_mixer_auto_volume | run_audio_ui.py | via base #13 | R | V | A | none |
| 86 | test_neon_progress.c | native_neon_progress | direct | none | D | N | A | none |
| 87 | test_output.c | output | direct | BSD sockets `<arpa/inet.h> <poll.h> <sys/socket.h> <sys/time.h>` :5-10; `recv/send(MSG_NOSIGNAL)/setsockopt(SO_RCVTIMEO,timeval)/shutdown(SHUT_WR)/poll/accept/socket/bind/getsockname/listen/close` :119-200 | N | N | C | rewrite the fake HTTP server (~80 lines) with `GSocketListener`/`GSocket` (as test_patchbay.c:54-66 does) |
| 88 | test_output_resampler.c | output_resampler | direct | none | N | N | A | none |
| 89 | test_patchbay.c | patchbay | direct | `"unix:/nonexistent/…"` :149,214; `env PULSE_SERVER pactl` :241-245; `pacat` spawn :261-264; `kill(SIGTERM)` :319; `<signal.h>` :7 | N | N(E) | B | `#ifndef G_OS_WIN32` around `pulse_devices`; server literal → `RT_AUDIO_SERVER_NONE`; GSocket servers portable |
| 90 | test_player_content_status_ui.c | native_player_content_status_{manual-kinds-MAIN-PCM, scoped-sweeper-hidden-pause-stop-MAIN-PCM} | run_playout_ui.py | via base | R | V | A | none |
| 91 | test_player_loader_ui.c | native_player_loader | direct | none | D | N | A | none |
| 92 | test_player_mapping_ui.c | native_player_mapping | run_player_mapping_ui.py | `struct stat`+`g_stat` :190-191 | R | V | B | `rt_file_size` |
| 93 | test_player_schema_weekly_ui.c | native_player_schema_weekly | run_automation_calendar_ui.py | via base #16 ("Asia/Istanbul" :96 via production) | R | N | A | none |
| 94 | test_player_topology.c | player_topology | direct | none | N | N | A | none |
| 95 | test_player_tree_ui.c | native_player_tree | run_rotation_editor_regressions.py | none (4 wraps) | X | N | A | none |
| 96 | test_player_waveform_markers.c | player_waveform_markers | run_rotation_editor_regressions.py | none (12 radios_* wraps; `g_chmod 0600` :122 harmless) | X | N | A | none |
| 97 | test_player_workspace_ui.c | native_player_workspace | run_audio_ui.py | none | R | V | A | none |
| 98 | test_playlist_browser_ui.c | playlist_browser_ui | direct (`gtk_test_init`) | none | D | N | A | none |
| 99 | test_playlist_export_ui.c | native_playlist_export | run_rotation_editor_regressions.py | via base #100 (`"/tmp/Özel şarkı.wav"` :61 is export data) | X | N | A | none |
| 100 | test_playlist_redesign_ui.c | native_playlist_redesign | direct | custom selection atom :239 | D | N | B | `rt_selection_data_new` |
| 101 | test_playout.c | playout | direct | non-canonical `database` (raw `RADIOS_TEST_PROJECT_ROOT`) :1810 with `"%s.objects/%s.wav"` :17 → planner containment check `src/playout/planner.c:308-311` | N | N | B | canonical database path + `g_build_filename` object paths (§10) |
| 102 | test_playout_event_blocks.c | playout_event_blocks | direct | via base #101; database :218 | N | N | B | as #101 |
| 103 | test_playout_jingle_ui.c | native_jingle-empty-unrouted-MAIN-PCM, native_jingle-later-append-unrouted-MAIN-PCM, native_jingle-unavailable-preserves-primary-and-explicit-player-PCM | run_playout_ui.py | via base | R | V | A | none |
| 104 | test_playout_policy_ui.c | native_playout_policy | run_data_ui.py | none | R | N | A | none |
| 105 | test_playout_ui.c | native_playout, native_player_waveform, native_player_sync, native_player_sync_stale (+22 includers) | run_playout_ui.py | capture readers :330-378,637-651; custom selection :835 | R | V | B | helpers (§10); base for 22 files |
| 106 | test_playout_vocal_jingle_ui.c | native_playout_vocal_jingle | run_playout_ui.py | via base | R | V | A | none |
| 107 | test_preferences_output.c | preferences_output | run_preferences_output.py | none in code (needs sink at 44.1 kHz + capture + proof) | N | V | A | none |
| 108 | test_preferences_storage.c | preferences_storage | direct | `struct stat`+`g_stat` :73,81-82; 0600 :83; `st_ino` :84; `symlink` :184,211; `mkfifo` :196; IS_SYMLINK :15 | N | N | B | `rt_assert_private_file`, `rt_file_id`, `rt_symlink`, FIFO guard, `rt_remove_tree` |
| 109 | test_preferences_ui.c | native_preferences | direct | none | D | N | A | none |
| 110 | test_programme_jingle_hint.c | programme_jingle_hint | direct | via base #112 | D | N | A | none |
| 111 | test_programme_model.c | programme_model | direct | none | N | N | A | none |
| 112 | test_programme_queue.c | native_programme_queue | direct | custom selection atom :185 (CSS via `__FILE__` :1747 is portable) | D | N | B | `rt_selection_data_new` |
| 113 | test_programme_queue_scale.c | native_programme_queue_scale | run_rotation_editor_regressions.py | none | X | N | A | none |
| 114 | test_programme_ui.c | native_programme | run_programme_ui.py | none (capture via `g_file_get_contents` :232) | R | V | A | none |
| 115 | test_programme_view_header_ui.c | native_programme_view_header | run_rotation_editor_regressions.py | none | X | N | A | none |
| 116 | test_programme_view_settings_ui.c | native_programme_view_settings | run_rotation_editor_regressions.py | none | X | N | A | none |
| 117 | test_programme_window.c | programme_window | direct | none | N | N | A | none |
| 118 | test_prompt12_drop_ui.c | native_prompt12_drop | run_data_ui.py | system CLIPBOARD trick :~110-123 | R | N | B | `rt_selection_data_new` |
| 119 | test_pulse_hotplug.c | pulse_hotplug | direct | `<pulse/pulseaudio.h>` :10; fake libpulse :39-249; compiles `src/audio/pulse_backend.c` | N | N | D | exclude; new `test_wasapi_hotplug.c` with fake enumerator (§7.3) |
| 120 | test_recovery.c | recovery | direct | `fork`/`_exit`/`waitpid` :52-54,222-224; `flock`+`open(O_RDWR)` :214-216,225; `lstat`+0600 :59,124-125; `symlink("/tmp")` :173, `symlink("/etc/passwd")` :180; `<sys/wait.h> <sys/file.h>` :7-8 | N | N | C | `g_test_trap_subprocess_with_envp` for both child processes; `radios_platform_lock_fd`; `rt_assert_private_file`; link targets inside a fixture sibling dir (never real TEMP/system files) |
| 121 | test_rekordbox_ui.c | native_rekordbox | run_rekordbox_ui.py | none (USB = directory with PIONEER/…/export.pdb) | R | N | A | none |
| 122 | test_reset_ui.c | native_reset_ui | run_data_ui.py | none | R | N | A | none |
| 123 | test_rhythm.c | rhythm | direct | none | N | N | A | none |
| 124 | test_rhythm_upkeep.c | rhythm_upkeep | run_rhythm_upkeep.py | none (CLI driver) | N | N | A | none |
| 125 | test_rotation_editor_regressions.c | rotation_editor_regressions | run_rotation_editor_regressions.py | `-ffunction-sections -Wl,--gc-sections` subset link (CMake 610-615) fails on PE (F13) | X | N | B | Windows stub TU for the undefined symbols (or link radios_ui objects) |
| 126 | test_routing_graph.c | routing_graph | direct | none ("monitor" strings are data) | N | N | A | none |
| 127 | test_routing_graph_ui.c | native_routing_graph | run_routing_graph_ui.py | via #5 with native input | R | V | C | via #5 |
| 128 | test_routing_ui.c | native_routing | run_routing_ui.py | via #5 without native input | R | V | B | via #5 (0600 helper, device ids, gate helpers) |
| 129 | test_runner.c | runner | direct | none (env-gated `pulse` case :455-458) | N | E | A | none |
| 130 | test_runner_pool_preload.c | runner_pool_preload, runner_pool_preload_pcm | direct; run_audio_ui.py --engine-only | none | N | E/V | A | none |
| 131 | test_runtime.c | runtime | direct | none (`g_test_trap_subprocess` :305-311 is portable) | N | N | A | none |
| 132 | test_schema_settings_ui.c | schema_settings_ui | run_rotation_editor_regressions.py | none | X | N | A | none (check subset link, §4.6) |
| 133 | test_schema_track_picker.c | schema_track_picker | direct | none | N | N | A | none |
| 134 | test_schema_track_picker_ui.c | native_schema_track_picker | run_playout_ui.py | via base | R | V | A | none |
| 135 | test_settings_sync.c | settings_sync | run_rotation_editor_regressions.py | none | X | N | A | none |
| 136 | test_silence_detector.c | silence_detector | direct | none | N | N | A | none |
| 137 | test_silence_guard_ui.c | native_silence_guard | direct | none | D | N | A | none |
| 138 | test_song_seek.c | song_seek | direct | none | N | N | A | none |
| 139 | test_storage_locations.c | storage_locations | direct | `realpath` :31; POSIX path-syntax cases :61-66; `symlink` :93,182; 0600 :104,130; "a/b" components :50-52,101,153,181; `/yok/…` :187 | N | N | B | `rt_make_tmp_dir`; `#ifdef` Windows syntax cases (§10); helpers |
| 140 | test_stream_metadata.c | native_stream_metadata | run_stream_metadata_ui.py | pactl hotplug thread :787-808; `getpid` :852; device hotplug case :844-870 | R | V/P | C | drive hotplug through `radios_audio_virtual_sink_add/remove` (§7.3); rest via base |
| 141 | test_tempo.c | tempo | direct | `newlocale/uselocale/freelocale/LC_NUMERIC_MASK` :77-86 (cases :522-523) | N | N | B | `rt_numeric_locale_push/pop` |
| 142 | test_transfer_ui.c | native_transfer_ui | run_transfer_ui.py | none | R | N | A | none |
| 143 | test_transition.c | transition | direct | `struct stat`+`g_stat`+0600 :52; `g_chmod` :24 | N | N | B | `rt_assert_private_file`, `rt_file_size` |
| 144 | test_ui.c | native_controls | direct (WD `/tmp`) | none in code | D | N | A | CMake working dir (§4.4) |
| 145 | test_ui_preferences.c | ui_preferences | direct | `struct stat` :103; 0600 :105,115; `chmod 0644` :109; `st_ino` :116; `symlink` :465,495,511; IS_SYMLINK :18 and assertion :479; `g_chdir("/")` :548 (ok) | N | N | B | helpers (§10) |
| 146 | test_view_presets_ui.c | view_presets_ui | run_rotation_editor_regressions.py | none | X | N | A | none |
| 147 | test_weekly_programme_plan.c | weekly_programme_plan | direct | none ("Europe/Istanbul", "Europe/Berlin" DST via production :157-246) | N | N | A | none (platform tz) |
| 148 | test_window_now_playing.c | native_window_title_{current-hidden-programme-song-pause-stop-future-PCM, long-UTF8-bounded} | run_playout_ui.py | via base | R | V | A | none |
| 149 | test_workflow_hub_ui.c | native_workflow_hub | run_data_ui.py | none | R | N | A | none |

Counts (re-counted from this table): A 99, B 36, C 11, D 3 (= 149). Files needing a display: 96 (D 15, D/R 1,
X 18, R 62); no display: 53. Files needing owned virtual outputs or sound-server control in at least one CTest entry
(V, V/P, P, N/V, E/V): 50; files with only env-gated realtime cases (E, N(E)): 2; no audio devices: 97.

---------------------------------------------------------------------------------------------------

## 10. Detailed notes for B / C / D files (what to change, why, semantics to preserve)

### 10.1 Shared bases

**test_playout_ui.c (B)** — `capture_offset` :330-335 uses `struct stat`+`g_stat` (compile error F5) → `rt_file_size`.
`capture` :336-353, `capture_signal_span` :356-378, `tone_energy` reader :637-651 use `open(O_RDONLY)`, `fstat`,
`pread`, `close` → `rt_open`, `rt_fd_size(fd)` (a plain `fstat` into `struct stat` compiles on MinGW but its
`st_size` is a 32-bit `off_t`; use the 64-bit helper), `rt_pread`. Semantics: read float32 stereo frames from `offset` to the current end of a file another writer is
extending; frames are 8 bytes; assert `bytes % 8 == 0` — the virtual sink must write whole frames atomically
(append 8-byte multiples). Custom selection atom :835 → `rt_selection_data_new` (F17). `#define _POSIX_C_SOURCE`
:1 is harmless on MinGW (it enables `__USE_MINGW_ANSI_STDIO`). After this, all 22 includers are A.

**routing_patchbay_ui_test.inc (C)** — native-input mode (`RADIOS_ROUTING_NATIVE_INPUT 1`): `connect_nodes`
:96-128 drags from an output port to an input port with XTest (motion, button1 press, 20 motion steps, release) and
checks that the pointer is over our toplevel (`gdk_device_get_window_at_position` :111-113 — on Windows backend P
cannot move the real cursor; replace that assertion by checking the target HWND chosen by `rt_input_motion`);
`main` :543-549 saves the pointer and requires XTest, :564-566 restores it → `rt_input_available`,
`rt_input_restore_pointer`. Non-native mode keeps the programmatic connection. `finalized_frames` :169-177:
`struct stat`+`g_stat` and `st_mode & 0777 == 0600` → `rt_assert_private_file` (recorder WAVs must be owner-only on
Windows too). Device ids `"routing_original.monitor"` :136 and `"routing_microphone.monitor"` :146 are PulseAudio
monitor sources of runner-created sinks → take from env (`RADIOS_TEST_ROUTING_{ORIGINAL,MICROPHONE}_SOURCE`, runner
defines `<sink>.monitor` of virtual sinks). Gate files :230-265, :357-365 (`.routing-context-hold/-ready/-only-user`)
→ `rt_set_contents`/`rt_unlink`. The local SHOUTcast server :270-310 (`GSocketListener` on loopback) is portable.

**content_scale_test_input.h (C)** — keep the `cs_*` API used by the two tests; implement with `rt_input_*` and GDK
coordinates on Windows; delete `cs_eval*`/`cs_devices` and the Wayland branches under `#ifdef GDK_WINDOWING_WAYLAND`.
Popup frame on Windows = origin + allocation (`:126-133` X11 branch). Include `<gdk/gdkwin32.h>` only on WIN32.

**test_audio_ui.c (B)** — capture helpers :227-259 like the playout base; symlink :586 (proves a managed object
reached through a link is rejected) → `RT_SYMLINK_OR_SKIP`; gate `g_chmod` :717 harmless; write gate with
`rt_set_contents`.

**test_dj_ui.c (B)** — capture helpers :218-288; `file:///etc/passwd` :1942 is a rejected-URL literal (portable).

**test_playout.c (B)** — `database = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "build", "plan-fixture.sdb")`
:1810 yields `C:/Users/…/RadioS\build\plan-fixture.sdb`; `bind()` :17 builds `"%s.objects/%s.wav"`. The planner
(`src/playout/planner.c:308-311`) canonicalizes the managed path (backslashes, F3) and checks
`g_str_has_prefix(path, database_file ".objects" G_DIR_SEPARATOR)` against the **raw** database string ⇒ every
fixture media is rejected on Windows ("Medya yönetilen istasyon arşivinin dışında"). Test fix: canonical `database`
(`g_build_filename(rt_project_root(), "build", "plan-fixture.sdb")`) and `g_build_filename(objects_dir, name)`.
Production fix (core/playout area): canonicalize the root too and compare case-insensitively on Windows.
`"/tmp/operator-file.wav"` :83 stays a valid "outside" path (rooted on the current drive).

**test_automation_calendar_plan.c (B)** — same containment rule: `"/tmp/radios-calendar-plan.sdb"` :8 and :35;
replace by a canonical path under `g_get_tmp_dir()`; no file is created.

**test_automation_calendar.c (A, one optional line)** — `g_time_zone_new_identifier(preview_timezone)` :1437 must use
`rt_time_zone` once the platform tz exists (otherwise the preview assertion fails). `FcInit`/`FcConfigAppFontAddFile`
:3046 return TRUE on Windows (MSYS2 fontconfig, WOFF2 via FreeType+brotli), but GTK-win32 text uses Pango's
win32/DirectWrite backend, so the Inter font is not used — layout/screenshot expectations must follow whatever the UI
area chooses (e.g. bundling a TTF and `AddFontResourceExW(FR_PRIVATE)`), and first `FcInit` may take seconds (cache).

### 10.2 Core tests

**test_recovery.c (C)** — preserves: (1) a *different process* journals an operation and a fresh `RadioSRecovery`
reopens it byte-identical, file is owner-private (`:49-61`); (2) the GUI lifecycle lock: two GUI locks in one process
coexist (shared), an external exclusive non-blocking lock fails while any GUI lock is held, succeeds after both are
closed; while the exclusive lock is held `radios_recovery_gui_lock` fails with `G_IO_ERROR_BUSY`; a
`restore-pending.json` also blocks; a crashed child that took the lock and exited without closing does not leave the
lock held (`:208-227`). Windows: replace `fork()` children by `g_test_trap_subprocess_with_envp(NULL, envp, 5 s, 0)`
with the fixture root in `RADIOS_TEST_RECOVERY_ROOT`; the child branch (`if (g_test_subprocess())`) rebuilds the config,
does the work and leaves via `_exit()` (without closing the lock fd for the crash case); parent asserts
`g_test_trap_assert_passed()`. Replace `open(lock_path, O_RDWR)`+`flock(LOCK_EX|LOCK_NB)` by `rt_open` +
`radios_platform_lock_fd(fd, RADIOS_LOCK_EX|RADIOS_LOCK_NB)` (LockFileEx at the shared high offset, same byte range the
Python shim uses). After the child exits, poll the exclusive lock for up to 2 s (Windows releases locks of a terminated
process asynchronously). `lstat`+0600 :59,124 → `rt_assert_private_file`. Link cases: `symlink("/tmp", data/recovery)`
:173 and `symlink("/etc/passwd", entry)` :180 → targets must be a sibling directory/file created inside the fixture
(`rt_symlink`; never `g_get_tmp_dir()` — with Windows link-following `remove_tree` that would wipe the user’s temp
directory). `remove_tree` :19-26 → `rt_remove_tree`.

**test_data_client.c (B)** — `g_chmod(credential_file, 0644)` :346 must make `radios_data_client_new` fail with
`RADIOS_DATA_CLIENT_ERROR_CREDENTIALS`; on Windows `rt_make_world_readable` (Everyone:R ACE) and the production
privacy check must reject it; `rt_make_private` instead of `g_chmod(0600)` :230,349,352,364,369 (after
`g_file_set_contents` recreates the file, Windows ACLs are inherited from the directory — the fixture's config dir must
itself be made private first). Symlinked credentials :372 → `RT_SYMLINK_OR_SKIP` (file link). Invalid credential
`database_file` `"../RadioS.sdb"` :355 — add `"..\\RadioS.sdb"`, `"C:\\RadioS.sdb"`, `"\\\\srv\\s\\RadioS.sdb"`.
`g_socket_listener_add_any_inet_port` :236 binds all interfaces → Windows Defender Firewall prompt per new exe path;
bind `g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4)` port 0 instead (like the other servers).

**test_config.c (B)** — symlinks :301,311,333,338 (dir link outside root rejected; dir link inside accepted and
resolved; local `radios.ini` linked outside rejected `INVALID`; dangling relative link `missing.ini` gives `IO`) →
`rt_symlink` with `is_directory` TRUE for :301,:311, FALSE for :333,:338 (+skip if file links unsupported). Escape
literals :271 (`../escape`, `/tmp/radios-escape`, `../../outside/file`) and :286 (`/tmp/radios-config.ini`) still
exercise the rule on Windows; add `..\\escape`, `C:\\outside\\x`, `C:relative`, `\\\\srv\\share\\x`,
`data\\..\\..\\x`, `NUL`, `radios.ini:stream`. `test_defaults` compares `config.project_root` with the
`g_dir_make_tmp` root (8.3, F2) → create the fixture with `rt_make_tmp_dir`. Template `python_command=python3` is just
a non-empty string.

**test_ui_preferences.c (B)** — `roundtrip` :95-124: owner-private after save even if the previous file was
world-readable (`rt_make_world_readable` + `rt_assert_private_file`), and replaced atomically (new identity:
`rt_file_id` instead of `st_ino`), no temp leftovers (directory has exactly one entry — the Windows atomic replace must
not leave `*.tmp`/backup files). `file_links` :459-486 (`rt_symlink` file; assertion :479 → `rt_is_link`).
`directory_links` :488-519: absolute dir link outside rejected, relative dir link `"local config"` inside accepted
(junction fallback is absolute — acceptable, still inside). `g_chdir("/")` :548 works (drive root).

**test_preferences_storage.c (B)** — same pattern: `struct stat` :73,81 → `GStatBuf`/`rt_file_id`; 0600 :83;
`st_ino` :84 → identity; symlink :184 (file), :211 (dir); FIFO :196 under `#if RT_HAVE_FIFO` (on Windows add a
"directory named audio-panel.ini" case for "not a regular file"); `count == 2` :101 (no leftovers).

**test_local_ini.c (B)** — `lstat`+`S_ISREG`+0600 :93-95 → `rt_assert_private_file`; `entries_in_config()==1`
(no temp leftovers) :97,102; names :110-123 — add Windows-only invalid names: `"a\\b.ini"`, `"x.ini:ads"`,
`"con.ini"`, `"NUL"`, `"x.ini."`, `"x.ini "`; links :125-144 (`rt_symlink` file; :137 → `rt_is_link`);
`config_escape` :163-184 (dir link to an outside temp dir; teardown must not follow it → `rt_remove_tree`).

**test_storage_locations.c (B)** — `realpath(made)` :31 → `rt_make_tmp_dir`. `test_path_syntax` :58-71 is POSIX
syntax: keep under `#ifndef G_OS_WIN32`; on Windows accept `D:\Radyo\Kayıt`, `\\srv\share\Kayıt` (decide in core
area), reject `""`, `göreli`, `C:`, `C:göreli`, `\kök`, `C:\`, `C:\a\..\b`, `C:\a\\b`, `C:\a\`, `C:\a\tb`,
`C:\a:b`, `C:\NUL\x`. `test_directory_rules` :81-117: dir link :93 rejected with a message containing "sembolik"
(junctions too); marker 0600 :104 → helper. `test_settings_and_apply` :119-176: compares paths built with
`g_build_filename(root, "data/database/RadioS.sdb")` :50-52 against production results — build per component.
`test_sizes` :178-189: file link not followed in `radios_storage_tree_bytes` (`RT_SYMLINK_OR_SKIP`);
`radios_storage_free_bytes("/yok/böyle/bir/yer") == -1` stays valid.

**test_media_archive.c (B)** — `realpath` :34; file link in setup :49-50 is required by `test_scan`
(`skipped_links == 1` :162): if `rt_symlink` fails, expect 0 skipped links instead of skipping the whole test;
0600 :98; expected paths :166-173 built from `"sub/copy.flac"` → `g_build_filename(archive_a, "sub", "copy.flac")`.
Sorting by path is unchanged with `\`. Dot-files are hidden on Linux; on Windows also add a file with
`FILE_ATTRIBUTE_HIDDEN` and expect it skipped (if the core area adopts that rule).

**test_library_analysis.c (B)** — 0600 :98-99 → `rt_assert_private_file`; `remove_tree` :15-29 → `rt_remove_tree`.

**test_transition.c (B)** — `struct stat st; g_stat(path,&st)` :52 (compile error F5) → `rt_assert_private_file` +
`rt_file_size`.

**test_audio_decoder.c (B)** — preserves: decoder refuses symlinked file (`INVALID`) :250-252 and a path through a
symlinked directory (`IO`) :253-256 (on Windows: reparse points anywhere in the path — production no-follow open);
FIFO refused :257-259 (Windows: FIFO case compiled out; add "directory named x.wav"); `changed_file` :266-277: another
handle rewrites 2 bytes at offset 48 while the decoder is open ⇒ next read/seek fails with `CHANGED` — needs the
decoder to open media with `FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE` (cross-area), the test uses
`rt_open(O_WRONLY)` + `rt_pwrite`; `limits` :313-316: 2 GiB+1 sparse file via `rt_create_sized_file` (the `off_t`
cast overflows on Windows: `off_t` is 32-bit). `cancellation_during_work` :203-204 64 MiB file via
`rt_create_sized_file`. `transcode` :82-91 spawns `ffmpeg` (`libmp3lame flac libvorbis aac libopus` — present in MSYS2
ffmpeg); `"%s/encoded.%s"` mixed separators are fine for ffmpeg and GLib. Teardown asserts every unlink/rmdir
succeeds → decoder handles must be closed synchronously by `radios_audio_decoder_close`.

**test_media.c (B)** — as above plus: `private_staging` :176-188: staged import file must be private
(`rt_assert_private_file`) and `relative_path` uses `/` (`"data/içe aktar/"` :181 — keep `/` in stored relative paths
on Windows); `resolved == result->path` :185 → `rt_assert_path_equal`. `confinement` :221-236: import root through a
dir link must be refused (`CONFINEMENT`) and nothing written outside. `changed_source` :281-297: a mutator thread
`pwrite`s one byte every ms while `radios_media_analyze` hashes the file ⇒ `CHANGED` (sharing modes as above).
`cancellation_cleanup` polls with `g_usleep(100)` (≈2 ms with `timeBeginPeriod(1)`).

**test_tempo.c (B)** — `fractional_numeric_locale` :74-87 → `rt_numeric_locale_push("tr_TR.UTF-8")`, assert
`localeconv()->decimal_point` is "," for Turkish and "." for "C", then pop. Must fail (not skip) if the Turkish locale
is unavailable (UCRT always has `tr-TR`).

**test_output.c (C)** — rewrite `Server`/`connection_worker`/`server_worker`/`server_start/stop` :17-202 with
`GSocketListener` (loopback, port 0), one `GThread` per accepted `GSocketConnection`, `g_socket_set_timeout(…,10)`
instead of `SO_RCVTIMEO`, `g_socket_shutdown(…, FALSE, TRUE)` for `SHUT_WR`, `g_socket_receive/send` (no SIGPIPE on
Windows, `MSG_NOSIGNAL` unnecessary). Preserve: request header capture (PUT, no `Transfer-Encoding`, no
`Content-Length`), metadata GET URL `/admin/metadata?mode=updinfo&mount=%2Flive.mp3&song=%C3%87alan%20%26%20eser%20%2F%2012`,
403 rejection after reading the body, early vs late 200, 1 MiB cap. `recording` :88-118 (no overwrite of an existing
WAV) and `validation` are portable.

**test_patchbay.c (B)** — `live_run`/`icecast_headers` are portable (GSocket servers, offline engine, render
thread); patchbay server literal `"unix:/nonexistent/radios-patchbay-test"` :149,214 means "never touch host audio" →
`RT_AUDIO_SERVER_NONE`. `pulse_devices` :240-331 (PulseAudio null sinks, pacat client, "virtual interface" sink +
remap-source microphone, `move_application` of another client's stream, `kill(SIGTERM)`) is Linux-only: wrap in
`#ifndef G_OS_WIN32`. Windows has no user-mode way to create system-visible virtual devices or move another process's
stream; if the Windows patchbay offers process-loopback capture (`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`) add a
Windows case that captures a helper process playing a tone.

**test_audio_soak.c (C)** — `rss_bytes` :28-37 → `rt_process_resident_bytes` (WorkingSetSize; keep
`SOAK_RSS_ALLOWANCE`); `fd_count` :38-43 → `rt_process_handle_count` — Windows handle counts include thread-pool,
event and WASAPI handles: allowance `fd_max <= fd_start + 12`, `fd_end <= fd_start + 6` (:165) must be re-measured
(e.g. +64/+16). `radios_audio_engine_new(TRUE, getenv("RADIOS_TEST_PULSE_SERVER"))` :87 → `"virtual:"` with sinks
`radios_soak_main/radios_soak_cue` created by the runner env. No `g_test_init` here: the constructor of §4.1 provides
the no-popup behaviour.

**test_midi.c (C)** — see §8; `test_keys` :57-66 is pure.

**test_header_ui.c (B)** — on Linux `-Wl,--wrap=g_get_real_time` redirects calls in `src/ui/header_status.c` (compiled
into the test) and in radios_core objects. Windows: add `tests/header_status_clock.c`:
`#include <glib.h>` / `gint64 radios_test_header_real_time(void);` / `#define g_get_real_time radios_test_header_real_time`
/ `#include "../src/ui/header_status.c"`; compile it instead of `src/ui/header_status.c`, rename
`__wrap_g_get_real_time` :95 to `radios_test_header_real_time`, drop the link option on WIN32. Verify no radios_core
object used by the test needs the fake clock. The station-timezone case :513-532 needs the platform IANA tz.

**test_rotation_editor_regressions.c (B)** — the test already stubs `radios_data_client_acquire`, `radios_data_*`,
`radios_json_*`, `radios_audio_ui_get_channel_label` :23-67; on WIN32 add the remaining undefined symbols (reported by
the first PE link) to a stub TU — they are unreachable from the tested paths; behaviour unchanged.

**header_layout_driver.c (B)** — `symlink(config.assets_root, temporary/assets)` :233 → `rt_symlink(…, TRUE)` (junction
fallback fine) or copy the assets tree; cleanup :365 `g_remove(assets_link)` removes the link only (verified for
`g_rmdir`). Exact window size check (header mode :304-307) needs `rt_screen_fits`.

**application_shutdown_driver.c (B)** — selects capture device `"routing_microphone.monitor"` :81 (a monitor source
of a sink created by the Python test) → env-provided virtual source. The Python driver sends SIGTERM/SIGINT
(`test_application_shutdown.py:121-122,205`) to `src/app/main.c` (`g_unix_signal_add` :55-56); on Windows the app
needs an equivalent graceful request (console Ctrl handler / `WM_CLOSE`/`WM_ENDSESSION` / named event) and the Python
side must use it — app/py areas.

### 10.3 GUI tests with B edits

* `test_admin_ui.c`: :548 `rt_assert_private_file(file)` for the person's private key written by the Python service
  (Windows: Python shim must create it owner-only); :233-275 hold files → `rt_set_contents`/`rt_unlink`; token copy
  :335-337 reads the system clipboard (production `src/ui/admin_ui.c:348`) → serialize via `RESOURCE_LOCK`. Security
  note for UI area: secrets copied to the Windows clipboard enter Clipboard History/cloud sync unless the
  `ExcludeClipboardContentFromMonitorProcessing`/`CanIncludeInClipboardHistory=0` formats are added.
* `test_advert_ui.c` :360-371, `test_dj_ui.c` :218-288, `test_player_mapping_ui.c` :188-192: capture helpers.
* `test_data_ui.c` :452 private key file; :626-636 `service_command` spawns `config.python_command
  tools/radios_database.py --project-root <root> stop|start` — stays (requires the Python service stop/start to work
  on Windows and `gspawn-win64-helper*.exe`).
* `test_broadcast_catalogue_ui.c` :235,547, `test_programme_queue.c` :185, `test_playlist_redesign_ui.c` :239,
  `test_playout_ui.c` :835 (custom atoms → fatal `Gdk-WARNING` on Windows), `test_media_drop_ui.c` :185 and
  `test_prompt12_drop_ui.c` :119 (CLIPBOARD → real system clipboard, races between parallel tests) →
  `rt_selection_data_new` (PRIMARY on Windows). `drag-data-received` emission with a NULL context stays.
* `test_media_drop_ui.c` URI cases :630-689: `g_filename_to_uri` gives `file:///C:/…`. `"file://remote.invalid/music.wav"`
  must be rejected *without filesystem access*: on Windows `g_filename_from_uri` turns a host into a UNC path
  (`\\remote.invalid\music.wav`), so production must reject any non-empty/non-`localhost` hostname before opening
  (otherwise an SMB connection — NTLM credential leak, multi-second stall). `"file:///first.wav"` maps to
  `\first.wav` (rooted, not found) — still a rejection.
* `test_autoplay_header_ui.c` :37, `test_header_playout_ui.c` :113: `rt_time_zone(radios_data_client_station_timezone(...))`.

### 10.4 D files and their replacements

* **test_pulse_hotplug.c** → `test_wasapi_hotplug.c` (§7.3). Keep the original for Linux builds.
* **test_midi_hotplug.c** → `test_midi_hotplug_win32.c` (§8). Keep the original for Linux builds.
* **test_audio_recovery.c** → `test_wasapi_recovery.c`: (1) `private-server-restart`: play deck 0 + microphone gate on;
  `radios_audio_virtual_server_stop()` ⇒ disconnected, buses not ready, `loss_epoch` ≥1, mic gate closed, deck paused at
  its confirmed cursor, quiet for 1 s; restart ⇒ ready, still paused until explicit Play; runner case: stop ⇒ runner
  PAUSED with the same epoch; `restart(keep_sinks=FALSE)` ⇒ connected, bus not ready with an error containing
  "fallback" or the Windows equivalent, `runner_resume` fails `UNAVAILABLE`; stop+restart with sinks ⇒ resume succeeds;
  receipts terminal NONE then INTERRUPTED after stop. (2) `device-hotplug-keeps-playing`: 6 add/remove cycles of
  `radios_hotplug_<pid>_<n>` while MAIN plays; 50 ms poll gap <500 ms, snapshot <20 ms, PCM stall <500 ms, each device
  appears without refresh and disappears again. (3) `flush-keeps-other-deck-continuous`: two tones on one sink, seek and
  stop deck 1 ⇒ ≥1 flush, deck 0 audible >3000 ms, longest gap ≤12000 frames (strict 480 with
  `RADIOS_TEST_FLUSH_GAP_STRICT=1`), measured from the virtual capture file. Use `getpid()` (available in MinGW) or
  `GetCurrentProcessId()`.

---------------------------------------------------------------------------------------------------

## 11. New Windows-only tests to add (cover Windows semantics the Linux suite cannot)

1. Path rules: drive-relative, rooted-without-drive, UNC and `\\?\` prefixes, reserved device names, ADS `:`,
   trailing dot/space, case-insensitive aliasing (`X.INI` vs `x.ini`), 8.3 aliases (`SONATT~1`), mixed separators — for
   config, local_ini, storage_locations, media confinement, planner containment.
2. Reparse points: directory junctions (not only symlinks) rejected everywhere symlinks are; `rt_remove_tree` never
   follows them.
3. Sharing violations: replace/unlink while another handle (no `FILE_SHARE_DELETE`) is open ⇒ production retries and
   then fails cleanly; Defender-like transient opens.
4. ACL privacy: files created by production are owner-only; inherited permissive ACL on the parent is not accepted.
5. Graceful shutdown of the app and of SDBASE through the Windows mechanism chosen (named event / WM_CLOSE).
6. WASAPI fake enumerator hotplug and recovery (§7.3); WinMM fake (§8).
7. IANA time zones through the platform layer, including DST edges of `Europe/Berlin` 2026-03-29 and `Europe/Istanbul`.

---------------------------------------------------------------------------------------------------

## 12. Risks

* `g_file_test(IS_SYMLINK)`/`g_lstat` behaviour (F7) also affects production security checks (config, local_ini,
  preferences, storage, recovery, media). The C tests will catch it only after their own `remove_tree` is fixed —
  a naive port can recursively delete the target of a test link (e.g. `%TEMP%`).
* Private-desktop input (backend P) bypasses the OS input pipeline; OLE drag-and-drop and some grab paths may need
  backend S on the interactive desktop (serialised, CI-only).
* Geometry tests depend on monitor size/DPI (F19, GDK scale); content-scale UI asked for 2560×1600 on Xvfb.
* `G_DEBUG=fatal-warnings` may surface Windows-only GLib/GTK warnings (selection atoms, UWP app-info scanning when GIO app
  info APIs run, icon theme). Fix the cause; only as last resort install a narrow writer filter in the test runtime TU.
* Timing assertions (5 ms heartbeats, <20 ms snapshots, 3.3 ms bursts) are fragile on Windows even with
  `timeBeginPeriod(1)` (≈2 ms granularity).
* Without the virtual audio backend 50 GUI/audio test files cannot run their PCM entries; with real hardware they
  would be non-deterministic.
* Python runners (Xvfb, pactl/parec, signals, `os.pipe`/`select` on fds, `resource.setrlimit`) must be ported
  before any R/X test runs (tests-py area).
* Tests compare against the Turkish comma locale implicitly on Turkish Windows; CI on an English Windows behaves
  differently from the developer machine. Pin `LANG`/`LC_ALL` per CTest job if a difference appears.

---------------------------------------------------------------------------------------------------

## 13. Platform API needs (C, `include/radios/platform.h`) used by the test layer

```c
gboolean radios_platform_replace_file(const gchar *path, const void *data, gsize length, gboolean owner_only, GError **error);
gint     radios_platform_unlink(const gchar *path);                       /* retries sharing violations ≤2 s */
gboolean radios_platform_path_is_private(const gchar *path, GError **error);
gboolean radios_platform_make_private(const gchar *path, GError **error);
gboolean radios_platform_is_link(const gchar *path);                      /* symlink or junction, never follows */
gchar   *radios_platform_real_path(const gchar *path, GError **error);    /* GetFinalPathNameByHandleW, \\?\ stripped */
gboolean radios_platform_file_identity(const gchar *path, gint fd, RadioSFileIdentity *out); /* volume+FileId+size+mtime */
gint     radios_platform_lock_fd(gint fd, guint flags);                   /* RADIOS_LOCK_SH|EX|NB|UN; LockFileEx at the shared high offset */
gint64   radios_platform_disk_free(const gchar *path); gint64 radios_platform_disk_total(const gchar *path);
gboolean radios_platform_path_equal(const gchar *a, const gchar *b);
GTimeZone *radios_platform_time_zone_new(const gchar *iana);              /* IANA ids on Windows; NULL if invalid */
gboolean radios_platform_time_zone_valid(const gchar *iana);
```
