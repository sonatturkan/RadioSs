export const meta = {
  name: 'radios-windows-c-port',
  description: 'Port all remaining C sources and C tests to Windows on the new platform/WASAPI layers, each area adversarially reviewed and fixed',
  phases: [
    { title: 'Port', detail: '9 areas in parallel with disjoint file ownership' },
    { title: 'Review', detail: 'adversarial reviewer per area' },
    { title: 'Fix', detail: 'apply confirmed findings' },
  ],
}

const ROOT = 'C:\\Users\\sonatturkan\\Desktop\\RadioS'
const SP = 'C:\\Users\\sonatturkan\\AppData\\Local\\Temp\\claude\\c--Users-sonatturkan-Desktop-radyo-projesi-kaynak-kodlar-RadioS\\0ead7b3c-7fb3-425d-a304-a7629cf56b77\\scratchpad'
const SPECS = SP + '\\specs'
const FOUNDATION = (args && args.foundation) ? args.foundation : '(foundation report not provided; read the files)'

const COMMON = `
You are a senior Windows C engineer on the team porting "RadioS" (radio broadcast automation: C11/GTK3 desktop app + Python "SDBASE" HTTP data service) from Linux to a PURE, industrial-grade Windows 10/11 x64 application.

Project root (edit here): ${ROOT}
Original Linux copy (read-only reference, never edit): C:\\Users\\sonatturkan\\Desktop\\radyo projesi\\kaynak kodlar\\RadioS

MANDATORY READING before you write code:
1. ${SPECS}\\PLATFORM_FACTS.md (measured facts) and ${SPECS}\\DECISIONS.md (BINDING: D1 Windows only + convert-don't-drop, D2 industrial fixes, D2b no useless leftovers, D3 platform contract, D4 layout/python, D5 fonts, D6 cross-language rules, D7 build rules + standalone compile recipe).
2. The finished foundation layers you build on (already in the tree, reviewed): include/radios/platform.h (+ src/core/platform*.c) — USE IT for every file/lock/ACL/path/process/time-zone/console operation; src/audio/wasapi_device.h (shared WASAPI layer), src/audio/audio_internal.h (renamed radios_audio_backend_* contract), include/radios/midi.h. Foundation summary:
${FOUNDATION}
3. Your area spec(s) listed below (detailed file:line inventories; DECISIONS.md wins on conflicts).

Working rules:
- Other agents edit other files right now. Edit ONLY the files you own. If you need a change in a file you do not own (including platform.h/wasapi_device.h), do not make it: add a precise request to requests_for_other_areas (and work around it locally if you can).
- The CMake build is being ported in parallel by another agent; until build\\compile_commands.json exists, verify with the D7 standalone compile recipe (strict flags -Werror) into your private scratch dir ${SP}\\work\\<area-key>\\ and small test executables you build there. Compile EVERY file you own warning-free. Never use C:\\MinGW. Python only via ${ROOT}\\runtime\\python\\python.exe -E -s -B -X utf8.
- Windows only: no POSIX branches, no <unistd.h>/<sys/*.h> POSIX APIs left (use platform.h), no Linux wording in UI texts. Preserve public contracts and Turkish UI texts. Large stack arrays -> heap. long/off_t never for sizes/offsets/time. Every open/fopen binary. UTF-8 paths through GLib/platform functions.
- FFmpeg here is 8.x (removed: old channel_layout/channels fields, avcodec_close, AVFrame.key_frame, ...). Port to the current APIs with identical behaviour.
- Industrial quality (D2): fix real defects you meet (hangs like the root-climbing loop in media.c, races, leaks, unchecked errors, unsafe paths, 32-bit truncation) and list them.
- Report honestly what you compiled, what you ran, and what remains.
`

