export const meta = {
  name: 'radios-windows-foundation-a-resume',
  description: 'Resume foundation-A: adversarial review + fix of platform layer, WASAPI audio and WinMM MIDI from saved implementer reports, then launch the 9-area C port',
  phases: [
    { title: 'Review', detail: 'fresh adversarial reviewer per area (the previous reviewers were interrupted when the session closed)' },
    { title: 'Fix', detail: 'apply confirmed review findings and re-verify' },
    { title: 'Summary', detail: 'write FOUNDATION_SUMMARY.md for the C port wave' },
    { title: 'C port', detail: 'nested c-port.js workflow (9 areas: port, review, fix)' },
  ],
}

const ROOT = 'C:\\Users\\sonatturkan\\Desktop\\RadioS'
const SP = 'C:\\Users\\sonatturkan\\AppData\\Local\\Temp\\claude\\c--Users-sonatturkan-Desktop-radyo-projesi-kaynak-kodlar-RadioS\\0ead7b3c-7fb3-425d-a304-a7629cf56b77\\scratchpad'
const SPECS = SP + '\\specs'
const REPORTS = SP + '\\reports\\foundation-a'

const COMMON = `
You are a senior Windows systems engineer on the team porting "RadioS" (radio broadcast automation: C11/GTK3 desktop app + Python "SDBASE" HTTP data service) from Linux to a PURE, industrial-grade Windows 10/11 x64 application.

Project root (edit here): ${ROOT}
Original Linux copy (read-only reference, never edit): C:\\Users\\sonatturkan\\Desktop\\radyo projesi\\kaynak kodlar\\RadioS

MANDATORY READING before you write code:
1. ${SPECS}\\PLATFORM_FACTS.md — facts measured on this machine (toolchain, time zones, fonts, Python runtime, WASAPI headers, other toolchains to avoid, NEVER open the real DDJ-FLX10 MIDI device, audio tests at most -60 dBFS).
2. ${SPECS}\\DECISIONS.md — BINDING lead decisions. Windows only, no Linux code paths left (D1); fix industrial-grade defects you meet (D2); platform contract (D3); layout (D4); cross-language rules (D6); build rules and the standalone compile recipe (D7).
3. Your area spec(s) listed below (written by analysis agents after reading the code; they are detailed, use them, but DECISIONS.md wins on conflicts).

Working rules:
- Other agents are working in parallel right now (the Python port agents read include/radios/platform.h; build-system agents configure CMake in ${ROOT}\\build). Edit ONLY the files you own (listed below). If you need something changed elsewhere, do not do it: put it in requests_for_other_areas. If an Edit fails because a file changed since you read it, re-read and retry.
- The CMake build is not ported yet. Verify your work with standalone compiles and your own small test executables built in your private scratch dir using the D7 recipe (strict flags, -Werror). Put C:\\msys64\\ucrt64\\bin first in PATH. Never use C:\\MinGW. Run Python only with ${ROOT}\\runtime\\python\\python.exe.
- Do not leave TODO stubs: implement fully. Preserve the existing public contracts and Turkish user-facing texts (except Linux-specific wording, which you rewrite for Windows). Keep the code style of the project.
- Report honestly: what you verified by running, what you only compiled, what is left.
`

const IMPL_SCHEMA = {
  type: 'object',
  properties: {
    area: { type: 'string' },
    summary: { type: 'string' },
    files_created: { type: 'array', items: { type: 'string' } },
    files_modified: { type: 'array', items: { type: 'string' } },
    files_deleted: { type: 'array', items: { type: 'string' } },
    api: { type: 'string', description: 'public API/contract you provide or changed (signatures), so other areas can use it' },
    verification: { type: 'array', items: { type: 'string' }, description: 'each compile/test you actually ran and its result' },
    industrial_fixes: { type: 'array', items: { type: 'string' }, description: 'real defects fixed beyond the mechanical port' },
    requests_for_other_areas: { type: 'array', items: { type: 'string' } },
    open_issues: { type: 'array', items: { type: 'string' } },
  },
  required: ['area', 'summary', 'files_created', 'files_modified', 'files_deleted', 'api', 'verification', 'industrial_fixes', 'requests_for_other_areas', 'open_issues'],
}

const REVIEW_SCHEMA = {
  type: 'object',
  properties: {
    area: { type: 'string' },
    verdict: { type: 'string', enum: ['pass', 'needs-fixes'] },
    findings: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          severity: { type: 'string', enum: ['critical', 'high', 'medium', 'low'] },
          file: { type: 'string' },
          line: { type: 'string' },
          problem: { type: 'string' },
          evidence: { type: 'string', description: 'how you know: compiler output, test run, code reasoning with exact lines' },
          fix: { type: 'string' },
        },
        required: ['severity', 'file', 'problem', 'evidence', 'fix'],
      },
    },
    checks_run: { type: 'array', items: { type: 'string' } },
  },
  required: ['area', 'verdict', 'findings', 'checks_run'],
}

