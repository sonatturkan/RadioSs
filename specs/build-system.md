# Area: build-system — Windows 10/11 x64 (MSYS2 UCRT64 GCC + Ninja) port specification

Scope: `CMakeLists.txt` (1137 lines, read completely), `cmake/*.cmake` (13 modules + README), `.gitignore`,
the `tools/css_to_gtk.py` build step, `packaging/README.md`, plus everything the build must produce on Windows:
compiler/linker flags, resources (manifest/icon/version), CTest registration, the runtime prefix
(`<root>\runtime\`), install/staging rules, the installer, and `CMakePresets.json`.

Binding inputs honoured: `specs/DECISIONS.md` (D1 Windows-only, D2/D2b quality + no leftovers, D3 platform layer,
D4 layout/names, D5 fonts, D6 cross-language rules, D7 build rules) and `specs/PLATFORM_FACTS.md`.
Where the task text and DECISIONS differ, DECISIONS wins: **no Linux branches are kept in CMake** (no
`if(WIN32)`/`else()` pairs), the shipped Python is **3.14.8** (not 3.13), the exe is `RadioS.exe`.

Every statement marked **VERIFIED** was measured on the target machine (MSYS2 at `C:\msys64`, Windows 11 26300,
Turkish locale) with throw-away probes in the scratchpad (`scratchpad\buildprobe`, `scratchpad\cm`,
`scratchpad\cfgprobe`); nothing under `C:\Users\sonatturkan\Desktop\RadioS` was modified. Section 13 lists every probe.

---------------------------------------------------------------------------------------------------

## 0. Decisions in one page

1. **Toolchain is pinned, never discovered**: `C:/msys64/ucrt64/bin/{gcc,windres,ninja,pkg-config,python}.exe`
   via `CMakePresets.json`; a toolchain-check module refuses any other compiler (C:\MinGW WinLibs GCC is on the
   machine PATH), non-UCRT CRTs, non-x64, and warns on non-ASCII/very long build paths.
2. **No Linux libraries**: `libpulse`, `alsa`, `xtst`, `x11` pkg-config checks are deleted; `fontconfig` is deleted
   from the app link once `window.c` drops `Fc*` (D5). Added: `gio-windows-2.0` (GWin32Input/OutputStream for the
   platform process API) and an explicit `pango>=1.56` check (`pango_font_map_add_font_file`, D5).
3. **Global defines** (D7 + this spec): `_FILE_OFFSET_BITS=64 _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE
   NOMINMAX GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_66`. **`__USE_MINGW_ANSI_STDIO` is NOT defined** — proven
   unnecessary (§4.3.2). Strict `-std=c11 -Wall -Wextra -Wpedantic -Werror` stays (C only, via generator expression).
4. **Every executable** (app, 144 test exes, tools) gets, through one deferred finalize function (no edits to the
   ~145 `add_executable` calls): an embedded manifest (activeCodePage UTF-8, PerMonitorV2, longPathAware,
   asInvoker, Win10/11 supportedOS, Common Controls v6), icon + VERSIONINFO, a `_fmode=_O_BINARY` object, and
   `-Wl,--stack,8388608`. Only `radios` (OUTPUT_NAME `RadioS`) is `WIN32_EXECUTABLE` (`-mwindows`).
   **VERIFIED** end-to-end in a mini CMake project (GetACP=65001, PMv2 active, fopen("w") writes LF, 8 MiB reserve).
5. **Manifest namespaces are `http://schemas.microsoft.com/SMI/2005|2016|2019/WindowsSettings`** (`SMI`, not `SM`).
   A manifest with the wrong namespace parses but is silently ignored (**VERIFIED**: GetACP stayed 1254).
6. **mingw-w64's `binmode.o` is an empty stub with UCRT** (**VERIFIED**: no symbols, no effect) — use the project
   object `src/app/crt_binmode.c` (`#undef _fmode` / `int _fmode = _O_BINARY;`).
7. **`--wrap` works on PE for our static-library symbols and GTK symbols, but NOT for GLib/GIO/json-glib/cairo/curl
   functions** (declared `__declspec(dllimport)` → references go to `__imp_<sym>`). Fix: additionally
   `-Wl,--wrap=__imp_<sym>` and define `__wrap___imp_<sym>` as a function pointer (**VERIFIED**). Only one current
   wrap is affected: `g_get_real_time` in `test_header_ui` (`cmake/Pdf2HeaderStatus.cmake:7`).
8. **`-Wl,--gc-sections` on PE does not drop unreferenced code before undefined-symbol checks** (**VERIFIED**); a
   static scan shows the two partial-link test targets have no unresolved references, so the flag stays harmless.
9. **The reference-CSS generator breaks on Windows today** (**VERIFIED** `UnicodeDecodeError` 0x9E, `index.html`
   contains "Ş" etc. and the default locale encoding is cp1254). Fix in two places: invoke Python with
   `-X utf8 -B -I`, and make `css_to_gtk.py` use `encoding="utf-8", newline="\n"`. Ninja + the Turkish path
   `design/reference/tasarrım` is fine (ninja.exe carries a UTF-8 activeCodePage manifest — **VERIFIED**).
10. **Two Pythons**: build-time generators use MSYS2 `python.exe` (pinned; `find_package(Python3)` otherwise picks the
    PyManager 3.15 from the registry — **VERIFIED**); every CTest Python command uses
    `<root>\runtime\python\python.exe -X utf8 -B -s` (D7).
11. **CTest**: remove all 64 `GDK_BACKEND=x11` occurrences (111 tests; with it `gtk_init_check()` fails — **VERIFIED** —
    so the 31 of them that declare `SKIP_RETURN_CODE 77` are silently *skipped* and the other 80 fail),
    replace `WORKING_DIRECTORY /tmp` (3 tests), fix `:`-separated `PYTHONPATH`
    (2 tests); a deferred global step adds `PATH` (MSYS2 bin first), `TZDIR`, `PYTHONUTF8`, `PYTHONNOUSERSITE`,
    `RADIOS_AUDIO_SERVER=null:` (except `hardware`), and a `FAIL_REGULAR_EXPRESSION` for the GLib TAP plan `1..0`
    (`-p /stale/case` runs **zero tests and exits 0** on GLib 2.90 — **VERIFIED** false-pass hazard).
12. **Runtime prefix** `<root>\runtime\` (D4) is produced by `cmake --install --component runtime|python` into a
    stage dir and an in-use-safe sync script; the same install rules feed the NSIS installer. DLL closure via
    `file(GET_RUNTIME_DEPENDENCIES)` (**VERIFIED** with CMake 4.4.4 + objdump; needs `CMP0207 NEW`).
13. **Installer**: hand-written NSIS 3.13 script (`Target amd64-unicode` — the only stubs MSYS2 ships), **per-user**
    default `%LOCALAPPDATA%\Programs\RadioS` (data/config live inside the prefix and must be writable), never
    removes `data\` or user config; CPack is not used.
14. **Licensing/size risk**: MSYS2 FFmpeg 9.0.2 is built `--enable-gpl --enable-version3` (GPLv3) and drags 127 DLLs /
    177.5 MB (incl. x265, aom, whisper/ggml, libplacebo, `vulkan-1.dll`). Recommendation: a minimal LGPL audio-only
    FFmpeg (`RADIOS_FFMPEG_PREFIX`) for shipping; MSYS2 FFmpeg remains fine for development. Lead decision needed.
15. **Release debug info**: `-gcodeview` + `-Wl,--pdb=RadioS.pdb` produces a real PDB usable for the D3 minidumps;
    `-Wl,--no-insert-timestamp` + `-ffile-prefix-map` make the binary reproducible (**VERIFIED**: byte-identical).

---------------------------------------------------------------------------------------------------

## 1. Verified environment facts relevant to the build

| Item | Value (VERIFIED) | Consequence |
|---|---|---|
| gcc | 16.2.0 (Rev6, MSYS2), binutils 2.47, mingw-w64 headers/crt 14.0.0 | `-gcodeview`, `--pdb`, `-fstack-protector-strong`, `_FORTIFY_SOURCE=2`, UBSan trap mode work; **no `-fsanitize=address`** (`cannot find -lasan`). |
| CMake / Ninja / pkgconf | 4.4.4 / 1.13.2 / 3.0.9 | Policy range up to 4.4 usable; `CMP0207` relevant. ninja.exe & pkgconf.exe have UTF-8 activeCodePage manifests. |
| GLib / GTK / json-glib / curl | 2.90.1 / 3.24.52 / 1.10.8 / 8.23.0 (OpenSSL backend only, CA bundle `/ucrt64/etc/ssl/certs/ca-bundle.crt`) | All headers compile clean under `-std=c11 -Wall -Wextra -Wpedantic -Werror`, even with plain `-I`. GLib/GIO/GObject/json-glib/cairo/curl declare `__declspec(dllimport)`; GTK/GDK use plain `extern`. |
| FFmpeg | 9.0.2: avformat 63.1.102, avcodec 63.1.102, avutil 61.1.102, swresample 7.1.102, avfilter 12.1.102; `--enable-gpl --enable-version3` | Satisfies the CMake minimums. Syntax probe of all FFmpeg-using sources: **no deprecation warnings, no FFmpeg API errors** (only POSIX file-API errors). |
| Python | MSYS2 3.14.8 (build-time); `runtime\python` = CPython 3.14.8 embeddable, `python314._pth` removed, tzdata 2026.5 in `Lib\tzdata`; cache zips match the PLATFORM_FACTS SHA-256 (`a93abe45…f0a310`, `b683bd1b…905ac`) | Registry Pythons (PyManager 3.15, uv 3.15) exist and win `find_package(Python3)` unless pinned. |
| Locale | ACP 1254, OEM 857, MSYS2 python `locale.getencoding()=cp1254`, `utf8_mode=0` | Any Python `read_text()`/`open()` without encoding decodes UTF-8 as cp1254 (crashes on byte 0x9E/0x8D/0x8F/0x90/0x9D). |
| Long paths | `RtlAreLongPathsEnabled()==0` even with `longPathAware` manifest (system policy off) | Keep build/install/test paths short. A File-API reply path in the deep scratchpad already exceeded MAX_PATH (Python `FileNotFoundError`). CMake warns when object paths approach 250 chars. |
| Tools without UTF-8 manifest | `as.exe`, `ld(.bfd).exe`, `ar.exe`, `windres.exe`, `collect2.exe`, `lto-wrapper.exe`, `objdump.exe`, `ntldd.exe` (narrow-API tools); `gcc.exe`, `cc1.exe`, `ninja.exe`, `pkgconf.exe`, `glib-compile-schemas.exe` **do** have it; `cmake.exe`/`ctest.exe` use wide APIs internally (the Turkish GLOB works) | Object/archive/resource paths must stay inside the ANSI code page (keep source/build/install paths ASCII). Only `design/reference/tasarrım` is non-ASCII and it is read only by CMake's GLOB, Ninja's stat and Python. |
| `gcc -dumpspecs` | `%{!shared:%:if-exists(default-manifest.o%s)}` always links the default manifest; `CMAKE_C_STANDARD_LIBRARIES` = `-lkernel32 -luser32 -lgdi32 -lwinspool -lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32` | Our RT_MANIFEST #1 replaces the default one (binutils special-cases it; **VERIFIED** single manifest in the image). |
| Import libs present | ole32 oleaut32 uuid winmm avrt ksuser ksguid mmdevapi propsys ws2_32 shlwapi shell32 user32 gdi32 dwmapi advapi32 bcrypt ntdll version psapi iphlpapi dbghelp synchronization cfgmgr32 setupapi userenv secur32 crypt32 winhttp shcore uxtheme comctl32 imm32 pathcch powrprof wtsapi32 rpcrt4 dsound | **No `libaudioses.a`** (WASAPI is COM-only). `CLSID_MMDeviceEnumerator`, `IID_IMMDeviceEnumerator`, `IID_IAudioClient`, `IID_IAudioRenderClient` are in **no** import lib → exactly one TU must `#include <initguid.h>` (the audio backend does). `PKEY_Device_FriendlyName` is in `libuuid.a`; `KSDATAFORMAT_SUBTYPE_{PCM,IEEE_FLOAT}` in `libksuser.a`/`libksguid.a`. |
| GDK backends | Only `gdkwin32.h`; `gdk/gdkx.h` does **not** exist | `GDK_BACKEND=x11` → `gtk_init_check()==FALSE` (**VERIFIED**). |
| NSIS | 3.13, only `amd64-unicode` stubs/plugins (System, nsExec, nsDialogs, …), Turkish.nlf/.nsh, MUI2 present | Script must say `Target amd64-unicode`. |
| Other machine hazards | `C:\MinGW\bin` (WinLibs GCC 13 + CMake 3.27 + ninja + libwinpthread/libgcc/libstdc++ DLLs) on the machine PATH; PyManager `python`/`py` → 3.15 | Pin tools; prepend `C:\msys64\ucrt64\bin` for every build/test process; bundle all DLLs next to `RadioS.exe`. |

---------------------------------------------------------------------------------------------------

## 2. Inventory of the current build (file:line) and the Windows replacement

### 2.1 `CMakeLists.txt`

| Line(s) | Current (Linux) | Why it exists | Windows replacement |
|---|---|---|---|
| 1 | `cmake_minimum_required(VERSION 3.20)` | | `cmake_minimum_required(VERSION 3.25...4.4)` — presets v6, `ENVIRONMENT_MODIFICATION` (3.22), `install(RUNTIME_DEPENDENCY_SET)` (3.21), `cmake_language(DEFER)` (3.19), sets `CMP0207 NEW`. |
| 2 | `project(RadioS VERSION 0.19.0 LANGUAGES C)` | Version is the single source of truth; `tools/radios_package.py:131` parses `project\(RadioS\s+VERSION\s+(\d+\.\d+\.\d+)\s` | `project(RadioS VERSION 0.19.0 LANGUAGES C RC)` — **keep the literal form** `project(RadioS VERSION x.y.z ` on one line (parser contract). |
| 4-6 | C11, extensions OFF (`-std=c11`) | Strict ISO C; feature macros per file | Keep. mingw-w64 hides `M_PI`, `strdup`, `strndup`, `memccpy` under `__STRICT_ANSI__` unless `_POSIX_C_SOURCE`/`_XOPEN_SOURCE`/`_GNU_SOURCE` is defined (`math.h:37`, `string.h:38,48`) — the same or a weaker requirement than glibc's, and the syntax scan found no such missing declaration. |
| 7-17 | `pkg_check_modules(GLIB … AVFILTER)` | Deps | Keep all minimums; add `GIO_WIN32` (`gio-windows-2.0>=2.66`) and `PANGO` (`pango>=1.56`); delete `FONTCONFIG` (line 10) when D5 lands (see §4.4). |
| 18 | `pkg_check_modules(PULSE … libpulse>=15.0)` | PulseAudio | **Delete.** WASAPI backend sources + Win32 libs (§4.7). MSYS2 has no `libpulse.pc` → configure fails today. |
| 19 | `pkg_check_modules(ALSA … alsa>=1.2)` | ALSA MIDI | **Delete.** `src/core/midi.c` already ported to WinMM (`winmm cfgmgr32`). |
| 20 | `find_package(Python3 COMPONENTS Interpreter REQUIRED)` | Generator + test runners | Pin (§4.5): `Python3_FIND_REGISTRY NEVER`, `Python3_FIND_STRATEGY LOCATION`, `Python3_ROOT_DIR=${RADIOS_MSYS2_PREFIX}`; tests use `RADIOS_TEST_PYTHON_COMMAND`. |
| 21-29 | GLOB `design/reference/tasarrım/*.css` + custom command `css_to_gtk.py --output-dir generated` (OUTPUT `reference-css.c`, BYPRODUCTS `reference.css`, `css-manifest.json`) | Embed design CSS rules into the binary | Keep structure; COMMAND becomes `${RADIOS_BUILD_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tools/css_to_gtk.py --output-dir …`; add `MAIN_DEPENDENCY` index.html; script fixed for UTF-8 (§4.6). |
| 31 | `option(RADIOS_STRICT_WARNINGS … ON)` | | Keep ON. |
| 32 | `option(RADIOS_GUI_TESTS … OFF)` "requires a display" | | Keep OFF by default; text → "etkileşimli Windows masaüstü oturumu gerekir". Presets `windows-debug-gui`/`windows-release-tests` turn it ON. |
| 33-39 | `radios_warnings` INTERFACE `-Wall -Wextra -Wpedantic [-Werror]` for GNU/Clang | | Same flags but wrapped in `$<$<COMPILE_LANGUAGE:C>:…>` so they never reach `windres` (§4.3). Drop the Clang branch (GCC only). |
| 41-45 | `radios_core` sources; links GLIB JSONGLIB CURL AV* `m` | | Add `radios_platform` (new static lib, PUBLIC). `m` harmless (`libm.a` stub exists) — may be dropped. |
| 46-49 | `radios_audio` incl. `src/audio/pulse_backend.c`; links `PkgConfig::PULSE` | | Replace `pulse_backend.c` with the audio-backend area's files (as of 19.10 01:56: `src/audio/wasapi_backend.c`, `src/audio/wasapi_device.c`, header `wasapi_device.h`; the audio spec also plans `null_provider.c`); link `ole32 uuid avrt winmm mmdevapi propsys ksuser version shlwapi advapi32`. `channel_input.c`/`patchbay.c` still include `pulse/pulseaudio.h` (audio-routing area). |
| 53-98 | `radios_ui` (+ generated `reference-css.c`); line 91 `RADIOS_VERSION`; line 98 links `PkgConfig::FONTCONFIG m` | | Remove FONTCONFIG after D5; add `dwmapi` only if the UI area calls `DwmSetWindowAttribute`. |
| 93-94 | `src/core/midi.c` + `target_link_libraries(radios_core PRIVATE PkgConfig::ALSA)` | | `target_link_libraries(radios_core PRIVATE winmm cfgmgr32)`. |
| 99-101 | `add_executable(radios src/app/main.c)` | | `set_target_properties(radios PROPERTIES OUTPUT_NAME RadioS WIN32_EXECUTABLE ON)`; resources via finalize (§4.8). |
| 103 | `include(CTest)` | | Keep. |
| 117-140 | GUI: `pkg_check_modules(CONTENT_XTEST xtst x11)` (118), `test_content_scale_module` (119-124), `test_content_scale_popups` (125-134), `test_view_presets_ui` (135-139) | XTest input injection on private Xvfb | Delete pkg check; link nothing extra (`user32` is default) — `tests/content_scale_test_input.h` gets a `SendInput` implementation (ui-platform area). Add `RESOURCE_LOCK desktop_input`. |
| 141-151 | `test_player_tree_ui` + 4 `--wrap` (static symbols OK) + `GDK_BACKEND=x11` (150) | | Drop `GDK_BACKEND=x11`. |
| 156-168 | Python tests `radios_launch`, `desktop_player_topology`, `stream_metadata_schema`, `routing_graph_schema` | | Python command → `${RADIOS_TEST_PYTHON_COMMAND}`. |
| 175-179 | `test_rhythm_upkeep` (compiles `src/ui/autodj_marker_jobs.c`, links `radios_core GTK3`) | | Unchanged. |
| 191-193 | `test_midi` links `PkgConfig::ALSA` | | Link `radios_core winmm` (file already ported: includes `<windows.h>`, `<mmsystem.h>`). |
| 194-208 | `test_midi_hotplug` `-Wl,--as-needed` (200), 5 GLib cases via Xvfb runner | Fake ALSA via `#include "../src/core/midi.c"` | Drop `--as-needed` (meaningless on PE). **Case list is stale**: the ported test registers `/midi-hotplug/ui-slow-enumeration` etc.; `ui-slow-sequencer` no longer exists → would run 0 tests and PASS. Use the validated case helper (§5.4). Non-GUI `input-*` cases run directly (midi spec §5). |
| 209-215 | `external_access` via `run_external_access.py` (needs `openssl` CLI) | | Works with MSYS2 `openssl.exe` on the test PATH. |
| 216-232 | `foreach` core and audio component tests; `RADIOS_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"` | | Unchanged (CMake paths use `/`; quoting is handled). |
| 238-243 | `audio_recovery` via `run_audio_recovery.py` (private Pulse daemon, `setsid`, `/proc`) | | Runner/test rewritten on the null provider (audio-backend spec 6.2). CMake unchanged. |
| 244-250 | `test_pulse_hotplug` compiles `pulse_backend.c` + `${PULSE_INCLUDE_DIRS}` | Fake libpulse | **Replace** by `test_audio_backend_hotplug` (audio spec 6.3a). |
| 251-256 | `audio_soak` (Pulse daemon, `parec`, `/proc`) | | Runner rewrite; add `RUN_SERIAL`. |
| 262-264 | `config_from_other_directory`: `WORKING_DIRECTORY /tmp` | "Run from a foreign cwd" | `WORKING_DIRECTORY ${RADIOS_TEST_FOREIGN_CWD}` (= `${CMAKE_BINARY_DIR}/test-cwd`, created at configure). |
| 275-281 | `foreach` 19 Python tests incl. `packaging`, `release_install` | | Python command; those two are ELF/`fcntl`/`/proc`/`run-radios.sh` based → port (§5.6). |
| 289,312,329,349,352,369,412,419,431,437,471,480,486,492 | `GDK_BACKEND=x11` | Force Xvfb | Remove the token. |
| 292-301, 315-318, 326, 364, 391, 399, 434 | `-Wl,--wrap=` on `radios_*` and `gtk_*` symbols | Test seams | Work unchanged on PE (**VERIFIED** for both kinds). |
| 405-407 | `pkg_check_modules(XTST xtst x11)`; `test_routing_graph_ui` links `PkgConfig::XTST` | XTest in `routing_patchbay_ui_test.inc` | Delete; `SendInput` (audio-routing spec); `RESOURCE_LOCK desktop_input`. |
| 442, 546 | `native_controls`, `native_window`: `WORKING_DIRECTORY /tmp` | | `${RADIOS_TEST_FOREIGN_CWD}`. |
| 543-546 | `native_window`: `radios --smoke-test --capture ${CMAKE_BINARY_DIR}/native-window.png` | | Unchanged command (GUI exe writes to CTest pipes — **VERIFIED** a GUI-subsystem exe's stdout reaches a pipe). |
| 550-600 | `include(cmake/*.cmake)` | | Unchanged includes; module edits in §2.2. |
| 604-619 | `test_schema_settings_ui`, `test_rotation_editor_regressions` (`-ffunction-sections -fdata-sections`, `-Wl,--gc-sections` 613-614) | Link UI sources without `radios_ui` | Keep flags (harmless); static scan: no unresolved symbol depends on GC (§4.10). |
| 620-631 | `test_player_waveform_markers` 12 wraps | | Unchanged. |
| 772 | `automation_midtrack_jingle` `ENVIRONMENT "RADIOS_AUDIO_NULL=1;…"` | Stale: no reader in `src/` or `tests/` | Remove; the global `RADIOS_AUDIO_SERVER=null:` covers it. |
| 903-904 | `automation_recovery_stress`: `run_automation_stress.py --build-dir` which **compiles `tools/otomasyon_testleri/e2e.c` at test time with `cc` and `pkg-config … libpulse libswscale`** | | Add CMake target `radios_automation_e2e` (links `radios_playout`) and pass `--driver $<TARGET_FILE:radios_automation_e2e>` (runner change, test area). Same for `tools/yayin_akisi_724/analiz.c` → target `radios_usb_analiz` (links `radios_core`), `EXCLUDE_FROM_ALL`. |
| 914-917, 1089-1093 | `desktop_playout_vocal_cues`, `desktop_autodj`: `-m unittest …` with `PYTHONPATH=${CMAKE_SOURCE_DIR}/database/SDBASE/src:${CMAKE_SOURCE_DIR}/tests` | | `:` is wrong on Windows (and collides with `C:`). Use `ENVIRONMENT_MODIFICATION "PYTHONPATH=path_list_prepend:…"` ×2 (**VERIFIED** produces `;`-joined value). |
| 1021-1022, 1027 | wraps `radios_programme_queue_set_window`, `radios_playout_ui_set_programme_window`, `radios_broadcast_header_snapshot` | | Unchanged. |
| remaining GUI blocks | 38 more `GDK_BACKEND=x11` (lines 578 … 1134; all 52 in this file listed in §5.2) | | Remove. |

### 2.2 `cmake/*.cmake`

| File:line | Current | Windows change |
|---|---|---|
| `Prompt7Tests.cmake:6-16,22-24` | wraps on `radios_*` | unchanged |
| `Prompt7Tests.cmake:28,38` | `GDK_BACKEND=x11` | remove |
| `Pdf2HeaderStatus.cmake:2` | `target_sources(radios_ui … header_status.c)` | unchanged |
| `Pdf2HeaderStatus.cmake:7` | `-Wl,--wrap=g_get_real_time` on `test_header_ui` | **does not intercept on PE** (dllimport). Use `radios_wrap(test_header_ui DLLIMPORT g_get_real_time)` and in `tests/test_header_ui.c` add `RADIOS_WRAP_DLLIMPORT(gint64, g_get_real_time, (void));` (§4.9). |
| `Pdf2HeaderStatus.cmake:12,42` | `GDK_BACKEND=x11` | remove |
| `Pdf2HeaderStatus.cmake:16-20,47` | wraps on `radios_*` | unchanged |
| `Preferences.cmake:3-5` | `preferences_storage.c`, `output_resampler.c`, `preferences.c`, `loudness_settings.c` | unchanged |
| `Preferences.cmake:43-47` | `pkg_check_modules(XTST xtst x11)` + `test_content_scale_ui` links `PkgConfig::XTST` | delete pkg check/link; `RESOURCE_LOCK desktop_input`; runner `run_content_scale_ui.py` (Xvfb + dbus-daemon) rewritten |
| `MediaArchive.cmake:19`, `Pdf4Neon.cmake:23,34`, `LibraryAnalysis.cmake:9`, `SilenceGuard.cmake:15`, `ResetUi.cmake:10`, `Prompt8Tests.cmake:17`, `Prompt5Tests.cmake:24` | `GDK_BACKEND=x11` | remove |
| `Pdf12Playlist.cmake:4` | `if(EXISTS tests/test_prompt12_${component}_ui.c)` (only `drop` exists) | unchanged; consider deleting the dead `playlist` loop entry (D2b) |
| `MediaArchiveService.cmake`, `Prompt10Playlist.cmake` | Python/data runners | Python command only |
| `README.md` | Linux wording | rewrite (§10) |

### 2.3 Other files in this area

* `.gitignore` — misses Windows/ported artefacts (§9).
* `packaging/README.md` — entirely Linux (`linux-x86_64.tar.gz`, ELF, `DT_NEEDED`, GLIBC, Flatpak SDK, PulseAudio/PipeWire, `run-*.sh`) → rewrite (§10).
* `tools/css_to_gtk.py:113,117,186,191,192` — `read_text()`/`write_text()` without encoding/newline → §4.6.
* `tools/radios_launch.py` (tools area) runs `cmake -S root -B build` without a generator/compiler and checks `build/radios` — must switch to the presets (§11).

---------------------------------------------------------------------------------------------------

## 3. Target architecture (overview)

```
CMakePresets.json  ──►  cmake/RadioSToolchain.cmake   (guards, pinned tools, global defines/options)
                        cmake/RadioSWindowsExe.cmake  (manifest/rc/icon/binmode objects, radios_wrap(),
                                                       deferred finalize of every executable and test)
                        cmake/RadioSTesting.cmake     (test python, env, labels, GLib case validation)
                        cmake/RadioSRuntime.cmake     (install components runtime|python|application,
                                                       radios_runtime / radios_installer targets)
                        cmake/RadioSInstallDeps.cmake (install-time script: DLL closure + GTK caches)
                        cmake/RadioSPythonRuntime.cmake (install-time script: embeddable + tzdata)
CMakeLists.txt     ──►  radios_platform ─► radios_core ─► radios_audio ─► radios_playout ─► radios_ui ─► RadioS.exe
                        (static libs, unchanged layering; tests keep linking the static libs so --wrap works)
packaging/windows/ ──►  radios.manifest.in, radios.rc.in, gtk-settings.ini, radios.nsi.in
src/app/crt_binmode.c, tests/test_process_setup.c, tests/radios_test_wrap.h, assets/icons/radios.ico(+svg)
tools/radios_sync_runtime.py (build-time, in-use-safe staging into <root>\runtime)
```

Static libraries stay static: wrapping `radios_*` functions only works when the callers are linked from objects, not
from inside a DLL.

---------------------------------------------------------------------------------------------------

## 4. CMakeLists.txt and modules — exact content

### 4.1 Header

```cmake
cmake_minimum_required(VERSION 3.25...4.4)
project(RadioS VERSION 0.19.0 LANGUAGES C RC)   # literal form parsed by tools/radios_package.py
if(NOT WIN32 OR NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "RadioS yalnız Windows x64 için MSYS2 UCRT64 GCC ile derlenir "
                        "(CMakePresets.json: windows-debug / windows-release).")
endif()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(RadioSToolchain)
include(RadioSWindowsExe)
```

### 4.2 `cmake/RadioSToolchain.cmake`

```cmake
set(RADIOS_MSYS2_PREFIX "C:/msys64/ucrt64" CACHE PATH "MSYS2 UCRT64 kökü (gcc, pkg-config, GTK, FFmpeg)")
file(TO_CMAKE_PATH "${RADIOS_MSYS2_PREFIX}" RADIOS_MSYS2_PREFIX)
set(RADIOS_FFMPEG_PREFIX "" CACHE PATH "İsteğe bağlı özel FFmpeg kökü (bin/, lib/pkgconfig/)")

# 1. Compiler must be the MSYS2 UCRT64 GCC (C:\MinGW WinLibs GCC is on the machine PATH).
string(TOLOWER "${CMAKE_C_COMPILER}" _radios_cc)
string(TOLOWER "${RADIOS_MSYS2_PREFIX}/bin/" _radios_want)
string(FIND "${_radios_cc}" "${_radios_want}" _radios_pos)
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR NOT _radios_pos EQUAL 0)
    message(FATAL_ERROR "Derleyici ${CMAKE_C_COMPILER}; beklenen ${RADIOS_MSYS2_PREFIX}/bin/gcc.exe. "
                        "Başka araç zincirleri (C:\\MinGW, Strawberry, LLVM) kullanılmaz.")
endif()
include(CheckSymbolExists)
check_symbol_exists(_UCRT "stdio.h" RADIOS_HAVE_UCRT)
if(NOT RADIOS_HAVE_UCRT)
    message(FATAL_ERROR "RadioS UCRT çalışma zamanı ister (MSYS2 UCRT64 ortamı).")
endif()
# 2. pkg-config must be the MSYS2 one and see only MSYS2 (+ optional FFmpeg) .pc files.
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${RADIOS_MSYS2_PREFIX}/lib/pkgconfig;${RADIOS_MSYS2_PREFIX}/share/pkgconfig")
if(RADIOS_FFMPEG_PREFIX)
    set(ENV{PKG_CONFIG_PATH} "${RADIOS_FFMPEG_PREFIX}/lib/pkgconfig")
endif()
# 3. Paths: as.exe/ld.exe/ar.exe/windres.exe are not UTF-8 aware; long object paths break (MAX_PATH policy off).
foreach(_radios_dir IN ITEMS "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
    if(_radios_dir MATCHES "[^ -~]")
        message(WARNING "ASCII dışı yol: ${_radios_dir}. Derleme araçlarının bir kısmı UTF-8 yol desteklemez.")
    endif()
endforeach()
string(LENGTH "${CMAKE_BINARY_DIR}" _radios_len)
if(_radios_len GREATER 90)
    message(WARNING "Derleme dizini çok uzun (${_radios_len} karakter); nesne yolları MAX_PATH sınırına yaklaşır.")
endif()

# 4. Language level and global definitions (D7).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_definitions(_FILE_OFFSET_BITS=64 _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE NOMINMAX
                        GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_66)
# Release: CodeView debug info for minidumps, hardening, reproducibility (all VERIFIED with gcc 16.2/ld 2.47).
add_compile_options(
    "$<$<AND:$<CONFIG:Release,RelWithDebInfo>,$<COMPILE_LANGUAGE:C>>:-gcodeview;-fstack-protector-strong;-D_FORTIFY_SOURCE=2>"
    "$<$<AND:$<CONFIG:Release,RelWithDebInfo>,$<COMPILE_LANGUAGE:C>>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.;-ffile-prefix-map=${CMAKE_BINARY_DIR}=build>")
add_link_options("$<$<CONFIG:Release,RelWithDebInfo>:-Wl,--no-insert-timestamp>")
# Bound parallel links (145 large static-linked executables).
set_property(GLOBAL APPEND PROPERTY JOB_POOLS radios_link=4)
set(CMAKE_JOB_POOL_LINK radios_link)

add_library(radios_warnings INTERFACE)
target_compile_options(radios_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic>")
if(RADIOS_STRICT_WARNINGS)
    target_compile_options(radios_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C>:-Werror>")
endif()
```

Notes:
* `PKG_CONFIG_LIBDIR` is set **inside CMake** (affects only the configure-time pkg-config calls) as a second line of
  defence; presets set it too.
* `NOMINMAX` (addition to D7): `<windows.h>` otherwise defines function-like `min`/`max`; GLib uses `MIN/MAX`, so
  nothing depends on the lowercase macros. `WIN32_LEAN_AND_MEAN` is **not** global — backend files define it
  themselves (wasapi_device.c, platform_tz.c do) and some need `shellapi.h`/`commdlg.h`.
* `GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_66` (addition to D7, matches the `glib-2.0>=2.66` floor): GLib only
  emits deprecation warnings for APIs deprecated *at or before* the floor, so a future MSYS2 `pacman -Syu` bumping
  GLib cannot break `-Werror` with new deprecations. Today there are zero deprecation diagnostics (§13 P9), so the
  macro changes nothing now. `GDK_VERSION_*` is not needed (GTK 3.24 is frozen).
* `-O3 -DNDEBUG` (CMake's GCC Release default) stays; `src/` contains no `assert()`/`g_assert()` (grep), tests use
  `g_assert*` which `NDEBUG` does not disable.
* Debug builds keep DWARF `-g` (gdb). ASan is unavailable with MinGW GCC; an optional `RADIOS_UBSAN_TRAP` option may
  add `-fsanitize=undefined -fsanitize-trap=all` (**VERIFIED** links). The Linux ASan/UBSan acceptance cannot be
  reproduced 1:1 (risk §12).

#### 4.3.1 Warnings policy
Keep `-Wall -Wextra -Wpedantic -Werror`. **VERIFIED**: every third-party header used (glib, gio, gio-windows, gtk,
gdkwin32, json-glib, curl, all 5 FFmpeg libs, fontconfig, mmdeviceapi, audioclient, avrt, mmsystem, shlobj,
`<windows.h>`) compiles clean under these flags even with plain `-I` (CMake turns imported-target include dirs into
`-isystem` anyway). Syntax-only scan of the current tree with GCC 16: 94/114 `src` files and 93/147 `tests` files
are already clean; the remaining diagnostics are all POSIX APIs owned by other areas (list in §13 P9/P10) — no
GCC-16-only warning appeared.

#### 4.3.2 `__USE_MINGW_ANSI_STDIO` — proven not necessary (D7 asked for proof)
* With UCRT, `stdio.h:229-231` declares every printf/scanf with `__gnu_printf__`/`__gnu_scanf__` format checking
  regardless of `__USE_MINGW_ANSI_STDIO`, so `%zu %lld %llu %hhd %jd %td` are accepted by `-Wformat` and are
  implemented by UCRT.
* `_mingw.h:463-473`: any TU that defines `_POSIX_C_SOURCE`, `_XOPEN_SOURCE` or `_GNU_SOURCE` (≈60 files: 20 in
  `src/`, ≈40 in `tests/`) automatically gets `__USE_MINGW_ANSI_STDIO=1` (mingw printf); the others use UCRT printf. Both are C99-correct
  (**VERIFIED** outputs of `%zu %lld %llu`, `G_GUINT64_FORMAT`="llu", `G_GSIZE_FORMAT`="llu", `PRId64`, `%.3f`).
* The only formatting that would differ is `%Lf` with 80-bit `long double` (UCRT treats it as double): the code uses
  `long double` only for arithmetic (`rhythm.c:72-74`, `analyzer.c:29,218,365,367`, `neon_progress.c:174`, tests)
  and never prints it. No `%m`, `%n`, `%I64` anywhere. Only 7 `%ld` uses, all with `long` HTTP status values.
* Conclusion: leave it undefined. Do not define it globally (it would silently switch the other ≈95 `src/` TUs to
  mingw's slower printf for no gain).

#### 4.3.3 Debug info / PDB for minidumps (D3 crash handler)
For `radios` only: `target_link_options(radios PRIVATE "$<$<CONFIG:Release,RelWithDebInfo>:-Wl,--pdb=RadioS.pdb>")`.
Ninja links from the binary dir, so `RadioS.pdb` lands next to `RadioS.exe` and the RSDS record stores the relative
name (no machine path). **VERIFIED**: PDB produced (`RSDS … pdb cv.pdb`), and two links with
`--pdb` + `--no-insert-timestamp` are byte-identical. The PDB is archived per release (symbol store), not installed.

### 4.4 Dependencies

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB       REQUIRED IMPORTED_TARGET glib-2.0>=2.66 gio-2.0>=2.66)
pkg_check_modules(GIO_WIN32  REQUIRED IMPORTED_TARGET gio-windows-2.0>=2.66)   # gio/gwin32inputstream.h
pkg_check_modules(GTK3       REQUIRED IMPORTED_TARGET gtk+-3.0>=3.24)
pkg_check_modules(PANGO      REQUIRED IMPORTED_TARGET pango>=1.56)              # pango_font_map_add_font_file (D5)
pkg_check_modules(JSONGLIB   REQUIRED IMPORTED_TARGET json-glib-1.0>=1.10)
pkg_check_modules(CURL       REQUIRED IMPORTED_TARGET libcurl>=7.85)
pkg_check_modules(AVFORMAT   REQUIRED IMPORTED_TARGET libavformat>=59.27.100)
pkg_check_modules(AVCODEC    REQUIRED IMPORTED_TARGET libavcodec>=59.37.100)
pkg_check_modules(AVUTIL     REQUIRED IMPORTED_TARGET libavutil>=57.28.100)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample>=4.7.100)
pkg_check_modules(AVFILTER   REQUIRED IMPORTED_TARGET libavfilter>=8.44.100)
# FONTCONFIG: delete together with the Fc* calls in src/ui/window.c (D5). Until then:
# pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
```
`gio-windows-2.0.pc` only adds `-I…/include/gio-win32-2.0` (**VERIFIED**: contains `gwin32inputstream.h`,
`gwin32outputstream.h`). The libfontconfig DLL still ships (cairo/pangocairo/avfilter import it).
pkgconf rewrites the `/ucrt64` prefix to `C:/msys64/ucrt64` (define-prefix default on Windows) — **VERIFIED** by a
successful configure + compile of the scratch copy.

### 4.5 Python at build and test time

```cmake
set(Python3_FIND_REGISTRY NEVER)          # otherwise PyManager's 3.15 wins (VERIFIED)
set(Python3_FIND_STRATEGY LOCATION)
set(Python3_ROOT_DIR "${RADIOS_MSYS2_PREFIX}")
find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)
# Build-time generators: isolated (no PYTHON* env, no user site, no script dir), UTF-8 mode, no .pyc.
set(RADIOS_BUILD_PYTHON_COMMAND "${Python3_EXECUTABLE}" -X utf8 -B -I)
# Tests: the shipped interpreter (D7). Not -E/-I: runners import sibling modules from tests/ and two tests use PYTHONPATH.
set(RADIOS_TEST_PYTHON "${CMAKE_SOURCE_DIR}/runtime/python/python.exe" CACHE FILEPATH "Testlerin Python yorumlayıcısı")
set(RADIOS_TEST_PYTHON_COMMAND "${RADIOS_TEST_PYTHON}" -X utf8 -B -s)
if(BUILD_TESTING AND NOT EXISTS "${RADIOS_TEST_PYTHON}")
    message(FATAL_ERROR "${RADIOS_TEST_PYTHON} yok. Önce: cmake --build --preset windows-runtime "
                        "(runtime\\python gömülü CPython 3.14.8 + tzdata kurulur).")
endif()
```
Every `${Python3_EXECUTABLE}` inside `add_test(...)` (129 occurrences: 112 in `CMakeLists.txt` + 17 in 10 modules;
the 130th is the generator at `CMakeLists.txt:24`) becomes `${RADIOS_TEST_PYTHON_COMMAND}` (unquoted, so the list
expands to separate arguments). Test children started with
`sys.executable` inherit UTF-8 mode via the global `PYTHONUTF8=1` (§5.1).

### 4.6 Generated reference CSS (`design/reference/tasarrım`) — analysis and fix

Findings (**VERIFIED**):
* `file(GLOB … CONFIGURE_DEPENDS ".../tasarrım/*.css")` works (CMake uses wide APIs). Ninja's build edge lists the
  Turkish paths correctly and ninja.exe stats them (UTF-8 activeCodePage manifest). The custom command line runs via
  `cmd.exe /C "cd /D <build> && python.exe …/css_to_gtk.py --output-dir …"` and contains no non-ASCII bytes.
* `css_to_gtk.py` as invoked today **fails**: `UnicodeDecodeError: 'charmap' codec can't decode byte 0x9e in
  position 14543` — `index.html` (2420 non-ASCII chars) and `live-desk.css` contain "Ş"/"Ğ" (UTF-8 `C5 9E`/`C4 9E`;
  0x9E is undefined in cp1254). `header-layout.css`, `emergency-workspace.css`, `readability.css` would decode as
  mojibake (wrong `sha256` in `css-manifest.json`).
* With `-X utf8` it succeeds: "Design CSS: 31 sources, 1572 native rules"; but `write_text()` translates `\n` to
  `\r\n` (1604 CRLF in `reference.css`). The embedded strings are JSON-escaped, so the CSS inside the binary is
  byte-identical to Linux; only the byproduct files differ. No test reads the byproducts (grep).
* Every 2000-character JSON chunk stays below the ISO C 4095-byte string limit enforced by `-Wpedantic`
  (`-Woverlength-strings`) because the CSS is mostly ASCII; `-std=c11` enables trigraphs but the CSS has no `??x`.

Required changes:
1. CMake (lines 23-29):
   ```cmake
   add_custom_command(OUTPUT "${REFERENCE_CSS_C}"
       COMMAND ${RADIOS_BUILD_PYTHON_COMMAND} "${CMAKE_SOURCE_DIR}/tools/css_to_gtk.py"
               --output-dir "${CMAKE_BINARY_DIR}/generated"
       MAIN_DEPENDENCY "design/reference/tasarrım/index.html"
       DEPENDS ${DESIGN_CSS} tools/css_to_gtk.py
       BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/reference.css" "${CMAKE_BINARY_DIR}/generated/css-manifest.json"
       COMMENT "Tasarım CSS'i GTK CSS'e aktarılıyor"
       VERBATIM)
   ```
2. `tools/css_to_gtk.py` (owner: this area): `read_text(encoding="utf-8")` at 113 and 117;
   `write_text(..., encoding="utf-8", newline="\n")` at 186, 191, 192. (Python 3.10+ supports `newline=`.) The
   `sha256` at 138 stays `source.encode()` (UTF-8 of the newline-normalised text → identical to Linux even if git
   checks the files out with CRLF).
3. The script needs no `font-family` change for D5: `PROPERTIES` (line 16-24) never emits `font-family`.

### 4.7 Libraries and Win32 system libraries

```cmake
add_library(radios_platform STATIC src/core/platform.c src/core/platform_win32.c src/core/platform_tz.c)
target_include_directories(radios_platform PUBLIC include)
target_link_libraries(radios_platform PUBLIC PkgConfig::GLIB PkgConfig::GIO_WIN32
                      PRIVATE advapi32 ntdll dbghelp shell32 ole32 user32 radios_warnings)
target_link_libraries(radios_core PUBLIC radios_platform …)            # existing line 44 + radios_platform
target_link_libraries(radios_core PRIVATE winmm cfgmgr32)              # replaces line 94 (midi.c, WinMM)
# radios_audio: replace src/audio/pulse_backend.c by the backend files of the audio area
target_link_libraries(radios_audio PRIVATE ole32 uuid avrt winmm mmdevapi propsys ksuser version shlwapi advapi32)
```

| Library | Needed by (current code) | APIs |
|---|---|---|
| `advapi32` | `platform.c` | `OpenProcessToken`, `GetTokenInformation`, `CreateWellKnownSid`, `CheckTokenMembership`, `ConvertStringSecurityDescriptorToSecurityDescriptorW`, `EqualSid`, `Get/SetSecurityInfo`; `wasapi_device.c`: `OpenSCManagerW`/`QueryServiceStatusEx` |
| `ntdll` | `platform.c` | `RtlNtStatusToDosError` |
| `dbghelp` | crash handler (D3) | `MiniDumpWriteDump` |
| `shell32`, `ole32`, `user32` | platform process/termination/console helpers | `CommandLineToArgvW`/`SHGetKnownFolderPath`/`CoTaskMemFree`/`EnumWindows`/`PostMessageW` |
| `ole32` `uuid` `propsys` | WASAPI | `CoCreateInstance`, `PropVariantClear`, `PKEY_Device_FriendlyName`, `PropVariantTo*` |
| `mmdevapi` | process loopback | `ActivateAudioInterfaceAsync` |
| `avrt` | audio threads | `AvSetMmThreadCharacteristicsW` |
| `winmm` | audio timer + MIDI | `timeBeginPeriod`, `midiIn*`/`midiOut*` |
| `cfgmgr32` | MIDI hotplug | `CM_Register_Notification` |
| `version` | `wasapi_device.c:1295-1302` | `GetFileVersionInfo*`, `VerQueryValueW` |
| `shlwapi` | `wasapi_device.c` includes `shlwapi.h` | `Path*W` |
| `ksuser` | safety for `KSDATAFORMAT_SUBTYPE_*` when not instantiated via `initguid.h` | GUID data |
| `psapi` | `test_audio_soak` port | `GetProcessMemoryInfo` |
| `dwmapi` | only if UI sets dark title bar | `DwmSetWindowAttribute` |
| `user32` | content-scale / routing GUI tests | `SendInput`, `SetCursorPos` (default library anyway) |

Most of these are already in `CMAKE_C_STANDARD_LIBRARIES`; listing them per target documents intent and keeps working
if that default ever changes. ICU (`icu.dll`) is loaded with `LoadLibraryExW(…, LOAD_LIBRARY_SEARCH_SYSTEM32)` —
no link dependency.

### 4.8 Executables: resources, binary file mode, stack, subsystem

Files (new):

`packaging/windows/radios.manifest.in` (**VERIFIED** effective — note `SMI`):
```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0" xmlns:asmv3="urn:schemas-microsoft-com:asm.v3">
  <assemblyIdentity type="win32" name="RadioS.@RC_INTERNAL_NAME@" version="@PROJECT_VERSION_MAJOR@.@PROJECT_VERSION_MINOR@.@PROJECT_VERSION_PATCH@.0" processorArchitecture="amd64"/>
  <description>@RC_DESCRIPTION@</description>
  <dependency><dependentAssembly>
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
  </dependentAssembly></dependency>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges>
    <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
  </requestedPrivileges></security></trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application>
    <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>   <!-- Windows 10 / 11 -->
  </application></compatibility>
  <asmv3:application>
    <asmv3:windowsSettings xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings"><dpiAware>true/pm</dpiAware></asmv3:windowsSettings>
    <asmv3:windowsSettings xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
      <dpiAwareness>PerMonitorV2, PerMonitor</dpiAwareness>
      <longPathAware>true</longPathAware>
    </asmv3:windowsSettings>
    <asmv3:windowsSettings xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings"><activeCodePage>UTF-8</activeCodePage></asmv3:windowsSettings>
  </asmv3:application>
</assembly>
```

`packaging/windows/radios.rc.in` (**VERIFIED**: `#pragma code_page(65001)` is required for the Turkish strings;
without it windres decodes UTF-8 as cp1252 → "yayÄ±n"):
```rc
#pragma code_page(65001)
#include <winver.h>
#include <winuser.h>
1 ICON "@RC_ICON_FILE@"                 /* first icon: GDK-win32 window + taskbar icon */
CREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST "@RC_MANIFEST_FILE@"
VS_VERSION_INFO VERSIONINFO
 FILEVERSION     @PROJECT_VERSION_MAJOR@,@PROJECT_VERSION_MINOR@,@PROJECT_VERSION_PATCH@,0
 PRODUCTVERSION  @PROJECT_VERSION_MAJOR@,@PROJECT_VERSION_MINOR@,@PROJECT_VERSION_PATCH@,0
 FILEFLAGSMASK   VS_FFI_FILEFLAGSMASK
 FILEFLAGS       @RC_FILEFLAGS@
 FILEOS          VOS_NT_WINDOWS32
 FILETYPE        VFT_APP
 FILESUBTYPE     VFT2_UNKNOWN
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "041F04B0"
    BEGIN
      VALUE "CompanyName",      "RadioS"
      VALUE "FileDescription",  "@RC_DESCRIPTION@"
      VALUE "FileVersion",      "@PROJECT_VERSION@"
      VALUE "InternalName",     "@RC_INTERNAL_NAME@"
      VALUE "OriginalFilename", "@RC_ORIGINAL_FILENAME@"
      VALUE "ProductName",      "RadioS"
      VALUE "ProductVersion",   "@PROJECT_VERSION@"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x041F, 1200
  END
END
```