const IMPL_SCHEMA = {
  type: 'object',
  properties: {
    area: { type: 'string' }, summary: { type: 'string' },
    files_created: { type: 'array', items: { type: 'string' } },
    files_modified: { type: 'array', items: { type: 'string' } },
    files_deleted: { type: 'array', items: { type: 'string' } },
    api: { type: 'string' },
    verification: { type: 'array', items: { type: 'string' } },
    industrial_fixes: { type: 'array', items: { type: 'string' } },
    requests_for_other_areas: { type: 'array', items: { type: 'string' } },
    open_issues: { type: 'array', items: { type: 'string' } },
  },
  required: ['area', 'summary', 'files_created', 'files_modified', 'files_deleted', 'api', 'verification', 'industrial_fixes', 'requests_for_other_areas', 'open_issues'],
}
const REVIEW_SCHEMA = {
  type: 'object',
  properties: {
    area: { type: 'string' }, verdict: { type: 'string', enum: ['pass', 'needs-fixes'] },
    findings: { type: 'array', items: { type: 'object', properties: {
      severity: { type: 'string', enum: ['critical', 'high', 'medium', 'low'] },
      file: { type: 'string' }, line: { type: 'string' }, problem: { type: 'string' }, evidence: { type: 'string' }, fix: { type: 'string' } },
      required: ['severity', 'file', 'problem', 'evidence', 'fix'] } },
    checks_run: { type: 'array', items: { type: 'string' } },
  },
  required: ['area', 'verdict', 'findings', 'checks_run'],
}

const UI_APP_FILES = 'src/app/main.c, src/ui/window.c, src/ui/data.c, src/ui/reset_ui.c, src/ui/preferences.c, src/ui/admin_ui.c, src/ui/technical.c, src/ui/content_scale.c, include/radios/window.h, include/radios/data.h, include/radios/preferences.h, include/radios/admin_ui.h, include/radios/reset_ui.h, include/radios/content_scale.h'
const UI_FILES_FILES = 'src/ui/media.c, src/ui/playlist.c, src/ui/transfer_ui.c, src/ui/media_archive_ui.c, src/ui/audio_ui.c, src/ui/playlist_browser.c, src/ui/import_profiles_ui.c, src/ui/media_jobs.c, src/ui/player_loader.c, include/radios/media_ui.h, include/radios/media_jobs.h, include/radios/transfer_ui.h, include/radios/media_archive_ui.h, include/radios/audio_ui.h, include/radios/playlist_browser.h, include/radios/import_profiles_ui.h, include/radios/player_loader.h'
const ROUTING_FILES = 'src/audio/patchbay.c, src/audio/channel_input.c, include/radios/audio_patchbay.h, include/radios/audio_channel_input.h, src/core/routing_graph.c, include/radios/routing_graph.h, src/ui/routing.c, src/ui/routing_graph_ui.c, src/ui/media_routing.c, include/radios/routing_ui.h, include/radios/routing_graph_ui.h, include/radios/media_routing.h'
const AUDIO_MEDIA_FILES = 'src/audio/output.c, src/audio/decoder.c, src/audio/transition.c, src/audio/tempo.c, src/audio/dsp.c, src/audio/output_resampler.c, src/media/analyzer.c, src/media/loudness.c, src/media/rhythm.c, include/radios/audio_output.h, include/radios/audio_decoder.h, include/radios/audio_transition.h, include/radios/audio_tempo.h, include/radios/audio_dsp.h, include/radios/output_resampler.h, include/radios/media.h, include/radios/loudness.h, include/radios/rhythm.h, include/radios/media_geometry.h'
const AUDIO_VIRTUAL_FILES = 'src/audio/wasapi_backend.c, src/audio/wasapi_device.c, src/audio/wasapi_device.h, src/audio/audio_internal.h, src/audio/engine.c, include/radios/audio.h, include/radios/audio_runner.h, new src/audio/virtual_backend.c (+ header) if you split it out, tests/test_wasapi_backend.c, new tests/test_wasapi_hotplug.c'
const CORE_FILES = 'every file in src/core/*.c EXCEPT platform*.c, midi*.c and routing_graph.c; every file in src/playout/*.c; their headers in include/radios/ (config.h, runtime.h, local_ini.h, preferences_storage.h, ui_preferences.h, storage_locations.h, media_archive.h, recovery.h, data_client.h, library_analysis.h, external_access.h, diagnostics.h, loudness_policy.h, automation_*.h, programme_model.h, player_topology.h, silence_detector.h, transition_preset.h, weekly_programme_plan.h, station_time.h, playout.h, copyright_policy.h, automation_policy.h)'
const TEST_SUPPORT_FILES = 'new tests/radios_test_support.h + tests/radios_test_support.c (the shared rt_* layer of tests-c.md §5/§6), new tools/win32/radios-test-desktop.c (private desktop launcher, §6.3), tests/content_scale_test_input.h, tests/routing_patchbay_ui_test.inc, and every C test file in categories C and D of the tests-c.md per-file table (rewrites: native input, sockets, fork/flock, /proc metrics, Pulse/ALSA-only cases; D twins such as test_wasapi_recovery.c replacing test_audio_recovery.c) EXCEPT test_midi*.c, test_platform.c, test_wasapi_backend.c, test_wasapi_hotplug.c'
const TEST_B_FILES = 'every C test file (tests/*.c) in category B of the tests-c.md per-file table, plus category-A files that only need a one-line time-zone/IANA or path fix the table lists — EXCEPT files owned by the test-support agent (categories C/D, the shared header/inc) and test_midi*.c, test_platform.c, test_wasapi_backend.c, test_wasapi_hotplug.c'