const AREAS = [
  {
    key: 'platform-layer',
    specs: ['core-posix-files.md (all of it, esp. sections 1, 3, 4, 5, 7.3)', 'ui-platform-c.md sections 2, 3, 4, 5, 13 (startup console/argv/logging, termination, process spawning, relaunch, realpath)'],
    owns: `include/radios/platform.h, src/core/platform.c, src/core/platform_win32.c, src/core/platform_tz.c (whichever exist), tests/test_platform.c, ${ROOT}\\runtime\\share\\zoneinfo\\**,
PLUS the time-zone migration lines (ONLY the time-zone lines and the #include "radios/platform.h" line) in: src/core/weekly_programme_plan.c, src/core/automation_calendar_plan.c, src/core/loudness_policy.c, src/playout/automation_policy.c, src/ui/audio_ui.c, src/ui/header_status.c, src/ui/header_navigation.c, src/ui/automation_calendar.c, src/ui/broadcast.c, src/ui/local_event_prepare.c, src/ui/instruments.c, src/ui/player_rotation.c, src/ui/probability_editor.c, src/ui/schema_track_picker.c, and the tests/*.c files whose time-zone calls were migrated. Do NOT touch src/audio/** or src/core/midi.c.`,
    prior_review_notes: 'The previous reviewer was interrupted after only reading the mandatory documents; start from scratch.',
  },
  {
    key: 'wasapi-audio',
    specs: ['audio-backend.md (all)', 'audio-routing-capture.md (to judge whether the shared WASAPI layer is a sufficient base for per-application capture, endpoint loopback, capture devices and channel inputs in the next phase)', 'audio-misc-media.md sections 4.6 and 5.3 (engine.c threads/clocks/time zone/stack arrays/realtime hooks)'],
    owns: `src/audio/wasapi_device.h, src/audio/wasapi_device.c, src/audio/wasapi_backend.c (replaced src/audio/pulse_backend.c, deleted), src/audio/audio_internal.h, src/audio/engine.c, include/radios/audio.h and include/radios/audio_runner.h (only if the contract really needs it), tests/test_wasapi_backend.c (tests/test_pulse_hotplug.c was deleted), plus pure symbol-rename lines (radios_pulse_backend_* -> radios_audio_backend_*, RadioSPulseBackend/RadioSPulseSnapshot -> RadioSAudioBackend/RadioSAudioBackendSnapshot) in other src/ or tests/ files.`,
    prior_review_notes: 'The previous reviewer was interrupted after reading the docs, wasapi_device.c and diffing engine.c/headers against the Linux originals, just before compiling. Its scratch outputs are in ' + SP + '\\work\\review-wasapi-audio\\ (engine.diff, audio_internal.diff, audio_h.diff, partial .o files). Redo every check yourself.',
  },
  {
    key: 'midi-winmm',
    specs: ['midi.md (all)'],
    owns: `src/core/midi.c (WinMM backend; possibly split into midi_winmm.c), include/radios/midi.h, src/ui/midi_ui.c (only Linux-specific labels/ids/behaviour), tests/test_midi.c, tests/test_midi_hotplug.c (injectable fake WinMM backend).`,
    prior_review_notes: 'The previous reviewer (interrupted) had: built everything twice independently (O0 and O2, clean) in ' + SP + '\\work\\review-midi-winmm\\, the suite passed 3/3 rounds in one process, confirmed the test executables do not import WINMM/CFGMGR32 (fake backend isolation), measured the real device listing at 1.31 s and was checking whether Windows MIDI Services (midisrv) changes first-call latency/exclusivity semantics, started the D1 leftover check, and was running MUTATION tests (mutate.ps1, run_hotplug.ps1, case_storm.c; mutants mut-no-removal-lose, mut-same-device-never, mut-storm, mut-keep-handle) to see whether the hotplug tests catch injected bugs — results unknown. You may reuse those scripts, but re-verify every conclusion yourself and finish the mutation analysis (a test suite that does not kill a meaningful mutant is a finding).',
  },
]

const implReport = (a) => `${REPORTS}\\${a.key}.impl.json`

