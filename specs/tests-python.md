# Area spec: tests-python (Windows 10/11 x64 port)

Analyzed copy: `C:\Users\sonatturkan\Desktop\RadioS` (read-only phase, nothing modified). Every `file:line`
below points into that copy. Binding inputs: `specs/DECISIONS.md` (D1–D7) and `specs/PLATFORM_FACTS.md`; where a
sibling spec disagrees with DECISIONS, DECISIONS wins and the conflict is listed in §9.

Scope (98 files): `tests/*.py` (74: 33 `run_*.py` runners, `rekordbox_device_fixture.py`, 40 `test_*.py`),
`tests/README.md`, `database/SDBASE/tools/*.py` (13), `tools/otomasyon_testleri/*` (7 `.py` + `build.sh`, `e2e.c`,
`README.md`).

---------------------------------------------------------------------------------------------------

## 0. Summary

### 0.1 What is Linux-specific here
The Python layer is the *fixture* half of the whole acceptance suite. Backend tests (`test_desktop_*.py` & co.) are
in-process SDBASE tests and are mostly portable; everything Linux-specific concentrates in the runners:
* **Audio**: `pactl load-module module-null-sink` + `parec` captures (run_audio_ui.py:22-25,129-145,
  run_preferences_output.py:51-64), private `pulseaudio`/PipeWire daemons with `setsid`, `flatpak-spawn`,
  `/proc/<pid>/fd/<dirfd>/native` socket aliases and a fake D-Bus address (run_audio_recovery.py,
  run_audio_soak.py, run_audio_routing.py, run_routing_ui.py, run_routing_graph_ui.py,
  test_application_shutdown.py), `pacat` tone players, `pactl --format=json list sink-inputs`, host Pulse probing
  with `os.getuid()` (run_routing_host.py:30).
* **Display**: `Xvfb -displayfd` + `select()` on a pipe + `pass_fds` + `/run/host` ld.so trick
  (run_rotation_editor_regressions.py, run_content_scale_ui.py), private `dbus-daemon`, `resource.setrlimit`,
  `DISPLAY/WAYLAND_DISPLAY` checks, `GDK_BACKEND=x11` (runners + ~64 CTest ENVIRONMENT entries).
* **Process/IPC**: SIGTERM/SIGINT/SIGKILL (`send_signal`, `os.kill`), `/proc` scans by cmdline, `/proc/<pid>/exe`,
  `os.pidfd_open` + `signal.pidfd_send_signal`, `fcntl.flock` probes of `gui.lock`/`server.lock`, `multiprocessing`
  + `fcntl` (SDBASE tools).
* **Files**: mode assertions `S_IMODE == 0o600/0o700`, `chmod(0o644)` "public" credentials, file/dir symlinks,
  `os.mkfifo`, `os.O_DIRECTORY`, text-mode/encoding-less `read_text()/write_text()`, `os.open` without `O_BINARY`.
* **Packaging/launch**: `run-radios.sh`, ELF/glibc/readelf checks, tar modes, flatpak SDK (`radios_sdk.py`).
* **Hidden Windows traps found in "portable" code**: `Path(argv[1]).name != "test_audio_ui"` (run_audio_ui.py:221,
  run_dj_ui.py:20 — `.exe` suffix silently disables per-case isolation), Windows paths with backslashes written into
  `radios.ini` (GKeyFile treats `\m`,`\U`… as invalid escapes → C config load fails), POSIX path literals fed to
  SDBASE validators (`/music/...`, `/media/USB`, `/tmp/radios`), `Popen.kill()` returncode is `1` not negative
  (failover_acceptance.py:166), SIGTERM-graceful worker exit code expected 0 (stream_acceptance.py:315),
  urllib + Windows registry proxy for `127.0.0.1`, `TemporaryDirectory` cleanup while a handle is still open.

### 0.2 Recommended Windows design (one paragraph)
Windows-only (D1). Keep every runner's *contract with its C test executable* (argv, cwd, env names, gate files,
proof files, TAP validation, exit code 77) and replace only the fixture mechanics: (1) a new test support module
`tests/radios_testkit.py` (+ `tests/pcm_analysis.py`) built on the canonical Python shim (`sdbase/_platform.py`,
loaded by path) gives environment building, private SDBASE installations, gate injection, GLib/TAP case runs,
a kill-on-close **Job Object** for every fixture, graceful stop through `Local\RadioS-stop-<pid>`, `LockFileEx`
lock probes, ACL privacy assertions, junction/symlink helpers and retrying cleanup; (2) audio comes from the audio
area's **in-process null provider** (`RADIOS_AUDIO_SERVER=null:<json>`, audio-backend.md §5.9) whose virtual sinks
write the same growing float32 capture files that `parec` produced (`RADIOS_TEST_<BUS>_CAPTURE`), with
`<sink>.monitor` loopback sources, tone "applications" for routing and a JSON-driven "server running" switch for
recovery; real WASAPI is an opt-in, labelled acceptance (loopback capture of an inaudible endpoint); (3) GUI runs on a
**private Win32 desktop** (tests-c.md §6.3) instead of Xvfb, gated by an interactive-window-station check
(exit 77 otherwise); (4) all Python runs with `<root>\runtime\python\python.exe -X utf8 -B` (D7), children via
`python_child_argv` (`-E -s -B -X utf8`), every byte-relevant text write uses `encoding="utf-8", newline="\n"` (D6).

### 0.3 Counts per category (all files in scope)

