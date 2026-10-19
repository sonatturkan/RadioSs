export const meta = {
  name: 'radios-windows-python-port-resume',
  description: 'Resume the Python port: finalize the interrupted _platform shim, then port engine/app/tools on it, each adversarially reviewed and fixed',
  phases: [
    { title: 'Shim', detail: 'finalize sdbase/_platform.py + tools/radios_platform.py + tests (already written, 53 tests passed before the interruption)' },
    { title: 'Port', detail: 'engine modules, application modules, tools/launchers in parallel (disjoint files)' },
    { title: 'Review', detail: 'adversarial reviewer per area: run service + backend tests on Windows, hunt POSIX leftovers' },
    { title: 'Fix', detail: 'apply confirmed findings, re-run tests' },
  ],
}

const ROOT = 'C:\\Users\\sonatturkan\\Desktop\\RadioS'
const SP = 'C:\\Users\\sonatturkan\\AppData\\Local\\Temp\\claude\\c--Users-sonatturkan-Desktop-radyo-projesi-kaynak-kodlar-RadioS\\0ead7b3c-7fb3-425d-a304-a7629cf56b77\\scratchpad'
const SPECS = SP + '\\specs'
const PY = ROOT + '\\runtime\\python\\python.exe'

const COMMON = `
You are a senior Python/Windows engineer on the team porting "RadioS" (radio broadcast automation: C11/GTK3 desktop app + the Python "SDBASE" HTTP data service in database/SDBASE/src/sdbase + Python tools/launchers in tools/) from Linux to a PURE, industrial-grade Windows 10/11 x64 application.

Project root (edit here): ${ROOT}
Original Linux copy (read-only reference, never edit): C:\\Users\\sonatturkan\\Desktop\\radyo projesi\\kaynak kodlar\\RadioS

MANDATORY READING before you write code:
1. ${SPECS}\\PLATFORM_FACTS.md (measured facts: Python runtime, -E -s -B -X utf8, tzdata, cp1254 default encoding trap, CRLF trap).
2. ${SPECS}\\DECISIONS.md (BINDING: D1 Windows only + convert-don't-drop, D2/D2b industrial fixes and no useless leftovers, D4 python interpreter/env, D6 cross-language rules: lock region 0x4000000000000000 len 1, private SDDL + checks, named stop event Local\\RadioS-stop-<pid>, identity = pid + creation time, portable '/' relative paths, newline="\\n"+utf-8, source digest rule).
3. ${SPECS}\\core-posix-files.md section 5 (cross-language contracts C<->Python) and ${ROOT}\\include\\radios\\platform.h (the C side; a reviewer/fixer may adjust it in parallel right now; constants/semantics must match).
4. Your area spec(s) listed below.

Working rules:
- Python interpreter: ONLY ${PY} (CPython 3.14.8 embeddable + tzdata), always with -E -s -B -X utf8. Never PATH python/py.exe (they resolve to an unrelated 3.15 install).
- Windows only (D1): remove fcntl/pwd/grp/resource imports, os.getuid, signal.SIGTERM-based control, /proc, pidfd, start_new_session, preexec_fn, os.statvfs, os.sync, O_NOFOLLOW/O_DIRECTORY/O_NONBLOCK usage etc. Replace each with the shim or a correct Windows construct. No "if os.name == 'posix'" branches left. Behaviour and wire/file formats stay the same (the Windows app must be able to open data created on Linux).
- Byte exactness: every text write whose bytes matter uses newline="\\n" and encoding="utf-8"; every read of hashed/compared content is binary or explicit utf-8. Never rely on locale encoding.
- Edit ONLY the files you own. Other agents edit other files now. Requests for other areas go into requests_for_other_areas.
- Verify by actually running code with ${PY}: import every module you own (python -X utf8 -c "import sdbase.<mod>" with cwd=${ROOT}\\database\\SDBASE\\src), start a private SDBASE service on a free port in a temp directory when relevant, and run the relevant existing backend tests in ${ROOT}\\tests (test_*.py) — if a test fails only because of Linux assumptions inside the TEST file, do not edit it (tests are ported later), just record it.
- Never touch ${ROOT}\\data (the user's real data) or the real config/credentials.json; use temp copies.
- Industrial quality (D2): fix real defects you meet (races, unbounded waits, swallowed errors, unsafe paths/path traversal, Windows sharing-violation windows, crash-consistency gaps). List them.
- Private scratch dir: ${SP}\\work\\py-<area>\\ .
`