const AREAS = [
  { key: 'c-core', specs: 'core-posix-files.md (sections 2, 6.1), tests-c.md rows for core tests (read-only for you)', owns: CORE_FILES,
    task: 'Port every core/playout source to platform.h exactly per the core-posix-files.md inventory and migration table: config.c (root discovery from radios_process_executable_path, realpath, path confinement with radios_path_is_inside, python_command resolution per D4), local_ini.c, preferences_storage.c, ui_preferences.c (consider reimplementing both on local_ini as the spec suggests, keeping messages/tests), storage_locations.c (Windows storage rules spec 5.5, identical to tools/radios_database.py), media_archive.c (hidden/system skipping, mtime from RadioSFileInfo, path equality), recovery.c (outbox + gui.lock via radios_fs_open_root/openat/statat/locks/private checks), data_client.c (private credentials check, portable relative path compare, CURLSSLOPT_NATIVE_CA for https, source digest byte order), library_analysis.c (radios_thread_lower_priority), external_access.c, loudness_policy.c and every other file still containing POSIX/Linux code. Build each file with the strict recipe.' },
  { key: 'c-ui-app', specs: 'ui-platform-c.md (sections 0-8, 12, 15) and gtk-windows-runtime.md (startup/font/settings parts)', owns: UI_APP_FILES,
    task: 'Port startup and service lifecycle: main.c (radios_platform_init first, crash handler, single-instance guard per data root via a named mutex with a clear Turkish message, console attach/log sink, UTF-8 argv via g_option_context_parse_strv, executable path, termination watch replacing g_unix_signal_add + WM_QUERYENDSESSION/WM_ENDSESSION routed to the same orderly teardown with ShutdownBlockReasonCreate while WAV finalization runs, keep --version/--check-config/--smoke-test contracts, Windows wording), window.c (Inter TTF via pango_font_map_add_font_file per D5, gui lock fd release via platform, SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED) while audio is on air so the PC never sleeps during broadcast, cleared on stop/exit), data.c (service lifecycle helper spawn via RadioSProcess with CREATE_NO_WINDOW, interpreter resolution and -E -s -B -X utf8 + child env per D4, timeouts, fail closed for an explicit missing python_command, no flatpak), reset_ui.c (relaunch: re-exec own exe with the remembered argv after this process exits; the new instance waits on the parent process handle), preferences.c (WASAPI-based device/codec inventory instead of /proc/asound, backend display name "Windows ses (WASAPI)", Inter Variable Text font in CSS strings), admin_ui.c (Windows command quoting for displayed commands, spawn via RadioSProcess), technical.c, content_scale.c (enable the synchronous popup re-anchoring path for GDK_IS_WIN32_DISPLAY, keep the RADIOS_CONTENT_SCALE_POPUP_MAP_ONLY seam, remove X11 code).' },
  { key: 'c-ui-files', specs: 'ui-platform-c.md (sections 9-11, 13, 14) and audio-routing-capture.md only where audio_ui.c is concerned', owns: UI_FILES_FILES,
    task: 'Port file/path/volume handling in the UI: media.c (window drop of file:// URIs incl. file:///C:/..., UNC; fix the infinite root-climbing loop (root-aware common prefix); skip HIDDEN/SYSTEM entries like $RECYCLE.BIN/System Volume Information), playlist.c (openat/O_* -> platform; file identity via RadioSFileInfo; USB/SD rekordbox device listing via Win32 volume enumeration (GetLogicalDriveStringsW/GetDriveTypeW/GetVolumeInformationW, removable/fixed USB) instead of GVolumeMonitor unmount/uuid), transfer_ui.c (portable file-name validator radios_path_component_valid; default folders via g_get_user_special_dir), media_archive_ui.c (radios_fs_realpath), audio_ui.c (case-insensitive separator-aware objects prefix check), and every other Linux construct in the owned files.' },
  { key: 'c-ui-sweep', specs: 'ui-platform-c.md section 14 (full grep of src/ui) and gtk-windows-runtime.md', owns: `every src/ui/*.c and include/radios/*.h UI header NOT owned by c-ui-app (${UI_APP_FILES}), c-ui-files (${UI_FILES_FILES}), c-audio-routing (${ROUTING_FILES}) or midi_ui.c; plus assets/themes/*.css and assets/README.md`,
    task: 'Grep-driven sweep of all remaining UI sources: any POSIX/Linux construct (unistd/fcntl/sys, g_spawn/GSubprocess, g_unix_*, gdk_x11/GDK_BACKEND, "/" path assumptions, file:// handling, getenv(HOME), Linux wording such as PulseAudio/PipeWire/pactl/Wayland/X11/run-radios.sh/flatpak in UI strings) -> Windows equivalent via platform.h; CSS: put \'Inter Variable Text\' first in every font-family list (D5) in C string CSS and assets/themes/*.css; time-zone call sites must already use radios_time_zone_* (fix any left). Compile every owned file with the strict recipe (they are many; compile all).' },
  { key: 'c-audio-routing', specs: 'audio-routing-capture.md (all)', owns: ROUTING_FILES,
    task: 'Rewrite the routing/patchbay/channel-input audio on WASAPI using src/audio/wasapi_device.h (do not edit it; request missing helpers): per-application capture via process loopback, list audio-producing applications via audio sessions, microphone/capture endpoints, endpoint loopback, channel inputs, compressor + peak/RMS meters, single WAV recording with start/pause/resume/split/stop, streaming outputs; stable persisted identifiers on Windows and graceful handling of Linux-era Pulse ids (radios_virtual_*, *.monitor, *_mic) as "not found" plus the virtual-backend names used by tests (tests-c.md §7). Keep the public patchbay/channel-input APIs. Recorder prefix validation with radios_path_component_valid.' },
  { key: 'c-audio-media', specs: 'audio-misc-media.md (all except engine.c parts already done)', owns: AUDIO_MEDIA_FILES,
    task: 'Port output.c (WAV writer: no-overwrite create, 64-bit offsets, finalize, 4 GiB RIFF limit; MP3/Icecast/SHOUTcast via libcurl/Winsock as the spec says), decoder.c, transition.c, tempo.c, dsp.c, output_resampler.c, media/analyzer.c, loudness.c, rhythm.c: platform.h file ops, binary opens, UTF-8 paths for FFmpeg, FFmpeg 8 API migration with identical behaviour, Winsock specifics, and every other item of the spec.' },
  { key: 'c-audio-virtual', specs: 'tests-c.md section 7 (virtual backend + control API + fake enumerator seam), audio-backend.md', owns: AUDIO_VIRTUAL_FILES,
    task: 'Add to the WASAPI backend the test-and-operations backends required by tests-c.md §7: RADIOS_AUDIO_BACKEND=virtual (process-wide virtual sinks from RADIOS_AUDIO_VIRTUAL_SINKS, <name>.monitor loopback sources, optional virtual sources, mixing when several engines share a sink, real-time pacing from a high-resolution waitable timer, any requested rate, continuous float32le capture files in RADIOS_AUDIO_VIRTUAL_CAPTURE_DIR written from process start with silence when idle, shared-delete file opens), RADIOS_AUDIO_BACKEND=none and the "none:"/"virtual:" server strings (map the old "unix:/nonexistent..." literal to none), the test-only control API (radios_audio_virtual_server_stop/restart, sink_add/remove) mapped to the same snapshot transitions as AUDCLNT_E_DEVICE_INVALIDATED / service loss / IMMNotificationClient events, and the fake-enumerator seam (radios_wasapi_create_enumerator() in its own TU) with tests/test_wasapi_hotplug.c porting the three assertions of the old Pulse hotplug test. Keep the real WASAPI path untouched in behaviour; re-run tests/test_wasapi_backend.c.' },
  { key: 'c-tests-support', specs: 'tests-c.md (all, esp. sections 3-8)', owns: TEST_SUPPORT_FILES,
    task: 'Implement the shared C test layer (rt_* helpers: no-follow tree removal, symlink with junction/skip fallback, private-file assertions on ACLs, file identity, pread/pwrite, sized/sparse files, retrying unlink/replace, path equality, temp dirs, IANA time zones, numeric locale, RSS/handle counts, selection data, rt_input_* posted-message input backend P (+ SendInput backend S behind RADIOS_TEST_INPUT=sendinput), timeBeginPeriod helper) and the private desktop launcher tools/win32/radios-test-desktop.c (CreateDesktopW + STARTUPINFO.lpDesktop, timeout, exit code); rewrite the category C/D tests listed in the table to use them (no XTest/X11/Wayland/GNOME-Shell, no fork/flock/pactl/parec/sockets-Linux). Build the support layer and a few converted tests standalone and run them (GUI ones on the private desktop).' },
  { key: 'c-tests-b', specs: 'tests-c.md (per-file table, sections 5 and 10)', owns: TEST_B_FILES,
    task: 'Apply the per-file B edits from the tests-c.md table using the rt_* API of tests-c.md §5 (being implemented in parallel in tests/radios_test_support.h by another agent: code against the spec signatures; if a needed helper is missing from the spec, request it), plus the one-line time-zone/path fixes listed for A files. Compile each edited test file standalone against the current headers (link failures for rt_* are expected until the support layer lands; compile with -c).' },
]