`src/app/crt_binmode.c` (**VERIFIED**: `_get_fmode()==0x8000`, `fopen("w")` writes `a\nb` as 3 bytes;
stdin/stdout/stderr stay text, as on any CRT):
```c
/* Every RadioS executable: the CRT default translation mode is binary. mingw-w64's
 * binmode.o is an empty stub with UCRT; crt2.o copies this variable into the UCRT
 * (__p__fmode) before main(). radios_platform_init() also calls _set_fmode(). */
#include <fcntl.h>
#include <stdlib.h>
#undef _fmode
int _fmode = _O_BINARY;
```

`tests/test_process_setup.c` (linked into every test/tool exe; **VERIFIED**: constructor runs before `main`, a failing
`g_assert` exits in 0.4 s without a WER dialog):
```c
/* Unattended test processes: no Windows Error Reporting / abort() dialogs that would
 * block CTest until its timeout. */
#include <windows.h>
#include <stdlib.h>
__attribute__((constructor)) static void radios_test_process_setup(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
```

`assets/icons/radios.ico` (+ source `assets/icons/radios.svg`): **does not exist yet** (`assets/icons` holds only
`.gitkeep`; the design brand is typographic "Radio" + "S"). Required: a multi-size ICO (16, 20, 24, 32, 40, 48, 64,
256; PNG-compressed entries are fine). A stdlib-only packer (`tools/make_ico.py`, ~40 lines; prototype in
`scratchpad\cm\src\make_ico.py`) can build it from PNGs rendered with MSYS2 `rsvg-convert`. Commit the `.ico`; do not
make the build depend on `rsvg-convert`.