| Group | As is | Small edits | Rewrite part | Exclude (+replacement) | Total |
|---|---|---|---|---|---|
| tests/run_*.py + rekordbox_device_fixture.py | 9 | 14 | 10 | 1 (`run_routing_host.py` → opt-in WASAPI probe) | 34 |
| tests/test_*.py | 22 | 13 | 5 | 0 | 40 |
| tests/README.md | – | 1 | – | – | 1 |
| database/SDBASE/tools/*.py | 3 | 9 | 1 | 0 | 13 |
| tools/otomasyon_testleri (7 .py, build.sh, e2e.c, README) | 4 (isaretler, lib, senaryolar, e2e.c) | 5 (api, e2e.py, senaryolar_724, senaryolar_kume, README) | – | 1 (`build.sh` → CMake target) | 10 |
| **Total** | **38** | **42** | **16** | **2** | **98** |

"As is" = no source edit needed in that file; it still depends on the base fixtures, the global rules of §2 and the
sibling areas (SDBASE/tools/audio/C) being ported. Partial deletions inside kept files (flatpak branches, SDK test,
`--host-runtime`) are listed per file.

---------------------------------------------------------------------------------------------------

## 1. How the Python test layer works today (contracts to preserve)

### 1.1 Families
1. **In-process backend tests** (`test_desktop_*.py`, `test_playlist_*.py`, `test_playout_*.py`,
   `test_*_schema.py`, `test_data_playlists.py`, `test_gui_backend_coverage.py`): `sys.path.insert(SDBASE/src)`,
   `open_radio_store(build/<tmp>/test.sdb)`, `bootstrap`, `authenticate`, workflow calls, optional in-process
   `BoundedHTTPServer(("127.0.0.1",0), make_handler(store))`. Shared fixtures: `test_desktop_api.DesktopAPI`
   (test_desktop_api.py:64-74), `test_desktop_media.DesktopMedia` (test_desktop_media.py:42-78),
   `test_desktop_automation_calendar.DesktopAutomationCalendar`, `test_desktop_playout.DesktopPlayout`,
   `test_database_setup.DatabaseSetupTests` (CLI + real service, test_database_setup.py:60-221).
2. **Tool/lifecycle tests**: `test_database_setup.py`, `test_database_recovery.py`, `test_reliability.py`,
   `test_storage_locations.py`, `test_packaging.py`, `test_radios_launch.py`, `test_release_install.py`,
   `run_release_acceptance.py` — import `tools/*.py` in-process and/or run their CLIs.
3. **GUI acceptance runners with a private SDBASE installation**: base runners `run_data_ui.main`,
   `run_admin_ui.main`, `run_audio_ui.main` (the latter adds owned audio outputs); derived runners monkeypatch
   `base.shutil.copytree` (to inject HTTP gates into the *copied* `http_server.py`) and `base.subprocess.run` (to
   seed the store after `init`), or call `base.main(collect_glib_cases=..., expected_glib_case=...)` to run every
   GLib case in a fresh installation.
4. **Audio runners with private sound servers**: run_audio_recovery, run_audio_soak, run_audio_routing (helpers
   only), run_preferences_output, run_routing_ui, run_routing_graph_ui, run_routing_host, test_application_shutdown.
5. **Private display runners**: run_rotation_editor_regressions (16 CTest uses), run_content_scale_ui.
6. **SDBASE acceptance tools** (not in CTest): multi-process stores, crash points (`os._exit(N)`), HTTP/TLS workers.
7. **Automation e2e** (`tools/otomasyon_testleri`): operator scripts against the live service + a C driver;
   `tests/run_automation_stress.py` reuses them in a private in-process server (CTest `automation_recovery_stress`).

### 1.2 Derivation graph (who patches whom)
```
run_data_ui.main ── run_native_data_ui, run_automation_calendar_ui, run_automation_probability_ui (subprocess),
                    CTest direct (media_drop, workflow_hub, media_rights, playout_policy, reset, Pdf12, MediaArchive)
run_admin_ui.main ── run_emergency_transfer_ui (copytree+run patch, seed), run_catalogue_playlist_ui (same),
                     run_broadcast_catalogue_ui (copytree+run patch, uses run_emergency_transfer_ui.seed), CTest direct
run_audio_ui.main ── run_playout_ui (copytree patch: quota+prepare gate) ── run_stream_metadata_ui (subprocess)
                  ├─ run_advert_ui, run_programme_ui, run_broadcast_ui (copytree patch)
                  ├─ run_dj_ui, run_player_mapping_ui (case loops), run_header_navigation_ui (subprocess)
                  └─ run_routing_ui imports run_audio_ui.post
run_audio_routing (helpers PROJECT/terminate/tone_file/tone_amplitudes) ── run_routing_ui, run_routing_graph_ui,
                  run_routing_host, test_application_shutdown
test_release_install (Installation, environment, packaging) ── run_release_acceptance
test_desktop_rekordbox.XML + rekordbox_device_fixture ── run_rekordbox_ui
```
**Seam rule for the port**: keep the module-level `shutil.copytree(...)`/`subprocess.run(...)` calls in the three
base runners (call sites, argument shapes `command + ["init", ...]`, `PROJECT / "database/SDBASE/src"` as source)
so the seven monkeypatching derived runners keep working unchanged.

### 1.3 Runner ⇄ C executable contract (unchanged on Windows; tests-c.md §3.3 is the C mirror)
| Item | Producer (Python) | Notes for Windows |
|---|---|---|
| `argv = [exe, <private root>, *GLib args]`, `cwd=<root>`; `--engine-only` drops the root (run_audio_ui.py:39-41,159) | all GUI runners | root contains spaces + Turkish letters → UTF-8 argv needs the test-exe manifest (D4) |
| `application_shutdown`: `[exe, "--project-root", root, "--profile", "routing"]`, `cwd=directory` (test_application_shutdown.py:100) | | |
| `audio_soak`: `[exe, root, seconds]`; `rhythm_upkeep`: `[exe, "analyze", file]` / `[exe, "scan", tmp, db, url, workers]` | | |
| exit 0 = pass, 77 = skip (CTest `SKIP_RETURN_CODE 77`), else fail | | keep; Windows abort() = 3, crashes = NTSTATUS (≥0xC0000000) |
| GLib TAP: `1..N`, `ok N /path`, `not ok`, `-l` lists `/path` or `# /path` (run_audio_ui.py:170-183, run_data_ui.py:156-168, run_automation_probability_ui.py:33-38) | | universal-newline decoding handles `\r\n`; keep `plans == [1] / ["1..1"]` checks |
| env `G_DEBUG=fatal-warnings`, `PYTHONDONTWRITEBYTECODE=1`; pop `RADIOS_SDBASE_TOKEN`, `PYTHONPATH` (run_audio_ui.py:56-60 and every base) | | add Windows GUI env (§3.2) |
| audio env: `RADIOS_TEST_{MAIN,CUE,MONITOR}_SINK` = `radios_gui_<hex>_{main,cue,monitor}`, `RADIOS_TEST_{BUS}_CAPTURE`, `RADIOS_TEST_MIC_SOURCE=<monitor>.monitor` (run_audio_ui.py:127-154); `RADIOS_TEST_OUTPUT_{SINK,CAPTURE,PROOF}` (run_preferences_output.py:56-60); `RADIOS_TEST_PULSE_SERVER` (soak/recovery/routing) | | names kept; `RADIOS_TEST_PULSE_SERVER` → `RADIOS_TEST_AUDIO_SERVER` (audio-backend.md:381) |
| capture file = raw float32le interleaved stereo at sink rate, growing in real time incl. silence, readers take `stat().st_size` as offset (test_playout_ui.c:330-378) | parec today | null provider writes it (§3.8) |
| audio preferences document posted before the window exists (run_audio_ui.py:148-158) | `POST /v1/workflows/desktop/put` kind `audio_preferences` | unchanged; device ids are free text ≤512, no control chars (desktop_api.py:695-698) |
| routing env `RADIOS_TEST_ROUTING_APPLICATION` (Pulse sink-input index), `RADIOS_TEST_ROUTING_ORIGINAL_SINK` (sink index), `RADIOS_TEST_ROUTING_UNRELATED`, `RADIOS_TEST_ROUTING_PROOF_DIR`, `RADIOS_TEST_ROUTING_STREAM_URL`, `RADIOS_ROUTING_GRAPH_CAPTURE_DIR`, `RADIOS_ROUTING_VIEWER_TOKEN` | run_routing_ui.py:118-188, run_routing_graph_ui.py:162-167 | Windows: application = fixture **pid**, original sink = **name** (audio-routing-capture.md §12.1) |
| tokens: `RADIOS_TEST_VIEW_TOKEN` (run_admin_ui.py:103), `RADIOS_TEST_CONNECTION_BAD_TOKEN/_VIEWER_TOKEN` (run_data_connection_ui.py:38,131) | | unchanged |
| HTTP fixtures: `RADIOS_TEST_STREAM_URL/_PASSWORD/_CAPTURE`, `RADIOS_TEST_METADATA_CAPTURE` (run_broadcast_ui.py:69-74, run_audio_soak.py:196), `RADIOS_TEST_ACCESS_URL/_CA` (run_external_access.py:55-56) | stdlib servers on `127.0.0.1:0` | unchanged |
| shutdown driver: `RADIOS_SHUTDOWN_READY` (JSON `{active, frames, path}`), `RADIOS_SHUTDOWN_RECORD` (test_application_shutdown.py:94-119) | | unchanged |
| marker protocols: `stop-N/stopped-N/restart-{1,missing,3}/restarted-*` (run_audio_recovery.py:113-123,212-224); `stop-server/server-stopped/restart-server/server-restarted` (run_audio_routing.py:225-245) | | recovery moves in-process (audio-backend.md §6.2); routing uses fixture control (§3.8) |
| optional captures: `RADIOS_SHOT_DIR`, `RADIOS_*_CAPTURE` (≈25 names), `RADIOS_TEST_USB_EXPORT` | | unchanged |

### 1.4 Runner ⇄ SDBASE CLI contract (`tools/radios_database.py --project-root <root> [--config] <cmd>`)
* `init --station-name <tr text>` → stdout one JSON object; runners require `schema_version == 4`,
  `endpoint_status == "offline"` (run_audio_ui.py:115-121, run_data_ui.py:132-140); test_database_setup requires
  UUID fields `store_id,user_id,station_id,endpoint_id` (L142-149).
* `start` → `{"state":"running","pid":…, "already_running":bool, "restarted":bool, "source_digest":hex64}`
  (run_data_ui.py:144-148, test_database_setup.py:355-380); `status` → `{"state": running|stopped|foreign|unhealthy,
  "pid"?, "stale_database"?}`; `check` → `{"status":"ok","pid","sequence","integrity":"ok",…}`
  (test_application_shutdown.py:160-162,226-239); `stop` → `{"state":"stopped","stopped":bool}`.
* Exit status ≠ 0 + non-empty stderr on refusal; stdout/stderr never contain `api_token`/`endpoint_token`
  (test_database_setup.py:105-117, test_release_install.py:85-97).
* Files: `config/credentials.json` (private), `data/database-service/service.json`, `data/logs/database-service.log`
  with `"(GET|POST) <path> HTTP/1.1" <code>` lines (run_data_ui.py:169-176, run_release_acceptance.py:125-139).
* Windows (tools spec): `service.json` gains `stop_event`/`platform`/`boot_time`; identity = pid + creation ticks;
  `stop` sets the stop event (D6 name) — the JSON shapes above stay.

### 1.5 Gate-injection contract with `database/SDBASE/src/sdbase/http_server.py`
Runners copy SDBASE and string-replace anchors in the **copy** (never the source):
* `"                result = self._dispatch(method, path, query, principal)\n"` must occur exactly once
  (run_audio_ui.py:82-92, run_data_ui.py:47-109, run_advert_ui.py:20-35, run_broadcast_ui.py:84-104,
  run_playout_ui.py:29-54, run_programme_ui.py:15-26, run_catalogue_playlist_ui.py:56-80,
  run_broadcast_catalogue_ui.py:67-79, run_emergency_transfer_ui.py:72-84, run_rekordbox_ui.py:42-61,
  run_routing_ui.py:75-91, run_data_connection_ui.py:60-72).
* `"    limits = RequestLimiter()\n"` exactly once (run_data_ui.py:104-108);
  `"capacity=120, refill_per_second=10"` exactly twice (run_playout_ui.py:25-28).
* `tools/radios_reliability.py`: `"from __future__ import annotations\n"` exactly once (run_admin_ui.py:47-62).
* Injected code uses names in scope: `method, path, query, principal, result, store, self.connection,
  self.close_connection`, plus module globals `StoreError`, `WriteRejected`, `RequestLimiter`.
* Flag files live in `store.root` (= `data/database/`): `.acceptance-*-hold/-ready/-fail/-drop/-fail-once/
  -unavailable`, counters `.acceptance-document-read-count-<kind>`, `.routing-context-*`, `.catalogue-*`,
  `.selection-*`, `.transfer-*`, `.data-put-*`, `.acceptance-tool-*` (root of project).
**Windows requirements**: (a) SDBASE's port must keep these anchors byte-identical (incl. indentation and LF);
(b) the gate installer must be newline-agnostic (normalise `\r\n`→`\n` on read, write UTF-8 with `newline="\n"`)
because a CRLF checkout (`core.autocrlf=true`) would otherwise make every anchor count 0; (c) gates must only `stat`
flag files inside wait loops (deleting a file another process has open without `FILE_SHARE_DELETE` fails on
Windows — tests-c.md F8); read flag contents once before waiting and release the handle immediately.

---------------------------------------------------------------------------------------------------

## 2. Global Windows rules for every Python test file

R1 **Interpreter** (D7, PLATFORM_FACTS): run all Python tests and runners with
`<root>\runtime\python\python.exe -X utf8 -B` (3.14.8, tzdata in `Lib\tzdata`, no `._pth`). Never `python`,
`python3`, `py` (3.15 / Store alias on PATH). Child Python processes: `_platform.python_child_argv(...)`
= `[sys.executable, "-E", "-s", "-B", "-X", "utf8", ...]` and `_platform.python_child_env()`. A runner preflight
asserts `os.name == "nt"`, `sys.flags.utf8_mode == 1`, `zoneinfo.ZoneInfo("Europe/Istanbul")` loads (fail fast
with a clear message instead of 200 timezone failures).

R2 **`-E` consequences**: `PYTHONPATH` is ignored by product-style children. Replace PYTHONPATH-based `-c` children
(test_database_setup.py:151-158, test_release_install.py:126-142, inherited by test_data_playlists.py:62-68) with
`[*python_child_argv("-c", PROGRAM), src_dir, db]` where PROGRAM starts with
`import sys; sys.path.insert(0, sys.argv[1])`. Self-contained acceptance tools that do not pass `-E`
(failover_acceptance.py:72-76, recovery_acceptance.py:120-123) keep working.

R3 **Encoding & newlines** (D6, PLATFORM_FACTS): every `read_text/write_text/open(…,"w")/json.dump` that feeds C,
hashes, manifests, INI or JSON compared byte-wise gets `encoding="utf-8"` and (writes) `newline="\n"`. 64 call sites
in tests/*.py lack `encoding` (list in §4 per file); with `-X utf8` they decode correctly but still write CRLF.
`subprocess.run(text=True)` decodes children as UTF-8 only in UTF-8 mode — keep `-X utf8` in CTest commands.

R4 **`os.open` text mode** (PLATFORM_FACTS: default `_fmode` text): any `os.open` in tests must use the shim
(`_platform.open_fd`) or add `os.O_BINARY` (run_transfer_ui.py:166).

R5 **INI values read by C GKeyFile**: `g_key_file_get_string` (config.c:176) unescapes `\s \n \t \r \\` and *rejects*
any other `\x` → a raw `C:\msys64\...\python.exe` in `radios.ini` makes the C config load fail. Runners write
`python_command = Path(sys.executable).as_posix()` (forward slashes; accepted by Windows APIs, D4 "existing absolute
path"). `storage.ini` paths must stay backslash form (storage rule 5.5-3 rejects `/`) and therefore be written
GKeyFile-escaped (`value.replace("\\", "\\\\")`), exactly like `g_key_file_set_string` does.

R6 **Executable names**: CTest passes `$<TARGET_FILE:…>` = `…\test_x.exe`. Compare with `Path(p).stem`
(run_audio_ui.py:221, run_dj_ui.py:20); build paths with `.exe` (tools/otomasyon_testleri/e2e.py:8,
senaryolar_kume.py:431). Product exe = `RadioS.exe` (D4).

R7 **Paths**: never feed POSIX absolute literals to *local-path* validators (they become root-relative on Windows);
use real temp paths or `testkit.fake_abs("music","Albüm")` → `C:\music\Albüm`. Keep POSIX literals only where the
value is foreign provenance text (mAirList CSV `file_path`, rekordbox `Location` URIs, Icecast mounts). Compare
paths with `_platform.path_equal` (case-insensitive, separator-agnostic) and resolve temp roots
(`Path(...).resolve()`): `%TEMP%` is an 8.3 path here (`C:\Users\SONATT~1\…`, tests-c F2). Keep fixture names short:
the deepest SDBASE paths (`…\data\recovery\<uuid>\<uuid>\<uuid>.json`, object store `…\RadioS.sdb.objects\ab\<sha256>`)
approach `MAX_PATH`; manifests are `longPathAware` but stdlib/CRT paths still prefer < 240 UTF-16 units.

R8 **Privacy assertions**: replace `S_IMODE(st_mode) == 0o600/0o700` and `st_mode & 0o777` with
`testkit.assert_private(path)` / `assert_private_dir(path)` = `_platform.is_private` + `owned_by_current_user`
(core-posix-files.md §5.2 SDDL/rule). Files the *test* creates and the product later validates as private
(credentials, outbox, markers) are created with `_platform.open_fd(..., private=True)` / `mkdir_private` /
`make_private` — this machine's Desktop leaks a `CodexSandboxUsers` read ACE into `build\` (tools spec §0.6).
"Public" counterpart: `testkit.make_public(path)` adds an allow-read ACE for `S-1-5-32-545` (Users).

R9 **Links**: file/dir symlinks need SeCreateSymbolicLinkPrivilege or Developer Mode (works on this machine,
tests-c F6, may fail on CI) → `testkit.symlink_or_skip(test, link, target, directory=False)`; every *directory*
link scenario gets a junction variant (`_winapi.CreateJunction(target, link)`, unprivileged, the realistic Windows
attack). `Path.is_symlink()` is False for junctions: use `_platform.is_link` (symlink OR name-surrogate reparse
point). `os.mkfifo` does not exist: replace FIFO cases by "directory named `x.wav`" and "reserved device name"
cases. Hard links (`os.link`) work on NTFS unprivileged.

R10 **Locks**: `fcntl.flock` → `_platform.lock(fd, shared=…, blocking=…)` (LockFileEx 1 byte at
`0x4000000000000000`, D6). A probe opens the lock file `"a+b"`, tries `EX|NB`, maps `BlockingIOError` → held.
After a process exits, Windows releases its locks asynchronously: lock-released assertions poll ≤ 2 s.

R11 **Signals**: `Popen.terminate()/send_signal(SIGTERM)/os.kill(pid,15)` = `TerminateProcess` (exit code 1 resp.
15, no handlers, no cleanup); `signal.SIGKILL` does not exist; `send_signal(SIGINT)` raises `ValueError`. Graceful
stop = `_platform.request_stop(pid)` (`OpenEventW(EVENT_MODIFY_STATE)` + `SetEvent` on `Local\RadioS-stop-<pid>`),
GUI alternatives `WM_CLOSE` and `WM_QUERYENDSESSION/WM_ENDSESSION(ENDSESSION_CLOSEAPP)` (ui-platform-c.md §3.3).
Assertions on killed-process return codes must use `!= 0`.

R12 **Process containment**: every runner creates a kill-on-close Job Object (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION`, **no** `BREAKAWAY_OK`) and assigns itself at start, so the GUI exe, the
SDBASE service (tools' `CREATE_BREAKAWAY_FROM_JOB` attempt fails with `ERROR_ACCESS_DENIED` and is retried without
it, D3/tools §4.5) and helpers die with the runner (also on CTest timeout). `/proc` cmdline scans
(test_database_setup.py:195-221, test_release_install.py:193-199) become `stop` CLI + `_platform.iter_processes()`
matching argv + `TerminateProcess` as last resort.

R13 **Cleanup**: `TemporaryDirectory.cleanup()`/`shutil.rmtree` fail on Windows while any handle is open (service log,
lock files, HTTP handler threads writing capture files, a child still running). Order in every `finally`: stop CLI →
graceful stop/terminate children → join server threads (incl. daemon handler threads) → `_platform.rmtree` with the
2 s sharing-violation retry budget → report leftovers instead of raising over the real test result.

R14 **Sockets**: never bind `0.0.0.0` (firewall prompt blocks unattended runs). Foreign-listener tests rely on
SDBASE **not** setting `SO_REUSEADDR` on Windows (`BoundedHTTPServer` inherits `allow_reuse_address=True` from
`http.server.HTTPServer`; on Windows it lets a second socket hijack a listening port) — cross-area SDBASE fix.
Use `http.client` or `testkit.no_proxy_opener()` (`urllib.request.build_opener(ProxyHandler({}))`) for loopback:
`urllib` reads the WinINet registry proxy and does not bypass `127.0.0.1` by default.

R15 **GUI environment** (gtk-windows-runtime.md §15.1): no `GDK_BACKEND=x11` (Win32 GTK fails, tests-c F15);
`GSETTINGS_BACKEND=memory` (else HKCU registry writes); `GTK_IM_MODULE=gtk-im-context-simple`; private
`XDG_CONFIG_HOME`/`XDG_DATA_HOME`/`XDG_CACHE_HOME` (keeps `recently-used.xbel` out of `%LOCALAPPDATA%`); remove
`GTK_THEME, GTK_CSD, PANGOCAIRO_BACKEND, GTK_PATH, GTK_MODULES`; `GDK_SCALE=1` unless a test varies it; drop
`NO_AT_BRIDGE/GTK_A11Y`, Xvfb, dbus-daemon. Interactive check: `GetProcessWindowStation` +
`GetUserObjectInformationW(UOI_FLAGS)` → `WSF_VISIBLE`; else exit 77.

R16 **Crash dialogs**: runners call `SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|
SEM_NOOPENFILEERRORBOX)` (inherited by children) in addition to the job's `DIE_ON_UNHANDLED_EXCEPTION`; C test exes
link the tests-c runtime TU (`WerSetFlags`, `_set_abort_behavior`).

R17 **DLL search**: test exes need `C:\msys64\ucrt64\bin` (or the staged `runtime\bin`) *before* `C:\MinGW\bin` in
PATH (PLATFORM_FACTS: incompatible runtime DLLs). Runners must not reorder PATH; CTest sets it (§6).

R18 **Timing**: Windows process start + Defender scanning of freshly copied SDBASE trees per case; keep CTest
timeouts but honour `RADIOS_TEST_TIMEOUT_SCALE` (default 1.0) in runner-level timeouts
(`test_timeout=100/150/180/240`, `timeout=20/25/30/45/75/90/100`); recommend Defender exclusion of `build\` on CI.

---------------------------------------------------------------------------------------------------

## 3. Windows fixture design

### 3.1 New support modules (tests/)
| Module | Content |
|---|---|
| `tests/radios_testkit.py` | environment (§3.2), `PrivateInstallation` helpers (§3.3), `install_gate` (§3.4), `run_glib_cases` (§3.5), `KillOnCloseJob`, `request_stop`, `post_wm_close`, `end_session`, `iter_fixture_processes`, `gui_lock_held`, `assert_private[_dir]`, `make_public`, `symlink_or_skip`, `make_junction`, `fake_abs`, `exe`, `scaled`, `has_interactive_desktop`, `private_desktop_env`, `no_proxy_opener`, `rmtree_quietly`, `suppress_error_dialogs`, `preflight()` |
| `tests/pcm_analysis.py` | moved from run_audio_routing.py:26-79 (`terminate`, `tone_file`, `tone_amplitudes`) and run_audio_soak.py:90-121 (`pcm_statistics`), run_preferences_output.py:24-33 (`amplitude`) — pure stdlib (`array`, `wave`, `math`) |
| `tests/radios_audio_fixture.py` | `NullAudioServer` writer of the null-provider spec (§3.8), `WasapiLoopback` (opt-in, §3.9) |
| `tests/radios_test_platform.py` (optional alias) | name used by tools spec §6.1; may simply re-export the testkit helpers |

The canonical shim is loaded **once by file path** (python-sdbase-engine.md §4.3):
`importlib.util.spec_from_file_location("radios_sdbase_platform", PROJECT/"database/SDBASE/src/sdbase/_platform.py")`
— tests never re-implement lock offsets, SDDL or event names.

### 3.2 Environment builder `testkit.base_environment(kind, tmp)`
* start from `os.environ.copy()` (Windows: keys upper-cased); pop `RADIOS_SDBASE_TOKEN`, `PYTHONPATH`,
  `PYTHONHOME`, `RADIOS_TEST_VIEW_TOKEN` (test_release_install.py:35-41 adds it);
* set `PYTHONDONTWRITEBYTECODE=1`, `PYTHONUTF8=1`, `PYTHONIOENCODING=utf-8`, `G_DEBUG=fatal-warnings`;
* `kind="gui"`: R15 variables, private XDG dirs under `tmp`, optional `RADIOS_TEST_PRIVATE_DESKTOP=1` (§3.7);
* `kind="audio"`: + null provider variables (§3.8); every GUI test *without* owned sinks gets
  `RADIOS_AUDIO_SERVER=none:` (tests-c.md §4.3: tests must never open the real speakers);
* never touches PATH (R17).

### 3.3 Private SDBASE installation (shared by run_audio_ui/run_data_ui/run_admin_ui/run_rekordbox_ui/
run_data_connection_ui/run_routing_ui/run_routing_graph_ui/test_application_shutdown/test_database_setup)
Steps (today's semantics, Windows mechanics):
1. `root = build/<prefix>-XXXX/<"RadioS Türkçe …">` via `TemporaryDirectory(dir=PROJECT/"build")`
   (prefix ≤ 20 chars, R7); resolve it.
2. Copy `CMakeLists.txt`, `assets`, `database/SDBASE/src` (ignore `__pycache__`, `*.pyc`),
   `database/SDBASE/pyproject.toml`, `config/radios.example.ini` — **module-level `shutil.copytree` call kept** (seam).
   (`CMakeLists.txt` + `config/radios.example.ini` + `database/SDBASE/{pyproject.toml,src}` also satisfy the exe's
   root discovery rule, D4/PLATFORM_FACTS.)
3. Copy tools: today `tools/radios_database*.py` (run_audio_ui.py:94, run_data_ui.py:111, run_rekordbox_ui.py:72,
   run_data_connection_ui.py:74, run_routing_ui.py:93, run_routing_graph_ui.py:74, test_application_shutdown.py:136,
   test_database_setup.py:78) or `radios_*.py` (run_admin_ui.py:41) or three named files (test_reliability.py:50).
   **Windows: also copy the tools shim (`tools/radios_platform.py` or whatever the tools area names it)** — otherwise
   the private CLI fails with `ImportError` before printing JSON. Centralise as `testkit.TOOLS_RUNTIME_FILES`
   provided/validated by the tools area.
4. Config: `ConfigParser(interpolation=None)` from the example; set `service_url=http://127.0.0.1:<free port>`,
   `python_command=Path(sys.executable).as_posix()` (R5), `connect_timeout_seconds=1`,
   `request_timeout_seconds=3|5|8`, optional `[application] initial_profile`; write with
   `open(..., "w", encoding="utf-8", newline="\n")`; create `[paths]` dirs (database_file → parent).
5. `init --station-name …` and `start` through `python_child_argv(root/"tools/radios_database.py",
   "--project-root", root, …)` with `cwd=root`, `capture_output=True`, `text=True` (UTF-8), timeouts scaled; validate
   JSON as §1.4.
6. Run the native exe (inside the job; private desktop if GUI); on failure print the last 30 HTTP lines of
   `data/logs/database-service.log` (run_data_ui.py:169-176) — open the log with share-read (the service holds it
   `FILE_APPEND_DATA` with share R/W/D, tools spec §4.5).
7. `finally`: `stop` (timeout 15) → R13 cleanup.

### 3.4 Gate installer `testkit.install_gate(path, anchor, code, *, count=1, replace=None)`
`text = path.read_bytes().decode("utf-8").replace("\r\n", "\n")`; require `text.count(anchor) == count`
(RuntimeError with the runner's existing message otherwise); insert after the anchor (or replace);
`path.write_bytes(text.encode("utf-8"))`. Gate bodies stay byte-identical to today (they are platform-neutral:
`time.monotonic`, `Path.exists/is_file/touch(mode=)`, `write_text`, `socket.SHUT_RDWR`). Recommended hardening for
Windows (same semantics): gates read `.acceptance-*` content once before their wait loop (already so in
run_data_ui.py:51,71) and write ready/counter files with `encoding="utf-8"`.

### 3.5 GLib case runner `testkit.run_glib_cases(main, argv0, exe, *, prefix=None, expect_count=None)`
Generalises run_audio_ui.py:215-245, run_dj_ui.py:18-42, run_native_data_ui.py:11-44,
run_automation_calendar_ui.py:11-45, run_player_mapping_ui.py:10-36, run_playout_ui.py:59-80: discover with `-l`
(and `-p <prefix>`), require a positive plan equal to the listed count, unique names, common prefix; run each case in
a fresh fixture with `expected_glib_case=case` (plan `[1]`, outcomes `["ok 1 <case>"]`). Parse lines after
`rstrip("\r")`. Fix R6 at the call sites that gate on the executable name.

### 3.6 Processes
* `KillOnCloseJob.adopt_current_process()`: `CreateJobObjectW(NULL,NULL)`;
  `SetInformationJobObject(h, JobObjectExtendedLimitInformation=9, JOBOBJECT_EXTENDED_LIMIT_INFORMATION{
  BasicLimitInformation.LimitFlags = 0x2000 /*KILL_ON_JOB_CLOSE*/ | 0x400 /*DIE_ON_UNHANDLED_EXCEPTION*/})`;
  `AssignProcessToJobObject(h, GetCurrentProcess())`; keep the handle for the runner's lifetime (closing it kills the
  runner itself). Nested jobs are fine on Windows 8+.
* `request_stop(pid) -> bool` = shim; `post_wm_close(pid) -> int` = `EnumWindows` + `GetWindowThreadProcessId` +
  `IsWindowVisible` + `PostMessageW(hwnd, WM_CLOSE=0x10, 0, 0)`; `end_session(pid)` =
  `SendMessageTimeoutW(hwnd, WM_QUERYENDSESSION=0x11, 0, ENDSESSION_CLOSEAPP=1, SMTO_ABORTIFHUNG, 5000)` then
  `WM_ENDSESSION=0x16, wParam=1, lParam=1` (ui-platform-c.md §15).
* `wait_lock_released(path, timeout=2.0)` (R10).
* `iter_fixture_processes(root)`: `_platform.iter_processes()` (Toolhelp32 + `NtQueryInformationProcess(60)` argv,
  tools spec §4.3) filtered by `str(root)` in argv and (`"serve"` in argv or image == root exe).

### 3.7 Display: private desktop instead of Xvfb
* Default for GUI runners: **private desktop** `WinSta0\RadioSTest-<pid>` (tests-c.md §6.3, F16: GTK maps, focuses,
  renders; posted mouse/keyboard messages work; `SendInput` does not). Two delivery options — pick one with the
  tests-c owner:
  1. **In-process (recommended for the Python side)**: the C test runtime TU switches the thread desktop before
     `gtk_init` when `RADIOS_TEST_PRIVATE_DESKTOP=1` (`CreateDesktopW` + `SetThreadDesktop`). Runners only set the
     env var; argv stays `[exe, root, …]`, so the `subprocess.run` monkeypatch in run_broadcast_catalogue_ui.py:82-102
     (`arguments[0] == executable`) keeps recognising native runs.
  2. **Launcher exe** `radios-test-desktop.exe CMDLINE` (tests-c.md §6.3): runners prepend it from
     `RADIOS_TEST_DESKTOP_LAUNCHER`; then run_broadcast_catalogue_ui.py:83 must test `executable in arguments`.
* Tests that need real OS input (`SendInput`, OLE DnD) run on the interactive desktop under CTest
  `RESOURCE_LOCK native_input`; runner sets nothing.
* No 2560×1600 virtual screen: windows larger than the monitor are clamped (tests-c F19); content-scale cases need an
  explicit window size or skip (77) when `GetSystemMetrics(SM_CXVIRTUALSCREEN/SM_CYVIRTUALSCREEN)` is too small.

### 3.8 Audio: null provider fixture (replaces pactl/parec/pacat/private pulseaudio)
Options considered: (a) virtual audio driver (VB-Cable/SysVAD) — needs a signed kernel driver, not shippable in CI,
mixes system sounds; (b) WASAPI loopback of the real default endpoint — only one physical endpoint here
(PLATFORM_FACTS), cannot separate MAIN/CUE/MONITOR, plays tones on the operator's speakers, system sounds leak in,
no packets while nothing renders; (c) **in-process null provider behind the same backend interface** (audio-backend.md
§5.9) — deterministic, per-bus, CI-safe. **Choice: (c) for every default test; (b) only as opt-in smoke (§3.9).**

Contract the runner writes (union of audio-backend.md §5.9 and audio-routing-capture.md §12.1; env name to be
unified by the lead, §9):
```json
{ "schema": 1, "running": true, "clock": "realtime",
  "sinks": [ { "name": "radios_gui_<hex>_main", "description": "RadioS sanal MAIN", "rate": 48000, "channels": 2,
               "capture": "C:\\…\\capture-main.f32", "capture_format": "f32le" } ],
  "sources": [ { "name": "radios_virtual_mic", "description": "Sanal mikrofon", "tone_hz": 880, "amplitude": 0.5 } ],
  "applications": [ { "pid": 41001, "start_time": 1, "name": "Routing Fixture Selected",
                      "binary": "fixture-selected.exe", "endpoint": "routing_original",
                      "tone": { "hz": 440, "amplitude": 0.2 } } ],
  "default_sink": "…", "default_source": "…" }
