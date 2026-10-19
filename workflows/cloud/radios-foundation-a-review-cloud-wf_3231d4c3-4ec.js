export const meta = {
  name: 'radios-foundation-a-review-cloud',
  description: 'Resume foundation-A of the RadioS Windows port: adversarial review + fix of platform layer, WASAPI audio and WinMM MIDI, then a foundation summary for the C port',
  phases: [
    { title: 'Review', detail: 'fresh adversarial reviewer per area (previous reviewers were interrupted)' },
    { title: 'Fix', detail: 'apply confirmed findings and re-verify under Wine' },
    { title: 'Summary', detail: 'FOUNDATION_SUMMARY.md for the C port wave' },
  ],
}

const ROOT = '/home/user/RadioSs'
const SP = '/opt/radios-port'
const SPECS = SP + '/specs'
const REPORTS = SP + '/reports/foundation-a'

const COMMON = `
You are a senior Windows systems engineer on the team porting "RadioS" (radio broadcast automation: C11/GTK3 desktop app + Python "SDBASE" HTTP data service) from Linux to a PURE, industrial-grade Windows 10/11 x64 application. Write all reports in English; Turkish UI texts in code stay Turkish.

Project root (edit here): ${ROOT}

MANDATORY READING before anything else:
0. ${SPECS}/ENV_CLOUD.md — the work moved from the user's Windows PC to a Linux cloud container: path mapping, the MSYS2 GCC 16 toolchain under Wine, the standalone compile recipe, 'wrun' to run Windows programs/tests, Wine fidelity limits and the triage rule (WINE-GAP). It overrides the Windows shell/paths in the other documents.
1. ${SPECS}/PLATFORM_FACTS.md — facts measured on the user's PC (toolchain, time zones, fonts, Python runtime, WASAPI headers, NEVER open real MIDI devices, audio tests at most -60 dBFS).
2. ${SPECS}/DECISIONS.md — BINDING lead decisions: Windows only, no Linux code paths (D1); fix industrial-grade defects (D2); no useless leftovers (D2b); platform contract (D3); layout (D4); cross-language rules (D6); build rules (D7); test audio backends (D8).
3. Your area spec(s) listed below (detailed; DECISIONS.md wins on conflicts).

Working rules:
- Other agents work in parallel right now (the Python port agents read include/radios/platform.h; build-system agents write CMakeLists.txt/cmake/ and configure ${ROOT}/build). Edit ONLY the files you own. If you need something changed elsewhere, put it in requests_for_other_areas. If an Edit fails because a file changed since you read it, re-read and retry.
- CMake may not be ported yet: verify with the standalone recipe of ENV_CLOUD.md and your own small test executables in your private scratch dir, run with wrun. Python only via ${ROOT}/runtime/python/python.exe through wrun.
- Do not leave TODO stubs. Preserve public contracts and Turkish user-facing texts (rewrite only Linux-specific wording). Keep the project's code style.
- Report honestly: what you verified by running under Wine, what you only compiled, what only the user's Windows PC can verify (real devices, ICU, ACL enforcement), what is left.
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
          evidence: { type: 'string', description: 'how you know: compiler output, test run, probe, code reasoning with exact lines' },
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
    owns: `include/radios/platform.h, src/core/platform.c, src/core/platform_win32.c, src/core/platform_tz.c, tests/test_platform.c, ${ROOT}/runtime/share/zoneinfo/**,
PLUS the time-zone migration lines (ONLY the time-zone lines and the #include "radios/platform.h" line) in: src/core/weekly_programme_plan.c, src/core/automation_calendar_plan.c, src/core/loudness_policy.c, src/playout/automation_policy.c, src/ui/audio_ui.c, src/ui/header_status.c, src/ui/header_navigation.c, src/ui/automation_calendar.c, src/ui/broadcast.c, src/ui/local_event_prepare.c, src/ui/instruments.c, src/ui/player_rotation.c, src/ui/probability_editor.c, src/ui/schema_track_picker.c, and the tests/*.c files whose time-zone calls were migrated. Do NOT touch src/audio/** or src/core/midi.c.`,
    prior_review_notes: 'The previous reviewer was interrupted after only reading the mandatory documents; start from scratch. The implementer verified on the real Windows PC (31 GLib tests passed there, including ICU and ACL enforcement). Here, re-run test_platform under Wine and separate real defects from WINE-GAPs (no icu.dll, no DACL enforcement) with probes.',
  },
  {
    key: 'wasapi-audio',
    specs: ['audio-backend.md (all)', 'audio-routing-capture.md (to judge whether the shared WASAPI layer is a sufficient base for per-application capture, endpoint loopback, capture devices and channel inputs in the next phase)', 'audio-misc-media.md sections 4.6 and 5.3 (engine.c threads/clocks/time zone/stack arrays/realtime hooks)'],
    owns: `src/audio/wasapi_device.h, src/audio/wasapi_device.c, src/audio/wasapi_backend.c (replaced src/audio/pulse_backend.c, deleted; see git show main: for history), src/audio/audio_internal.h, src/audio/engine.c, include/radios/audio.h and include/radios/audio_runner.h (only if the contract really needs it), tests/test_wasapi_backend.c (tests/test_pulse_hotplug.c was deleted), plus pure symbol-rename lines (radios_pulse_backend_* -> radios_audio_backend_*, RadioSPulseBackend/RadioSPulseSnapshot -> RadioSAudioBackend/RadioSAudioBackendSnapshot) in other src/ or tests/ files.`,
    prior_review_notes: 'The previous reviewer was interrupted after reading the docs, wasapi_device.c and diffing engine.c/headers against the Linux originals, just before compiling. Redo every check yourself. Wine has no audio endpoints: the fake-provider / fault-injection / no-hardware cases of test_wasapi_backend must run here; real-device cases are for the PC (they must skip cleanly here). Compile engine.c and the tests for real; build scratch link stubs only where the rest of radios_audio is not ported yet.',
  },
  {
    key: 'midi-winmm',
    specs: ['midi.md (all)'],
    owns: `src/core/midi.c (WinMM backend), include/radios/midi.h, src/ui/midi_ui.c (only Linux-specific labels/ids/behaviour), tests/test_midi.c, tests/test_midi_hotplug.c (injectable fake WinMM backend).`,
    prior_review_notes: 'The previous reviewer (interrupted, on the PC) had built everything at O0 and O2 cleanly, the suite passed 3/3 rounds, confirmed the test executables do not import WINMM/CFGMGR32 (fake backend isolation), and had started MUTATION tests (mutants: no-removal-lose, same-device-never, storm, keep-handle) to see whether the hotplug tests catch injected bugs — results unknown and the scripts are lost. Redo: build, run the suites under Wine (no MIDI devices exist here, which is fine for the fake backend), check import isolation with objdump, and finish a mutation analysis yourself (a test suite that does not kill a meaningful mutant is a finding). Never open real MIDI devices (none exist here anyway).',
  },
]

const implReport = (a) => `${REPORTS}/${a.key}.impl.json`

const review = (a) => agent(
  COMMON + `\nYou are the ADVERSARIAL REVIEWER for area "${a.key}". Another engineer implemented it on the Windows PC; their full structured report (JSON) is in ${implReport(a)} — READ IT FIRST.\n\nNote from the lead: ${a.prior_review_notes}\n\nSpecs: ${a.specs.join('; ')} (in ${SPECS}). Owned files of that area:\n${a.owns}\n\nYour job: find real defects, do not rubber-stamp. Do NOT edit project files (read-only review; build/run only in ${SP}/work/review-${a.key}/). Check: (1) every owned file compiles with the strict D7 flags (actually compile them with the recipe of ENV_CLOUD.md); (2) the tests the implementer wrote build and pass when YOU run them under Wine, and they really test what they claim (classify Wine-only failures with a probe); (3) spec + DECISIONS compliance, especially D1 (no Linux code paths left anywhere in the owned files), D3/D6 contracts; (4) Windows correctness by reading against Microsoft documentation: handle/COM reference leaks, wrong share modes, missing FILE_SHARE_DELETE, text-mode opens, 32-bit long/off_t truncation, wrong error mapping, thread-affinity violations (GTK/GLib main context, COM apartments), races and deadlocks (callbacks under locks, WinMM/WASAPI callback restrictions), unbounded waits, missing error paths, UTF-8/UTF-16 conversion bugs, MAX_PATH, quoting/injection in command lines, security (ACL/SDDL), ICU dynamic loading failure handling; (5) industrial robustness. Report each finding with severity, exact location, evidence and a concrete fix. verdict = "pass" only if no critical/high/medium findings remain.`,
  { label: `review:${a.key}`, phase: 'Review', schema: REVIEW_SCHEMA })

const fix = (rev, a) => {
  const serious = (rev && rev.findings || []).filter(f => ['critical', 'high', 'medium'].includes(f.severity))
  if (!rev || serious.length === 0) return Promise.resolve({ area: a.key, skipped: true, reason: rev ? 'review passed' : 'no review', review: rev, fix: null })
  return agent(
    COMMON + `\nYou own area "${a.key}" now (same file ownership as below) and must FIX the confirmed review findings. Owned files:\n${a.owns}\n\nImplementer report (JSON file, read it): ${implReport(a)}\n\nReview findings to fix (critical/high/medium; also fix low ones if cheap):\n${JSON.stringify(rev.findings, null, 2)}\n\nFor each finding: verify it is real (if you are certain it is not, say why), fix it, then rebuild and re-run the area's tests under Wine (private scratch dir ${SP}/work/fix-${a.key}/). Return the structured report (area = "${a.key}") listing what you changed and the verification you ran; put any finding you rejected (with reason) in open_issues.`,
    { label: `fix:${a.key}`, phase: 'Fix', schema: IMPL_SCHEMA }).then(r => ({ area: a.key, review: rev, fix: r }))
}

const results = await pipeline(AREAS, review, fix)
const failed = AREAS.filter((a, i) => !results[i] || (results[i].review == null) || (!results[i].skipped && results[i].fix == null))
if (failed.length) {
  log('foundation-A NOT complete for: ' + failed.map(a => a.key).join(', '))
  return { complete: false, failed: failed.map(a => a.key), results }
}

phase('Summary')
const summary = await agent(COMMON + `\nFoundation-A (platform layer, WASAPI audio core/backend, WinMM MIDI) is now implemented, adversarially reviewed and fixed. Implementer reports: ${REPORTS}/platform-layer.impl.json, ${REPORTS}/wasapi-audio.impl.json, ${REPORTS}/midi-winmm.impl.json. Review + fix results:\n${JSON.stringify(results, null, 2)}\n\nYour job (do NOT edit project source files): write ${REPORTS}/FOUNDATION_SUMMARY.md for the 9 C-port agents that build on this foundation next (areas: c-core, c-ui-app, c-ui-files, c-ui-sweep, c-audio-routing, c-audio-media, c-audio-virtual, c-tests-support, c-tests-b). Read the ACTUAL headers on disk (include/radios/platform.h, src/audio/wasapi_device.h, src/audio/audio_internal.h, include/radios/midi.h, include/radios/audio.h) and document: files that exist now; the public APIs with exact signatures grouped by purpose (files/fd ops and RADIOS_O_* flags, RadioSFileInfo, locks, privacy, paths, realpath, process spawn/watch/stop/communicate, console/argv/logging/startup/crash handler, time zones, WASAPI enumeration/streams/loopback/process loopback/sessions/notifications, audio backend contract and provider seam, MIDI); conventions (errno vs GError, UTF-8 paths, ownership/free rules, thread rules); link libraries; test status (what passes on the PC and under Wine); remaining open issues and the consolidated requests for other areas, grouped by which C-port area should handle them. Then return the text of a concise version (max ~6000 characters) of that summary.`,
  { label: 'foundation-summary', phase: 'Summary' })

return { complete: true, foundation: results, summary }
