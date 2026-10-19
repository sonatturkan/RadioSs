export const meta = {
  name: 'radios-windows-build-system-resume',
  description: 'Resume the build system: CMake port to MSYS2/Windows, runtime staging + NSIS installer, minimal LGPL FFmpeg 9.0.2, each adversarially reviewed and fixed',
  phases: [
    { title: 'Implement', detail: 'cmake-port, runtime-packaging, ffmpeg-minimal in parallel (disjoint files); restarted after the session closed' },
    { title: 'Review', detail: 'adversarial reviewer per area: configure/build/install for real' },
    { title: 'Fix', detail: 'apply confirmed findings' },
  ],
}

const ROOT = 'C:\\Users\\sonatturkan\\Desktop\\RadioS'
const SP = 'C:\\Users\\sonatturkan\\AppData\\Local\\Temp\\claude\\c--Users-sonatturkan-Desktop-radyo-projesi-kaynak-kodlar-RadioS\\0ead7b3c-7fb3-425d-a304-a7629cf56b77\\scratchpad'
const SPECS = SP + '\\specs'

const COMMON = `
You are a senior build/release engineer on the team porting "RadioS" (radio broadcast automation: C11/GTK3 desktop app + Python "SDBASE" HTTP data service) from Linux to a PURE, industrial-grade Windows 10/11 x64 application.

Project root (edit here): ${ROOT}
Original Linux copy (read-only reference): C:\\Users\\sonatturkan\\Desktop\\radyo projesi\\kaynak kodlar\\RadioS

MANDATORY READING: ${SPECS}\\PLATFORM_FACTS.md, ${SPECS}\\DECISIONS.md (BINDING; note D7 build rules, D8 audio backends for tests, D9 GUI test input, D10 minimal LGPL FFmpeg 9.0.2, D11 debug info), ${SPECS}\\build-system.md (the detailed, VERIFIED build spec — follow it), ${SPECS}\\gtk-windows-runtime.md (runtime files to bundle, settings.ini, fonts), and for test registration ${SPECS}\\tests-c.md sections 3-4 and ${SPECS}\\python-tools-launcher.md where it concerns packaging/launchers.

Context: other agents are working in parallel. The platform layer (include/radios/platform.h, src/core/platform*.c, tests/test_platform.c), WASAPI (src/audio/wasapi_device.{h,c}, wasapi_backend.c, engine.c, tests/test_wasapi_backend.c) and WinMM MIDI (src/core/midi.c, tests/test_midi*.c) are implemented and are being reviewed/fixed right now; after that a 9-area C port of the rest of src/ and tests/ starts (that code will not compile until then). The Python service/tools are being ported too (tools/radios_package.py is being redesigned by the Python tools agent: read its current state, do not edit it). So: the build must be RIGHT for the final tree; you can only fully build the already-ported pieces now. Make the CMake source lists match the actual files on disk (src/audio/pulse_backend.c is gone; re-check with Glob before finishing because files may still appear: virtual backend, tests/radios_test_support.c, tools/win32/radios-test-desktop.c, tests/test_wasapi_hotplug.c).

Rules: edit ONLY the files you own. Toolchain only from C:\\msys64 (never C:\\MinGW, never PATH python/py). Keep build/install paths short and ASCII (MAX_PATH; narrow-API binutils). Do not touch ${ROOT}\\data or the user's original folder. Industrial quality (D2): reproducible, pinned, verified downloads (SHA-256), no silent fallbacks, clear errors. Report honestly what you ran.
`