```
Env: `RADIOS_AUDIO_SERVER=null:<abs json path>` (process-wide engine selection; Windows analogue of `PULSE_SERVER`),
`RADIOS_TEST_AUDIO_SERVER=<same>` for C tests that pass an explicit server (soak/recovery/patchbay), plus the
unchanged `RADIOS_TEST_*` names of §1.3.

Semantics the tests rely on (must be implemented by the audio area, verified by `test_null_provider.c`):
* sink clock runs from server creation to process exit at real-time pace (waitable timer + QPC, no drift) and
  appends the mixed block — **silence when idle** — to `capture` (`FILE_APPEND_DATA`, share R/W/D, one write per
  tick, visible size growth ≤ 10 ms): this is exactly the parec monitor timeline that `capture_offset()`/`run_for()`/
  `capture_signal_span()` assume (test_playout_ui.c:330-378);
* `capture_format` `f32le` (default) or `s16le` (audio_soak's `monitor.pcm`, run_audio_soak.py:192-194);
* `<sink>.monitor` = loopback of the sink mix (`RADIOS_TEST_MIC_SOURCE=<monitor>.monitor`, run_audio_ui.py:150-154;
  `routing_microphone.monitor`, application_shutdown_driver.c:81);
* applications = synthetic tone players attached to an endpoint (replace `pacat` at run_audio_routing.py:189-198,
  run_routing_ui.py:164-175, run_routing_graph_ui.py:140-151, test_application_shutdown.py:198-204); each endpoint
  mix = its applications + RadioS streams; process-loopback of `pid` = that application only;
* `"running": false` rewrite ⇒ every stream fails like a killed sound server, `true` ⇒ devices return
  (`restart`); add/remove sinks by rewriting the file (runner: write temp + `_platform.replace`, retry); routing
  spec's control files (`stop-server`, `restart-server`, `exit-<pid>`) are the alternative — see §9 conflict 1;
* sink rates other than 48000 allowed (run_preferences_output 44100);
* runner pre-creates (truncates) every capture file so `g_stat` succeeds before the first tick.

`radios_audio_fixture.NullAudioServer` API: `add_sink(name, *, rate=48000, capture=None, fmt="f32le")`,
`add_tone_source(name, hz, amplitude)`, `add_application(pid, name, endpoint, hz, amplitude, binary=…)`,
`set_defaults(sink, source)`, `write()`, `set_running(bool)`, `exit_application(pid)`, `environment() -> dict`,
`captures() -> dict[bus, Path]`. Warm-up constants (`time.sleep(2.3)`, run_routing_ui.py:193,
run_routing_graph_ui.py:172) become `fixture.warmup_seconds = 0.0` (tones exist from tick 0); keep the report field.

### 3.9 Opt-in real-WASAPI acceptance (label `real-audio`, not in default CTest)
* Requires `RADIOS_TEST_WASAPI_ENDPOINT=<render endpoint id>` of an **inaudible** device (virtual cable) — never the
  operator's speakers (audio-routing-capture.md §12.3); exit 77 otherwise.
* Independent capture = small C helper (WASAPI loopback, owned by tests-c/audio): event/poll 10 ms, convert mix
  format to f32le 48 k stereo, write zeros for `AUDCLNT_BUFFERFLAGS_SILENT` and for timeline gaps (QPC positions),
  keep a **silent keep-alive render stream** on the endpoint so loopback delivers packets while nothing plays
  (audio-routing-capture.md:427). Pitfalls: system sounds share the mix (use distinctive frequencies + thresholds),
  APO enhancements/spatial sound alter PCM (disable on the test endpoint), exclusive-mode apps block, device format
  changes mid-run.
* Python runner `tests/run_wasapi_acceptance.py` (new): tone players = C helper `tests/win/tone_player.exe` (or
  `winsound.PlaySound(SND_ASYNC|SND_LOOP)` child, stdlib) on the endpoint; RadioS MAIN on the endpoint; asserts the
  MAIN-only isolation numbers of run_routing_ui (440 ≥ 0.05, 997 ≤ 0.02) and soak RMS > 0.01; defaults/volumes
  unchanged (helper prints default endpoint ids for eRender/eCapture × roles before/after).

### 3.10 Acceptance assertions that change on Windows
| Linux assertion (file:line) | Windows assertion |
|---|---|
| independent `parec` monitor capture of each owned sink (run_audio_ui.py:138-145) | null-provider capture written below the engine's backend interface (same bytes the WASAPI render client would receive); independence downgrade documented; opt-in loopback (§3.9) covers the real path |
| "Virtual output capture stopped unexpectedly" (run_audio_ui.py:184-185) | removed (no capture processes); optional: capture file size > 0 when a test reported playback |
| owned module ids unloaded (run_audio_ui.py:201-206, run_preferences_output.py:111-113, run_routing_ui.py:216-219) | nothing to unload (devices die with the process); fixture JSON deleted with the temp dir |
| `pactl info` reachable (run_audio_ui.py:68, run_preferences_output.py:46) | `fixture.write()` succeeded; provider presence is asserted by the C side |
| `restored_sinks == baseline_sinks` via `pactl list sink-inputs` (run_routing_graph_ui.py:204-210) | asserted by the C test: fixture application bindings released, no host routing API called (Windows never moves apps) |
| `pactl list source-outputs` empty after signal shutdown (test_application_shutdown.py:224-225) | app process handle signalled + `gui.lock` released within 2 s + WAV finalized (already asserted) |
| SIGTERM and SIGINT both exit 0 (test_application_shutdown.py:121-127,205) | three graceful paths each exit 0: named stop event, `WM_CLOSE`, `WM_QUERYENDSESSION/WM_ENDSESSION`; `TerminateProcess` is a negative control (WAV may be unfinalized, recovery path) |
| WAV `st_mode & 0o777 == 0o600` (test_application_shutdown.py:219) | `assert_private(path)` |
| credentials `S_IMODE == 0o600` (test_database_setup.py:228, test_database_recovery.py:57, test_release_install.py:287-288, run_release_acceptance.py:167,221, test_reliability.py:124, test_desktop_admin.py:294-295, test_desktop_api.py:869, test_desktop_transfers.py:175-176, test_desktop_transfer_package.py:436) | `assert_private` / `assert_private_dir` |
| `/proc/<pid>/exe == build/radios` (test_release_install.py:168) | `QueryFullProcessImageNameW(pid)` `path_equal` the expected `RadioS.exe` |
| killed child `returncode < 0` (failover_acceptance.py:166) | `returncode != 0` |
| follower exits 0 on SIGTERM (stream_acceptance.py:314-315) | follower exits 0 after `request_stop(pid)` |
| "error while loading shared libraries" (run_release_acceptance.py:152-154) | exit codes `0xC0000135` (DLL not found), `0xC0000139` (entry point), `0xC000007B` (bad image) = packaging failure, never a fallback |
| `\x7fELF` + readelf/GLIBC (test_packaging.py:252-276) | PE: `MZ`, `PE\0\0`, machine `0x8664`, DLL closure via `objdump -p`/`ntldd` (tools spec §5.5) |
| `.sh` mode 0o755 in archives (test_packaging.py:121-124,260) | windows kind is a zip: no exec bits; the tamper test alters a member's content/size or adds a symlink external attribute instead |
| no DISPLAY ⇒ launch fails before init (test_release_install.py:263-268) | no Windows counterpart in Python (a non-interactive window station cannot be simulated cheaply); moved to a C unit test of the interactive-session check (ui-platform area) |
| Pulse null sink start-up silence → 2.3 s warm-up | not needed (null provider clocks start with the tones) |

---------------------------------------------------------------------------------------------------

## 4. Per-file analysis

Legend: **A** as is · **S** small edits · **R** rewrite part · **X** exclude (+ replacement).
"Inherits" = depends on base-runner/global changes only.

### 4.1 tests/run_*.py and helpers

**run_audio_ui.py — R** (base of ~25 CTest targets: native_audio, audio_devices, header_navigation_realtime,
native_player_workspace, native_automation_workspace_acceptance, native_live_layout_connected, native_general_*,
native_mixer_auto_volume, native_loudness_policy_*, runner_pool_preload_pcm, + derived runners)
* Does: private SDBASE copy + "media GET hold" gate (L77-92), init/start (L114-125), three owned null sinks + parec
  captures (L127-145), posts audio preferences (L146-158), runs exe (`--engine-only` without root) (L159-163),
  optional TAP validation (L160-183), per-case isolation CLI (L215-245).
* Linux-only: `pactl()` (L22-25), `shutil.which("pactl"/"parec")` (L45-48), `DISPLAY` check (L49-51),
  `pactl info/load-module/unload-module` (L68,131-135,203-206), `parec` Popen (L142-145), capture-alive check
  (L184-185), `python_command = sys.executable` with backslashes (L105, R5), `.name != "test_audio_ui"` (L221, R6),
  urllib `post()` (L28-34, R14).
* Windows: replace L45-51 with `testkit.preflight()` + `has_interactive_desktop()` (GUI mode only); replace
  L127-145 with `NullAudioServer`: `unique = "radios_gui_" + uuid4().hex`, sinks `_main/_cue/_monitor` at 48000 with
  `capture-<bus>.f32` in root, env `RADIOS_AUDIO_SERVER/RADIOS_TEST_AUDIO_SERVER`, `RADIOS_TEST_<BUS>_SINK`,
  `RADIOS_TEST_<BUS>_CAPTURE`, `RADIOS_TEST_MIC_SOURCE=<unique>_monitor.monitor`; keep the preferences POST verbatim
  (L148-158) through `no_proxy_opener`; delete capture-process bookkeeping (L62-63, L184-185, L191-206); job + GUI
  env + private desktop; `.stem`; keep `main(*, test_timeout, collect_glib_cases, expected_glib_case)` signature and
  the module-level `shutil.copytree`/`subprocess.run` calls (seams for 6 derived runners).

**run_data_ui.py — S** (base of run_native_data_ui, run_automation_calendar_ui, run_automation_probability_ui and 9
direct CTest entries)
* Does: private copy with LIST hold/document read hold+counter/import drop/playlist-drop gates (L41-109), init/start
  (L131-148), exe with TAP validation (L149-168), HTTP log tail on failure (L169-176), stop (L178-180).
* Linux-only: none intrinsic; Windows traps: `python_command` (L121, R5), gate `.read_text()` of counter (L66; fine
  with `-X utf8`), cleanup with a live service handle (R13).
* Windows: shared env/job/private desktop/R5/R13; gate installer (newline-agnostic); keep everything else.

**run_admin_ui.py — S** (native_admin, native_import_profiles, native_external_access; base of three runners)
* Does: copies `tools/radios_*.py` (L41-42; also copies the Windows tools shim automatically), injects a launch
  barrier into `radios_reliability.py` after `from __future__ import annotations\n` (L43-62), creates a view-only
  user in-process via `radios_database_setup` imported from the private copy (L91-103), start (L104-108), exe
  timeout 150 (L109-111).
* Windows: R5 (L72), env/job/desktop; the barrier writes `.acceptance-tool-ready` with `write_text("ready")` (L55) →
  add `encoding="utf-8"`; the in-process tools import needs the shim to import cleanly on Windows (cross-area).

**run_advert_ui.py — A** (native_advert): patches `run_audio_ui.shutil.copytree` (L12-42) to add prepare/run-start
gates (L21-32); `touch(mode=0o600)` is harmless; inherits base.

**run_audio_recovery.py — R** (audio_recovery)
* Does: chooses `pulseaudio` binary (`RADIOS_TEST_PULSEAUDIO_BIN`, PATH, `build/stage13-pulse`) or PipeWire via
  `flatpak-spawn` (L15-28,141-154); PipeWire path: `env -i … setsid … &` through `sh -c`, `kill -0`, `kill -9`, own
  XDG dirs in `~/.cache` (L31-139); Pulse path: `O_DIRECTORY` fd + `/proc/<pid>/fd/<fd>/native` socket alias
  (L161-170), fake `DBUS_SESSION_BUS_ADDRESS` (L173), `private.pa` with `module-native-protocol-unix` + null sinks
  (L178-201), marker loop stop-N/restart-{1,missing,3} (L207-229).
* Windows (audio-backend.md §6.2): the restart choreography moves **into** `test_audio_recovery.c` through the
  null provider API (`radios_audio_null_set_running`, add/remove sink). The runner becomes: preflight, job,
  `RADIOS_TEST_AUDIO_SERVER=null:` (in-memory), run `[exe, root]` with a private temp dir (still `argv[1]` for
  proof/marker files the C test writes), timeout 90 → return code. Delete PipeWire/flatpak/pulseaudio code (D1).
  Skip-77 path stays only for "provider not compiled".

**run_audio_routing.py — R** (no CTest entry; `main()` targets a removed executable — `engine-proof.json` is now
written only by test_audio_soak.c:180, confirmed by grep)
* Helpers `terminate` (L26-33), `tone_file` (L36-43), `tone_amplitudes` (L46-79) are portable → move to
  `tests/pcm_analysis.py`; keep `run_audio_routing.py` as a 5-line re-export (imported by run_routing_ui.py:19,
  run_routing_graph_ui.py:20, run_routing_host.py:16, test_application_shutdown.py:26) or update the importers.
* Delete `main()` (L82-307: pulseaudio daemon L88-94,154-183, `chmod 0o700` L102, `O_DIRECTORY`+`/proc` L106-107,
  pacat L184-198, `pactl --format=json list sink-inputs` L199-213) per D1 (no native target exists).

**run_audio_soak.py — R** (audio_soak, label for the 300 s soak)
* Does: private pulseaudio (L143-150,164-191), `parec` s16le monitor capture (L192-194), raw-socket MP3 receiver
  (L32-87), runs `[exe, root, seconds]` (L197-199), checks finalized WAV vs `engine-proof.json` (L205-210), PCM stats
  (L211-217), receiver bytes == proof (L219-220), `ffprobe`/`ffmpeg` decode (L221-232), report (L252-258),
  SIGTERM handler (L262-264), `os.chmod(root, 0o700)` (L154), `O_DIRECTORY`/`/proc` (L166-168).
* Windows: null spec with `radios_soak_main` (`capture=root/"monitor.pcm"`, `capture_format="s16le"`) and
  `radios_soak_cue`; env `RADIOS_TEST_AUDIO_SERVER`; keep Receiver, statistics, MP3 checks (MSYS2 `ffmpeg.exe`,
  `ffprobe.exe` from PATH, else 77); delete the SIGTERM handler (job object guarantees cleanup on CTest timeout);
  replace chmod with `mkdir_private`; `read_text()` at L202,205,236,254 → `encoding="utf-8"`; the `Receiver` thread
  must be closed and joined before `shutil.rmtree(root)` (L255-256, R13).

**run_automation_calendar_ui.py — A**: case-loop wrapper over `run_data_ui.main` (L11-45); inherits.

**run_automation_probability_ui.py — A**: runs `[sys.executable, tests/run_data_ui.py, binary, "-p", case]` per fixed
case (L26-38); child inherits `PYTHONUTF8=1` from CTest env; inherits.

**run_automation_stress.py — S** (automation_recovery_stress)
* Does: compiles `tools/otomasyon_testleri/e2e.c` with `pkg-config` (incl. `libpulse`), `cc`, `-Wl,--start-group`
  against `build/libradios_{playout,core,audio}.a` (L35-44); in-process SDBASE server (L73-83); fake `api` module
  (L100-104); 13 scenarios (L126-167); results JSON (L190).
* Windows: replace `compile_driver` by a CMake target `otomasyon_e2e` (tools spec §0.11) and a `--driver` argument
  (`$<TARGET_FILE:otomasyon_e2e>`); `e2e.E2E = str(driver)` (L109) then has `.exe`; results `write_text(...,
  encoding="utf-8", newline="\n")` (L190). WAV fixtures via `wave` (portable). `TemporaryDirectory` cleanup after
  the in-process server is shut down (already in `finally`, L170-172).

**run_broadcast_catalogue_ui.py — S** (native_broadcast_catalogue): gate read/write without encoding (L66, L79 →
UTF-8, LF); native-run detection `arguments[0] == executable` (L83) must stay valid (§3.7 option 1) or become
`executable in arguments`; seeding imports private tools in-process (L16-48, L98-101); inherits.

**run_broadcast_ui.py — S** (native_broadcast, native_broadcast_automatic_metadata_filter): HTTP mount fixture
(L25-63) + env via `os.environ` (L69-76) + gates (L85-101). Windows: daemon handler threads may still hold
`stream.mp3` open when `TemporaryDirectory` exits (L19, L117-119) → track handler threads (or a per-server list of
open files) and close/join before cleanup; otherwise portable.

**run_catalogue_playlist_ui.py — S** (Prompt10 playlist): `server.read_text()` without encoding (L55); seed writes
8 kHz WAVs and `chmod(0o600)` (L26-44, harmless); inherits.

**run_content_scale_ui.py — R** (content_scale_ui)
* Linux-only: `binary()` with `/run/host/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2` (L13-22), Xvfb + dbus-daemon
  required (L29-32), `resource.setrlimit(RLIMIT_CORE)` (L33), `chmod(0o700)` of a TemporaryDirectory (L34-37),
  `os.pipe` + `pass_fds` + `-displayfd` + `select.select` on a pipe (L50-63; `select` works only on sockets on
  Windows), `DISPLAY` (L64), private D-Bus session config (L65-79), `GDK_BACKEND=x11`, `PULSE_SERVER=unix:…`
  (L38-48).
* Windows: `testkit.gui_run(cmd, timeout=90)` = interactive-desktop check → GUI env (R15, `GDK_SCALE=1`) +
  `RADIOS_AUDIO_SERVER=none:` → private desktop (§3.7) → `SetErrorMode` (replaces RLIMIT_CORE) → run → return code.
  The content-scale C test needs real input (tests-c: backend P on the private desktop, or SendInput + `RESOURCE_LOCK
  native_input`) and an explicit window size (`RADIOS_CONTENT_SCALE_WINDOW_WIDTH/HEIGHT`) or skip when the virtual
  screen is smaller than requested (F19).

**run_data_connection_ui.py — S** (native_data_connection)
* Does: 8 fixed cases, each a fresh installation (L18-27,45-141); flatpak host boundary (L39-42,84,113-118);
  "outdated service" by appending to `runtime_version.py` (L119-121); "ACK viewer" via urllib (L122-131); asserts
  credentials bytes unchanged and status running (L136-139).
* Windows: delete flatpak branches (D1); R5 (L84); `no_proxy_opener` (L130); `root.mkdir(mode=0o700)` (L48)
  harmless; job/env/desktop. ui-platform-c.md §15 adds: no `ConsoleWindowClass` window owned by the native
  helper launch.

**run_dj_ui.py — S** (native_dj, native_effect_return): R6 at L20 (`Path(...).stem != "test_dj_ui"`), else
inherits; `RADIOS_SHOT_DIR` docs example becomes PowerShell (`$env:RADIOS_SHOT_DIR = "$PWD\build\dj-review"`).

**run_emergency_transfer_ui.py — S** (native_emergency_transfer): L71 `read_text()`, L84 `write_text()` → UTF-8/LF;
seed (L17-60) portable (`"/display-only/untrusted.wav"` is display text); inherits.

**run_external_access.py — S** (external_access)
* Does: generates a self-signed cert with `openssl req -x509 … -addext subjectAltName=…` (L43-46), TLS
  `ThreadingHTTPServer` (L48-53), asserts 6 HEAD requests, no redirect follow, no credentials (L59-64).
* Windows: resolve `openssl.exe` from MSYS2 explicitly (`RADIOS_TEST_OPENSSL` or `C:\msys64\ucrt64\bin\openssl.exe`
  on PATH) else exit 77; `chmod(0o600)` harmless; C side must accept PEM CAINFO (tests-c §3.3; with Schannel also
  `CURLSSLOPT_NO_REVOKE` for a CRL-less test CA — curl area). Join the server thread before the temp dir exits.

**run_header_navigation_ui.py — A**: subprocess per scenario over run_audio_ui (L23-46).

**run_native_data_ui.py — A**: case-loop wrapper (L11-44).

**run_player_mapping_ui.py — A**: case-loop wrapper (L10-36).

**run_playout_ui.py — A** (≈38 CTest targets): quota widening + prepare gates (L15-55), case isolation
(L59-84); all through run_audio_ui seams.

**run_preferences_output.py — R** (preferences_output)
* Linux-only: `pactl` (L19-21,46,52-55,111-113), `parec` 44100 capture (L61-64), `shutil.which` gate (L40-42).
* Windows: `NullAudioServer` with one sink `radios_output_accept_<hex>` rate 44100, `capture=root/"actual-44100.f32"`;
  env `RADIOS_TEST_OUTPUT_SINK/_CAPTURE/_PROOF` + `RADIOS_AUDIO_SERVER`; keep proof parsing and the
  1000/1500 Hz amplitude checks (L73-96, `amplitude` → pcm_analysis); delete the 0.15 s parec warm-up (L65).

**run_programme_ui.py — S** (native_programme): L14 `read_text()`, L26 `write_text()` → UTF-8/LF; inherits.

**run_rekordbox_ui.py — S** (native_rekordbox): R5 (L82); writes `export.pdb` under `"USB örnek/PIONEER/rekordbox"`
(L63-65) — the C test sends that native path as `usb_root`, which only works once SDBASE accepts Windows drive/UNC
roots (rekordbox_device.py:135-137 requires `startswith("/")`, cross-area); `RADIOS_TEST_USB_EXPORT` copy (L66-70).

**run_release_acceptance.py — R** (manual; README §14)
* Does: verifies + extracts the binary and source `tar.gz` (L45-80: `PurePosixPath` member checks, `chmod(member.mode)`
  L58/L71, `open("xb")`, production-marker scan), binary acceptance via `run-radios.sh` (L206-287), source rebuild
  with CMake (L290-312), optional `--host-runtime` with flatpak-spawn, `/proc` + `pidfd` stop, `run-database.sh`
  (L93-203), DISPLAY/WAYLAND (L325-326).
* Windows: archives = windows-kind **zip** (tools spec §0.12): member checks (no absolute/drive/`..`/backslash
  names, no symlink external attrs, only listed files, size bounds), write bytes without chmod; launch
  `<extracted>\RadioS\bin\RadioS.exe` (`--version`, `--check-config`, `--smoke-test --capture`) — no shell script;
  readiness/identity as test_release_install (§4.2); source acceptance builds with the pinned MSYS2 toolchain
  (`-G Ninja -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe`, D7) and is optional (`--source-build`); delete
  `--host-runtime` (flatpak). Loader failure codes per §3.10. Report written UTF-8/LF.

**run_rhythm_upkeep.py — S** (rhythm_upkeep)
* Does: synthesises WAVs (L38-56), runs `BINARY analyze` (L79), imports in-process, writes `config/credentials.json`
  (L96-102, L166-172) for the C `scan` against an in-process `BoundedHTTPServer` (L104-111).
* Windows: create `credentials.json` privately (`open_fd(private=True)` or `make_private`) — the C data client checks
  privacy (R8); `.meta` `read_text()` (L96,L166) → UTF-8; `chmod` harmless.

**run_rotation_editor_regressions.py — R** (16 CTest uses: native_content_scale_module/_popup_*, view_presets_ui,
native_player_tree, native_midi_hotplug_*, native_automation_workspace, automation_scope_fetch, schema_settings_ui,
rotation_editor_regressions, player_waveform_markers, native_playlist_export, native_programme_view_*,
native_programme_queue_scale, loudness_settings, settings_sync)
* Linux-only: Xvfb discovery incl. `/run/host` loader (L16-25), env `GDK_BACKEND=x11` (L26-27), `os.pipe` +
  `pass_fds` + `-displayfd` + `select` (L28-43), Xvfb terminate (L46-50).
* Windows: same file name (CTest references), body = `testkit.gui_run(sys.argv[1:], timeout=scaled(30))`
  (interactive check → 77, GUI env, `RADIOS_AUDIO_SERVER=none:`, private desktop, return code). Keep 77 propagation.

**run_routing_graph_ui.py — R** (native_routing_graph)
* Linux-only: reuse of `unix:/tmp/radios-audio-private-` server (L29-31), pulseaudio (L32-38,111-139), X11/DISPLAY
  requirement (L39-41), `GDK_BACKEND=x11` (L54), `mkdir(mode=0o700)` (L45,50), `O_DIRECTORY`/`/proc` socket
  (L59-60), fake D-Bus (L103), `private.pa` (L104-110), pacat players (L140-151), `pactl … sink-inputs` (L152-161,
  204-210), 2.3 s warm-up (L172).
* Kept: private installation (L68-98), env `RADIOS_TEST_ROUTING_*` (L162-167), screenshots dir (L166-167), exact
  case `/native-routing-graph/drag-connect-save-reopen-pcm` (L65,174) and TAP check (L183-186), WAV prefix checks
  `graph-ui-app`, `graph-ui-mix` and isolation thresholds (L187-203), report (L211-214,233-234).
* Windows: `NullAudioServer` sinks `routing_original/routing_output/routing_microphone/routing_unrelated`,
  applications Selected 440 Hz/0.20 on routing_original (pid 41001), Unrelated 997 Hz/0.30 on routing_unrelated
  (pid 41002), Microphone 880 Hz/0.50 on routing_microphone (pid 41003); defaults sink routing_original, source
  `routing_microphone.monitor`; export `RADIOS_TEST_ROUTING_APPLICATION=41001`,
  `RADIOS_TEST_ROUTING_ORIGINAL_SINK=routing_original` (names/pids, tests-c/routing area adapt the C side);
  native input: tests-c backend (SendInput needs interactive desktop + `RESOURCE_LOCK native_input`, else 77);
  restored-routes check moves to C (§3.10); report `read_text(errors="replace")` → `encoding="utf-8"`.

**run_routing_host.py — X** (no CTest entry; orphan host Pulse/PipeWire probe: `/run/user/<uid>/pulse/native`
(L30), `pactl list short modules` (L36-53), `load-module module-null-sink` (L83-84), `move-sink-input` (L141),
defaults (L158); its `host-compat` executable no longer exists). Subject has no Windows counterpart (Windows never
creates system-visible virtual devices or moves apps). **Replacement**: opt-in `tests/run_wasapi_acceptance.py`
(§3.9) that proves the same "host audio untouched, owned source reaches MAIN exclusively" on a real endpoint.

**run_routing_ui.py — R** (native_routing): same conversion as run_routing_graph_ui (L29-41,54,59-60,124-193,
216-219) without the native-input requirement; keep pending-context gate (L71-91), viewer user via `post`
(L118-123, `no_proxy_opener`), WAV checks `routing-ui*.wav` (L200-207), report. Unrelated app on `routing_output`
(997 Hz) as today (L164-166).

**run_stream_metadata_ui.py — A**: subprocess per case over run_playout_ui (L8-43).

**run_transfer_ui.py — S** (native_transfer_ui)
* Does: fake desktop-transfer service (L38-137), private root with fake credentials (L140-168).
* Windows: L166-168 `os.open(…, O_WRONLY|O_CREAT|O_EXCL, 0o600)` → `_platform.open_fd(path, …, private=True)`
  (binary + private DACL); R5 (L149); `meta.write_text` (L160-161) `newline="\n"`; `export-start` builds
  `Path(body["destination_dir"]) / file_name` (L97) — Windows paths from the GTK file chooser are fine; join the
  server before cleanup (L194-197 already).

**rekordbox_device_fixture.py — A**: pure `struct` page builder (L1-93); POSIX-looking strings inside the fixture
(`/PIONEER/USBANLZ/…`, `/Contents/Örnek DJ/…`) are DeviceSQL content, not host paths.

### 4.2 tests/test_*.py

**test_application_shutdown.py — R** (native_application_shutdown)
* Does: private installation (L129-163), private Pulse with mic tone (L164-204), for SIGTERM and SIGINT: launch the
  real `main.c` + driver (`--project-root root --profile routing`, L94-119), require `gui.lock` held (L114-115),
  signal (L121-127), verify WAV RIFF/data sizes (L214-218), privacy (L219-220), 880 Hz in WAV (L221-223), no Pulse
  source-outputs (L224-225), service pid/sequence unchanged (L226-228), reopen without capture (L231-241).
* Linux-only: `fcntl.flock` (L12,29-39), `signal` (L18,121-127,205), pulseaudio/pactl/pacat (L48-57,90-92,
  164-204), `DISPLAY/WAYLAND` (L55-57), `O_DIRECTORY`+`/proc` (L73-74,169), `st_mode` (L219).
* Windows: loop over `("stop-event", "wm-close", "end-session")` (§3.6); `lock_held` → `_platform` probe with
  `wait_lock_released` (R10); audio via `NullAudioServer` sinks `routing_output`, `routing_microphone` + application
  880 Hz on `routing_microphone` (driver selects `routing_microphone.monitor`, application_shutdown_driver.c:81),
  defaults as today; delete source-output check (§3.10); privacy via `assert_private`; WAV checks unchanged
  (`tone_amplitudes` from pcm_analysis); add negative control `TerminateProcess` → app exit code ≠ 0 and next
  launch's recovery path (optional). Report UTF-8/LF (L262).

**test_data_playlists.py — A**: real CLI service via `test_database_setup.DatabaseSetupTests` fixture (L16-24) and
its `backend()` (L62-68) — inherits R2 fix.

**test_database_recovery.py — S** (database_recovery): `stat.S_IMODE(...) == 0o600` (L57) → `assert_private`;
`database_file.chmod(0o600)` (L95) harmless; rest in-process tools (`setup._publish`, `operation_lock`) — needs the
tools shim.

**test_database_setup.py — S** (database_setup; also fixture for test_data_playlists)
* L84-89: config keeps `python_command = python3` from the example → write `Path(sys.executable).as_posix()` (R5;
  D4 resolution would otherwise look for `<root>\runtime\python`).
* L121-126 `command()`: use `python_child_argv`; `text=True` UTF-8.
* L151-158 `backend()`: R2.
* L187-221 `cleanup_service`: `/proc` scan + SIGTERM/SIGKILL → `stop` + `iter_fixture_processes(root)` +
  `TerminateProcess`, plus the class-level job object.
* L228: `assert_private`.
* L281: `read_text()` → encoding.
* L292-307 `test_public_and_symlinked_credentials_are_rejected`: `chmod(0o644)` → `make_public` (assert
  `not is_private` instead of `S_IMODE == 0o644`, L299), restore with `make_private`; file symlink → `symlink_or_skip`.
* L319-326 `test_symlink_database_escape_is_rejected`: add the junction variant (always runs).
* L328-335 non-loopback URLs: `0.0.0.0` is only *validated*, never bound → keep.
* L355-380: `source.write_text(source.read_text() + …)` (L363) → UTF-8/LF.
* L445-461 foreign listener: needs SDBASE without `SO_REUSEADDR` (R14).
* New cases (tools spec §6.1): `ensure`, `stop` via event (exit 0, `service.json` removed), `stop --force`,
  excluded-port message (pre-bound `SO_EXCLUSIVEADDRUSE` socket from another process).

**test_desktop_admin.py — S**: L294-295 mode assertions → `assert_private`/`assert_private_dir`; L296 `read_text()`
→ encoding. Rest inherits `test_desktop_api.DesktopAPI`.

**test_desktop_advert.py — A**: `mkdir(mode=0o700)`/`chmod(0o600)` (L53,59) harmless; time zones need tzdata (R1).

**test_desktop_api.py — S** (also the shared fixture `DesktopAPI` L64-74, `verified_binding` L592-635)
* L869: `file.stat().st_mode & 0o777 == 0o600` → `assert_private`; L870 `read_text()` → encoding.
* L336 timezone negatives: keep; optionally add Windows forms `"..\\UTC"`, `"C:\\Windows\\win.ini"`,
  `"Europe\\Istanbul"`, `"UTC:"` (shim `canonical_timezone`, python-sdbase-app.md §2.3-11).
* L967 `file:///etc/passwd` device rejection: keep, add `file:///C:/Windows/win.ini`.
* Fixture `tearDown` = `temp.cleanup()` (L73-74): SDBASE keeps no persistent handles (storage.py:236 opens the lock
  per operation; python-sdbase-engine.md:403), so cleanup works; use `rmtree_quietly` for robustness.