const IMPL_SCHEMA = {
  type: 'object',
  properties: {
    area: { type: 'string' },
    summary: { type: 'string' },
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
    area: { type: 'string' },
    verdict: { type: 'string', enum: ['pass', 'needs-fixes'] },
    findings: { type: 'array', items: { type: 'object', properties: {
      severity: { type: 'string', enum: ['critical', 'high', 'medium', 'low'] },
      file: { type: 'string' }, line: { type: 'string' }, problem: { type: 'string' }, evidence: { type: 'string' }, fix: { type: 'string' } },
      required: ['severity', 'file', 'problem', 'evidence', 'fix'] } },
    checks_run: { type: 'array', items: { type: 'string' } },
  },
  required: ['area', 'verdict', 'findings', 'checks_run'],
}

const ENGINE_FILES = 'storage.py, disk_index.py, endpoint_journal.py, migration.py, failover.py, failover_server.py, integrity.py, online_maintenance.py, ops_scheduler.py, ops_archive.py, replication.py, replication_stream.py, wal_archive.py, pitr.py, backup.py, media.py, retention.py, portable_export.py, portable_import.py, snapshot_contract.py, read_snapshot.py, store_health.py, store_provision.py, capacity_monitor.py, transport.py, http_pool.py, http_limits.py, identity.py'

phase('Shim')
const shim = await agent(COMMON + `
AREA: python-shim (FINALIZE — the previous session closed while this agent was finishing)
Specs: ${SPECS}\\python-sdbase-engine.md section 4 and Appendix A (reference _platform.py prototype in ${SP}\\scripts\\proto\\), ${SPECS}\\python-sdbase-app.md section 2 (functions the application modules call), ${SPECS}\\python-tools-launcher.md section 4.3 and the lock/privacy/stop sections, ${SPECS}\\core-posix-files.md section 5.
FILES YOU OWN: database/SDBASE/src/sdbase/_platform.py, tools/radios_platform.py, tests/test_sdbase_platform.py.

STATE: A previous engineer already WROTE all three files (they are on disk) and reached "all 53 tests pass" with ${PY}; they were checking flakiness with repeated runs, the package-style import (import sdbase._platform from src) and a survey of how other modules/tests call the shim (${SP}\\work\\py-python-shim\\survey_tests.py, survey-result.txt, package_import.py, smoke.py) when the session closed. They did not deliver a report.
ORIGINAL TASK (the files must fully satisfy it): the complete Windows-only shim (no POSIX branches): flock-equivalent (LockFileEx/UnlockFileEx via ctypes on msvcrt.get_osfhandle, region 0x4000000000000000 len 1, SH/EX/NB/UN, BlockingIOError on contention), open-flag helper (O_BINARY|O_NOINHERIT always, missing POSIX flags -> 0, no-follow implemented by checking reparse points), fsync_dir no-op, replace/unlink with POSIX-semantics + bounded retry on sharing violations (WinError 5/32), link_noreplace, dir_fd-free safe open helpers, disk_usage, private file/dir creation with the exact SDDL of spec 5.2 and owner/no-others checks matching the C rules, current-user identity, process identity (pid + creation time via OpenProcess/GetProcessTimes, boot-time helper), named stop events (create/wait/signal: Local\\RadioS-stop-<pid>), detached/no-window child spawning (CREATE_NO_WINDOW|DETACHED_PROCESS|CREATE_NEW_PROCESS_GROUP|CREATE_BREAKAWAY_FROM_JOB with fallback, close_fds, handle-list safety), python_child_argv()/python_child_env() per D4, temporary_directory and valid_new_name (Windows file-name rules), portable path helpers (to_portable/from_portable/same_path case-insensitive). tests/test_sdbase_platform.py (unittest): lock SH/EX/NB within and across two processes, private file/dir round trip and a public file rejected, replace over a file held open by another handle, stop event signal delivery to a child process, process identity stability and mismatch, child spawn with no console window and UTF-8 output, Turkish-character paths.
YOUR JOB: read the three files and the specs, complete anything missing or wrong (do not rewrite what works), cross-check every constant and rule against the CURRENT ${ROOT}\\include\\radios\\platform.h and src/core/platform*.c and say explicitly where they match, run the suite 3 times with ${PY} -E -s -B -X utf8 (all must pass, no flakiness), confirm both import paths, finish the call-site survey (which functions the engine/app/tools modules will need; add any missing ones now so the port agents do not have to), and return the structured report (area = "python-shim") with a precise api section (every public function with signature and semantics).`, { label: 'shim-finalize', phase: 'Shim', schema: IMPL_SCHEMA })