`cmake/RadioSWindowsExe.cmake`:
```cmake
set(RADIOS_ICON_FILE "${CMAKE_SOURCE_DIR}/assets/icons/radios.ico")
function(radios_configure_resources kind description internal original flags)
    set(RC_DESCRIPTION "${description}")
    set(RC_INTERNAL_NAME "${internal}")
    set(RC_ORIGINAL_FILENAME "${original}")
    set(RC_FILEFLAGS "${flags}")
    set(RC_ICON_FILE "${RADIOS_ICON_FILE}")
    set(RC_MANIFEST_FILE "${CMAKE_BINARY_DIR}/win32/${kind}.manifest")
    configure_file("${CMAKE_SOURCE_DIR}/packaging/windows/radios.manifest.in" "${RC_MANIFEST_FILE}" @ONLY)
    configure_file("${CMAKE_SOURCE_DIR}/packaging/windows/radios.rc.in" "${CMAKE_BINARY_DIR}/win32/${kind}.rc" @ONLY)
    set_source_files_properties("${CMAKE_BINARY_DIR}/win32/${kind}.rc" PROPERTIES
        OBJECT_DEPENDS "${RADIOS_ICON_FILE};${RC_MANIFEST_FILE}")
    add_library(radios_exe_${kind} OBJECT src/app/crt_binmode.c "${CMAKE_BINARY_DIR}/win32/${kind}.rc")
endfunction()
radios_configure_resources(app  "RadioS yayın otomasyonu"   "RadioS"      "RadioS.exe" "0")
radios_configure_resources(tool "RadioS test/araç programı" "RadioS-tool" ""           "VS_FF_PRIVATEBUILD")
add_library(radios_test_process OBJECT tests/test_process_setup.c)

# GNU ld on PE wraps only references named exactly <sym>. Functions declared __declspec(dllimport)
# (GLib, GIO, GObject, json-glib, cairo, curl) are referenced as __imp_<sym>: wrap that pointer too,
# and define it in the test with RADIOS_WRAP_DLLIMPORT() from tests/radios_test_wrap.h.
function(radios_wrap target)
    cmake_parse_arguments(W "" "" "DLLIMPORT" ${ARGN})
    foreach(sym IN LISTS W_UNPARSED_ARGUMENTS)
        target_link_options(${target} PRIVATE "-Wl,--wrap=${sym}")
    endforeach()
    foreach(sym IN LISTS W_DLLIMPORT)
        target_link_options(${target} PRIVATE "-Wl,--wrap=${sym}" "-Wl,--wrap=__imp_${sym}")
    endforeach()
endfunction()

# Runs after the whole top-level directory (all include()d modules) has been processed.
function(radios_finalize_targets)
    get_property(targets DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(NOT type STREQUAL "EXECUTABLE")
            continue()
        endif()
        target_link_options(${target} PRIVATE "LINKER:--stack,8388608")   # main + default GThread stacks (D7)
        if(target STREQUAL "radios")
            target_link_libraries(${target} PRIVATE radios_exe_app)
        else()
            target_link_libraries(${target} PRIVATE radios_exe_tool radios_test_process)
        endif()
    endforeach()
    radios_finalize_tests()          # cmake/RadioSTesting.cmake (§5.1)
    radios_check_source_coverage()   # §4.11
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL radios_finalize_targets)
```
```cmake
# CMakeLists.txt, replacing lines 99-101:
add_executable(radios src/app/main.c)
set_target_properties(radios PROPERTIES OUTPUT_NAME RadioS WIN32_EXECUTABLE ON)   # -mwindows (VERIFIED: Windows GUI)
target_link_libraries(radios PRIVATE radios_ui radios_warnings)
target_compile_definitions(radios PRIVATE RADIOS_VERSION="${PROJECT_VERSION}")
target_link_options(radios PRIVATE "$<$<CONFIG:Release,RelWithDebInfo>:-Wl,--pdb=RadioS.pdb>")
```
`tests/radios_test_wrap.h` (new):
```c
#ifndef RADIOS_TEST_WRAP_H
#define RADIOS_TEST_WRAP_H
/* After defining __wrap_<name>, redirect the dllimport'ed call sites (link with
 * radios_wrap(<target> DLLIMPORT <name>)). */
#define RADIOS_WRAP_DLLIMPORT(ret, name, params) ret (*__wrap___imp_##name) params = __wrap_##name
#endif
```