**test_desktop_autodj.py — A** (CTest uses `-m unittest` + `PYTHONPATH=…:…`, CMakeLists.txt:1090-1092 → §6).

**test_desktop_automation_calendar.py — A**: L698 zone negatives include `"/etc/passwd"` (string validation).

**test_desktop_broadcast.py — A**: profile validation lists (L101-120) are platform-neutral; recommended additions for
Windows-specific rejection in record filenames: `"CON.wav"`, `"x:y.wav"`, `"x.wav."`, `"C:x.wav"`, `"C:\\x.wav"`,
`"x|y.wav"`. L310 `chmod(0o600)` harmless.

**test_desktop_copyright.py — A**.

**test_desktop_integrity.py — S**: L147 `calls[0][1:3] == ["-m", "sdbase.cli"]` breaks once desktop_integrity spawns
`python_child_argv("-m", …)` (`[exe, "-E", "-s", "-B", "-X", "utf8", "-m", "sdbase.cli", …]`,
python-sdbase-app.md:453-454) → assert `cmd[cmd.index("-m") + 1].endswith(".cli")` and `"-E" in cmd`; L149 `--db`
must be absolute (`Path(store.journal).absolute()`); L205 `os.chmod(path, 0o600)` harmless.

**test_desktop_local_event.py — A**.