log('shim done: ' + (shim ? shim.summary.slice(0, 200) : 'FAILED'))
if (!shim) return { shim: null, results: [], error: 'shim finalize failed; port not started' }

const AREAS = [
  { key: 'py-engine', specs: 'python-sdbase-engine.md (all)', owns: `database/SDBASE/src/sdbase/{${ENGINE_FILES}} and database/SDBASE/tools/*.py (SDBASE acceptance/benchmark tools)`,
    task: 'Port every engine module to Windows on the shim exactly as the engine spec migration table says (locks, replace/rename while open, directory fsync, O_* flags + O_BINARY, link semantics, statvfs, chmod/fchmod/umask/st_uid checks -> private SDDL rules, mmap/sync, path separators stored in records/digests (relative keys stay "/"), CRLF in byte-exact files). Make sure a database created on Linux (journal format sdbase-journal-v1, objects dir layout) opens unchanged. Verify: import all modules; create a store in a temp dir, write/read/checkpoint/compact, run integrity, backup + verify + restore-plan, concurrent writer lock contention from a second process, crash-consistency (kill the writer mid-write with TerminateProcess and reopen); run the existing engine-related tests and SDBASE acceptance tools that do not need audio/GUI.' },
  { key: 'py-app', specs: 'python-sdbase-app.md (all)', owns: `every other file in database/SDBASE/src/sdbase/*.py (all modules NOT in this list: ${ENGINE_FILES}, and not _platform.py), plus database/SDBASE/pyproject.toml if needed`,
    task: 'Port all application/service modules per the app spec: http_server/api_server (bind 127.0.0.1 only, SO_EXCLUSIVEADDRUSE instead of SO_REUSEADDR semantics, ThreadingHTTPServer on Windows, timeouts), graceful shutdown on the named stop event (replacing SIGTERM handlers) with orderly durable flush, credentials/private key creation with the private SDDL instead of 0600/O_NOFOLLOW/getuid, desktop_media dir_fd opens, rekordbox USB/SD device discovery on Windows drive letters/removable volumes instead of /media,/run/media mounts, importers/exporters accepting Windows paths in playlists (M3U8/CSV with drive letters and backslashes, file:/// URIs), subprocess calls to cli/tools via the shim argv/env, path separators in API payloads (portable "/" for relative paths), file:// URI handling, time zones via zoneinfo+bundled tzdata. Verify: import every module; start the service on a free port in a temp project skeleton with the tools agent conventions, exercise the HTTP API (health/identity/a write+read round trip), stop it via the named stop event and confirm a clean shutdown; run the existing desktop backend tests (tests/test_desktop_*.py and similar) and report pass/fail per file with root causes.' },
  { key: 'py-tools', specs: 'python-tools-launcher.md (all); ui-platform-c.md section 4 (how the C app will call the tools)', owns: 'tools/*.py except tools/radios_platform.py (owned by the shim agent), tools/otomasyon_testleri/**, tools/yayin_akisi_724/**, run-radios.sh, run-database.sh, run-radios-sdk.sh, new Windows launchers in the project root (run-radios.ps1, run-radios.cmd, run-database.ps1, run-database.cmd), config/radios.example.ini, config/README.md, tools/README.md',
    task: 'Port the launch + service lifecycle tooling per the tools spec: radios_launch.py, radios_database.py (init/start/check/status/backup/stop with Windows process identity, detached spawn with log redirection, named-event stop + bounded fallback TerminateProcess, server.lock via the shim, port checks), radios_database_setup.py (credentials/private files/portable relative paths), radios_reliability.py, radios_package.py (redesign for Windows: produce the portable runtime folder + NSIS installer inputs; keep its hashing/manifest safety; the build-system agents write cmake/RadioSRuntime.cmake, cmake/RadioSInstaller.cmake, packaging/windows/** and build-windows.ps1 in parallel — align with them, do not edit their files), radios_hardware_check.py (CONVERT the pactl inventory to a Windows inventory, e.g. by asking the C app/WASAPI or via a small ctypes MMDevice enumeration), css_to_gtk.py (build-time, must work with the Turkish path design/reference/tasarrım and emit \'Inter Variable Text\' first in font-family lists per D5), islev_envanteri.py, gui_backend_coverage.py, tools/otomasyon_testleri (replace build.sh with a .ps1 or Python builder), tools/yayin_akisi_724. DELETE what has no function on Windows (tools/radios_sdk.py and run-radios-sdk.sh: flatpak SDK path) and replace run-radios.sh / run-database.sh with .ps1 + .cmd launchers offering the same subcommands and options (they call runtime\\\\python\\\\python.exe -E -s -B -X utf8 tools\\\\... and find the RadioS root from their own location). Update config/radios.example.ini (python_command default resolves to runtime/python/python.exe per D4) and the READMEs for Windows. Verify: run radios_database.py init/start/status/check/stop end-to-end in a temp copy of the project skeleton (never touch the real data/ folder), check that stop works via the named event and that a second start does not duplicate the service, run css_to_gtk.py into a temp output dir and check the output, run the tools-related tests (test_database_setup.py, test_radios_launch.py, test_reliability.py, test_storage_locations.py, test_packaging.py) and report.' },
]

