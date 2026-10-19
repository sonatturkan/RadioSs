# Verified platform facts (measured on the target machine, 2026-10-19)

Machine: Windows 11 Pro build 26300, x64, 8 logical CPUs, Turkish locale (system time zone "Turkey Standard Time").
Audio hardware: 1 render endpoint "Hoparlör (4- Cirrus Logic XU)", 1 capture endpoint "Microphone Array on SoundWire Device (Cirrus Logic XU)"; both shared-mode mix format 48000 Hz, 2 ch, 32-bit float. (The user may plug in more devices: the DJ controller may also add audio endpoints.)
MIDI (updated 02:40): 1 REAL input "DDJ-FLX10" (Pioneer DJ controller, usbaudio KS path) and rekordbox is running and probably holds it. Classic WinMM input is EXCLUSIVE: NEVER open the user's real MIDI devices in tests or probes (listing is fine). Use the fake/injectable WinMM backend in tests.
Machine load: CPU often 57-85 % (rekordbox, Defender, parallel agents); an idle GTK loop showed 203-472 ms heartbeat gaps at normal priority. Timing assertions in tests must be deterministic (counts/ordering) or generously bounded, and audio threads must use MMCSS.

## Toolchain (MSYS2 UCRT64 at C:\msys64, binaries in C:\msys64\ucrt64\bin)
gcc 16.2.0 (Rev6), cmake 4.4.4, ninja 1.13.2, pkgconf, GTK 3.24.52, GLib 2.90.1, json-glib 1.10.8, libcurl 8.23.0,
FFmpeg libavformat 63.1.102 / libavcodec 63.1.102 / libavutil 61.1.102 / libswresample 7.1.102 / libavfilter 12.1.102 (FFmpeg 8.x — APIs removed after 5.1 such as old channel_layout/channels fields, avcodec_close, AVFrame.key_frame etc. will not compile; use AVChannelLayout / ch_layout APIs),
fontconfig 2.18.3, pango 1.58.2 (pangoft2 + pangowin32 both available), cairo 1.18.6, harfbuzz 14.6.0, FreeType 26.7.20 linked with brotli (=> WOFF2 readable through FreeType/fontconfig),
MSYS2 python 3.14.8 (build-time tools only), NSIS 3.13 (makensis), ntldd, gdb, objdump, windres, glib-compile-schemas, gdk-pixbuf-query-loaders, gtk-update-icon-cache, rsvg-convert, ffmpeg.exe (for generating test fixtures).
Shell for agents: PowerShell 5.1. Prepend `C:\msys64\ucrt64\bin;C:\msys64\usr\bin;` to $env:PATH before using the toolchain. Do not redirect native stderr with 2>&1 in PowerShell 5.1.

## Other toolchains on this machine (DO NOT USE for the build; guard against them)
- The user installed WinLibs GCC 13.2.0 + LLVM/Clang 16.0.6 + CMake 3.27.1 + ninja 1.11.1 at C:\MinGW, and C:\MinGW\bin is on the MACHINE PATH. It has no GTK/FFmpeg/pkg-config. Mixing it with MSYS2 libraries risks ABI/DLL mismatches.
  => Every build/test script must put C:\msys64\ucrt64\bin (and C:\msys64\usr\bin where needed) FIRST in PATH, pin -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe and use C:\msys64\ucrt64\bin\cmake.exe / ninja.exe explicitly.
  => At runtime the app must not pick up foreign DLLs (libwinpthread-1.dll, libgcc_s_seh-1.dll, libbz2.dll, libstdc++-6.dll ... exist in C:\MinGW\bin): bundle every dependent DLL next to RadioS.exe, call SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) + SetDllDirectoryW(L"") at the very start of main, and have tests/launchers run with a PATH where MSYS2/staged bin precedes C:\MinGW\bin.
- The user also installed the Python install manager (C:\Program Files\PyManager\python.exe, py.exe; it auto-installed CPython 3.15.0 at C:\Users\sonatturkan\AppData\Local\Python\pythoncore-3.15-64) and a uv-managed CPython 3.15.0. `python`/`py` on PATH therefore resolve to 3.15 — never rely on PATH python; always use the explicit runtime\python\python.exe (3.14.8).