**test_desktop_media.py — S** (also shared fixture `DesktopMedia` L42-78)
* L101, L127 provenance literals `"/music/Albüm/Disc 1/song.wav"`, `"/music"`, `"/other/song.wav"` →
  `Path("/music").is_absolute()` is False on Windows (desktop_media.py:596-599) → use `fake_abs("music", …)`; keep
  the expected relative `"Albüm/Disc 1/song.wav"` (POSIX form, desktop_media.py:601).
* L302-318 `test_source_confinement…`: `symlink_to` (L303-304) → `symlink_or_skip`; `os.mkfifo` (L305-306) → remove
  and add Windows cases: directory named `pipe.wav`, `"CON"`, `"nul.wav"`, `"x.wav:ads"`, `"..\\x.wav"`,
  `"folder\\file.wav"`, `"C:x.wav"`, `str(self.path)` (absolute, already).
* L523-543 object-store links: file symlink (L535) → `symlink_or_skip`; directory symlink (L541-542) → junction
  variant (always).

**test_desktop_media_rights.py — A**.

**test_desktop_player_topology.py — A**.

**test_desktop_playout.py — S** (also fixture `DesktopPlayout`): L635 `path.symlink_to(source)` → `symlink_or_skip`
+ hardlink variant (`os.link`, must also be ineligible if SDBASE rejects `st_nlink > 1`, else document).