const port = (a) => agent(COMMON + `\nAREA: ${a.key}\nThe shim is done. Shim agent report:\n${JSON.stringify(shim, null, 2)}\nSpecs: ${a.specs} (in ${SPECS}).\nFILES YOU OWN: ${a.owns}\nTASK: ${a.task}\nReturn the structured report (area = "${a.key}").`,
  { label: `port:${a.key}`, phase: 'Port', schema: IMPL_SCHEMA })

const review = (impl, a) => agent(COMMON + `\nYou are the ADVERSARIAL REVIEWER for area "${a.key}". Implementer report:\n${JSON.stringify(impl, null, 2)}\nSpecs: ${a.specs}. Owned files: ${a.owns}.\nRead-only: do not edit project files (you may run anything in ${SP}\\work\\review-${a.key}\\ on copies/temp dirs; never touch ${ROOT}\\data). Find real defects: grep the owned files for every POSIX-ism (fcntl, getuid, O_NOFOLLOW, O_DIRECTORY, O_NONBLOCK, SIGTERM, SIGKILL, start_new_session, preexec_fn, statvfs, os.sync, /proc, pidfd, chmod-based privacy, '/tmp', 'python3', shell=True with sh syntax), text-mode writes without newline="\\n"/encoding, locale-dependent encodings, Windows sharing-violation races (replace/unlink of files open elsewhere), lock-region or SDDL mismatches with include/radios/platform.h and sdbase/_platform.py, process-identity/stop-event correctness, port binding (localhost only, exclusive), path traversal and separator handling, data written on Linux still readable. Actually import the modules and re-run the implementer's checks and the relevant tests with ${PY}. verdict="pass" only if no critical/high/medium findings.`,
  { label: `review:${a.key}`, phase: 'Review', schema: REVIEW_SCHEMA })

const fix = (rev, a, impl) => {
  const serious = (rev && rev.findings || []).filter(f => ['critical', 'high', 'medium'].includes(f.severity))
  if (!rev || serious.length === 0) return Promise.resolve({ area: a.key, impl, review: rev, fix: null })
  return agent(COMMON + `\nYou own area "${a.key}" (files: ${a.owns}) and must FIX these confirmed review findings (also cheap low ones):\n${JSON.stringify(rev.findings, null, 2)}\nImplementer report:\n${JSON.stringify(impl, null, 2)}\nVerify each finding is real, fix, re-run the checks/tests with ${PY}. Rejected findings (with reason) go to open_issues. Return the structured report (area = "${a.key}").`,
    { label: `fix:${a.key}`, phase: 'Fix', schema: IMPL_SCHEMA }).then(r => ({ area: a.key, impl, review: rev, fix: r }))
}

const results = await pipeline(AREAS, port, review, fix)
return { shim, results }