## C runtime facts (mingw-w64 UCRT, gcc -std=c11 with _POSIX_C_SOURCE=200809L)
- strtok_r, localtime_r, gmtime_r, clock_gettime(CLOCK_MONOTONIC) are available.
- sizeof(off_t) == 4 and sizeof(long) == 4 by default (LLP64). => Build with -D_FILE_OFFSET_BITS=64 globally; never use long/glong/off_t (unless _FILE_OFFSET_BITS=64)/ftell/fseek for byte offsets, sizes, frame counts or microseconds; use gint64/guint64, gsize, _fseeki64/_ftelli64 (or fseeko/ftello with _FILE_OFFSET_BITS=64).
- O_BINARY = 0x8000, O_NOINHERIT = 0x80. Default _fmode is text. Every open()/fopen() of data must be binary (add O_BINARY / "b"); additionally the app sets the process default to binary.
- time_t is 64-bit.

## Time zones (CRITICAL)
- GLib 2.90 on Windows does NOT accept IANA identifiers: g_time_zone_new_identifier("Europe/Istanbul") returns NULL even with TZDIR set. It accepts Windows zone names ("Turkey Standard Time"), "UTC" and fixed offsets ("+03:00"). g_time_zone_new_local() returns identifier "Turkey Standard Time". Windows registry rules give correct historical DST (2015-01 +2h, 2015-07 +3h for Turkey).
- Windows' system ICU (C:\Windows\System32\icu.dll, load with LoadLibraryExW(L"icu.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32); unversioned C symbols) works on this machine:
  ucal_getWindowsTimeZoneID (IANA -> Windows: Europe/Istanbul -> Turkey Standard Time, America/New_York -> Eastern Standard Time, Etc/UTC -> UTC, Australia/Lord_Howe -> Lord Howe Standard Time; every mapped id is accepted by g_time_zone_new_identifier),
  ucal_getTimeZoneIDForWindowsID (Windows -> IANA, region NULL: Turkey Standard Time -> Europe/Istanbul),
  ucal_openTimeZoneIDEnumeration(UCAL_ZONE_TYPE_CANONICAL=1, NULL, NULL, &status) + uenum_next/uenum_close => 477 canonical IANA ids. ICU data version 2022g (used only for the id mapping; actual rules come from the Windows registry, kept current by Windows Update).
  => The C side must use a platform wrapper: radios_time_zone_new_identifier(iana_or_windows_id) / radios_time_zone_get_identifier(tz) returning the IANA id / radios_time_zone_list_identifiers(). Persisted ids stay IANA (the Python service uses IANA with zoneinfo).
- Python: the embeddable CPython has no tz database (zoneinfo.available_timezones() == 0). The bundled runtime must include the PyPI `tzdata` package (verified wheel: packaging/windows/cache/tzdata-2026.5-py2.py3-none-any.whl, sha256 b683bd1b6659ddcd810ff02ad09ba821d4bf1065072805063eb35c49617905ac).

## Python runtime for the SDBASE service (shipped)
- Official CPython 3.14.8 embeddable amd64 (actively maintained bugfix line; verified sha256 a93abe456ab01bd96d7a085b3cdb6566b3063f4241360d114142fbdb07f0a310 against the python.org release page): packaging/windows/cache/python-3.14.8-embed-amd64.zip. Contains ctypes, msvcrt, _winapi, ssl, sqlite3, hashlib, lzma, bz2, zlib, socket, select, _overlapped, asyncio, multiprocessing, unicodedata, _zoneinfo. (3.13 was rejected: it entered security-only mode and gets no more Windows binaries.)
- DEV RUNTIME ALREADY STAGED: C:\Users\sonatturkan\Desktop\RadioS\runtime\python\python.exe (3.14.8, ._pth removed, tzdata 2026.5 in Lib\tzdata; verified 598 zones, Europe/Istanbul 2015-07 = +03:00, utf8_mode=1). Use THIS interpreter to run the SDBASE service and all Python tests. MSYS2's python 3.14 (C:\msys64\ucrt64\bin\python.exe) is only for build-time generators. The user also installed CPython 3.15.0 via uv (C:\Users\sonatturkan\AppData\Roaming\uv\python\cpython-3.15.0-windows-x86_64-none\python.exe); it has no tzdata; do not depend on it.
- With the shipped python314._pth present, the script directory is NOT on sys.path and PYTHONPATH is ignored. Decision: the bundled runtime removes python314._pth and the app always launches Python as:  python.exe -E -s -B -X utf8 <script> ...  (-E ignores PYTHON* env vars, -s no user site, -B no .pyc, -X utf8 forces UTF-8 I/O — without it open() defaults to cp1254 on this Turkish system). Verified: without ._pth, sys.path = [script dir, python314.zip, <prefix>, <prefix>\Lib]; all stdlib modules import. tzdata goes into <prefix>\Lib\tzdata.
- Python text-mode writes translate "\n" to "\r\n" on Windows: any file whose bytes matter (journals, JSON that is hashed/compared, manifests, CSV, INI read by C) must be written with newline="\n" (or in binary) and explicit encoding="utf-8".