**test_desktop_programme.py — A**.

**test_desktop_rekordbox.py — A**: XML `Location="file://localhost/not-accessible/…"` (L27-36) is foreign text;
recommended addition: a `file://localhost/C:/Music/x.wav` track (rekordbox.py:317 already strips the slash before a
drive) whose `source_path` must be `C:/Music/x.wav` (or the agreed Windows form).

**test_desktop_rekordbox_device.py — S**: `usb_root` literals `"/media/…"` (L36,45,56,74,90-95,114,125,172,184) are
validated with `startswith("/")` (rekordbox_device.py:135); once SDBASE accepts Windows roots (cross-area) keep the
POSIX vectors as *foreign-format* tests if still accepted, and add Windows vectors: root `"E:\\"` → `source_path`
`"E:\\Contents\\Örnek DJ\\Track 1.wav"`; rejections `"E:"`, `"E:\\..\\x"`, `"\\\\?\\E:\\"`, `"E:\\USB\x00"`; L156
`startswith(usb_root + "/Contents/")` → computed with the platform join helper. `RADIOS_TEST_USB_EXPORT` opt-in stays.

**test_desktop_reset.py — A**: borrows `DesktopMedia.setUp/tearDown` (L27-31); disk-space check via
`shutil.disk_usage` (SDBASE).

**test_desktop_routing.py — A**: Pulse-looking device names (L91-92) are opaque text ≤512; optionally add a Windows
endpoint id (`"{0.0.0.00000000}.{…}"`) and `.monitor` of it.

**test_desktop_transfer_package.py — S**
* L436 `package.stat().st_mode & 0o777 == 0o600` → `assert_private`.
* L581, L604-605 provenance literals `"/indirilenler/…"`, `"/old/music/…"` → `fake_abs` (SDBASE validators
  desktop_transfer_package.py:228,255 require `startswith("/")` today → cross-area Windows rule); L624
  `str(root / paths[index])` already native.
* L638-639 `os.symlink(self.out, link)` → junction (always) + `symlink_or_skip`; L642-645 add `"C:\\"`, `"C:"`,
  `str(self.out) + "\\"`, `"\\\\?\\" + str(self.out)` to rejected destinations (keep `"/"`).
* L656-661 zip with `"../escape.json"`: add `"..\\escape.json"`, `"C:/x.json"`, `"CON.json"` member names.

**test_desktop_transfers.py — S**: L175-176 → `assert_private` / `assert_private_dir`; L184 `"/etc/passwd"` keep +
`"C:\\Windows\\win.ini"`; CSV `file_path` literals (L42,109,135,142) are foreign mAirList text (keep).

**test_desktop_weekly_programme.py — A**.

**test_desktop_workflow_bridge.py — S**: L62, L96 `write_text(json.dumps(..., ensure_ascii=False))` without encoding →
`encoding="utf-8", newline="\n"` (labels may contain characters outside cp1254).

**test_gui_backend_coverage.py — A**: imports `tools/gui_backend_coverage.py` (already UTF-8 reads, L74-136); needs
sdbase importable without `fcntl` (tools spec §8 last bullet).

**test_packaging.py — R** (packaging)
* Kept (source kind, platform-neutral): determinism/allowlist (L67-91), private sessions/vendor (L93-110), changed
  file (L112-119), unlisted file (L126-134), manifest credentials (L136-152), traversal/symlink/hardlink/duplicate/
  dir members (L154-174), corruption/bounds (L176-191), poisoned template (L207-213), gzip trailer/types (L223-241),
  failed stage (L243-250).
* Changed: mode-tamper half of L112-124 targets `run-radios.sh` → the Windows launcher files (`run-radios.cmd`/`.ps1`,
  tools spec §0.10) or another member; L193-205 source symlink file/dir/dist → `symlink_or_skip` + junction variants
  (os.walk descends into junctions on Windows — the tool must use `_platform.is_link`); L215-221 hardlink works on
  NTFS; L252-276 ELF binary kind → **windows kind**: fake PE bytes (`b"MZ" … b"PE\0\0"` with machine 0x8664) through
  patched `runtime_info`, zip layout `RadioS/bin/RadioS.exe`, `runtime/python/**`, tamper cases (backslash names,
  case-fold duplicates, reserved names, symlink external attr `0xA000`, encrypted flag, unlisted file, missing data dir).
* setUp (L25-42): `TemporaryDirectory()` in `%TEMP%` (8.3) → `.resolve()` the root before comparisons; copy
  `design/reference/tasarrım/index.html` (Turkish path, fine).

**test_playlist_selection.py — A**. **test_playlist_sweeper_schema.py — A**. **test_playlist_workspace.py — A**.
**test_playout_policy.py — A**. **test_playout_vocal_cues.py — A** (CTest `-m unittest` + `:`-joined PYTHONPATH →
§6). **test_probability_policy_schema.py — A**.

**test_radios_launch.py — R** (radios_launch)
* L16-17 imports `tools/radios_launch.py` which imports `fcntl` at module level (radios_launch.py:12) → Windows port
  of the launcher first. LauncherBuild (L20-85): fake `which` → `"C:\\msys64\\ucrt64\\bin\\" + name + ".exe"` (L30);
  binary `build\radios.exe` resp. `RadioS.exe` per D4 (L49-53: write any bytes; `os.access(X_OK)` is existence on
  Windows); `default-binary.lock` (L61) via shim; new case: fresh configure passes `-G Ninja` and the pinned
  compiler (D7).
* SdkSandbox (L88-96): delete (flatpak SDK has no Windows function, D1; `tools/radios_sdk.py` removed).

**test_release_install.py — R** (release_install; `Installation` reused by run_release_acceptance)
* L12 `fcntl`, L144-154 `gui_lock_held` → shim probe; L105-108,157-160 `run-radios.sh` → `root\bin\RadioS.exe`
  (no shell, D4/tools §0.1 — the exe runs `ensure` itself); L156-176 `native_start` readiness: process image path
  (`QueryFullProcessImageNameW`) + `gui_lock_held` + `/health` store id (L168-172); L178-185 `native_stop`:
  `request_stop(pid)` → wait 5 s → `TerminateProcess`; L187-200 `close()`: `/proc` scan + `os.kill(…,15)` → `stop`
  + `iter_fixture_processes` + job; L126-142 `inspect()`: R2; L69-72 `write_config` `chmod(0o600)` → `make_private`.