const IMPL_SCHEMA = {
  type: 'object',
  properties: {
    area: { type: 'string' }, summary: { type: 'string' },
    files_created: { type: 'array', items: { type: 'string' } },
    files_modified: { type: 'array', items: { type: 'string' } },
    files_deleted: { type: 'array', items: { type: 'string' } },
    api: { type: 'string', description: 'options/presets/targets/commands other areas use' },
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

const AREAS = [
  { key: 'cmake-port',
    prior: 'A previous agent read the specs and probed for ~60 tool calls but wrote nothing to the project before the session closed; start fresh (the build directory is empty). Earlier probes from the analysis phase may help: ' + SP + '\\buildprobe, ' + SP + '\\cfgprobe, ' + SP + '\\cm.',
    owns: 'CMakeLists.txt, cmake/*.cmake EXCEPT cmake/RadioSRuntime.cmake and cmake/RadioSInstaller.cmake (owned by runtime-packaging; you add the include() lines for them), new CMakePresets.json, new src/app/radios.rc, src/app/radios.manifest, src/app/crt_binmode.c, new assets/icons/* (application icon: look for a logo/favicon in design/reference/tasarrım first; otherwise design a clean SVG mark and generate a multi-size ICO 16-256 px with rsvg-convert + a small Python packer you place in tools/), .gitignore, cmake/README.md, and the build directory ' + ROOT + '\\build (you may configure/build there). NOTE: the user initialised a git repository in ' + ROOT + ' (no commits yet); never run git commands that change it.',
    task: 'Port the whole CMake build per build-system.md: pinned toolchain presets (windows-release, windows-debug, windows-release-tests, windows-debug-gui), toolchain guard refusing C:\\MinGW or non-UCRT compilers, delete libpulse/alsa/xtst/x11/fontconfig deps (fontconfig only after verifying no source still uses Fc*), add gio-windows-2.0 and pango>=1.56, global defines (D7 + spec), strict C-only warning flags, the deferred finalize step giving EVERY executable the UTF-8/PerMonitorV2/longPathAware manifest (SMI namespaces!), icon + VERSIONINFO from project(VERSION), crt_binmode object and -Wl,--stack,8388608, RadioS.exe as WIN32_EXECUTABLE (OUTPUT_NAME RadioS), Win32 link libraries per target, --wrap fixes for dllimport symbols (__imp_ variant), the reference-CSS custom command with the pinned MSYS2 python and -X utf8 -B -I, RADIOS_FFMPEG_PREFIX option (pkg-config path) per D10, debug info per D11, and CTest on Windows: remove every GDK_BACKEND=x11, foreign cwd instead of /tmp, Windows PYTHONPATH separators, deferred global test environment (PATH with MSYS2/runtime bin first, TZDIR=<root>/runtime/share/zoneinfo, PYTHONUTF8=1, PYTHONNOUSERSITE=1, RADIOS_AUDIO_BACKEND=none per D8), RADIOS_TEST_PYTHON_COMMAND = <root>/runtime/python/python.exe -X utf8 -B -s, FAIL_REGULAR_EXPRESSION for the GLib "1..0" zero-test plan, GUI tests wrapped with the private desktop launcher target (tools/win32/radios-test-desktop.c, build it as a target when the file exists) per D9, stale test case lists fixed (e.g. midi hotplug cases must match the ported test), Linux-only test registrations replaced by their Windows twins (test_wasapi_backend, test_wasapi_hotplug when present, test_platform). Make the source lists match the files on disk. Verify: configure with the presets; build every target that can compile today (at least the platform library/test_platform, the MIDI module/tests, the WASAPI files) and RUN test_platform and the MIDI tests through ctest; show that a manifest is embedded (GetACP=65001 probe via a tiny test or ctest) and that RadioS.exe target is GUI subsystem when it links later.' },
  { key: 'runtime-packaging',
    prior: 'A previous agent only read the specs (35 tool calls) and wrote nothing before the session closed; start fresh. Earlier GTK runtime probes from the analysis phase: ' + SP + '\\tools-gtkrt, ' + SP + '\\probe_ui.',
    owns: 'new cmake/RadioSRuntime.cmake and cmake/RadioSInstaller.cmake (install components and staging), everything under packaging/windows/ EXCEPT packaging/windows/build-ffmpeg.ps1, packaging/windows/ffmpeg-components.md and packaging/windows/cache/** (inputs; you may read them), packaging/README.md, new build-windows.ps1 + build-windows.cmd in the project root, the staged runtime tree ' + ROOT + '\\runtime\\ EXCEPT runtime\\python (preserve it exactly) and runtime\\share\\zoneinfo (preserve), and the output folder ' + ROOT + '\\dist\\.',
    task: 'Implement the runtime prefix + installer pipeline per build-system.md (sections on runtime prefix, DLL closure, install components, staging, NSIS) and gtk-windows-runtime.md: CMake install rules (components runtime and python) that place RadioS.exe and every dependent DLL (closure via file(GET_RUNTIME_DEPENDENCIES) with MSYS2 and the FFmpeg prefix as search dirs, excluding system DLLs, failing on unresolved ones) into <prefix>\\bin, GTK data (share/glib-2.0/schemas compiled, share/icons Adwaita+hicolor with index.theme and icon caches, lib/gdk-pixbuf-2.0/2.10.0 loaders + regenerated loaders.cache with relative paths, share/locale/tr for GTK/GLib, etc/gtk-3.0/settings.ini tuned for Windows per the runtime spec, gspawn helpers only if still needed), share/zoneinfo (from the tzdata wheel), python\\ (embeddable 3.14.8 without ._pth + tzdata; reuse runtime\\python or rebuild it from packaging/windows/cache with hash checks), the RadioS resources the app needs at runtime inside the installed prefix (config/radios.example.ini, database/SDBASE/src + pyproject.toml, tools/*.py the app spawns, assets/, an empty data/ skeleton), an in-use-safe staging/sync script that refreshes <root>\\runtime from a fresh install stage without deleting runtime\\python/zoneinfo and refusing to overwrite while RadioS.exe runs, a hand-written NSIS 3.13 script (Target amd64-unicode, per-user default %LOCALAPPDATA%\\Programs\\RadioS, Turkish + English UI, Start menu + optional desktop shortcut, uninstaller that NEVER removes data\\ or user config, upgrade-in-place, version info, refuses to install while RadioS.exe runs), a portable ZIP option, and build-windows.ps1 (configure, build, optional tests, install-stage, sync runtime, installer; pinned MSYS2 tools; clear Turkish/English messages; non-zero exit on any failure). Verify what can run today: the GTK runtime staging part with an already-buildable small GTK test program (e.g. build a tiny GTK window exe in your scratch dir, install it with the same DLL-closure logic into a temp prefix, run it from a clean PATH (only C:\\Windows\\System32) and show it starts and finds schemas/icons/pixbuf loaders/fonts), makensis compiles the script against a staged temp prefix, and the uninstaller keeps data\\. Never install anything into the user\'s real %LOCALAPPDATA%\\Programs during verification unless you uninstall it again and report it; prefer silent installs into a temp dir (/D=).' },
  { key: 'ffmpeg-minimal',
    prior: 'A previous agent already (a) installed the MSYS2 packages it needed via pacman (dry run showed exactly two packages, no dependency changes — check which with pacman -Q mingw-w64-ucrt-x86_64-lame mingw-w64-ucrt-x86_64-nasm), (b) set up the workspace C:\\msys64\\tmp\\radios-ffmpeg (dl\\, gnupg-probe\\, probe-dl\\, src\\), (c) downloaded the official ffmpeg-9.0.2 release and verified it: SHA-256 independently matches the pin in MSYS2\'s PKGBUILD for 9.0.2-1 (8c3850283eb2...6e002e — recompute and record the full hash yourself), signature probe in gnupg-probe\\, (d) checked that the MSYS2 patches are irrelevant (frei0r/pathtools only), and (e) was extracting the source into src\\ to inspect configure component lists when the session closed. Reuse that work after re-verifying the hash of the tarball on disk; finish everything else.',
    owns: 'new packaging/windows/build-ffmpeg.ps1 (and any helper .sh it runs inside MSYS2), the prefix it produces at packaging/windows/cache/ffmpeg-9.0.2-lgpl-radios/, its download/build workspace C:\\msys64\\tmp\\radios-ffmpeg (keep it OUTSIDE the project), and new packaging/windows/ffmpeg-components.md documenting the component list and licence.',
    task: 'Per D10, build a minimal LGPL-2.1+ shared FFmpeg 9.0.2 (exactly the version of the MSYS2 headers: confirm with pkg-config --modversion libavformat = 63.1.102) for RadioS. Steps: (1) derive the exact component list from the code: grep src/ and tools/ for avformat/avcodec/avfilter/swresample usage (av_find_input_format, avcodec_find_decoder(_by_name), avcodec_find_encoder(_by_name), avfilter_get_by_name, filter graph strings, AV_CODEC_ID_*, codec names) and read docs/07-medya-kutuphanesi.md + src/media for the supported import format matrix; include the needed demuxers, decoders, parsers, bitstream filters, filters, the file/pipe protocols, swresample, and libmp3lame for MP3 encoding; no GPL/nonfree, no video encoders, no hardware/Vulkan, no network protocols unless the code uses them. (2) Use the verified ffmpeg-9.0.2.tar.xz (never skip verification). (3) Configure with --disable-everything + explicit --enable-* lists, --enable-shared --disable-static --disable-programs --disable-doc --disable-network (unless needed) --enable-libmp3lame --arch=x86_64 --target-os=mingw32 --prefix=<cache prefix>, build with make -j8 inside MSYS2 UCRT64, install. (4) Verify: the DLL set and total size, `objdump -p` imports (no GPL libs, only system + libmp3lame + zlib if used), license check via avcodec_license(), compile src/audio/decoder.c and src/media/analyzer.c objects against the new prefix headers (they may not compile yet if the C port has not reached them; then compile a probe that uses the same APIs), and write + run a small C test in your scratch dir that decodes one generated fixture of every supported format (generate fixtures with C:\\msys64\\ucrt64\\bin\\ffmpeg.exe) and encodes a short MP3 with libmp3lame through the new DLLs. Make build-ffmpeg.ps1 idempotent (skip when the prefix with matching manifest exists) and log to a file.' },
]

const implement = (a) => agent(COMMON + `\nAREA: ${a.key}\nPRIOR STATE: ${a.prior}\nFILES YOU OWN:\n${a.owns}\n\nTASK:\n${a.task}\n\nPrivate scratch dir: ${SP}\\work\\${a.key}\\ . Return the structured report (area = "${a.key}").`,
  { label: `implement:${a.key}`, phase: 'Implement', schema: IMPL_SCHEMA })

const review = (impl, a) => agent(COMMON + `\nYou are the ADVERSARIAL REVIEWER for area "${a.key}". Implementer report:\n${JSON.stringify(impl, null, 2)}\nOwned files: ${a.owns}\nRead-only for project files (you may configure/build/install into your own scratch dirs ${SP}\\work\\review-${a.key}\\ and copies). Re-run the implementer's verification yourself and try to break it: configure from a clean build dir with each preset, check the toolchain guard really rejects C:\\MinGW, manifests (namespaces, activeCodePage effective), stack/subsystem flags, CTest environment and zero-test-plan guard, DLL closure completeness from a clean PATH, GTK runtime data, NSIS install/uninstall/upgrade behaviour keeping data\\, licence/size of FFmpeg and that decoding/encoding works, hash-pinned downloads, idempotency, MAX_PATH/non-ASCII path hazards, anything Linux left. verdict="pass" only if no critical/high/medium findings.`,
  { label: `review:${a.key}`, phase: 'Review', schema: REVIEW_SCHEMA })

const fix = (rev, a, impl) => {
  const serious = (rev && rev.findings || []).filter(f => ['critical', 'high', 'medium'].includes(f.severity))
  if (!rev || serious.length === 0) return Promise.resolve({ area: a.key, impl, review: rev, fix: null })
  return agent(COMMON + `\nYou own area "${a.key}" (files: ${a.owns}) and must FIX these confirmed review findings (also cheap low ones):\n${JSON.stringify(rev.findings, null, 2)}\nImplementer report:\n${JSON.stringify(impl, null, 2)}\nVerify each is real, fix, re-run the verification. Rejected findings (with reason) go to open_issues. Return the structured report (area = "${a.key}").`,
    { label: `fix:${a.key}`, phase: 'Fix', schema: IMPL_SCHEMA }).then(r => ({ area: a.key, impl, review: rev, fix: r }))
}

return await pipeline(AREAS, implement, review, fix)