Facts behind this design (all **VERIFIED** in `scratchpad\cm`, CMake 4.4.4/Ninja):
* An OBJECT library containing an `.rc` can be linked with `target_link_libraries`; the RC object carries the icon
  (8 `RT_ICON` + `RT_GROUP_ICON`), `VERSIONINFO` and `RT_MANIFEST #1`; `FileDescription` reads
  "RadioS yayın otomasyonu".
* `WIN32_EXECUTABLE ON` → `Subsystem 2 (Windows GUI)`; test exe → `Subsystem 3 (CUI)`; both `SizeOfStackReserve
  0x800000`. GLib threads use `_beginthreadex(..., 0, ...)` and therefore inherit the 8 MiB reserve.
* `DllCharacteristics 0x160` (DYNAMIC_BASE | HIGH_ENTROPY_VA | NX_COMPAT) is the binutils default.
* windres works with spaces in paths in its default (popen) mode; **never** add `--use-temp-file` (fails with spaces).
* mingw's GCC always links `default-manifest.o`; our resource #1 replaces it (single manifest in the image), and the
  language of our resource (0x409) does not matter.

### 4.9 `--wrap` on PE — verified matrix and the one required fix

| Wrapped symbol kind | Example | Plain `-Wl,--wrap=sym` | Fix |
|---|---|---|---|
| Our static library, call from another object | `radios_data_request`, `radios_window_present` | **works** | — |
| Our static library, call inside the defining object | (any) | not wrapped (same as ELF) | — |
| GTK/GDK function (plain `extern`, import-lib thunk) | `gtk_drag_finish`, `gtk_show_uri_on_window` | **works** (`__real_` reaches the DLL) | — |
| GLib/GIO/GObject/json-glib/cairo/curl function (`__declspec(dllimport)`) | `g_get_real_time` | **NOT wrapped** | `radios_wrap(t DLLIMPORT sym)` + `RADIOS_WRAP_DLLIMPORT(...)`; `__real_sym` keeps working through the thunk |

All 18 wrapping targets (header_layout_driver, test_application_shutdown, test_automation_scope_ui,
test_automation_workspace_ui, test_broadcast_catalogue_ui, test_broadcast_header, test_external_access_ui,
test_header_ui, test_live_players_ui, test_player_loader_ui, test_player_tree_ui, test_player_waveform_markers,
test_playlist_export_ui, test_playlist_redesign_ui, test_programme_jingle_hint, test_programme_queue,
test_programme_view_header_ui, test_programme_view_settings_ui) wrap only `radios_*` or `gtk_*` except
`test_header_ui` (`g_get_real_time`, called from `src/ui/header_status.c`, compiled into the test). Never enable LTO
on targets that use `--wrap`.

### 4.10 Other linker options
* `-Wl,--as-needed` (`test_midi_hotplug`, CMakeLists:200): accepted on PE but meaningless (an import library only adds
  DLLs whose symbols are referenced) → delete.
* `-ffunction-sections -fdata-sections -Wl,--gc-sections` (`test_rotation_editor_regressions`, 613-614): on PE, ld
  still reports undefined references from unreferenced sections (**VERIFIED**), so the flags cannot hide missing
  symbols. A whole-tree scan of the 6 UI sources + test shows **no** reference resolved only outside
  `radios_playout/radios_audio/radios_core` (also for `test_schema_settings_ui`). Keep the flags or drop them; if a
  future UI change makes these links fail, link `radios_ui` instead.
* No `-static`, no `-static-libgcc`: GTK/FFmpeg DLLs import `libgcc_s_seh-1.dll`/`libwinpthread-1.dll` anyway;
  `clock_gettime`/`nanosleep` come from winpthreads.

### 4.11 Source coverage guard (new sources from parallel porting must not be forgotten)
As of 19.10 01:56, `src/core/platform.c`, `src/core/platform_win32.c`, `src/core/platform_tz.c`,
`src/audio/wasapi_backend.c`, `src/audio/wasapi_device.c`, `tests/test_platform.c`, `tests/test_wasapi_backend.c`
exist but no target compiles them.
```cmake
function(radios_check_source_coverage)
    file(GLOB_RECURSE all RELATIVE "${CMAKE_SOURCE_DIR}" CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.c")
    set(used "")
    foreach(t radios_platform radios_core radios_audio radios_playout radios_ui radios)
        get_target_property(srcs ${t} SOURCES)
        foreach(s IN LISTS srcs)
            cmake_path(ABSOLUTE_PATH s BASE_DIRECTORY "${CMAKE_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE abs)
            cmake_path(RELATIVE_PATH abs BASE_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE rel)
            list(APPEND used "${rel}")
        endforeach()
    endforeach()
    foreach(f IN LISTS all)
        if(NOT f IN_LIST used)
            message(FATAL_ERROR "${f} hiçbir RadioS hedefine eklenmemiş (CMakeLists.txt).")
        endif()
    endforeach()
endfunction()
```

---------------------------------------------------------------------------------------------------

## 5. CTest on Windows

### 5.1 Global test environment and safety nets (`cmake/RadioSTesting.cmake`, called from the deferred finalize)

```cmake
set(RADIOS_TEST_FOREIGN_CWD "${CMAKE_BINARY_DIR}/test-cwd")
file(MAKE_DIRECTORY "${RADIOS_TEST_FOREIGN_CWD}")
set(RADIOS_ZONEINFO_DIR "${CMAKE_SOURCE_DIR}/runtime/share/zoneinfo" CACHE PATH "Testlerin TZDIR'i (tzdata 2026.5)")
set(RADIOS_TEST_TIMEOUT_SCALE 2 CACHE STRING "Windows'ta test zaman aşımı çarpanı")

function(radios_finalize_tests)
    get_property(tests DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY TESTS)
    if(NOT tests)
        return()
    endif()
    set(mods
        "PATH=path_list_prepend:${RADIOS_MSYS2_PREFIX}/bin"   # DLLs, gspawn helpers, ffmpeg/ffprobe/openssl
        "PYTHONUTF8=set:1" "PYTHONIOENCODING=set:utf-8" "PYTHONNOUSERSITE=set:1" "PYTHONDONTWRITEBYTECODE=set:1"
        "TZDIR=set:${RADIOS_ZONEINFO_DIR}"
        "GDK_BACKEND=unset:" "DISPLAY=unset:" "WAYLAND_DISPLAY=unset:" "RADIOS_SDBASE_TOKEN=unset:")
    if(RADIOS_FFMPEG_PREFIX)
        list(APPEND mods "PATH=path_list_prepend:${RADIOS_FFMPEG_PREFIX}/bin")   # applied last → first in PATH
    endif()
    set_property(TEST ${tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION ${mods})
    # GLib: "-p /renamed/case" runs 0 tests and exits 0 (VERIFIED) -> make it a failure.
    set_property(TEST ${tests} APPEND PROPERTY FAIL_REGULAR_EXPRESSION "(^|[\r\n])1\\.\\.0[\r\n]")
    foreach(test IN LISTS tests)
        get_test_property(${test} LABELS labels)
        if(NOT "hardware" IN_LIST labels)
            set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT_MODIFICATION "RADIOS_AUDIO_SERVER=set:null:")
        endif()
        get_test_property(${test} TIMEOUT timeout)
        if(timeout)
            math(EXPR scaled "${timeout} * ${RADIOS_TEST_TIMEOUT_SCALE}")
            set_tests_properties(${test} PROPERTIES TIMEOUT ${scaled})
        endif()
    endforeach()
endfunction()
```
**VERIFIED** with CTest 4.4.4 (mini project `scratchpad\cm\src`, deferred exactly like this): `Path=C:/msys64/ucrt64/bin;
C:\WINDOWS\system32;…`, `PYTHONPATH` joined with `;`, `TZDIR`, `PYTHONUTF8`, `GDK_BACKEND` removed; per-test
`ENVIRONMENT` entries coexist, **but `ENVIRONMENT_MODIFICATION` is applied after `ENVIRONMENT`**, so the global
`RADIOS_AUDIO_SERVER=set:null:` overrides a per-test `ENVIRONMENT "RADIOS_AUDIO_SERVER=…"` (measured: `wasapi` →
`null:`) — a test that needs another server must carry the `hardware` label or set the variable in its runner's child
environment; the `hardware`-labelled probe kept no override; `TIMEOUT 20` became 40; a target and test declared
*after* the `cmake_language(DEFER …)` call were still finalized (8 MiB stack, manifest, binary `_fmode`);
`-p /probe/renamed-case` was reported as **Failed** by the `1..0` regex while the real case passed.