* setUp L213-234: stage `packaging.collect(PROJECT, "windows")` files + `bin\RadioS.exe` + DLL closure + runtime
  (hard-link large DLLs from the staged `runtime\` on the same volume, fall back to copy) instead of writing
  `build/radios` with `chmod(0o755)`; `RADIOS_RELEASE_BINARY` override kept.
* Tests: L241-247 passive modes (exe must write to inherited std handles although GUI subsystem — C area); L249-261
  invalid options/config; L263-268 no-desktop → delete (§3.10); L270-305 smoke/first launch (`has_interactive_desktop`
  instead of DISPLAY; L287-288 `assert_private`); L307-317 missing service restart; L319-347 foreign listener (R14);
  L349-360 pending restore (`marker.chmod` → `make_private`).

**test_reliability.py — S** (reliability)
* L50-51 copy also the tools shim (§3.3-3); L52-57 `config.read_text()/write_text()` of the **non-ASCII** example INI
  (67 non-ASCII bytes) → UTF-8/LF and set `python_command` (R5) since L165-167 starts the real service.
* L64-66, L143 `ui-preferences.ini` `write_text` → `newline="\n"`; chmods (L66,69,77,202,204,213,345,497,504-513,
  542) → `make_private`/`mkdir_private` where the product checks privacy (outbox dirs/files L198-214), else delete.
* L124 → `assert_private`; L157 `read_text()` → encoding.
* L291-318: `fcntl.flock(held, LOCK_EX|LOCK_NB)` / `LOCK_SH|LOCK_NB` on `server.lock`/`gui.lock` → `_platform.lock`
  on a handle opened `"a+b"` (same 0x4000000000000000 byte as the product, R10).
* L320-354: L340 settings symlink → `symlink_or_skip` + hardlink variant; L325 `"/tmp/foreign"` keep + `"C:\\foreign"`.
* L487-536: L498 `unsafe.symlink_to(private)` → `symlink_or_skip` (+ junction to a dir variant); L528 `is_symlink()` →
  `_platform.is_link`; `os.utime` (L515) works.
* L165, L403-405, L418-420, L453-455 CLI runs → `python_child_argv` (UTF-8 JSON stdout).

**test_routing_graph_schema.py — S**: recorder `folder` `"/tmp/radios"` (L86) is validated with
`folder.startswith("/")` (desktop_api.py:431-433); with the Windows rule (routing spec §12.2: validators accept both
syntaxes) keep it and add `"C:\\Kayıt"`, `"\\\\nas\\yayin"` (accepted), `"C:kayit"`, `"\\\\?\\C:\\x"` (rejected).

**test_storage_locations.py — R** (part) (media_archive_service)
* L88-95 `test_path_rule_matches_the_c_reader` → Windows rule set (core-posix-files.md §5.5): bad `""`, `relative`,
  `C:\a\..\b`, `C:\a\\b`, `C:\a\`, `C:\`, `C:/a/b`, `C:\a<TAB>b`, `C:\a\.\b`, `\\?\C:\a`, `C:\a:s`, `\a`, `C:a`,
  `c:\a`, `C:\CON\x`, `\\server\share`; good `D:\Türkçe disk\arşiv`, `\\server\share\dir`.
* L55-57 `write_ini`: escape backslashes for GKeyFile (R5); L97-99 escapes test → `recording_root=D:\\kay\sıt` ⇒
  `{"recording_root": "D:\\kay ıt"}`; add invalid escape `D:\data` (`\d`) ⇒ rejected by both C and Python (the
  Python unescaper must mirror GKeyFile's error, radios_database.py:105-116 currently keeps unknown escapes —
  cross-area).
* L105 file symlink → `symlink_or_skip`; L123-127 dir symlink → junction variant; L34 `.resolve()` already.
* L209-223 patches `_process_identity` → still valid with tools' Windows identity tuple.

**test_stream_metadata_schema.py — A** (Icecast mount `/canli` is not a path).

### 4.3 tests/README.md — S
Linux content to rewrite (D1 wording rule): `GDK_BACKEND=x11` note (L28-29), `pactl`/`parec`/`ffmpeg` requirements
(L204, L212), private Pulse daemons (L331), Xvfb/Wayland/X11 mentions (L178), `python3 …` commands (L173, 216, 239,
253, 294, 402, 443) and POSIX `RADIOS_SHOT_DIR="$PWD/…"` syntax, Linux archive names
(`RadioS-0.17.0-linux-x86_64.tar.gz`, L173). New text: interpreter `runtime\python\python.exe -X utf8`, null audio
provider + capture files, private desktop / interactive session requirement (exit 77), opt-in `real-audio` label,
Defender exclusion advice, PowerShell examples (`$env:RADIOS_SHOT_DIR="$PWD\build\dj-review"; & runtime\python\
python.exe -X utf8 tests\run_dj_ui.py build\test_dj_ui.exe -p /native-dj/…`). Historical acceptance figures stay as
history.

### 4.4 database/SDBASE/tools (13; not in CTest today — §6 proposes label `sdbase-acceptance`)

**acceptance.py — S**: `_write` (L31-38) → `_platform.replace` with retry (the center process reads clock/config
JSON concurrently, L41-48,198-216 → sharing violations), `os.fsync` keep; `api()` urllib (L235-250) →
`no_proxy_opener`; child workers (L184-200) → `python_child_argv` or keep plain `sys.executable` + `PYTHONUTF8=1`
(JSON with `ensure_ascii=False` crosses the pipe, L124); object `rename`s (L104,110) may hit a reader → retry;
`os._exit(0)` crash (L117) fine; `TemporaryDirectory` (L553) after `run.cleanup()` (L538) fine.

**acceptance_playout.py — S**: publish `os.link(temporary, output)` + unlink (L156-165) → `_platform.link_noreplace`
/ `rename_noreplace` (FAT/exFAT/SMB-safe); `receipts` file `open("x", encoding="utf-8")` (L181) add `newline="\n"`;
urllib stream fetch (L106) → no-proxy opener.

**benchmark.py — S**: module-level `import resource` (L23) → ImportError on Windows; `_rss()` (L57-66) uses
`/proc/self/statm` + `ru_maxrss` → `GetProcessMemoryInfo(PROCESS_MEMORY_COUNTERS_EX)` (`WorkingSetSize`,
`PeakWorkingSetSize`; psapi via ctypes); `--memory-limit-mib` (L521-539) → Job Object
`JOB_OBJECT_LIMIT_PROCESS_MEMORY` on the current process (or reject the option); `address_space_limit_bytes` (L494)
→ the job limit or `None`; docstring `python3 tools/benchmark.py` (L4) → runtime python. Disk path option fine.

**disk_index_acceptance.py — S**: `process.send_signal(signal.SIGKILL)` (L205) → `process.kill()` (`signal.SIGKILL`
does not exist on Windows); worker launch (L50-60) fine; "killed builder" semantics hold with `TerminateProcess`
(SDBASE build lock must be released by handle close — engine area).

**engine_acceptance.py — S**: module-level `import fcntl` (L4) and `_hold_lock` (L20-24) → `_platform.lock` (the
`multiprocessing` child is spawned on Windows and re-imports the module; `__main__` guard exists L251-252); raw-socket
checks (L121-170) can see `ConnectionResetError` instead of EOF when the server closes with unread input (Windows RST)
→ treat reset after a complete `HTTP/1.1 422` prefix as the same outcome, or the server drains before close
(SDBASE); `multiprocessing.Event` works.

**failover_acceptance.py — S**: L166 `process.returncode < 0` → `!= 0`; L137-141 renames `source.journal` while the
child HTTP process may have it open → `_platform` retrying rename (SDBASE opens per operation; race only);
`process.kill()` (L165,231) fine; PYTHONPATH child (L66-76) fine (no `-E`).

**maintenance_acceptance.py — S**: L227-231 `link.symlink_to(foreign, target_is_directory=True)` → junction
(`_winapi.CreateJunction`) and assert via `_platform.is_link(link)`; the scenario exercises replace-over-open
semantics (checkpoint/journal publication while a read view and a read-only handle exist, L93-117) → depends on
SDBASE's POSIX-semantics `replace` (python-sdbase-app.md §2.2 `replace`); `os._exit(71)` (L46) fine.

**ops_archive.py — A**: operator CLI; `is_symlink` checks (L29-31) should become `_platform.is_link` inside SDBASE
(junction) — optional here.

**ops_scheduler.py — S**: `--loop` stops on SIGINT/SIGTERM (L92-105): add `signal.SIGBREAK` and a
`_platform.StopRequest` (named event `Local\RadioS-stop-<pid>`) so `request_stop(pid)` works; help text "existing
mode-0700 directory" (L35) → "private directory".

**query_integrity_acceptance.py — A** (in-process, one writer thread).

**recovery_acceptance.py — A**: child `-c` with `PYTHONPATH` (L115-124,162-174) and `os._exit(87/88)` work with the
runtime interpreter (no `-E`); `read_text()` (L192) → encoding recommended.

**replication_acceptance.py — S**: `Run.request` urllib (L146-154) → no-proxy opener; workers `os._exit(71/72/73)`
(L67,76,83) fine; `os.chmod(path, 0o600)` (L117) harmless.

**stream_acceptance.py — R** (worker lifecycle part)
* Workers install `signal.signal(SIGTERM, …)` (L37) and the parent stops them with `terminate()` (L148-155) — on
  Windows `TerminateProcess`, so "continuous follower exits cleanly on signal" (L314-315, `returncode == 0`) fails.
  Replace with `_platform.StopRequest` in `worker()` and `Run.stop()` → `request_stop(pid)` + wait, `kill=True` paths
  (L215,305) stay `process.kill()`.
* `openssl req` (L290-292) → resolve MSYS2 `openssl.exe` or skip with a clear message; `os.chmod(key, 0o600)` →
  `make_private`.
* Restart on the same port right after kill (L214-217, L310-312): SDBASE bind without `SO_REUSEADDR` is fine after a
  killed listener (aborted connections leave no TIME_WAIT); keep a 2 s bind retry in the worker for robustness.
* 260 MiB fixture (L390-461) fine on NTFS.

### 4.5 tools/otomasyon_testleri
* **api.py — S**: `json.load(open(credentials.json))` (L5) → `encoding="utf-8"`; urllib (L21-27) → no-proxy
  opener; `BASE = http://127.0.0.1:8000` (live service; Windows excluded-port ranges may make 8000 unusable — read
  `service_url` from `config/radios.ini` instead).
* **e2e.py — S**: `E2E = os.path.join(H, "e2e")` (L8) → `e2e.exe` (or the CMake target path from env); `json.dump(...,
  open(tag + "-wp.json", "w"), ensure_ascii=False)` (L35,50,63) → `open(..., "w", encoding="utf-8", newline="\n")`
  with `with` (the C driver parses UTF-8; cp1254 bytes would be invalid JSON); results file (L116) same;
  `subprocess.run(text=True)` (L17) UTF-8 via `-X utf8`.
* **isaretler.py — A**, **lib.py — A**, **senaryolar.py — A** (pure scenario builders over api/e2e).
* **senaryolar_724.py — S**: L273 `json.dump(..., open(..., "w"))` → UTF-8/LF; L19-21 imports fine.
* **senaryolar_kume.py — S**: L486-487 `json.dump` → UTF-8/LF; L431 `mtime(e2e.E2E)` needs `.exe`; L432-433 library
  names `libradios_core.a`/`libradios_playout.a` stay (MinGW static libs keep `lib*.a`).
* **build.sh — X**: POSIX shell + `cc` + `pkg-config … libpulse` + `-Wl,--start-group` (L1-16) → CMake target
  `otomasyon_e2e` (links `radios_playout radios_core radios_audio`, manifest, D4/D7); README commands use
  `cmake --build build --target otomasyon_e2e`.
* **e2e.c — A**: portable C (json-glib, GLib); builds via the CMake target; needs the UTF-8 manifest (argv paths
  contain Turkish letters) and `-Wl,--stack,8388608` (D7).
* **README.md — S**: `build.sh`, `flock build-4c/.build.lock env …` (L74), `python3 …` commands → Windows commands
  (`$env:RADIOS_BUILD_DIR="build-4c"; & runtime\python\python.exe -X utf8 senaryolar_kume.py`).

---------------------------------------------------------------------------------------------------

## 5. Shim API the tests need (Python) and C contracts they observe

### 5.1 From `sdbase/_platform.py` (python-sdbase-app.md §2.2; loaded by path; tools mirror it)
| Function | Used by (tests) | Semantics required |
|---|---|---|
| `lock(fd, *, shared=False, blocking=True)`, `unlock(fd)` (+ `flock` alias with LOCK_* if the tools shim keeps that name) | gui/server lock probes (test_release_install, test_application_shutdown, test_reliability, engine_acceptance) | LockFileEx 1 byte at `0x4000000000000000`; NB contention → `BlockingIOError` |
| `is_private(path)`, `owned_by_current_user(path)`, `make_private(path)`, `mkdir_private(path, parents=, exist_ok=)`, `open_fd(path, flags, *, private=False, nofollow=False)` | privacy assertions/fixtures (R8) | core-posix-files.md §5.2 SDDL + rule |
| `is_link(path)`, `stat_is_link(st)` | link assertions (R9) | symlink or name-surrogate reparse point |
| `replace(src, dst)`, `rename_noreplace(src, dst)`, `link_noreplace`, `unlink(path)`, `rmtree_quietly(path)` | fixture writes, cleanup (R13), SDBASE tools | 2 s sharing-violation retry; POSIX-semantics replace |
| `python_child_argv(*args)`, `python_child_env(base=None)` | every CLI/child launch (R1/R2) | `-E -s -B -X utf8`; env without PYTHONHOME/PYTHONPATH, + PYTHONUTF8/IOENCODING |
| `process_identity(pid)` | readiness/ownership checks | (pid, creation ticks) of a running process |
| `stop_event_name(pid)`, `request_stop(pid) -> bool`, `StopRequest` | graceful stop (R11), SDBASE tool workers | `Local\RadioS-stop-<pid>` |
| `path_equal(a, b)`, `is_inside(root, p)`, `is_absolute(p)`, `normalize(p)`, `component_valid(name)` | path assertions (R7), storage/media vectors | CompareStringOrdinal(ignore case); §5.5 rules |
| `canonical_timezone(name)`, `check_timezone_database()` | preflight (R1) | tzdata present |
| `disk_free(path)` | (indirect) | GetDiskFreeSpaceExW |

