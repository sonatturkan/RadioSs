# Cloud work environment for the RadioS Windows port (BINDING for every agent)

The port started on the user's Windows 11 PC. It now continues in a Linux cloud container.
You still build and test REAL Windows binaries: the exact MSYS2 UCRT64 toolchain
(GCC 16.2.0 Rev6, binutils 2.47, same GTK 3.24.52 / GLib 2.90.1 / json-glib 1.10.8 /
libcurl 8.23.0 / FFmpeg 9.0.2 packages as on the user's PC) runs under Wine 9.0. The
target is still real Windows 10/11: write code for documented Windows behaviour, never
for Wine behaviour.

## Path mapping (the specs were written on Windows)

| In specs / reports / DECISIONS                                         | Here |
|------------------------------------------------------------------------|------|
| `C:\Users\sonatturkan\Desktop\RadioS` (project root, ROOT)             | `/home/user/RadioSs` (git repo, branch `windows-port`) |
| `...\0ead7b3c-...\scratchpad` (SP), `SP\specs`, `SP\reports`           | `/opt/radios-port`, `/opt/radios-port/specs`, `/opt/radios-port/reports` |
| `SP\work\<area>\` private scratch dirs                                 | `/opt/radios-port/work/<area>/` |
| `C:\msys64\ucrt64` (MSYS2 prefix)                                      | `/opt/msys2/ucrt64` (inside Wine: `Z:\opt\msys2\ucrt64`) |
| `C:\Users\...\Desktop\radyo projesi\kaynak kodlar\RadioS` (Linux original) | NOT AVAILABLE. Use the specs (they inventory the original code file:line) and `git show main:<path>` for the tree as it was when this cloud work started. |
| PowerShell 5.1 shell for agents                                         | bash. `pwsh` (PowerShell 7.5 for Linux) exists only to syntax-check/lint .ps1 files you write. |

Inside Wine, a Linux absolute path `/x/y` is the Windows path `Z:\x\y`.

## Toolchain (always put the wrappers first in PATH)

    export PATH=/opt/radios-port/bin:$PATH

- `x86_64-w64-mingw32-gcc` : MSYS2 GCC 16 under Wine (same flags/diagnostics as on the PC). Linux paths are fine in arguments.
- `x86_64-w64-mingw32-pkg-config` (= `ucrt64-pkg-config`) : pkg-config over the MSYS2 sysroot.
- `x86_64-w64-mingw32-windres`, `-ar`, `-ranlib`, `-objdump`.
- `ucrt64-tool <name> args` : any other MSYS2 tool, e.g. `ucrt64-tool glib-compile-schemas ...`, `ucrt64-tool ffmpeg ...` (fixtures), `ucrt64-tool makensis ...`, `ucrt64-tool ntldd ...`, `ucrt64-tool python ...` (MSYS2 build-time python 3.14).
- Linux `cmake` is 4.4.4 (= the PC's). Cross toolchain file: `/opt/radios-port/cmake/wine-msys2-ucrt64.cmake`
  (sets the compiler wrappers, `PKG_CONFIG_EXECUTABLE`, `CMAKE_CROSSCOMPILING_EMULATOR=wrun`, `RADIOS_MSYS2_PREFIX=/opt/msys2/ucrt64`, objdump-based `GET_RUNTIME_DEPENDENCIES`). This file is cloud-only and must never be referenced from the repository.

Standalone compile recipe (replaces the PowerShell recipe of DECISIONS.md D7):

    export PATH=/opt/radios-port/bin:$PATH
    PC=$(ucrt64-pkg-config --cflags glib-2.0 gio-2.0 gio-windows-2.0 gtk+-3.0 json-glib-1.0 libcurl libavformat libavcodec libavutil libswresample libavfilter)
    x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -D_FILE_OFFSET_BITS=64 \
      -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -I/home/user/RadioSs/include $PC \
      -c <file.c> -o /opt/radios-port/work/<area>/<file>.o
    # link: x86_64-w64-mingw32-gcc <objs> -o x.exe $(ucrt64-pkg-config --libs ...) -lole32 -luuid -lavrt -lwinmm \
    #       -lpropsys -lmmdevapi -ladvapi32 -lshell32 -lshlwapi -ldbghelp -lntdll -Wl,--stack,8388608

A compile takes ~3 s (Wine start-up included). Build several files with `&` + `wait` or `xargs -P4`. The machine has 4 CPUs shared by several agents: never run more than 4 parallel compiles.

## Running Windows programs: `wrun`

    wrun ./test_platform.exe -p /platform/locks          # tests, the app, tools
    wrun /home/user/RadioSs/runtime/python/python.exe -E -s -B -X utf8 script.py args

- Shared Wine prefix `/opt/wineprefix`, virtual display `:99` (Xvfb, 1920x1080) started on demand, Turkish locale (LCID 1055) and TZ Europe/Istanbul.
- Translates absolute Linux paths in arguments and in path-like environment variables (`*DIR`, `*PATH`, `*ROOT`, `*FILE`, `TMP`, `TEMP`, `TZDIR`, `*_COMMAND`, ...) to `Z:\...`, and a Windows-style `PATH=a;b;c` (as CTest sets it) into Wine's search path. `--keep-paths` disables the translation.
- stdin is NUL unless `RADIOS_WRUN_STDIN=1` (then the caller's stdin is passed through). stdout/stderr are forwarded through pipes.
- `RADIOS_WRUN_CLEAN_PATH=1` does not add the MSYS2 bin dir to Wine's search path (use it to prove a staged runtime prefix is self-contained; Wine's own system32 still resolves system DLLs).
- `RADIOS_WRUN_CONSOLE=1` runs the program on a pseudo console (stdout+stderr merged) when a test needs a real Win32 console.
- Several Wine-only noise sources were removed so `G_DEBUG=fatal-warnings` GTK tests work (fonts-wine, keyboard layout DLL, monitor device properties, a DWM stub). They live in `/opt/radios-port/wine-support` (test environment only, never shipped, never referenced from the repo).
- The embedded CPython 3.14.8 runtime is in `runtime/python` (python.exe and DLLs were restored from the verified `packaging/windows/cache/python-3.14.8-embed-amd64.zip`; .exe/.dll are git-ignored). Run Python project code and Python tests ONLY with it, through `wrun`, with `-E -s -B -X utf8`. Linux `python3` is for host-side helper scripts only.

## Fidelity: what Wine does and does not prove

Identical to the PC: compiler, headers, import libraries, warnings, PE output, GTK/GLib/FFmpeg/curl DLLs, CRT (UCRT), Win32 API signatures.

Different or missing under Wine (a failing check in these areas is NOT automatically a defect; prove it):
- `icu.dll`: Wine has none; the test prefix has a test-only proxy `C:\windows\system32\icu.dll` exporting the unversioned ICU C API RadioS uses (ucal_getWindowsTimeZoneID, ucal_getTimeZoneIDForWindowsID, ucal_getCanonicalTimeZoneID, ucal_openTimeZoneIDEnumeration, uenum_next, uenum_close), statically linked to ICU 78 (newer data than Windows' ICU 2022g). So the ICU mapping path of platform_tz.c runs here (Europe/Istanbul -> Turkey Standard Time, and Wine's registry zones have Turkey's historical DST). Wine's LOCAL zone still resolves to "Arab Standard Time" (Wine picks the first +03:00 zone), not "Turkey Standard Time".
- No audio devices: WASAPI enumerates nothing. Use `RADIOS_AUDIO_BACKEND=none|virtual` (D8). Real-device WASAPI behaviour is verified on the user's PC later (hardware tests are labelled and skip with 77 here).
- No MIDI devices (WinMM lists none). Use the injectable fake WinMM backend.
- Security descriptors are stored and returned, but Wine does NOT enforce DACL access checks; owner SIDs and integrity levels are simplified. ACL/SDDL creation code can be verified by reading the descriptor back, not by denied access.
- File semantics: LockFileEx, sharing modes and FILE_SHARE_DELETE work; POSIX-semantics rename/delete (FILE_RENAME_POSIX_SEMANTICS, FILE_DISPOSITION_POSIX_SEMANTICS) and reparse points/junctions may be partial. Check with a probe before concluding.
- Private desktops (CreateDesktopW), SendInput, DPI/scale changes, DWM, WM_ENDSESSION and power requests are partial or stubbed.
- Timing: process start-up costs ~0.3-1 s; keep timing assertions deterministic or generous (they already must be, see PLATFORM_FACTS).

Triage rule for every failure you see here: (a) a real defect for Windows -> fix it in the files you own (or request it); (b) a Wine gap -> prove it with a minimal probe (the same probe must show Wine deviating from documented Windows behaviour), report it in `open_issues` prefixed `WINE-GAP:` with the probe path, and do NOT weaken the test, add Wine-specific code, or skip it in the repository. If a test is hardware/Wine-bound by design it must already skip cleanly (exit 77) when the resource is absent.

## Rules that stay the same

- DECISIONS.md and PLATFORM_FACTS.md stay binding (except the shell/paths above). Windows only, no Linux code paths, convert don't drop, industrial fixes, no useless leftovers.
- Edit only the files you own. Never run git commands that change the repository (commit, checkout, reset, stash, clean, rebase, add). `git status`, `git diff`, `git log`, `git show main:<path>` are fine. The lead commits between phases.
- The repository's `data/` and `config/` hold no real user data here, but still never write test output into the tree; use temp dirs under /opt/radios-port/work/<area>/ or /tmp.
- Report honestly: what you compiled, what you ran (under Wine), what only the user's PC can verify.