Reasons:
* `PATH`: test executables import ~60-130 DLLs from `C:\msys64\ucrt64\bin`; `C:\MinGW\bin` on the machine PATH holds
  incompatible `libwinpthread-1.dll`/`libgcc_s_seh-1.dll`/`libstdc++-6.dll`. Also provides `ffmpeg.exe`/`ffprobe.exe`
  (fixture generation), `openssl.exe` (`run_external_access.py`) and GLib's `gspawn-win64-helper*.exe` (tests use
  `g_spawn_*` for ffmpeg). GTK finds `share\glib-2.0\schemas`, icons and `loaders.cache` relative to its DLL, i.e. in
  the MSYS2 prefix — complete there (**VERIFIED** `org.gtk.Settings.FileChooser` compiled schema, Adwaita +
  `icon-theme.cache`, relative loaders cache).
* `TZDIR`: `radios_time_zone_directory()` (`include/radios/platform.h:408-410`) otherwise falls back to
  `<GLib DLL dir>\..\share\zoneinfo` = MSYS2 tzdata **2026e**, while the service uses Python tzdata **2026.5** —
  `runtime\share\zoneinfo` (already staged) is the same data as the service.
* `PYTHONUTF8`/`PYTHONIOENCODING`: runners start children with `sys.executable` without `-X utf8`.
* `RADIOS_AUDIO_SERVER=null:`: no test may touch the station's real audio devices (Linux tests used private Pulse
  daemons/null sinks); the audio backend resolves `NULL` server → `$RADIOS_AUDIO_SERVER` (`include/radios/audio.h:115`).
  Opt-out: label `hardware` (e.g. `test_wasapi_smoke`).
* `TIMEOUT` ×2: process creation, Python start-up, GTK init and Defender scanning are slower than on Linux.
  **VERIFIED**: the first launch of a freshly linked GLib test exe took 9.2 s, the next launches 0.04-0.06 s
  (first-run AV scan); after a full rebuild 144 test exes hit this at once.

### 5.2 Mechanical edits in `CMakeLists.txt` / `cmake/*.cmake`
1. Remove `;GDK_BACKEND=x11` / `GDK_BACKEND=x11;` — 52 sites in `CMakeLists.txt` (lines 150, 289, 312, 329, 349, 352,
   369, 412, 419, 431, 437, 471, 480, 486, 492, 578, 665, 668, 679, 684, 698, 713, 731, 745, 808, 816, 828, 833, 856,
   864, 874, 887, 898, 911, 926, 937, 948, 957, 975, 1006, 1015, 1032, 1040, 1056, 1065, 1073, 1075, 1085, 1102,
   1107, 1117, 1134) and 12 in modules (`MediaArchive.cmake:19`, `Pdf4Neon.cmake:23,34`, `Pdf2HeaderStatus.cmake:12,42`,
   `LibraryAnalysis.cmake:9`, `SilenceGuard.cmake:15`, `ResetUi.cmake:10`, `Prompt8Tests.cmake:17`,
   `Prompt7Tests.cmake:28,38`, `Prompt5Tests.cmake:24`). Keep `G_DEBUG=fatal-warnings`,
   `GTK_IM_MODULE=gtk-im-context-simple`; `NO_AT_BRIDGE=1` is Linux-only (AT-SPI) → remove (D1).
2. `WORKING_DIRECTORY /tmp` → `${RADIOS_TEST_FOREIGN_CWD}` (264, 442, 546). `/tmp` does not exist → CTest fails
   before running the test.
3. `PYTHONPATH=a:b` (916, 1092) → `ENVIRONMENT_MODIFICATION "PYTHONPATH=path_list_prepend:${CMAKE_SOURCE_DIR}/tests"
   "PYTHONPATH=path_list_prepend:${CMAKE_SOURCE_DIR}/database/SDBASE/src"`.
4. `${Python3_EXECUTABLE}` in `add_test` → `${RADIOS_TEST_PYTHON_COMMAND}` (keep `${CMAKE_SOURCE_DIR}/tests/…` paths).
5. Remove `RADIOS_AUDIO_NULL=1` (772), `PkgConfig::XTST`/`CONTENT_XTEST`/`ALSA`/`PULSE` links, `-Wl,--as-needed`.
6. `test_header_ui`: `radios_wrap(test_header_ui DLLIMPORT g_get_real_time)` replaces `Pdf2HeaderStatus.cmake:7`.
7. Replace `test_pulse_hotplug` (244-250) by the audio area's `test_audio_backend_hotplug`; add `test_null_provider`,
   `test_wasapi_smoke` (`LABELS hardware`, `SKIP_RETURN_CODE 77`), `test_platform` (`tests/test_platform.c`, links
   `radios_platform`), `test_wasapi_backend` (`tests/test_wasapi_backend.c`, links `radios_audio`).
8. `automation_recovery_stress`: add `radios_automation_e2e` target; runner gets `--driver $<TARGET_FILE:…>`.

### 5.3 Labels, resource locks, serialisation
| Property | Tests |
|---|---|
| `LABELS gui` | every test created under `RADIOS_GUI_TESTS` and the three always-on GTK runner tests (`schema_settings_ui`, `rotation_editor_regressions`, `player_waveform_markers`) |
| `LABELS audio` | null-provider realtime tests (`*-PCM`, `audio_devices`, `runner_pool_preload_pcm`, `header_navigation_realtime`, `preferences_output`, `audio_recovery`, `audio_soak`) |
| `LABELS service` | tests that start a private SDBASE service (all `run_data_ui`/`run_admin_ui`/`run_audio_ui` families, `desktop_*`, `database_*`) |
| `LABELS hardware` | `test_wasapi_smoke` (and any future physical-device check) — excluded by the default test presets |
| `RESOURCE_LOCK desktop_input` | tests that synthesise real input (`SendInput`/`SetCursorPos`): `native_content_scale_module`, `native_content_scale_popup_*` (5), `content_scale_ui`, `native_routing_graph`, `native_routing` |
| `RUN_SERIAL` | `audio_soak`, `audio_recovery`, `automation_recovery_stress` (real-time measurements) |

GUI tests on Windows open real windows on the current desktop (Linux used private Xvfb). They must not run on a
production broadcast PC. The rewritten `run_rotation_editor_regressions.py` (test area) must return 77 when no
interactive desktop is available (session 0 / locked workstation / `GetUserObjectInformation` ≠ `WinSta0`). Optional
isolation for non-input tests: create a private desktop (`CreateDesktopW`) and start the test with
`STARTUPINFO.lpDesktop` (ctypes in the runner) — input-injection tests cannot use it.

### 5.4 GLib case registration guard
```cmake
# radios_add_glib_cases(<test-prefix> <target> <source> <path-prefix> RUNNER <script|NONE> CASES <c>...)
# FATAL_ERROR when "<path-prefix>/<case>" is not literally present in <source> (stale names run 0 tests).
function(radios_add_glib_cases prefix target source path)
    cmake_parse_arguments(G "" "RUNNER" "CASES" ${ARGN})
    file(READ "${CMAKE_SOURCE_DIR}/${source}" text)
    foreach(case IN LISTS G_CASES)
        string(FIND "${text}" "\"${path}/${case}\"" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "${source}: \"${path}/${case}\" GLib testi yok.")
        endif()
        if(G_RUNNER STREQUAL "NONE")
            add_test(NAME ${prefix}${case} COMMAND ${target} -p ${path}/${case})
        else()
            add_test(NAME ${prefix}${case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                     "${CMAKE_SOURCE_DIR}/tests/${G_RUNNER}" $<TARGET_FILE:${target}> -p ${path}/${case})
        endif()
    endforeach()
endfunction()
```
Apply at least to `native_midi_hotplug_*` (current list contains the removed `ui-slow-sequencer`). The global
`FAIL_REGULAR_EXPRESSION` (§5.1) is the runtime net for all others.

### 5.5 Test counts (from a configure of a scratch copy with the Linux-only modules stubbed)
* `RADIOS_GUI_TESTS=OFF`: 96 tests, 56 executables. `ON`: **251 tests, 145 executables** (radios + 144).
* By command family (GUI ON): 65 direct C exes; 89 `run_audio_ui.py` family (`run_audio_ui` 16, `run_playout_ui` 66,
  `run_dj_ui` 2, `run_programme_ui` 1, `run_advert_ui` 1, `run_broadcast_ui` 2, `run_player_mapping_ui` 1); 24 Xvfb
  runner (`run_rotation_editor_regressions.py`); 24 data/service runners; 40 Python unit/schema tests; 5 private-Pulse
  runners; 4 others (`content_scale_ui`, `external_access`, `rhythm_upkeep`, `automation_recovery_stress`).
* 111 tests carry `GDK_BACKEND=x11` (31 of them with `SKIP_RETURN_CODE 77`, 12 are direct exes); 59 tests in total
  declare `SKIP_RETURN_CODE 77`.

### 5.6 Every test, by family, with the Windows action (owner area in brackets)

| Family / tests | Linux-only dependency | Action |
|---|---|---|
| **Direct C exes, no Linux deps at CMake level (46)**: automation_scope, live_control, programme_window, player_topology, routing_graph, rhythm, library_analysis, loudness_policy, loudness, loudness_engine, config, runtime, ui_preferences, data_client, recovery, media, programme_model, audio, audio_decoder, tempo, dsp, effect_return, transition, runner, output, channel_input, playout, patchbay, broadcast_header, output_resampler, preferences_storage, media_archive, storage_locations, silence_detector, local_ini, header_navigation_backend, song_seek, automation_calendar_plan, weekly_programme_plan, automation_probability_policy, automation_pdf_plan, automation_midtrack_jingle, runner_pool_preload, schema_track_picker, automation_probability_playout, playout_event_blocks | none at CMake level; source-level POSIX use in some (§13 P10 list: `test_config`, `test_data_client`, `test_media`, `test_audio_decoder`, `test_local_ini`, `test_media_archive`, `test_preferences_storage`, `test_storage_locations`, `test_ui_preferences`, `test_recovery` (`sys/wait.h`), `test_output` (`arpa/inet.h`), `test_patchbay` (`kill`), `test_tempo` (`newlocale/uselocale`)) | **Port** sources via `platform.h` (owning areas). CMake unchanged except global env. |
| `midi` | ALSA link | **Port** (done in source) → link `winmm`. |
| `pulse_hotplug` | fake libpulse + `pulse_backend.c` | **Replace** → `audio_backend_hotplug`. |
| `config_from_other_directory`, `native_controls`, `native_window` | cwd `/tmp` | **Port** (cwd). |
| Direct GTK tests (14): native_programme_queue, playlist_browser_ui, native_playlist_redesign, native_live_players, native_player_loader, native_header_status, native_preferences, native_silence_guard, native_library_analysis, native_neon_progress, native_header_progress, native_calendar_regressions, native_live_layout, programme_jingle_hint | `GDK_BACKEND=x11` (12 of them; `native_preferences`, `native_live_layout` have none) | **Port** (env only). They run on the current desktop. |
| **Xvfb runner (24)**: native_content_scale_module, native_content_scale_popup_{context-menus-at-pointer-and-widget, context-menus-before-map, combo-lists-and-menu-button, popover-and-tooltip, tree-search-entry}, view_presets_ui, native_player_tree, native_midi_hotplug_{input-announcements, input-liveness-without-announcements, ui-unplug-replug-refresh, ui-slow-sequencer, ui-teardown-pending-job}, native_automation_workspace, automation_scope_fetch, loudness_settings, settings_sync, schema_settings_ui, rotation_editor_regressions, player_waveform_markers, native_playlist_export, native_programme_view_settings, native_programme_view_header, native_programme_queue_scale | `Xvfb`, `GDK_BACKEND=x11`, `select` on pipes; content-scale tests link XTest (`gdk/gdkx.h`) | **Port**: keep the runner file name, rewrite its body (run on current desktop, 77 without interactive desktop) [test infra]; content-scale input via `SendInput` [ui-platform]; MIDI case list regenerated [midi]. |
| `content_scale_ui` | `run_content_scale_ui.py` (Xvfb + dbus-daemon), XTest | **Port** (GDK_SCALE variants on the Win32 backend) [ui-platform]. |
| **Audio runner family (89)**: native_audio, audio_devices, native_player_workspace, native_automation_workspace_acceptance, native_loudness_policy_* (5), header_navigation_realtime, native_live_layout_connected, native_general_queue/schema_entry/schema_apply, runner_pool_preload_pcm, native_mixer_auto_volume, all 66 `run_playout_ui.py` tests (native_playout, header/prompt8/jingle/autoplay/live_row/live_step/local_event/event_*/autodj …), native_effect_return, native_dj, native_programme, native_advert, native_broadcast, native_broadcast_automatic_metadata_filter, native_player_mapping | `pactl`/`parec` module-null-sink capture, `DISPLAY` check | **Port** runners to the null provider (`RADIOS_AUDIO_SERVER=null:<json>`, capture files) [audio-backend 6.2]; CMake: env only. |
| `preferences_output`, `audio_recovery`, `audio_soak` | private Pulse daemon, `setsid`, `/proc`, `parec` | **Port** [audio-backend 6.2]. |
| `native_routing`, `native_routing_graph` (+ XTST link) | private Pulse daemon, `pactl`/`pacat`, XTest | **Port** (WASAPI host fixture + `SendInput`) [audio-routing-capture]. |
| **Data/service runners (24)**: native_media_drop, native_media, native_media_archive, native_reset_ui, native_prompt12_drop, native_media_rights, native_copyright_settings, native_playout_policy, native_workflow_hub, native_data, native_automation_calendar, native_player_schema_weekly, native_import_profiles, native_external_access, native_admin, native_catalogue_playlist, native_broadcast_catalogue, native_emergency_transfer, native_transfer_ui, native_rekordbox, native_data_connection, native_stream_metadata, native_header_navigation, native_automation_probability | `chmod 0o600` expectations, `flatpak-spawn` branch (`run_data_connection_ui.py:39-40`), service stop | **Port** runners (Python shim, D6) [tests/SDBASE]; CMake env only. |
| `native_application_shutdown` | SIGTERM/SIGINT, `fcntl.flock`, Pulse device | **Port** (named stop event, `WM_CLOSE`, `WM_ENDSESSION`, LockFileEx probe) [ui-platform §15]. |
| `external_access` | `openssl` CLI | keep (MSYS2 `openssl.exe` via PATH). |
| `rhythm_upkeep` | none | keep. |
| `automation_recovery_stress` | compiles C at test time with `cc` + `libpulse` | **Replace** compile step by `radios_automation_e2e` target. |
| **Python (40)**: radios_launch, desktop_player_topology, stream_metadata_schema, routing_graph_schema, database_setup, database_recovery, playlist_selection, playlist_workspace, desktop_api, desktop_routing, desktop_reset, desktop_programme, desktop_local_event, desktop_rekordbox, desktop_rekordbox_device, data_playlists, desktop_media, desktop_playout, desktop_advert, desktop_broadcast, desktop_admin, reliability, packaging, release_install, storage_locations_service, desktop_automation_calendar, desktop_weekly_programme, desktop_media_rights, desktop_copyright, playout_policy, playlist_sweeper_schema, desktop_probability_policy, desktop_workflow_bridge, desktop_transfers, desktop_transfer_package, desktop_integrity, gui_backend_coverage, desktop_playout_vocal_cues, desktop_autodj | `/proc`, `os.kill(SIGTERM)`, `chmod` modes, `fcntl` (database_setup, reliability, release_install), ELF/`readelf`/tar modes (packaging), `run-radios.sh`/flatpak (radios_launch, release_install) | **Port** in their areas; `radios_launch` drops the `SdkSandbox` case (D1). CMake: python command + env. |