### 5.2 From the tools shim (`tools/radios_platform.py`, python-tools-launcher.md §4.3)
`iter_processes() -> Iterator[ProcessInfo(pid, image_path, argv|None)]` (fixture reaping), `PinnedProcess`
(optional), `no_proxy_opener()`, `configure_stdio_utf8()`, `python_argv(python_command, root)` (to assert the
resolution rules in test_database_setup), `request_graceful_exit(pid)` (if kept; must use the D6 event name).

### 5.3 New test-only helpers (owned here, `tests/radios_testkit.py`)
```python
def preflight() -> None                                    # R1 checks; raises SystemExit(77|1) with a message
def base_environment(kind: str, tmp: Path) -> dict[str, str]   # §3.2
def has_interactive_desktop() -> bool                       # GetProcessWindowStation + UOI_FLAGS & WSF_VISIBLE
def gui_run(argv, *, env, timeout, private_desktop=True) -> int
class KillOnCloseJob: adopt_current_process(); handle            # §3.6
def post_wm_close(pid) -> int; def end_session(pid, timeout_ms=5000) -> bool
def wait_lock_released(path, timeout=2.0) -> bool; def gui_lock_held(root) -> bool
def assert_private(testcase, path); def assert_private_dir(testcase, path); def make_public(path)
def symlink_or_skip(testcase, link, target, *, directory=False); def make_junction(link, target)
def fake_abs(*parts) -> str                                 # "C:\\" + "\\".join(parts)
def exe(path) -> Path                                       # add ".exe" if missing
def scaled(seconds: float) -> float                         # RADIOS_TEST_TIMEOUT_SCALE
def install_gate(path, anchor, code, *, count=1, replace=None)
def write_config(root, port, *, timeouts, initial_profile=None) -> ConfigParser   # R5
def run_cli(root, action, *args, timeout=25) -> dict        # §1.4 JSON contract
def run_glib_cases(main, argv0, exe, *, prefix=None, expect_count=None) -> int
def iter_fixture_processes(root) -> list[int]; def reap(root) -> None
def no_proxy_opener() -> urllib.request.OpenerDirector
def suppress_error_dialogs() -> None                        # SetErrorMode(...)
```
ctypes needs: kernel32 `CreateJobObjectW, SetInformationJobObject, AssignProcessToJobObject, GetCurrentProcess,
SetErrorMode, GetProcessWindowStation(user32), GetUserObjectInformationW(user32), OpenProcess,
QueryFullProcessImageNameW, CloseHandle`; user32 `EnumWindows, GetWindowThreadProcessId, IsWindowVisible,
PostMessageW, SendMessageTimeoutW, GetSystemMetrics`; advapi32 `SetNamedSecurityInfoW, SetEntriesInAclW,
GetNamedSecurityInfoW` (make_public, or a shim function `make_public_for_tests`); `_winapi.CreateJunction`.

### 5.4 C-side contracts the Python tests observe (other areas implement)
* GUI exe creates `Local\RadioS-stop-<pid>` at start-up and exits 0 on it, on `WM_CLOSE` and on
  `WM_ENDSESSION(TRUE)` after finalizing recordings and releasing `gui.lock` (ui-platform-c.md §3; name per D6).
* `gui.lock` held `LOCK_SH` on the D6 byte for the GUI lifetime, opened non-inheritable.
* `RadioS.exe` passive modes write to inherited std handles (GUI subsystem) and exit non-zero with a stderr message
  on invalid options/config before any initialization.
* Null provider (§3.8) incl. silence-while-idle capture, `.monitor`, applications, `running` toggle, 44.1 kHz sinks,
  s16le capture.
* Test exes: UTF-8 manifest (argv), binary `_fmode`, crash-dialog suppression, `timeBeginPeriod(1)`
  (tests-c.md §4.1).

---------------------------------------------------------------------------------------------------

## 6. CTest / CMake integration (build area owns; listed because every Python test depends on it)
1. `Python3_EXECUTABLE` (or a dedicated `RADIOS_TEST_PYTHON`) = `<root>/runtime/python/python.exe`; every
   `add_test(… COMMAND ${Python3_EXECUTABLE} …)` becomes `… -X utf8 -B …` (≈90 registrations incl.
   cmake/*.cmake: Preferences, Prompt5/7/8/10, Pdf2/12, MediaArchive(Service), ResetUi).
2. Environment on WIN32: drop `GDK_BACKEND=x11` everywhere (≈64 entries, tests-c F15); add `PYTHONUTF8=1;
   PYTHONIOENCODING=utf-8;GSETTINGS_BACKEND=memory`; `ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:C:/msys64/
   ucrt64/bin"` (or the staged `runtime/bin`) so `C:\MinGW\bin` never wins (R17); `RADIOS_AUDIO_SERVER=none:` for GUI
   tests without owned sinks; `RADIOS_TEST_DESKTOP_LAUNCHER=$<TARGET_FILE:radios-test-desktop>` if option 2 of §3.7.
3. `desktop_autodj` (CMakeLists.txt:1090-1092) and `desktop_playout_vocal_cues` (915-916) set
   `PYTHONPATH=<src>:<tests>` — `:` is not the Windows separator and CMake lists use `;`: run the files as scripts
   (`… -X utf8 -B ${CMAKE_SOURCE_DIR}/tests/test_desktop_autodj.py`; both have `unittest.main`) — `sys.path[0]` is
   then `tests/` and the fixture modules insert SDBASE/src themselves.
4. `WORKING_DIRECTORY /tmp` (CMakeLists.txt:264,442,546) → `${CMAKE_BINARY_DIR}/cwd-elsewhere`.
5. `automation_recovery_stress` (903-904): add `--driver $<TARGET_FILE:otomasyon_e2e>`.
6. Labels/serialisation: `real-audio` (opt-in WASAPI), `windows-gui` (needs interactive session), `sdbase-acceptance`
   (register the 13 SDBASE tools: acceptance, engine, recovery, replication, failover, maintenance, stream,
   disk_index, query_integrity with their default arguments — they are the best regression net for SDBASE's Windows
   file semantics); `RESOURCE_LOCK native_input` for SendInput tests, `system_clipboard` for native_admin
   (tests-c.md §4.8).
7. Timeouts: keep, expose `RADIOS_TEST_TIMEOUT_SCALE` (R18); Defender exclusion of `build\` on CI.
8. `.gitattributes` (`* text=auto eol=lf`, CRLF only for `*.cmd`/`*.bat`): gate anchors, source digest (spec 5.7),
   manifests and SHA tests depend on byte-exact LF sources.

---------------------------------------------------------------------------------------------------

## 7. New tests owned by this area (requested by sibling specs or needed by the fixtures)
| File | Purpose | Source of requirement |
|---|---|---|
| `tests/test_sdbase_platform.py` | 11 shim cases (binary fidelity, replace over open readers, rename_noreplace, POSIX unlink, junction/symlink, open_beneath, privacy, locks, StopRequest, path vectors, time zones) | python-sdbase-app.md §2.3 |
| `tests/test_platform_shim.py` | tools shim: flock between processes, is_link, open_nofollow binary round trip, replace/publish, private mkstemp, process_identity argv round trip with Turkish/space, stop event, spawn_detached log append, UTF-8 stdout | python-tools-launcher.md §6.2 |
| `tests/test_platform_contract.py` | tools shim and `sdbase._platform` constants equal (lock offset/length, SDDL, allowed SIDs); C helper holds `gui.lock` SH ⇒ Python EX|NB fails and vice versa | tools §6.2, core §5.1, python-sdbase-engine §4.3 |
| `tests/test_testkit.py` | job object kills a grandchild on runner exit; `install_gate` on CRLF and LF copies; `rmtree_quietly` with a held handle; `write_config` produces GKeyFile-parseable `python_command` (round trip through `RadioS.exe --check-config`); `has_interactive_desktop` returns a bool | this spec |
| `tests/run_wasapi_acceptance.py` | opt-in real endpoint smoke (§3.9) replacing run_routing_host | this spec, audio-routing-capture §12.3 |

---------------------------------------------------------------------------------------------------

## 8. Implementation order
1. `tests/radios_testkit.py` + `tests/pcm_analysis.py` (+ `test_testkit.py`); preflight; CTest interpreter/env
   changes (§6.1-3).
2. Mechanical edits (R3/R5/R6/R8/R9/R10/R14) in the S-class files; backend suites green once SDBASE + tools shims land
   (`test_desktop_*`, `test_playlist_*`, `test_database_*`, `test_reliability`, `test_storage_locations`).
3. Base runners: run_data_ui, run_admin_ui (GUI env, job, private desktop) → all data/admin-derived GUI targets.
4. `radios_audio_fixture.NullAudioServer` with the audio area's provider → run_audio_ui, run_preferences_output,
   run_audio_soak, run_audio_recovery, then routing runners + test_application_shutdown (needs ui-platform's stop
   event and the C routing changes).
5. Display runners (run_rotation_editor_regressions, run_content_scale_ui) with the tests-c private desktop.
6. Release/packaging (test_packaging windows kind, test_radios_launch, test_release_install, run_release_acceptance)
   after the tools/packaging port.
7. SDBASE tools + otomasyon edits; register `sdbase-acceptance`; README rewrites; opt-in WASAPI runner.

---------------------------------------------------------------------------------------------------

## 9. Risks, conflicts and open questions
**Conflicts between sibling specs (lead decision needed; tests must use one source of truth):**
1. Audio test-host selector: audio-backend.md §5.9 `RADIOS_AUDIO_SERVER=null:<json>` (+ `RADIOS_TEST_AUDIO_SERVER`,
   running toggle by JSON rewrite) vs audio-routing-capture.md §12.1 `RADIOS_AUDIO_HOST=fixture:<json>` (control
   files `stop-server/restart-server/exit-<pid>`, applications) vs tests-c.md §7.2 `RADIOS_AUDIO_BACKEND=virtual|none`
   + `RADIOS_AUDIO_VIRTUAL_SINKS/_CAPTURE_DIR`. Recommendation: one provider, one variable
   (`RADIOS_AUDIO_SERVER`), one JSON (§3.8 union), `none:` for "no devices". `radios_audio_fixture` keeps the name
   in one constant.
2. Lock byte: DECISIONS D3/D6 + core §5.1 + sdbase-app = `0x4000000000000000`; python-tools-launcher §4.3/4.7 and
   python-sdbase-engine §4.1/4.3 = `0x7FFFFFFF00000000`. DECISIONS wins; both shims must be corrected; the contract
   test (§7) catches drift.
3. Stop event name: DECISIONS D6 + core §5.3 + sdbase-app = `Local\RadioS-stop-<pid>`; ui-platform-c.md §3 =
   `Local\RadioS.Desktop.Terminate.<pid>`; tools §4.6 = `Local\RadioS-SDBASE-<instance>` / `Local\RadioS-GUI-<pid>`.
   Tests call `request_stop(pid)` only; implementers align to D6.
4. Private-desktop delivery (in-process `SetThreadDesktop` vs launcher exe) changes the runner seam (§3.7); and
   gtk-windows-runtime.md §15.1 / ui-platform-c §15 prefer SendInput on the interactive desktop for content-scale
   tests while tests-c prefers posted messages on a private desktop.
5. Python version: PLATFORM_FACTS/D7 3.14.8 runtime vs older specs mentioning 3.13 — tests assume 3.14.8 (`-X utf8`,
   `os.path.isreserved`, `Path.is_junction`).

**Risks:**
* Null-provider captures are produced inside the process under test (no external parec) → a backend bug that writes
  the same wrong bytes to the device and the capture is invisible; mitigated by the opt-in WASAPI loopback run and the
  provider's own deterministic tests (`test_null_provider.c`).
* Per-case fresh installations (native_playout ~38 targets, native_dj 18 cases, native_audio) copy `assets` +
  SDBASE each time; with Defender real-time scanning and `-B` imports (117 modules) Windows can be 2-5× slower →
  timeouts (CTest 600/1800 s) — scale factor, Defender exclusion, optional `-X pycache_prefix` in the service.
* GUI tests need an interactive, unlocked session; CI agents running as services see 77 everywhere (silent loss of
  coverage) — CI must assert that the `windows-gui` label did not skip.
* Sharing violations: gate flag files deleted by C while the Python gate reads them; cleanup of temp dirs with
  lingering handles; `replace` of JSON read concurrently (acceptance.py) — retry budgets everywhere, still a flake
  source under Defender.
* Symlink coverage depends on Developer Mode; junction variants keep directory-link coverage, file-symlink coverage may
  silently skip on CI.
* Port behaviour: `SO_REUSEADDR` hijack if SDBASE is not fixed (foreign-listener tests would pass *wrongly* or flap);
  Hyper-V/WSL excluded port ranges only affect fixed ports (live 8000), not port-0 fixtures.
* Windows RST semantics on close-with-unread-data (engine_acceptance raw socket checks).
* Routing/application capture semantics differ (Windows never moves apps; process loopback needs build ≥ 20348; the
  target machine is 26300) — some Linux assertions move to C or become opt-in.
* `G_DEBUG=fatal-warnings` turns Windows-only GTK warnings (missing loaders.cache/schemas/hicolor, custom selection
  atoms) into aborts — requires the gtk-windows-runtime self-check before GUI suites run.
* Long paths: fixture roots under `C:\Users\sonatturkan\Desktop\RadioS\build\…\RadioS Türkçe …\data\recovery\…`
  approach 260 units; keep prefixes short.
* OneDrive/Controlled Folder Access on Desktop-located checkouts can block writes in `build\`.