const review = (a) => agent(
  COMMON + `\nYou are the ADVERSARIAL REVIEWER for area "${a.key}". Another engineer implemented it; their full structured report (JSON) is in the file ${implReport(a)} — READ IT FIRST.\n\nNote from the lead: ${a.prior_review_notes}\n\nSpecs: ${a.specs.join('; ')} (in ${SPECS}). Owned files of that area:\n${a.owns}\n\nYour job: find real defects, do not rubber-stamp. Do NOT edit project files (read-only review; you may build/run things in ${SP}\\work\\review2-${a.key}\\). Check: (1) every owned file compiles with the strict D7 flags (actually compile them); (2) the tests the implementer wrote build and pass when YOU run them, and they really test what they claim; (3) spec + DECISIONS compliance, especially D1 (no Linux code paths left anywhere in the owned files), D3/D6 contracts; (4) Windows correctness: handle/COM reference leaks, wrong share modes, missing FILE_SHARE_DELETE, text-mode opens, 32-bit long/off_t truncation, wrong error mapping, thread-affinity violations (GTK/GLib main context, COM apartments), races and deadlocks (callbacks under locks, WinMM/WASAPI callback restrictions), unbounded waits, missing error paths, UTF-8/UTF-16 conversion bugs, MAX_PATH, quoting/injection in command lines, security (ACL/SDDL), ICU dynamic loading failure handling; (5) industrial robustness. Report each finding with severity, exact location, evidence and a concrete fix. verdict = "pass" only if no critical/high/medium findings remain.`,
  { label: `review:${a.key}`, phase: 'Review', schema: REVIEW_SCHEMA })

const fix = (rev, a) => {
  const serious = (rev && rev.findings || []).filter(f => ['critical', 'high', 'medium'].includes(f.severity))
  if (!rev || serious.length === 0) return Promise.resolve({ area: a.key, skipped: true, reason: rev ? 'review passed' : 'no review', review: rev, fix: null })
  return agent(
    COMMON + `\nYou own area "${a.key}" now (same file ownership as below) and must FIX the confirmed review findings. Owned files:\n${a.owns}\n\nImplementer report (JSON file, read it): ${implReport(a)}\n\nReview findings to fix (critical/high/medium; also fix low ones if cheap):\n${JSON.stringify(rev.findings, null, 2)}\n\nFor each finding: verify it is real (if you are certain it is not, say why), fix it, then rebuild and re-run the area's tests (private scratch dir ${SP}\\work\\fix-${a.key}\\). Return the structured report (area = "${a.key}") listing what you changed and the verification you ran; put any finding you rejected (with reason) in open_issues.`,
    { label: `fix:${a.key}`, phase: 'Fix', schema: IMPL_SCHEMA }).then(r => ({ area: a.key, review: rev, fix: r }))
}

const results = await pipeline(AREAS, review, fix)
const failed = AREAS.filter((a, i) => !results[i] || (results[i].review == null) || (!results[i].skipped && results[i].fix == null))
if (failed.length) {
  log('foundation-A NOT complete for: ' + failed.map(a => a.key).join(', ') + ' — C port not launched')
  return { complete: false, failed: failed.map(a => a.key), results }
}

phase('Summary')
const summary = await agent(COMMON + `\nFoundation-A (platform layer, WASAPI audio core/backend, WinMM MIDI) is now implemented, adversarially reviewed and fixed. Implementer reports: ${REPORTS}\\platform-layer.impl.json, ${REPORTS}\\wasapi-audio.impl.json, ${REPORTS}\\midi-winmm.impl.json. Review + fix results:\n${JSON.stringify(results, null, 2)}\n\nYour job (do NOT edit project source files): write ${REPORTS}\\FOUNDATION_SUMMARY.md for the 9 C-port agents that build on this foundation next (areas: c-core, c-ui-app, c-ui-files, c-ui-sweep, c-audio-routing, c-audio-media, c-audio-virtual, c-tests-support, c-tests-b). Read the ACTUAL headers on disk (include/radios/platform.h, src/audio/wasapi_device.h, src/audio/audio_internal.h, include/radios/midi.h, include/radios/audio.h) and document: files that exist now; the public APIs with exact signatures grouped by purpose (files/fd ops and RADIOS_O_* flags, RadioSFileInfo, locks, privacy, paths, realpath, process spawn/watch/stop, console/argv/logging/startup/crash handler, time zones, WASAPI enumeration/streams/loopback/process loopback/sessions/notifications, audio backend contract, MIDI); conventions (errno vs GError, UTF-8 paths, ownership/free rules, thread rules); link libraries; test status (what passes); remaining open issues and the consolidated requests for other areas, grouped by which C-port area should handle them. Then return the text of a concise version (max ~6000 characters) of that summary.`,
  { label: 'foundation-summary', phase: 'Summary' })

phase('C port')
const foundationText = `FULL foundation summary (READ IT): ${REPORTS}\\FOUNDATION_SUMMARY.md\nImplementer reports: ${REPORTS}\\*.impl.json\n\n${summary || ''}`
const cport = await workflow({ scriptPath: SP + '\\workflows\\c-port.js' }, { foundation: foundationText })
return { complete: true, foundation: results, summary, cport }