### 5.7 Executables that depend on Linux-only libraries or Linux-only sources (task item)
| Target | Linux-only dependency | Recommendation |
|---|---|---|
| `radios_audio` (all 130+ dependents) | `PkgConfig::PULSE`; `pulse_backend.c`, `channel_input.c`, `patchbay.c` include `pulse/pulseaudio.h` | **Port** (WASAPI backend + host fixture; audio areas). |
| `radios_core` (all dependents) | `PkgConfig::ALSA` (`midi.c` already WinMM) | **Port** — link `winmm cfgmgr32`. |
| `radios`, `test_application_shutdown` | `src/app/main.c`: `glib-unix.h`, `g_unix_signal_add` | **Port** (ui-platform). |
| `radios_core` sources | `recovery.c` (`sys/file.h` flock), `library_analysis.c` (`sys/resource.h`, `sys/syscall.h`), `storage_locations.c` (`sys/statvfs.h`), file-API users | **Port** via `platform.h` (core-posix-files). |
| `test_pulse_hotplug` | `pulse_backend.c`, `${PULSE_INCLUDE_DIRS}` | **Replace** (`test_audio_backend_hotplug`). |
| `test_midi` | `PkgConfig::ALSA` | **Port** (done) — link `winmm`. |
| `test_midi_hotplug` | `alsa/asoundlib.h` (old), `-Wl,--as-needed`, Xvfb runner | **Port** (WinMM fake, already in progress), drop `--as-needed`, validated case list. |
| `test_content_scale_module`, `test_content_scale_popups` | `xtst x11`, `gdk/gdkx.h`, XTest | **Port** (`SendInput` branch in `content_scale_test_input.h`; link nothing extra). |
| `test_content_scale_ui` | `xtst x11` (Preferences.cmake:43-47), `gdk/gdkx.h` | **Port** (same). |
| `test_routing_graph_ui`, `test_routing_ui` | `xtst x11`; `routing_patchbay_ui_test.inc` uses `gdk/gdkx.h` + XTest | **Port** (`SendInput`; audio-routing area). |
| `test_audio_recovery` | `sys/wait.h`, `signal.h`, `kill`, `waitpid`, `pactl` | **Port** (null provider API). |
| `test_audio_soak` | `/proc/self/statm`, `sysconf`, `/proc/self/fd` | **Port** (`GetProcessMemoryInfo`, `GetProcessHandleCount`; link `psapi`). |
| `test_recovery` | `sys/wait.h`, `sys/file.h` | **Port** (process + lock APIs). |
| `test_output` | `arpa/inet.h`, `poll.h`, `sys/socket.h` | **Port** (GSocket/GIO or Winsock2 `ws2_32`). |
| `test_patchbay` | `signal.h`/`kill`, `env PULSE_SERVER=… pactl` | **Port** (audio-routing fixture). |
| `test_tempo` | `newlocale`/`uselocale`/`locale_t` | **Port** (`_create_locale` + `_configthreadlocale(_ENABLE_PER_THREAD_LOCALE)`). |
| 25 GUI/audio tests using `pread`, 13 using `symlink`, 3 `mkfifo`, `pwrite`, `lstat`, `realpath` | POSIX file APIs | **Port** (`radios_fd_pread`, `radios_platform_test_symlink`, …). `mkfifo` cases: **exclude** those cases (no FIFOs on NTFS) with `g_test_skip` + reason. |
None needs to be excluded as a whole: every subject has a Windows counterpart (D1).

---------------------------------------------------------------------------------------------------

## 6. Runtime prefix, staging and install layout

### 6.1 Layout (D4)
```
<prefix>\                       dev: <root>\runtime\ (only bin/lib/share/etc/python)   installed: %LOCALAPPDATA%\Programs\RadioS\
  bin\RadioS.exe  + every dependent DLL  + gspawn-win64-helper.exe, gspawn-win64-helper-console.exe
  lib\gdk-pixbuf-2.0\2.10.0\loaders\{libpixbufloader-png.dll, pixbufloader_svg.dll, libpixbufloader-jpeg.dll,
                                      libpixbufloader-gif.dll, libpixbufloader-ico.dll, libpixbufloader-bmp.dll}
  lib\gdk-pixbuf-2.0\2.10.0\loaders.cache          (regenerated, relative paths)
  share\glib-2.0\schemas\org.gtk.Settings.{FileChooser,ColorChooser,EmojiChooser,Debug}.gschema.xml + gschemas.compiled
  share\icons\Adwaita\ (incl. cursors, symbolic, icon-theme.cache)   share\icons\hicolor\ (index.theme + cache)
  share\themes\Default\gtk-3.0\gtk-keys.css        share\gtk-3.0\emoji\ (optional)
  share\locale\tr\LC_MESSAGES\{gtk30.mo, glib20.mo, gdk-pixbuf.mo, json-glib-1.0.mo}
  share\zoneinfo\…                                 (tzdata 2026.5 wheel, same data as the service)
  etc\gtk-3.0\settings.ini                         (packaging/windows/gtk-settings.ini, content from the UI area)
  etc\fonts\fonts.conf + conf.d\                   (cairo/avfilter still load fontconfig)
  etc\ssl\certs\ca-bundle.crt                      (libcurl/OpenSSL; network area should also set CURLSSLOPT_NATIVE_CA)
  python\                                          (CPython 3.14.8 embeddable, no ._pth, Lib\tzdata)
installed only (component "application"):
  config\README.md, config\radios.example.ini      (root marker #1; python_command must be valid on Windows — §11)
  database\SDBASE\pyproject.toml (root marker #2), README.md, src\sdbase\**\*.py
  tools\radios_database.py, radios_database_setup.py, radios_reliability.py, radios_hardware_check.py, _platform.py,
        radios_launch.py (if the installed launcher keeps it)
  assets\fonts\InterVariable.ttf, LICENSE.txt; assets\themes\*.css
  docs\*.md (user documentation)
  data\{backups,cache,database,endpoints,imports,logs,recordings}\   (empty; created by the installer)
  RadioS-veritabani.cmd (service tool launcher) — tools area
```
Root discovery (`src/core/config.c:83-116`): walks up from the exe to the first directory containing
`config\radios.example.ini` **and** `database\SDBASE\pyproject.toml` → `runtime\bin` → `runtime` → `<root>` (dev),
`<prefix>\bin` → `<prefix>` (installed). Both marker files are therefore mandatory in the installer.
GTK/GLib/gdk-pixbuf/fontconfig locate `share`, `lib`, `etc` relative to `bin\libgtk-3-0.dll` → the prefix layout above
works unmodified in both places.

### 6.2 Install components (`cmake/RadioSRuntime.cmake`)
```cmake
set(RADIOS_GTK_LOADERS png svg jpeg gif ico bmp)
install(TARGETS radios RUNTIME DESTINATION bin COMPONENT runtime)
install(FILES "${RADIOS_MSYS2_PREFIX}/bin/gspawn-win64-helper.exe" "${RADIOS_MSYS2_PREFIX}/bin/gspawn-win64-helper-console.exe"
        DESTINATION bin COMPONENT runtime)
# loaders: libpixbufloader-<x>.dll except svg = pixbufloader_svg.dll (MSYS2 naming)
…install(FILES <loader dlls> DESTINATION lib/gdk-pixbuf-2.0/2.10.0/loaders COMPONENT runtime)
install(FILES <4 GTK schema xml> DESTINATION share/glib-2.0/schemas COMPONENT runtime)
install(DIRECTORY "${RADIOS_MSYS2_PREFIX}/share/icons/Adwaita" "${RADIOS_MSYS2_PREFIX}/share/icons/hicolor"
        DESTINATION share/icons COMPONENT runtime)
install(DIRECTORY "${RADIOS_MSYS2_PREFIX}/share/themes/Default" DESTINATION share/themes COMPONENT runtime)
install(FILES <4 tr .mo> DESTINATION share/locale/tr/LC_MESSAGES COMPONENT runtime)
install(DIRECTORY "${RADIOS_MSYS2_PREFIX}/etc/fonts" DESTINATION etc COMPONENT runtime)
install(FILES "${RADIOS_MSYS2_PREFIX}/etc/ssl/certs/ca-bundle.crt" DESTINATION etc/ssl/certs COMPONENT runtime)
install(FILES packaging/windows/gtk-settings.ini DESTINATION etc/gtk-3.0 RENAME settings.ini COMPONENT runtime)
install(CODE "set(RADIOS_MSYS2_PREFIX \"${RADIOS_MSYS2_PREFIX}\")
              set(RADIOS_FFMPEG_PREFIX \"${RADIOS_FFMPEG_PREFIX}\")
              set(RADIOS_GTK_LOADERS \"${RADIOS_GTK_LOADERS}\")" COMPONENT runtime)
install(SCRIPT cmake/RadioSInstallDeps.cmake COMPONENT runtime)
install(CODE "include(\"${CMAKE_SOURCE_DIR}/cmake/RadioSPythonRuntime.cmake\")
              radios_python_runtime(\"\${CMAKE_INSTALL_PREFIX}\")" COMPONENT python)
# component "application": config/, database/SDBASE/, tools/, assets/, docs/ (explicit allow-lists, never data/ or user config)
```
`cmake/RadioSInstallDeps.cmake` (install time; **VERIFIED** call shape resolves 46 DLLs for GTK + loaders + helper):
```cmake
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)   # normalise paths before the exclude regexes (CMake 4.x)
endif()
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${RADIOS_MSYS2_PREFIX}/bin/objdump.exe")
set(prefix "${CMAKE_INSTALL_PREFIX}")
set(dirs "${RADIOS_MSYS2_PREFIX}/bin")
if(RADIOS_FFMPEG_PREFIX)
    list(PREPEND dirs "${RADIOS_FFMPEG_PREFIX}/bin")
endif()
file(GLOB loaders "${prefix}/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${prefix}/bin/RadioS.exe" "${prefix}/bin/gspawn-win64-helper.exe"
                "${prefix}/bin/gspawn-win64-helper-console.exe"
                "${RADIOS_MSYS2_PREFIX}/bin/gdk-pixbuf-query-loaders.exe"
    MODULES ${loaders}
    DIRECTORIES ${dirs}
    PRE_EXCLUDE_REGEXES "^api-ms-win-.*" "^ext-ms-.*"
    POST_EXCLUDE_REGEXES "^[A-Za-z]:/[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/.*"
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    CONFLICTING_DEPENDENCIES_PREFIX conflict)
if(unresolved OR conflict_FILENAMES)
    message(FATAL_ERROR "Çözülemeyen DLL bağımlılıkları: ${unresolved} ${conflict_FILENAMES}")
endif()
file(INSTALL ${resolved} DESTINATION "${prefix}/bin")
file(INSTALL "${RADIOS_MSYS2_PREFIX}/bin/gdk-pixbuf-query-loaders.exe" DESTINATION "${prefix}/bin")
execute_process(COMMAND "${prefix}/bin/gdk-pixbuf-query-loaders.exe" --update-cache COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE "${prefix}/bin/gdk-pixbuf-query-loaders.exe")
execute_process(COMMAND "${RADIOS_MSYS2_PREFIX}/bin/glib-compile-schemas.exe" "${prefix}/share/glib-2.0/schemas"
                COMMAND_ERROR_IS_FATAL ANY)
foreach(theme Adwaita hicolor)
    execute_process(COMMAND "${RADIOS_MSYS2_PREFIX}/bin/gtk-update-icon-cache.exe" -q -t -f "${prefix}/share/icons/${theme}"
                    COMMAND_ERROR_IS_FATAL ANY)
endforeach()
```
Notes: the copied `gdk-pixbuf-query-loaders.exe` writes `loaders.cache` with paths relative to *its* prefix (MSYS2's own
cache already uses `"lib\\gdk-pixbuf-2.0\\2.10.0\\loaders\\…dll"` — **VERIFIED**), so the cache is relocatable.
`gdbus.exe` is not needed (`main.c:166` uses `G_APPLICATION_NON_UNIQUE`); no GIO modules are installed in MSYS2;
no GtkBuilder/GModule symbol lookup exists (no `--export-all-symbols`).

`cmake/RadioSPythonRuntime.cmake` (`radios_python_runtime(<prefix>)`):
* inputs: `RADIOS_PYTHON_EMBED_ZIP` (default `packaging/windows/cache/python-3.14.8-embed-amd64.zip`),
  `RADIOS_PYTHON_EMBED_SHA256=a93abe456ab01bd96d7a085b3cdb6566b3063f4241360d114142fbdb07f0a310`,
  `RADIOS_TZDATA_WHEEL` (default `packaging/windows/cache/tzdata-2026.5-py2.py3-none-any.whl`),
  `RADIOS_TZDATA_SHA256=b683bd1b6659ddcd810ff02ad09ba821d4bf1065072805063eb35c49617905ac` (both **VERIFIED**).
* skip when `<prefix>/python/.radios-runtime.json` records both hashes; else `file(SHA256)` check → FATAL on
  mismatch → `file(ARCHIVE_EXTRACT)` into a temp dir → delete `python314._pth` (D4; present in the zip — **VERIFIED**)
  → extract wheel → copy `tzdata/` to `python/Lib/tzdata` and `tzdata/zoneinfo/` to `share/zoneinfo` → write stamp.
* never touches a running interpreter: if `python\python.exe` cannot be opened for writing (the SDBASE service runs
  from it), stop with "Önce veritabanı servisini durdurun (run-database.cmd stop)".
* adoption of the hand-staged `runtime\python` (D4 "must be preserved"): when the stamp is missing but every file of
  the zip (except `python314._pth`) exists with identical SHA-256 and `Lib\tzdata\__init__.py` matches the wheel,
  write the stamp without re-extracting.
* install order: these `install()` rules must be declared after the `radios`, helper and loader rules, because
  `cmake_install.cmake` runs them in declaration order and the dependency scan reads the already-installed files.