## WASAPI / Windows SDK in mingw-w64
- Available and linkable: mmdeviceapi.h, audioclient.h, audiopolicy.h, functiondiscoverykeys_devpkey.h, avrt.h, mmsystem.h; link -lole32 -luuid -lavrt -lwinmm -lpropsys -lmmdevapi (ActivateAudioInterfaceAsync is in mmdevapi). COM from C works with COBJMACROS + initguid.h.
- audioclientactivationparams.h is MISSING: declare AUDIOCLIENT_ACTIVATION_TYPE, PROCESS_LOOPBACK_MODE, AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS, AUDIOCLIENT_ACTIVATION_PARAMS and VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK (L"VAD\\Process_Loopback") manually (layout per the Windows SDK). Process loopback needs Windows 10 build 20348+ (target machine is 26300).
- AvSetMmThreadCharacteristicsW(L"Pro Audio") succeeds.

## Fonts (DECIDED, verified empirically)
- Pango 1.58 on Windows uses PangoCairoWin32FontMap (DirectWrite). The Linux code path FcInit()+FcConfigAppFontAddFile(woff2) "succeeds" but Pango never sees the font (falls back to Tahoma). PANGOCAIRO_BACKEND=fc also failed to resolve the app font (Verdana fallback). Do NOT use fontconfig for app fonts on Windows.
- DirectWrite rejects WOFF2 ("not supported by DirectWrite") but accepts TTF. The project's assets/fonts/InterVariable.woff2 is byte-identical (sha256 693b77d4f32ee9b8...) to web/InterVariable.woff2 of the official Inter 4.1 release (Inter-4.1.zip sha256 9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e), whose InterVariable.ttf (same OFL LICENSE.txt, identical) is now at assets/fonts/InterVariable.ttf.
- Load it with pango_font_map_add_font_file(pango_cairo_font_map_get_default(), "<assets>/fonts/InterVariable.ttf", &error) (Pango >= 1.56) BEFORE any widget is realized; treat failure as the same startup error the Linux code raised.
- Under DirectWrite the family is named "Inter Variable Text" with faces Thin, Ultra-Light, Light, Normal, Medium, Semi-Bold, Bold, Ultra-Bold, Heavy (+ Oblique variants); "Inter Variable Text 11", "... Bold 11", "... Semi-Bold 11", "... Light 11" all resolve exactly. The plain name "Inter Variable" does NOT resolve. => Every CSS font-family list and every pango_font_description_from_string()/cairo font string that names 'Inter Variable' or 'Inter' must list 'Inter Variable Text' first (keep the old names after it as fallbacks), including CSS generated by tools/css_to_gtk.py from design/reference and assets/themes/*.css and CSS strings embedded in C.

## Layout decisions
- Project root (source + data): C:\Users\sonatturkan\Desktop\RadioS. Runtime data stays under <root>\data, config under <root>\config.
- Dev/staged runnable app: <root>\runtime\ = standard GTK prefix (bin\RadioS.exe + DLLs, lib\, share\, etc\, python\). The exe finds the RadioS root by walking up from its own directory to the first directory containing config\radios.example.ini and database\SDBASE\src (so runtime\bin -> root). The installer/portable package puts config\, database\, tools\, assets\, data\ inside the prefix itself, so the same rule finds the prefix.
- Windows only: no Linux code paths, no PulseAudio/ALSA/X11/flatpak/.sh. Every Linux facility is replaced by its best Windows equivalent.