const port = (a) => agent(COMMON + `\nAREA: ${a.key}\nSpecs (in ${SPECS}): ${a.specs}\n\nFILES YOU OWN:\n${a.owns}\n\nTASK:\n${a.task}\n\nReturn the structured report (area = "${a.key}").`,
  { label: `port:${a.key}`, phase: 'Port', schema: IMPL_SCHEMA })

const review = (impl, a) => agent(COMMON + `\nYou are the ADVERSARIAL REVIEWER for area "${a.key}". Implementer report:\n${JSON.stringify(impl, null, 2)}\nSpecs: ${a.specs}. Owned files: ${a.owns}.\nRead-only (build/run only in ${SP}\\work\\review-${a.key}\\). Find real defects: compile every owned file with the strict recipe; grep for POSIX/Linux leftovers (unistd, fcntl, sys/, O_NOFOLLOW, O_DIRECTORY, flock, getuid, realpath(, lstat(, /proc, /tmp, g_unix_, gdk_x11, GDK_BACKEND, PulseAudio, pactl, Wayland, X11, flatpak, run-radios.sh, python3); check platform.h usage correctness, handle/COM/GLib leaks and refcounts, thread affinity (GTK main thread, COM apartments, WASAPI/WinMM callbacks), long/off_t truncation, stack arrays, text-mode opens, UTF-8 conversions, path comparisons (case/separators/roots), command-line quoting, error paths, Turkish texts preserved, contracts with tests (tests-c.md) and with the Python service (python-*.md). Report findings with severity/location/evidence/fix. verdict="pass" only if no critical/high/medium findings.`,
  { label: `review:${a.key}`, phase: 'Review', schema: REVIEW_SCHEMA })

const fix = (rev, a, impl) => {
  const serious = (rev && rev.findings || []).filter(f => ['critical', 'high', 'medium'].includes(f.severity))
  if (!rev || serious.length === 0) return Promise.resolve({ area: a.key, impl, review: rev, fix: null })
  return agent(COMMON + `\nYou own area "${a.key}" (files: ${a.owns}) and must FIX these confirmed review findings (also cheap low ones):\n${JSON.stringify(rev.findings, null, 2)}\nImplementer report:\n${JSON.stringify(impl, null, 2)}\nVerify each is real, fix, recompile/re-run. Rejected findings (with reason) go to open_issues. Return the structured report (area = "${a.key}").`,
    { label: `fix:${a.key}`, phase: 'Fix', schema: IMPL_SCHEMA }).then(r => ({ area: a.key, impl, review: rev, fix: r }))
}

const SELECTED = (args && args.areas) ? AREAS.filter(a => args.areas.includes(a.key)) : AREAS
log('areas: ' + SELECTED.map(a => a.key).join(', '))
const results = await pipeline(SELECTED, port, review, fix)
return results