### 6.3 Dev staging into `<root>\runtime` (target `radios_runtime`)
```cmake
add_custom_target(radios_runtime
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_BINARY_DIR}/runtime-stage"
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --component runtime --prefix "${CMAKE_BINARY_DIR}/runtime-stage"
    COMMAND ${RADIOS_BUILD_PYTHON_COMMAND} "${CMAKE_SOURCE_DIR}/tools/radios_sync_runtime.py"
            --stage "${CMAKE_BINARY_DIR}/runtime-stage" --prefix "${CMAKE_SOURCE_DIR}/runtime" --keep python --keep share/zoneinfo
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --component python --prefix "${CMAKE_SOURCE_DIR}/runtime"
    DEPENDS radios USES_TERMINAL VERBATIM)
```
`tools/radios_sync_runtime.py` (new, stdlib only, owner: this area):
1. Build the change list (size + SHA-256) between stage and prefix (excluding `--keep` subtrees).
2. **In-use check before touching anything**: try `os.open(path, os.O_RDWR | os.O_BINARY)` on every file to be
   replaced or removed under `bin\` and `lib\`; a running `RadioS.exe` maps its image and DLLs without
   `FILE_SHARE_WRITE` → `PermissionError` (WinError 32). If any fails: print
   "RadioS çalışıyor; çalışma ortamı güncellenmedi (kapatıp yeniden deneyin)" and exit 1 with nothing changed
   (the launcher then starts the last good runtime, like Linux's "son sağlam build/radios").
3. Copy each changed file to `<name>.new-<pid>` in the same directory, `os.replace()` it over the target; remove files
   that disappeared from the stage; remove empty directories. Never touch `python\` or files outside the prefix.
4. Write `runtime\.radios-stage.json` (version, build type, file list with hashes) for diagnostics.

### 6.4 Sizes (VERIFIED closures via `objdump -p` BFS)
* GTK + json-glib + curl + fontconfig + loaders + gspawn: **63 files / 55.7 MB**.
* plus MSYS2 FFmpeg (avformat/avcodec/avutil/swresample/avfilter): **127 files / 177.5 MB** — largest: libx265 21.6,
  avcodec 19.7, librsvg 11.2, libshaderc_shared 10.2, libaom 9.2, SvtAv1Enc 7.2, avfilter 6.5, libjxl 5.9 MB; also
  `vulkan-1.dll` (must never be shipped by an app), `libva*`, `libvpl-2.dll`, `ggml*`/`libwhisper-1.dll`.
* Icons ≈ 7 MB (Adwaita 4.8 MB, of which cursors 3.8 MB), Python ≈ 30 MB, app ≈ 10 MB.

---------------------------------------------------------------------------------------------------

## 7. Packaging and installer

### 7.1 NSIS (`packaging/windows/radios.nsi.in`, configured by CMake, built by target `radios_installer`)
```cmake
add_custom_target(radios_installer
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${RADIOS_PACKAGE_STAGE}"
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --prefix "${RADIOS_PACKAGE_STAGE}"
    COMMAND ${RADIOS_BUILD_PYTHON_COMMAND} "${CMAKE_SOURCE_DIR}/tools/radios_package.py" stage-manifest
            --stage "${RADIOS_PACKAGE_STAGE}" --nsis-files "${CMAKE_BINARY_DIR}/packaging/files.nsh"
    COMMAND "${RADIOS_MSYS2_PREFIX}/bin/makensis.exe" -INPUTCHARSET UTF8 -V2
            "-DRADIOS_VERSION=${PROJECT_VERSION}" "-DRADIOS_STAGE=${RADIOS_PACKAGE_STAGE}"
            "-DRADIOS_OUT=${CMAKE_SOURCE_DIR}/dist/RadioS-${PROJECT_VERSION}-windows-x64-setup.exe"
            "${CMAKE_BINARY_DIR}/packaging/radios.nsi"
    DEPENDS radios USES_TERMINAL VERBATIM)
