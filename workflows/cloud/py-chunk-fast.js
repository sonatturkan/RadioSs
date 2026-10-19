export const meta = {
  name: 'radios-py-chunk-fast',
  description: 'RadioS Python port, write-only pass (review deferred to the end): port SDBASE engine, SDBASE application and tools/launchers to Windows on the finished shim',
  phases: [
    { title: 'Port', detail: 'engine modules, application modules, tools/launchers (disjoint files); review deferred to the final audit' },
  ],
}

const ROOT = '/home/user/RadioSs'
const SP = '/opt/radios-port'
const SPECS = SP + '/specs'
const PY = ROOT + '/runtime/python/python.exe'
const PYRUN = `wrun ${PY} -E -s -B -X utf8`

const COMMON = `
You are a senior Python/Windows engineer on the team porting "RadioS" (radio broadcast automation: C11/GTK3 desktop app + the Python "SDBASE" HTTP data service in database/SDBASE/src/sdbase + Python tools/launchers in tools/) from Linux to a PURE, industrial-grade Windows 10/11 x64 application. Write reports in English.

Project root (edit here): ${ROOT}

MANDATORY READING before you write code:
0. ${SPECS}/ENV_CLOUD.md — the work moved from the user's Windows PC to a Linux cloud container: path mapping, running the real Windows CPython 3.14.8 (embeddable + tzdata) under Wine with 'wrun', Wine fidelity limits (no DACL enforcement, partial POSIX-semantics rename, no icu.dll) and the WINE-GAP triage rule. It overrides the Windows shell/paths in the other documents.
1. ${SPECS}/PLATFORM_FACTS.md (Python runtime, -E -s -B -X utf8, tzdata, cp1254 default-encoding trap, CRLF trap).
2. ${SPECS}/DECISIONS.md (BINDING: D1 Windows only + convert-don't-drop, D2/D2b industrial fixes and no useless leftovers, D4 python interpreter/env, D6 cross-language rules: lock region 0x4000000000000000 len 1, private SDDL + checks, named stop event Local\\RadioS-stop-<pid>, identity = pid + creation time, portable '/' relative paths, newline="\\n"+utf-8, source digest rule; D12 user data; D13 line endings).
3. ${SPECS}/core-posix-files.md section 5 (cross-language contracts C<->Python) and ${ROOT}/include/radios/platform.h + src/core/platform*.c (the C side; a reviewer/fixer may adjust it in parallel right now; constants/semantics must match).
4. Your area spec(s) listed below.

Working rules:
- Python interpreter: ONLY ${PY} through wrun, always with -E -s -B -X utf8, i.e. \`${PYRUN} ...\`. Linux python3 is only for host-side helper scripts, never to run project code or tests.
- Windows only (D1): remove fcntl/pwd/grp/resource imports, os.getuid, signal.SIGTERM-based control, /proc, pidfd, start_new_session, preexec_fn, os.statvfs, os.sync, O_NOFOLLOW/O_DIRECTORY/O_NONBLOCK usage etc. Replace each with the shim or a correct Windows construct. No "if os.name == 'posix'" branches left. Behaviour and wire/file formats stay the same (the Windows app must open data created on Linux).
- Byte exactness: every text write whose bytes matter uses newline="\\n" and encoding="utf-8"; every read of hashed/compared content is binary or explicit utf-8. Never rely on locale encoding.
- Edit ONLY the files you own. Other agents edit other files now. Requests for other areas go into requests_for_other_areas. Never run git commands that change the repository.
- Verify by actually running code: import every module you own (cwd=${ROOT}/database/SDBASE/src), start a private SDBASE service on a free port in a temp directory when relevant, and run the relevant existing backend tests in ${ROOT}/tests (test_*.py) with the Windows CPython under Wine — if a test fails only because of Linux assumptions inside the TEST file, do not edit it (tests are ported later), just record it. Classify Wine-only failures with a probe (WINE-GAP).
- Never write into ${ROOT}/data or real config files; use temp copies under ${SP}/work/py-<area>/ or /tmp.
- FAST WRITE-ONLY PASS (user decision): the user wants the Windows port WRITTEN as fast as possible and will build, test and review on their own Windows PC. So: port every file you own completely and correctly (no TODO stubs, no shortcuts in behaviour), then import each module you changed ONCE with the Windows CPython under wrun (python -E -s -B -X utf8 -c "import ...") and fix import/syntax errors. Do NOT run tests, do NOT write probes or harnesses, do NOT run programs under Wine, do NOT do self-review passes. Work fast; list anything you could not finish or are unsure about in open_issues.
- CONTINUATION: earlier agents for this area may have been stopped by the lead (plan change, not a quality problem) after editing some of your files. FIRST run 'git diff HEAD --stat -- <your files>' and read the existing changes; continue from them, do not redo or revert finished work.
- Industrial quality (D2): fix real defects you meet (races, unbounded waits, swallowed errors, unsafe paths/path traversal, Windows sharing-violation windows, crash-consistency gaps). List them.
- Machine: 4 CPUs shared with other agents. Keep parallel test processes modest.
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

const ENGINE_FILES = 'storage.py, disk_index.py, endpoint_journal.py, migration.py, failover.py, failover_server.py, integrity.py, online_maintenance.py, ops_scheduler.py, ops_archive.py, replication.py, replication_stream.py, wal_archive.py, pitr.py, backup.py, media.py, retention.py, portable_export.py, portable_import.py, snapshot_contract.py, read_snapshot.py, store_health.py, store_provision.py, capacity_monitor.py, transport.py, http_pool.py, http_limits.py, identity.py'

const shim = args && args.shim
if (!shim) return { error: 'shim report missing (pass args.shim)' }

const AREAS = [
  { key: 'py-engine', specs: 'python-sdbase-engine.md (all)', owns: `database/SDBASE/src/sdbase/{${ENGINE_FILES}} and database/SDBASE/tools/*.py (SDBASE acceptance/benchmark tools)`,
    task: 'Port every engine module to Windows on the shim exactly as the engine spec migration table says (locks, replace/rename while open, directory fsync, O_* flags + O_BINARY, link semantics, statvfs, chmod/fchmod/umask/st_uid checks -> private SDDL rules, mmap/sync, path separators stored in records/digests (relative keys stay "/"), CRLF in byte-exact files). Make sure a database created on Linux (journal format sdbase-journal-v1, objects dir layout) opens unchanged. Verify under Wine: import all modules; create a store in a temp dir, write/read/checkpoint/compact, run integrity, backup + verify + restore-plan, concurrent writer lock contention from a second process, crash-consistency (kill the writer mid-write with TerminateProcess and reopen); run the existing engine-related tests and SDBASE acceptance tools that do not need audio/GUI.' },
  { key: 'py-app', specs: 'python-sdbase-app.md (all)', owns: `every other file in database/SDBASE/src/sdbase/*.py (all modules NOT in this list: ${ENGINE_FILES}, and not _platform.py), plus database/SDBASE/pyproject.toml if needed`,
    task: 'Port all application/service modules per the app spec: http_server/api_server (bind 127.0.0.1 only, SO_EXCLUSIVEADDRUSE instead of SO_REUSEADDR semantics, ThreadingHTTPServer on Windows, timeouts), graceful shutdown on the named stop event (replacing SIGTERM handlers) with orderly durable flush, credentials/private key creation with the private SDDL instead of 0600/O_NOFOLLOW/getuid, desktop_media dir_fd opens, rekordbox USB/SD device discovery on Windows drive letters/removable volumes instead of /media,/run/media mounts, importers/exporters accepting Windows paths in playlists (M3U8/CSV with drive letters and backslashes, file:/// URIs), subprocess calls to cli/tools via the shim argv/env, path separators in API payloads (portable "/" for relative paths), file:// URI handling, time zones via zoneinfo+bundled tzdata. Verify under Wine: import every module; start the service on a free port in a temp project skeleton with the tools agent conventions, exercise the HTTP API (health/identity/a write+read round trip), stop it via the named stop event and confirm a clean shutdown; run the existing desktop backend tests (tests/test_desktop_*.py and similar) and report pass/fail per file with root causes.' },
  { key: 'py-tools', specs: 'python-tools-launcher.md (all); ui-platform-c.md section 4 (how the C app will call the tools)', owns: 'tools/*.py except tools/radios_platform.py (owned by the shim agent), tools/otomasyon_testleri/**, tools/yayin_akisi_724/**, run-radios.sh, run-database.sh, run-radios-sdk.sh, new Windows launchers in the project root (run-radios.ps1, run-radios.cmd, run-database.ps1, run-database.cmd), config/radios.example.ini, config/README.md, tools/README.md',
    task: 'Port the launch + service lifecycle tooling per the tools spec: radios_launch.py, radios_database.py (init/start/check/status/backup/stop with Windows process identity, detached spawn with log redirection, named-event stop + bounded fallback TerminateProcess, server.lock via the shim, port checks), radios_database_setup.py (credentials/private files/portable relative paths), radios_reliability.py, radios_package.py (redesign for Windows: produce the portable runtime folder + NSIS installer inputs; keep its hashing/manifest safety; the build-system agents write cmake/RadioSRuntime.cmake, cmake/RadioSInstaller.cmake, packaging/windows/** and build-windows.ps1 in parallel — align with them, do not edit their files), radios_hardware_check.py (CONVERT the pactl inventory to a Windows inventory via a small ctypes/comtypes-free MMDevice enumeration or by asking the C app), css_to_gtk.py (build-time, must work with the Turkish path design/reference/tasarrım and emit \'Inter Variable Text\' first in font-family lists per D5), islev_envanteri.py, gui_backend_coverage.py, tools/otomasyon_testleri (replace build.sh with a .ps1 or Python builder), tools/yayin_akisi_724. DELETE what has no function on Windows (tools/radios_sdk.py and run-radios-sdk.sh: flatpak SDK path) and replace run-radios.sh / run-database.sh with .ps1 + .cmd launchers offering the same subcommands and options (they call runtime\\\\python\\\\python.exe -E -s -B -X utf8 tools\\\\... and find the RadioS root from their own location; PowerShell 5.1 compatible; syntax-check every .ps1 with pwsh\'s parser). Update config/radios.example.ini (python_command default resolves to runtime/python/python.exe per D4) and the READMEs for Windows. Verify under Wine: run radios_database.py init/start/status/check/stop end-to-end in a temp copy of the project skeleton, check that stop works via the named event and that a second start does not duplicate the service, run css_to_gtk.py into a temp output dir and check the output, run the tools-related tests (test_database_setup.py, test_radios_launch.py, test_reliability.py, test_storage_locations.py, test_packaging.py) and report.' },
]

const port = (a) => agent(COMMON + `\nAREA: ${a.key}\nThe shim is done. Shim agent report:\n${JSON.stringify(shim, null, 2)}\nSpecs: ${a.specs} (in ${SPECS}).\nFILES YOU OWN: ${a.owns}\nTASK: ${a.task}\nReturn the structured report (area = "${a.key}").`,
  { label: `port:${a.key}`, phase: 'Port', schema: IMPL_SCHEMA })

const CHUNK_NOTE = `
CHUNK MODE: this area was split into small chunks so many agents work in parallel. You own EXACTLY the files listed below (plus include/radios/<same-base-name>.h of one of your .c files when only it needs changing). Every other file belongs to another agent working right now: do not edit it; put needed changes in requests_for_other_areas. To be fast, do NOT read the large specs end to end: read ENV_CLOUD.md and DECISIONS.md, skim PLATFORM_FACTS.md, then grep the area spec(s) for your file names and read only those sections plus the general migration rules/tables.
`
const chunkAgent = (c) => {
  const parent = AREAS.find(a => a.key === c.parent)
  return agent(COMMON + CHUNK_NOTE + `\nAREA: ${c.key} (chunk of ${c.parent})\nThe shim is done. Shim summary:\n${JSON.stringify(shim, null, 2)}\nSpecs: ${parent.specs} (in ${SPECS}).\nFILES YOU OWN (exactly):\n${c.files.join('\n')}\nTASK (of the whole parent area; do it for YOUR files only, skip verification steps that need other chunks):\n${parent.task}\nReturn the structured report (area = "${c.key}").`,
    { label: `port:${c.key}`, phase: 'Port', schema: IMPL_SCHEMA })
}
log('chunks: ' + args.chunks.map(c => c.key).join(', '))
return await parallel(args.chunks.map(c => () => chunkAgent(c)))