```
Script requirements:
* `Unicode true`, `Target amd64-unicode` (only stubs MSYS2 ships), `ManifestDPIAware true`,
  `ManifestSupportedOS {8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}`, `SetCompressor /SOLID lzma`, MUI2 with
  `!insertmacro MUI_LANGUAGE "Turkish"` (Turkish.nlf/.nsh **VERIFIED** present), file saved UTF-8 (`-INPUTCHARSET UTF8`).
* `RequestExecutionLevel user`; `InstallDir "$LOCALAPPDATA\Programs\RadioS"`. Data and config live inside the prefix
  (D4) and must stay writable without elevation; reject `$PROGRAMFILES*` targets with an explanation; warn when
  `StrLen($INSTDIR) > 80` (MAX_PATH; long-path policy is off on this machine).
* Pre-install on upgrade: if `$INSTDIR\bin\RadioS.exe` cannot be opened for writing → "RadioS açık; kapatın" and abort;
  if `$INSTDIR\python\python.exe` exists, run
  `"$INSTDIR\python\python.exe" -E -s -B -X utf8 "$INSTDIR\tools\radios_database.py" --project-root "$INSTDIR\." stop`
  via `nsExec::ExecToLog` before replacing `python\` and `database\SDBASE\src\` (the service outlives the GUI, D4).
* Files: only the generated `files.nsh` list (exact; from the stage manifest). Create the seven empty `data\` dirs.
  **Never install, overwrite or delete** `data\**`, `config\radios.ini`, `config\credentials*`, `config\private\`,
  `config\{ui-preferences,audio-panel,media-archive,library-analysis,storage}.ini`, `config\*.local.ini`.
* Uninstall: delete exactly the installed files (same list) and empty directories; leave data/config; final page states
  where the station data remains.
* Registry (per-user): `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\RadioS` (`DisplayName`,
  `DisplayVersion`, `Publisher`, `DisplayIcon=$INSTDIR\bin\RadioS.exe`, `InstallLocation`, `UninstallString`,
  `QuietUninstallString`, `EstimatedSize`, `NoModify=1`, `NoRepair=1`).
* Shortcuts: Start menu "RadioS" → `$INSTDIR\bin\RadioS.exe` (working dir `$INSTDIR`); optional desktop shortcut.
* Signing hook: `!finalize` / `!uninstfinalize` with `${RADIOS_SIGN_COMMAND}` when defined (signtool/osslsigncode);
  sign `RadioS.exe` in the stage before `makensis` runs, then the installer and uninstaller via the hooks.

### 7.2 Portable archive and `tools/radios_package.py` contract (tools area implements)
* `binary` kind on Windows: input is the stage prefix, not `build/radios`; checks become: PE32+ (`MZ` + `PE\0\0`,
  machine 0x8664), `RadioS.exe --version` prints `RadioS <ver> `, CMake cache `CMAKE_BUILD_TYPE=Release`,
  every non-system DLL imported (recursively, `objdump -p`) is present in `bin\`, nothing imports `asan`, no absolute
  build path in the image (thanks to `-ffile-prefix-map` and relative `--pdb`), `vulkan-1.dll` not bundled.
  `runtime.json` lists: architecture `x86_64`, os `Windows`, minimum Windows 10 (supportedOS), bundled DLLs with
  SHA-256, Python `3.14.8`, `system_libraries_bundled: true`.
* Archive: deterministic **zip** (sorted names, fixed timestamps 1980-01-01, no extra fields) named
  `RadioS-<ver>-windows-x64.zip` + `.sha256`; `source` kind unchanged in spirit (tar.gz or zip).
* Allow-lists must grow for Windows inputs: `packaging` (`.in`, `.nsi`, `.ini`, `.ico`), `assets` (`.ico`, `.ttf`),
  `tools` (`.cmd`, `.ps1`), `src/app/crt_binmode.c`, `tests/test_process_setup.c`, `CMakePresets.json`,
  `.gitattributes`; remove `run-*.sh` (D1). `packaging/windows/cache/**` and `runtime/**` are never archived.
* The `_read()` helper uses `O_NOFOLLOW`, `O_DIRECTORY`, `dir_fd` (absent on Windows) → tools `_platform` shim (D6).
* `packaging` test (`tests/test_packaging.py`) changes: ELF fixtures → PE fixtures; symlink cases skip without the
  privilege (`radios_platform_test_symlink` semantics); `os.link` hard-link case still valid on NTFS.

### 7.3 Symbols, reproducibility, signing
* Keep `RadioS.pdb` per release in a symbol store (WinDbg/VS read it; DWARF would be useless for minidumps).
* `-Wl,--no-insert-timestamp`, `-ffile-prefix-map`, sorted zip → identical inputs produce identical outputs.
* Unsigned installers trigger SmartScreen; budget an Authenticode certificate (organisational decision).

### 7.4 Licensing and FFmpeg size (lead decision)
MSYS2 FFmpeg is GPLv3 (`--enable-gpl --enable-version3`, x264/x265/xvid/vidstab/frei0r…). Bundling it makes the
distributed RadioS package subject to GPLv3 (the Linux package avoided this by not bundling system libraries;
`packaging/README.md:27`). The project defines no licence. Options:
1. **Recommended**: build a minimal LGPL, audio-only FFmpeg once with the MSYS2 toolchain into
   `third_party/ffmpeg-win64` (shared libs), point `RADIOS_FFMPEG_PREFIX` at it. What the code needs (grep):
   filters `abuffer`, `atempo`, `abuffersink` (+ `aformat`/`aresample` for negotiation), encoder `libmp3lame`
   (fallback `AV_CODEC_ID_MP3`), decoders/demuxers/parsers for WAV, MP3, FLAC, Ogg (Vorbis/Opus), M4A/MP4 (AAC, ALAC),
   ADTS AAC, PCM variants; protocol `file`; `swresample`. Typical configure:
   `--enable-shared --disable-static --disable-programs --disable-doc --disable-everything --disable-autodetect
   --disable-network --enable-avformat --enable-avcodec --enable-avfilter --enable-swresample
   --enable-protocol=file --enable-demuxer=wav,w64,mp3,flac,ogg,mov,aac,aiff
   --enable-decoder=pcm_*,mp3*,flac,vorbis,opus,aac,aac_latm,alac --enable-parser=mpegaudio,flac,aac,vorbis,opus
   --enable-filter=abuffer,abuffersink,atempo,aformat,aresample,anull --enable-encoder=libmp3lame
   --enable-libmp3lame --enable-zlib`. Result ≈ 6-10 MB instead of 122 MB, no Vulkan/VA/whisper DLLs.
   The DLL names collide with MSYS2's (`avcodec-63.dll` …), so the custom `bin` must come first in every PATH/DIRECTORIES
   list (handled in §5.1 and §6.2). Tests keep using MSYS2 `ffmpeg.exe` for fixture generation.
2. Ship MSYS2 FFmpeg and comply with GPLv3 (sources/notices for FFmpeg and its GPL deps; RadioS licence compatible).

---------------------------------------------------------------------------------------------------

## 8. `CMakePresets.json` (new, repository root)

```json
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 25, "patch": 0 },
  "configurePresets": [
    {
      "name": "windows-base",
      "hidden": true,
      "generator": "Ninja",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "environment": {
        "PATH": "C:/msys64/ucrt64/bin;C:/msys64/usr/bin;$penv{PATH}",
        "PKG_CONFIG_PATH": "",
        "PKG_CONFIG_LIBDIR": "C:/msys64/ucrt64/lib/pkgconfig;C:/msys64/ucrt64/share/pkgconfig"
      },
      "cacheVariables": {
        "CMAKE_C_COMPILER": "C:/msys64/ucrt64/bin/gcc.exe",
        "CMAKE_RC_COMPILER": "C:/msys64/ucrt64/bin/windres.exe",
        "CMAKE_MAKE_PROGRAM": "C:/msys64/ucrt64/bin/ninja.exe",
        "PKG_CONFIG_EXECUTABLE": "C:/msys64/ucrt64/bin/pkg-config.exe",
        "Python3_EXECUTABLE": "C:/msys64/ucrt64/bin/python.exe",
        "RADIOS_MSYS2_PREFIX": "C:/msys64/ucrt64",
        "RADIOS_TEST_PYTHON": "${sourceDir}/runtime/python/python.exe",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
      }
    },
    { "name": "windows-debug", "displayName": "Debug (testler, GUI testleri kapalı)", "inherits": "windows-base",
      "binaryDir": "${sourceDir}/build",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug", "BUILD_TESTING": "ON", "RADIOS_GUI_TESTS": "OFF" } },
    { "name": "windows-debug-gui", "displayName": "Debug + GUI testleri", "inherits": "windows-base",
      "binaryDir": "${sourceDir}/build/gui",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug", "BUILD_TESTING": "ON", "RADIOS_GUI_TESTS": "ON" } },
    { "name": "windows-release", "displayName": "Release (uygulama, runtime, kurulum)", "inherits": "windows-base",
      "binaryDir": "${sourceDir}/build/release",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "BUILD_TESTING": "OFF", "RADIOS_GUI_TESTS": "OFF",
                          "RADIOS_PACKAGE_STAGE": "${sourceDir}/build/release/stage/RadioS" } },
    { "name": "windows-release-tests", "displayName": "Release + bütün testler (kabul)", "inherits": "windows-base",
      "binaryDir": "${sourceDir}/build/release-tests",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "BUILD_TESTING": "ON", "RADIOS_GUI_TESTS": "ON" } }
  ],
  "buildPresets": [
    { "name": "windows-debug", "configurePreset": "windows-debug" },
    { "name": "windows-debug-gui", "configurePreset": "windows-debug-gui" },
    { "name": "windows-release", "configurePreset": "windows-release", "targets": [ "radios" ] },
    { "name": "windows-runtime", "configurePreset": "windows-release", "targets": [ "radios_runtime" ] },
    { "name": "windows-installer", "configurePreset": "windows-release", "targets": [ "radios_installer" ] },
    { "name": "windows-release-tests", "configurePreset": "windows-release-tests" }
  ],
  "testPresets": [
    { "name": "windows-debug", "configurePreset": "windows-debug",
      "output": { "outputOnFailure": true },
      "execution": { "jobs": 4, "noTestsAction": "error" },
      "filter": { "exclude": { "label": "hardware" } } },
    { "name": "windows-debug-gui", "inherits": "windows-debug", "configurePreset": "windows-debug-gui" },
    { "name": "windows-release-tests", "inherits": "windows-debug", "configurePreset": "windows-release-tests" }
  ],
  "workflowPresets": [
    { "name": "windows-installer", "steps": [
        { "type": "configure", "name": "windows-release" },
        { "type": "build", "name": "windows-installer" } ] }
  ]
}
```
Usage (PowerShell, from the RadioS root): `cmake --preset windows-debug; cmake --build --preset windows-debug;
ctest --preset windows-debug` — run with `C:\msys64\ucrt64\bin\cmake.exe`/`ctest.exe` explicitly or with that dir first
in PATH (the CMake 3.27 in `C:\MinGW` understands presets v6 but not `CMP0207` and must not be used). `windows-debug`
uses the shared `build\` (integrator-owned, D7). Never rely on MSYS2 CMake's default generator or install prefix
(default prefix measured: `C:/Program Files (x86)/RadioS`). `RADIOS_PACKAGE_STAGE` is declared in
`cmake/RadioSRuntime.cmake` as `set(RADIOS_PACKAGE_STAGE "${CMAKE_BINARY_DIR}/stage/RadioS" CACHE PATH …)`.

---------------------------------------------------------------------------------------------------

## 9. `.gitignore` and `.gitattributes`

`.gitignore` additions/changes (keep the existing private-data rules):
```gitignore
/build/*
!/build/.gitkeep
/dist/
/runtime/                       # dev runtime prefix (bin, lib, share, etc, python) — generated
/packaging/windows/cache/       # python embeddable zip + tzdata wheel (hashes are pinned in CMake)
/third_party/ffmpeg-win64/      # optional custom FFmpeg build output
/default-binary.lock            # launcher build lock (tools/radios_launch.py)
*.obj
*.pdb
*.dmp
*.ilk
/config/audio-panel.ini
/config/media-archive.ini
/config/library-analysis.ini
/config/storage.ini
/config/private/
```
(`*.o *.a *.so *.dll *.exe` already present; `*.so` is obsolete under D1 and may go.) Note: a real
`config/credentials.json` and `config/ui-preferences.ini` exist in this working copy — they must never be packaged.

`.gitattributes` (new) — Git for Windows defaults to `core.autocrlf=true`; CRLF checkouts change bytes that tests
and manifests hash (e.g. SDBASE source digest D6, `islev_envanteri.py --check`, package manifests):
```gitattributes
* text=auto eol=lf
*.cmd text eol=crlf
*.bat text eol=crlf
*.ico binary
*.ttf binary
*.woff2 binary
*.png binary
*.wav binary
*.zip binary
*.whl binary
*.sdb binary
```
(`.cmd`/`.bat` must be CRLF: cmd.exe mis-parses labels in LF-only batch files.)

---------------------------------------------------------------------------------------------------

## 10. Documentation in this area (Turkish drafts)

`cmake/README.md`:
> Derleme desteği. `../CMakeLists.txt` C11 kütüphane/uygulama/test hedeflerini kurar; yalnız Windows x64 ve MSYS2
> UCRT64 araç zinciri (`C:\msys64\ucrt64`) desteklenir. Ön ayarlar `../CMakePresets.json` içindedir:
> `cmake --preset windows-debug`, `cmake --build --preset windows-debug`, `ctest --preset windows-debug`.
> `RadioSToolchain.cmake` araç zincirini doğrular ve genel bayrakları koyar; `RadioSWindowsExe.cmake` her
> programa manifest, simge, sürüm bilgisi, ikili dosya kipi ve 8 MiB yığın ekler; `RadioSTesting.cmake` test
> ortamını (PATH, TZDIR, UTF-8 Python, `RADIOS_AUDIO_SERVER=null:`) kurar; `RadioSRuntime.cmake` çalışma ortamını
> (`runtime\`) ve kurulum paketini hazırlar. Tasarım CSS'i build sırasında `tools/css_to_gtk.py` ile C'ye gömülür.
> GUI testleri (`RADIOS_GUI_TESTS=ON`) etkileşimli masaüstü oturumunda pencere açar; yayın bilgisayarında koşturmayın.

`packaging/README.md` (replace the whole Linux text): two outputs — `dist\RadioS-<sürüm>-windows-x64-setup.exe` (NSIS,
kullanıcı başına kurulum, varsayılan `%LOCALAPPDATA%\Programs\RadioS`) and the deterministic portable
`dist\RadioS-<sürüm>-windows-x64.zip` (+ `.sha256`); commands
`cmake --workflow --preset windows-installer` and `python tools\radios_package.py verify --archive …`; package
contents (§6.1), never-packaged private data (unchanged policy), bundled DLL list in `packaging/runtime.json`, Python
3.14.8 + tzdata 2026.5 hashes, upgrade behaviour (service stopped, data/config untouched), uninstall keeps data,
licence notice for bundled third-party DLLs (GTK/GLib LGPL, FFmpeg per §7.4, OpenSSL Apache-2.0, Inter OFL).

`README.md` "Derleme ve çalıştırma" (docs area, commands supplied here): Windows 10/11 x64; MSYS2 UCRT64 packages
`mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,gtk3,json-glib,curl,ffmpeg,fontconfig,python,librsvg,adwaita-icon-theme,hicolor-icon-theme,nsis}`;
`cmake --preset windows-debug` / `cmake --build --preset windows-debug` / `ctest --preset windows-debug`;
`run-radios.cmd` builds `windows-runtime` and starts `runtime\bin\RadioS.exe`; `RADIOS_SKIP_BUILD=1` unchanged.

---------------------------------------------------------------------------------------------------

## 11. Cross-area contracts

| With | Contract |
|---|---|
| platform (core-posix-files) | `radios_platform` = `src/core/platform.c`, `platform_win32.c`, `platform_tz.c`; header `include/radios/platform.h`. `radios_platform_init()` stays idempotent (tests may call it; the build already forces binary `_fmode` before `main`). `radios_time_zone_directory()` honours `TZDIR` (tests). |
| audio-backend / audio-routing / midi | Final source lists for `radios_audio`/`radios_core`; Win32 link libs of §4.7; new tests (`test_audio_backend_hotplug`, `test_null_provider`, `test_wasapi_smoke` label `hardware`, `test_wasapi_backend`); exactly one TU with `initguid.h`; `RADIOS_AUDIO_SERVER` semantics (`null:`) used by the global test env; MIDI hotplug case names registered through `radios_add_glib_cases`. |
| ui-platform-c | `main.c` drops `glib-unix.h`/signals; `window.c` drops `Fc*` (then FONTCONFIG pkg check is deleted); `content_scale_test_input.h` `SendInput`; icon file `assets/icons/radios.ico`; `gtk-settings.ini` content; manifest DPI policy vs GDK scale (verify at 125/150/175 %). |
| tests / test infra | Keep runner file names, rewrite bodies (Xvfb → desktop, pactl → null provider); `run_automation_stress.py` takes `--driver`; Python tests use the D6 shims; no test writes `__pycache__` (CTest passes `-B` + env). `tests/test_header_ui.c` adds `RADIOS_WRAP_DLLIMPORT(gint64, g_get_real_time, (void));` (include `radios_test_wrap.h`). |
| tools / launchers | `run-radios.cmd`/`run-database.cmd` (CRLF, `--project-root "%~dp0."` — a trailing backslash before a quote is an escape in MSVCRT argv rules); `radios_launch.py` builds with `cmake --build --preset windows-runtime` (configure with `--preset windows-release` when `build/release/CMakeCache.txt` is missing), uses `runtime\bin\RadioS.exe`, never `build/radios`; `radios_sdk.py`, `run-radios-sdk.sh` deleted (D1); `radios_package.py` per §7.2; `default-binary.lock` via LockFileEx shim. |
| config | `config/radios.example.ini:24` `python_command = python3` is not a path on Windows; with the D4 rule ("explicit path must exist, else fail closed") the shipped example must use an empty value (→ bundled interpreter) or `runtime/python/python.exe` semantics defined by the core area. |
| network (curl) | Set `CURLSSLOPT_NATIVE_CA` (OpenSSL backend + Windows cert store); the installer still ships `etc\ssl\certs\ca-bundle.crt`. |
| SDBASE / Python | Service started with `python.exe -E -s -B -X utf8` (D4); test env adds `PYTHONUTF8=1` for runner children. |

---------------------------------------------------------------------------------------------------

## 12. Risks

1. **GUI tests on the real desktop** (no Xvfb): focus stealing, operator disruption, DPI-dependent geometry
   (machine scaling ≠ 100 %), locked screen/session 0 → flaky or skipped. Mitigations: §5.3 labels/locks/77, optional
   private desktop, document "do not run on the broadcast PC".
2. **False passes**: GLib `-p` with a stale name runs 0 tests and exits 0; a leftover `GDK_BACKEND=x11` turns 31 tests
   into silent SKIPs (and fails 80). Mitigated by §5.1/§5.4 (the global `GDK_BACKEND=unset:` also neutralises any
   leftover); review the first Windows CTest report for SKIPs.
3. **MSYS2 is rolling**: a `pacman -Syu` can change GCC/GLib/FFmpeg ABI and warnings. Record `pacman -Q` of the
   UCRT64 set in `packaging/windows/msys2-packages.txt`, compare at configure (warning), and freeze the toolchain
   for release builds. `GLIB_VERSION_MIN_REQUIRED` neutralises new GLib deprecations.
4. **DLL hijacking / foreign runtimes**: `C:\MinGW\bin` libgcc/libwinpthread/libstdc++ on PATH. Mitigated by
   app-dir-first loading of bundled DLLs, test PATH prepend, and D3's `SetDefaultDllDirectories`.
5. **In-use files**: staging while RadioS or the SDBASE service runs (DLL/python image locks) → partial runtime.
   Mitigated by the all-or-nothing pre-check in `radios_sync_runtime.py` and the installer's service stop.
6. **Long paths / non-ASCII paths**: system long-path policy off; binutils/windres/ar not UTF-8 → keep build and
   install paths short and ASCII; installer warns.
7. **Licensing** (GPLv3 FFmpeg bundle) and **size** (177 MB DLLs, Vulkan loader) — §7.4, needs a lead decision.
8. **Sanitizers**: no ASan with MinGW GCC; only UBSan trap mode. The Linux ASan/UBSan acceptance cannot be repeated
   1:1 (a future `clang64` environment could provide ASan, with a different C library and package set).
9. **Defender real-time scanning** slows builds (145 links) and service-heavy tests (first launch of a new exe: 9.2 s
   measured vs 0.05 s warm); exclude `build\` on dev/CI machines (admin action) — never the installed data directory.
10. **Parallel porting drift**: new files not added to targets (now 5 src + 2 tests) — §4.11 guard.
11. `cmake --install` component scripts depend on MSYS2 tools at install time (objdump, glib-compile-schemas,
    gtk-update-icon-cache); packaging must run on a machine with the same MSYS2 snapshot.
12. Python 3.14 (not 3.13 as in the task text) is what is staged and verified (PLATFORM_FACTS); keep both hashes in
    CMake so the version is explicit and reviewable.

---------------------------------------------------------------------------------------------------

## 13. Verification log (probes, scratchpad only)

| # | Probe | Result |
|---|---|---|
| P1 | `nm` over import libs for WASAPI GUIDs | `CLSID_MMDeviceEnumerator`/`IID_IMMDeviceEnumerator`/`IID_IAudioClient` in no lib; `PKEY_Device_FriendlyName` in libuuid; KSDATAFORMAT in libksuser/libksguid; `ActivateAudioInterfaceAsync` in libmmdevapi; no libaudioses. |
| P2 | `_mingw.h`/`stdio.h` reading + `flags_main.c`, `posix_main.c` | UCRT → gnu_printf format attrs; POSIX-macro TUs get `__USE_MINGW_ANSI_STDIO=1`; `sizeof(off_t)=8`, `st_size` 8 bytes with `_FILE_OFFSET_BITS=64`; `sizeof(long)=4`. |
| P3 | `wrap_main.c` with GLib/GTK/static symbols | `g_get_real_time` not wrapped; with `--wrap=__imp_g_get_real_time` + pointer: wrapped; GTK and cross-object static: wrapped; same-object: not. |
| P4 | manifest bisection (`t*`, `u*`, `w*`, `x*` exes) | `SM/` namespaces silently ignored; `SMI/` → GetACP 65001, PMv2 on; our manifest replaces `default-manifest.o`. |
| P5 | `mode_main.c` (+ binmode.o / crt_binmode) | `binmode.o` empty, no effect; `int _fmode=_O_BINARY` → binary fopen, `_get_fmode=0x8000`; stdout stays text. |
| P6 | windres | `#pragma code_page(65001)` needed for UTF-8 strings; spaces OK in default mode; `--use-temp-file` breaks with spaces. |
| P7 | `--as-needed`, `--gc-sections` | accepted; GC does not prevent undefined-reference errors on PE. |
| P8 | `-gcodeview -Wl,--pdb=`, `--no-insert-timestamp`, `-ffile-prefix-map` | PDB + RSDS produced; links byte-identical; `__FILE__` relative. |
| P9 | `-fsyntax-only` over `src/**/*.c` | 114 files: 94 clean, 0 deprecation warnings; blockers: `pulse/pulseaudio.h` (channel_input, patchbay), `glib-unix.h` (main.c), `sys/resource.h` (library_analysis), `sys/statvfs.h` (storage_locations), POSIX file APIs (decoder, output, transition, config, data_client, local_ini, media_archive, preferences_storage, recovery, ui_preferences, analyzer, media_archive_ui, playlist). FFmpeg-using sources show no API removals. |
| P10 | `-fsyntax-only` over `tests/*.c` | 147 files: 93 clean; blockers: `gdk/gdkx.h` (content_scale ×3, routing_graph_ui), `sys/wait.h` (audio_recovery, recovery), `arpa/inet.h` (output), `pread` ×29, `symlink` ×13, `mkfifo` ×3, `newlocale` (tempo), `kill` (patchbay), `sysconf` (audio_soak). |
| P11 | `css_to_gtk.py` on Windows | default: `UnicodeDecodeError 0x9e`; `-X utf8`: 31 sources / 1572 rules, outputs CRLF; Ninja query lists Turkish inputs correctly. |
| P12 | Scratch configure of a copy (Pulse/ALSA/XTST stubbed) with MSYS2 CMake + Ninja | configure OK; `find_package(Python3)` picked `…/pythoncore-3.15-64/python.exe` until pinned; default install prefix `C:/Program Files (x86)/RadioS`; 251/96 tests, 145/56 exes; File-API inventory of all targets and wraps. |
| P13 | GLib test `-p /probe/renamed-case` | `1..0`, exit 0. |
| P14 | `gtkinit.c` with `GDK_BACKEND=x11` | `gtk_init_check=0` (exit 77); default display `1\WinSta0\Default`. |
| P15 | Mini CMake project (`scratchpad\cm`) | OBJECT lib with `.rc` + binmode; `WIN32_EXECUTABLE`; `--stack`; `ENVIRONMENT_MODIFICATION` path_list_prepend; `install(RUNTIME_DEPENDENCY_SET)` copies glib/iconv/intl/pcre2 and the installed exe runs with `PATH=System32` only; CMP0207 warning without the policy. |
| P16 | `file(GET_RUNTIME_DEPENDENCIES)` in `cmake -P` | 46 resolved (gtk + svg/png loaders + gspawn helper), 0 unresolved. |
| P17 | `objdump -p` BFS closure | 63 files/55.7 MB without FFmpeg; 127 files/177.5 MB with MSYS2 FFmpeg. |
| P18 | `test_process_setup.c` constructor + failing `g_assert` | constructor ran; process ended in 0.4 s, no dialog. |
| P19 | Hardening flags | `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -O2` link; UBSan trap links; ASan fails (`-lasan` missing). |
| P20 | Hashes of `packaging/windows/cache/*` | match PLATFORM_FACTS; zip contains `python314._pth`; wheel contains `tzdata/zoneinfo/*`. |
| P21 | Mini project v2 (`scratchpad\cm\src\CMakeLists.txt`, Release): §4.2 global options, genex `-Wl,--pdb=RadioS.pdb`, `cmake_language(DEFER)` finalize, §5.1 env/labels/timeout/fail-regex | builds clean with `-Werror`; `RadioS.pdb` produced; late-declared target finalized; stale GLib case → Failed; `RADIOS_AUDIO_SERVER` overridden except `hardware`; TIMEOUT 20→40; first GLib exe launch 9.2 s, warm 0.05 s. |

---------------------------------------------------------------------------------------------------

## 14. Implementation checklist (ordered)

1. Add `CMakePresets.json`, `cmake/RadioSToolchain.cmake`, `.gitattributes`, `.gitignore` changes.
2. Header (§4.1), dependencies (§4.4), Python pinning (§4.5), `css_to_gtk.py` UTF-8 fix + custom command (§4.6).
3. `radios_platform` + library source lists from the porting areas; Win32 link libs (§4.7); delete PULSE/ALSA/XTST/X11.
4. `packaging/windows/radios.{manifest,rc}.in`, `assets/icons/radios.ico`, `src/app/crt_binmode.c`,
   `tests/test_process_setup.c`, `tests/radios_test_wrap.h`, `cmake/RadioSWindowsExe.cmake` (finalize, `radios_wrap`,
   `RadioS` OUTPUT_NAME/WIN32_EXECUTABLE/PDB).
5. Test edits (§5.2) + `cmake/RadioSTesting.cmake` (§5.1, §5.3, §5.4); new test targets; `radios_automation_e2e`,
   `radios_usb_analiz`.
6. `cmake/RadioSRuntime.cmake`, `RadioSInstallDeps.cmake`, `RadioSPythonRuntime.cmake`, `tools/radios_sync_runtime.py`,
   target `radios_runtime`; stage `<root>\runtime` and smoke-run `runtime\bin\RadioS.exe --version` with
   `PATH=C:\Windows\System32` only.
7. `packaging/windows/radios.nsi.in`, `gtk-settings.ini`, target `radios_installer`; `tools/radios_package.py` Windows
   contract (tools area).
8. Rewrite `cmake/README.md`, `packaging/README.md`; hand the README build section to the docs area.
9. First full `ctest --preset windows-debug`: review every SKIP and every `1..0` failure; then GUI preset on a
   non-production desktop.
