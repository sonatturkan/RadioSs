# RadioS runtime prefix: install components and the dev runtime <root>/runtime (Windows only).
#
#   include(RadioSRuntime)     # at the END of CMakeLists.txt, after add_executable(radios) and
#                              # after every other install() rule (see "Order" below)
#
# Install components (cmake --install <build> --component <c> --prefix <dir>):
#   runtime      bin/RadioS.exe + every non-system DLL it needs (DLL closure, see
#                packaging/windows/cmake/stage-runtime-deps.cmake), gspawn helpers,
#                lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll + regenerated relative
#                loaders.cache, share/glib-2.0/schemas (compiled), share/icons/{Adwaita (without
#                cursors), AdwaitaLegacy, hicolor} + icon caches, share/locale/tr (GTK/GLib/
#                gdk-pixbuf/json-glib), etc/gtk-3.0/settings.ini, etc/ssl/certs/ca-bundle.crt
#   python       python/ (CPython embeddable, no ._pth, + tzdata) and share/zoneinfo, from the
#                SHA-256-pinned inputs in packaging/windows/cache (existing trees: verified only)
#   application  config/radios.example.ini + README.md, database/SDBASE/{pyproject.toml,README.md,
#                src/**.py}, the tools the app spawns, assets/ (fonts, themes), docs/*.md and the
#                empty data/ skeleton - never user data or user configuration (explicit lists)
#   symbols      bin/RadioS.pdb when the build produced one (D11; optional installer component)
#
# Targets:
#   radios_runtime   refreshes <root>/runtime (D4 dev prefix: bin lib share etc python) from a
#                    fresh install stage with packaging/windows/sync_runtime.py: in-use safe,
#                    keeps runtime/python and runtime/share/zoneinfo untouched.
#
# Host neutrality: Windows programs needed at build/install time (MSYS2 python.exe,
# glib-compile-schemas.exe, gdk-pixbuf-query-loaders.exe, gtk-update-icon-cache.exe) run through
# ${CMAKE_CROSSCOMPILING_EMULATOR} (empty on the Windows build PC); the DLL closure uses
# CMAKE_OBJDUMP; the MSYS2 location comes from RADIOS_MSYS2_PREFIX.
#
# Order: cmake_install.cmake executes install rules in declaration order. The DLL closure is
# the last rule of the "runtime" component and reads what is already installed, so any rule
# that puts an executable or DLL into bin/ must be declared before include(RadioSRuntime).

include_guard(GLOBAL)

if(NOT TARGET radios)
    message(FATAL_ERROR "RadioSRuntime: include(RadioSRuntime) after add_executable(radios).")
endif()

# --- MSYS2 UCRT64 prefix ------------------------------------------------------------------
if(NOT RADIOS_MSYS2_PREFIX)
    # Same default as the toolchain module: <prefix>/bin/gcc.exe -> <prefix>.
    get_filename_component(_radios_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_radios_msys2 "${_radios_cc_dir}" DIRECTORY)
    set(RADIOS_MSYS2_PREFIX "${_radios_msys2}" CACHE PATH "MSYS2 UCRT64 kökü / MSYS2 UCRT64 prefix")
    unset(_radios_cc_dir)
    unset(_radios_msys2)
endif()
set(_radios_msys2_inputs
    bin/libgtk-3-0.dll
    bin/gspawn-win64-helper.exe
    bin/gspawn-win64-helper-console.exe
    bin/gdk-pixbuf-query-loaders.exe
    bin/glib-compile-schemas.exe
    bin/gtk-update-icon-cache.exe
    bin/python.exe
    lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll
    share/glib-2.0/schemas/org.gtk.Settings.FileChooser.gschema.xml
    share/glib-2.0/schemas/org.gtk.Settings.ColorChooser.gschema.xml
    share/glib-2.0/schemas/org.gtk.Settings.EmojiChooser.gschema.xml
    share/glib-2.0/schemas/org.gtk.Settings.Debug.gschema.xml
    share/icons/Adwaita/index.theme
    share/icons/AdwaitaLegacy/index.theme
    share/icons/hicolor/index.theme
    share/locale/tr/LC_MESSAGES/gtk30.mo
    share/locale/tr/LC_MESSAGES/glib20.mo
    share/locale/tr/LC_MESSAGES/gdk-pixbuf.mo
    share/locale/tr/LC_MESSAGES/json-glib-1.0.mo
    etc/ssl/certs/ca-bundle.crt)
set(_radios_missing "")
foreach(_radios_input IN LISTS _radios_msys2_inputs)
    if(NOT EXISTS "${RADIOS_MSYS2_PREFIX}/${_radios_input}")
        list(APPEND _radios_missing "${RADIOS_MSYS2_PREFIX}/${_radios_input}")
    endif()
endforeach()
if(_radios_missing)
    list(JOIN _radios_missing "\n  " _radios_missing)
    message(FATAL_ERROR "MSYS2 UCRT64 çalışma zamanı dosyaları eksik / runtime inputs missing:\n  ${_radios_missing}\n"
                        "Paketler / packages: mingw-w64-ucrt-x86_64-{gtk3,gdk-pixbuf2,librsvg,adwaita-icon-theme,"
                        "adwaita-icon-theme-legacy,hicolor-icon-theme,gtk-update-icon-cache,json-glib,python,"
                        "ca-certificates} (sürümler / versions: packaging/windows/msys2-packages.lock).")
endif()
unset(_radios_missing)
unset(_radios_input)

if(NOT CMAKE_OBJDUMP OR NOT EXISTS "${CMAKE_OBJDUMP}")
    message(FATAL_ERROR "CMAKE_OBJDUMP yok / not found ('${CMAKE_OBJDUMP}'): the MSYS2 binutils objdump.exe is "
                        "needed for the DLL closure.")
endif()
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    message(FATAL_ERROR "Çapraz derlemede CMAKE_CROSSCOMPILING_EMULATOR gerekir / cross builds need "
                        "CMAKE_CROSSCOMPILING_EMULATOR to run the MSYS2 tools of the runtime staging.")
endif()

# Interpreter for the packaging scripts: the pinned MSYS2 python of the build (never PATH/py.exe,
# PLATFORM_FACTS), isolated, UTF-8 mode, no .pyc - the same command the build-time generators use.
if(DEFINED RADIOS_BUILD_PYTHON_COMMAND)
    set(RADIOS_PACKAGING_PYTHON ${RADIOS_BUILD_PYTHON_COMMAND})
else()
    set(RADIOS_PACKAGING_PYTHON ${CMAKE_CROSSCOMPILING_EMULATOR} "${RADIOS_MSYS2_PREFIX}/bin/python.exe" -X utf8 -B -I)
endif()

# --- MSYS2 package snapshot (packaging/windows/msys2-packages.lock) ---------------------------
# MSYS2 is rolling: a "pacman -Syu" silently changes the compiler and every DLL that ends up in
# the runtime. Configure only warns; the package stage refuses a drift (RadioSInstaller.cmake)
# and build-windows.ps1 refuses before configuring. Checked again only when the lock or the
# pacman database changes.
set(RADIOS_MSYS2_LOCK "${CMAKE_SOURCE_DIR}/packaging/windows/msys2-packages.lock")
include("${CMAKE_SOURCE_DIR}/packaging/windows/cmake/msys2-lock.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RADIOS_MSYS2_LOCK}")
radios_msys2_pacman_db("${RADIOS_MSYS2_PREFIX}" _radios_pacman_db)
file(SHA256 "${RADIOS_MSYS2_LOCK}" _radios_lock_key)
if(IS_DIRECTORY "${_radios_pacman_db}")
    file(TIMESTAMP "${_radios_pacman_db}" _radios_db_time "%Y%m%d%H%M%S" UTC)
    string(APPEND _radios_lock_key "|${_radios_db_time}")
    unset(_radios_db_time)
endif()
if(NOT RADIOS_MSYS2_LOCK_CHECKED STREQUAL _radios_lock_key)
    radios_msys2_lock_check("${RADIOS_MSYS2_LOCK}" "${RADIOS_MSYS2_PREFIX}" _radios_lock_status _radios_lock_report)
    if(_radios_lock_status STREQUAL "OK")
        message(STATUS "MSYS2: ${_radios_lock_report}")
        set(RADIOS_MSYS2_LOCK_CHECKED "${_radios_lock_key}" CACHE INTERNAL "")
    elseif(_radios_lock_status STREQUAL "MISMATCH")
        message(WARNING "MSYS2 paketleri doğrulanmış kilitten farklı / MSYS2 packages differ from the verified "
                        "lock ${RADIOS_MSYS2_LOCK}:${_radios_lock_report}\nKurulum paketi bu durumda üretilmez; "
                        "testlerden sonra 'build-windows.cmd -UpdateMsys2Lock' / packages are refused until the "
                        "lock is updated after a passing test run.")
    else()
        message(STATUS "MSYS2 paket kilidi doğrulanamadı / lock not verifiable: ${_radios_lock_report}")
    endif()
    unset(_radios_lock_status)
    unset(_radios_lock_report)
endif()
unset(_radios_pacman_db)
unset(_radios_lock_key)

# --- Pinned Python runtime (PLATFORM_FACTS, D4) ---------------------------------------------
set(RADIOS_PYTHON_VERSION "3.14.8")
set(RADIOS_PYTHON_EMBED_ZIP "${CMAKE_SOURCE_DIR}/packaging/windows/cache/python-3.14.8-embed-amd64.zip"
    CACHE FILEPATH "CPython 3.14.8 embeddable amd64 zip (SHA-256 sabit / pinned)")
set(RADIOS_PYTHON_EMBED_SHA256 "a93abe456ab01bd96d7a085b3cdb6566b3063f4241360d114142fbdb07f0a310")
set(RADIOS_TZDATA_WHEEL "${CMAKE_SOURCE_DIR}/packaging/windows/cache/tzdata-2026.5-py2.py3-none-any.whl"
    CACHE FILEPATH "PyPI tzdata 2026.5 wheel (SHA-256 sabit / pinned)")
set(RADIOS_TZDATA_SHA256 "b683bd1b6659ddcd810ff02ad09ba821d4bf1065072805063eb35c49617905ac")

# --- DLL policy -----------------------------------------------------------------------------
# Windows 10/11 components imported by RadioS, GTK, GLib, curl, FFmpeg and their dependencies.
# They are never bundled; any other DLL must come from the LGPL FFmpeg prefix or MSYS2 UCRT64
# (msvcrt.dll is deliberately absent: a UCRT64 build must not mix CRTs).
set(RADIOS_EXTRA_SYSTEM_DLLS "" CACHE STRING
    "Ek Windows sistem DLL adları (yalnız gerçek Windows bileşenleri) / extra Windows component DLL names")
set(RADIOS_SYSTEM_DLLS
    advapi32.dll avrt.dll bcrypt.dll bcryptprimitives.dll cfgmgr32.dll comctl32.dll comdlg32.dll
    crypt32.dll d2d1.dll d3d11.dll d3d9.dll dbghelp.dll dnsapi.dll dwmapi.dll dwrite.dll dxgi.dll
    dxva2.dll gdi32.dll gdiplus.dll hid.dll imm32.dll iphlpapi.dll kernel32.dll kernelbase.dll
    ksuser.dll mf.dll mfplat.dll mfreadwrite.dll mmdevapi.dll msimg32.dll mswsock.dll ncrypt.dll
    netapi32.dll normaliz.dll ntdll.dll ole32.dll oleaut32.dll opengl32.dll powrprof.dll propsys.dll
    psapi.dll rpcrt4.dll secur32.dll setupapi.dll shcore.dll shell32.dll shlwapi.dll user32.dll
    userenv.dll usp10.dll uxtheme.dll version.dll winhttp.dll wininet.dll winmm.dll winspool.drv
    wintrust.dll wldap32.dll ws2_32.dll wsock32.dll wtsapi32.dll
    ${RADIOS_EXTRA_SYSTEM_DLLS})
# Never in any runtime: sanitizer runtimes (no ASan on MinGW; UBSan is trap-only).
set(RADIOS_FORBIDDEN_DLL_REGEX "^(libasan|libubsan|libtsan|liblsan)[-0-9]*\\.dll$")
# Additionally never in a distributed package (D10, build-system 7.4): GPL-only codecs, the
# Vulkan loader and every FFmpeg DLL that does not come from the LGPL build.
set(RADIOS_PACKAGE_FORBIDDEN_DLL_REGEX
    "^(vulkan-1|libx264-[0-9]+|libx265(-[0-9]+)?|xvidcore|libvidstab|libpostproc-[0-9]+|postproc-[0-9]+|librubberband-[0-9]+|libfrei0r.*|libasan|libubsan|libtsan|liblsan)[-0-9]*\\.dll$")
set(RADIOS_ICON_THEMES Adwaita AdwaitaLegacy hicolor)

# --- component "runtime" ------------------------------------------------------------------------
install(TARGETS radios RUNTIME DESTINATION bin COMPONENT runtime)
# GLib launches child processes through these helpers (GSubprocess/g_spawn_* and GIO's URI
# launching behind gtk_show_uri_on_window and GTK link labels); without them g_spawn fails.
install(PROGRAMS "${RADIOS_MSYS2_PREFIX}/bin/gspawn-win64-helper.exe"
                 "${RADIOS_MSYS2_PREFIX}/bin/gspawn-win64-helper-console.exe"
        DESTINATION bin COMPONENT runtime)
# Only the SVG loader is external; png/jpeg/gif/ico/bmp/... are built into gdk-pixbuf 2.44.
install(FILES "${RADIOS_MSYS2_PREFIX}/lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll"
        DESTINATION lib/gdk-pixbuf-2.0/2.10.0/loaders COMPONENT runtime)
install(FILES
        "${RADIOS_MSYS2_PREFIX}/share/glib-2.0/schemas/org.gtk.Settings.FileChooser.gschema.xml"
        "${RADIOS_MSYS2_PREFIX}/share/glib-2.0/schemas/org.gtk.Settings.ColorChooser.gschema.xml"
        "${RADIOS_MSYS2_PREFIX}/share/glib-2.0/schemas/org.gtk.Settings.EmojiChooser.gschema.xml"
        "${RADIOS_MSYS2_PREFIX}/share/glib-2.0/schemas/org.gtk.Settings.Debug.gschema.xml"
        DESTINATION share/glib-2.0/schemas COMPONENT runtime)
# Adwaita without cursors/ (GDK-Win32 uses the native Windows cursors); caches are regenerated.
install(DIRECTORY "${RADIOS_MSYS2_PREFIX}/share/icons/Adwaita"
        DESTINATION share/icons COMPONENT runtime
        PATTERN "cursors" EXCLUDE
        PATTERN "icon-theme.cache" EXCLUDE)
install(DIRECTORY "${RADIOS_MSYS2_PREFIX}/share/icons/AdwaitaLegacy"
        DESTINATION share/icons COMPONENT runtime
        PATTERN "icon-theme.cache" EXCLUDE)
# hicolor: only the index (GTK needs the fallback theme; the gtk3-demo PNGs are not shipped).
install(FILES "${RADIOS_MSYS2_PREFIX}/share/icons/hicolor/index.theme"
        DESTINATION share/icons/hicolor COMPONENT runtime)
install(FILES
        "${RADIOS_MSYS2_PREFIX}/share/locale/tr/LC_MESSAGES/gtk30.mo"
        "${RADIOS_MSYS2_PREFIX}/share/locale/tr/LC_MESSAGES/glib20.mo"
        "${RADIOS_MSYS2_PREFIX}/share/locale/tr/LC_MESSAGES/gdk-pixbuf.mo"
        "${RADIOS_MSYS2_PREFIX}/share/locale/tr/LC_MESSAGES/json-glib-1.0.mo"
        DESTINATION share/locale/tr/LC_MESSAGES COMPONENT runtime)
# libcurl (OpenSSL backend) finds its CA bundle relative to the DLL: <prefix>/etc/ssl/certs.
install(FILES "${RADIOS_MSYS2_PREFIX}/etc/ssl/certs/ca-bundle.crt"
        DESTINATION etc/ssl/certs COMPONENT runtime)
install(FILES "${CMAKE_SOURCE_DIR}/packaging/windows/gtk-settings.ini"
        DESTINATION etc/gtk-3.0 RENAME settings.ini COMPONENT runtime)
# Last rule of the component: DLL closure, loaders.cache, schemas, icon caches.
install(CODE "
set(RADIOS_MSYS2_PREFIX [==[${RADIOS_MSYS2_PREFIX}]==])
set(RADIOS_FFMPEG_PREFIX [==[${RADIOS_FFMPEG_PREFIX}]==])
set(RADIOS_OBJDUMP [==[${CMAKE_OBJDUMP}]==])
set(RADIOS_EMULATOR [==[${CMAKE_CROSSCOMPILING_EMULATOR}]==])
set(RADIOS_SYSTEM_DLLS [==[${RADIOS_SYSTEM_DLLS}]==])
set(RADIOS_FORBIDDEN_DLL_REGEX [==[${RADIOS_FORBIDDEN_DLL_REGEX}]==])
set(RADIOS_ICON_THEMES [==[${RADIOS_ICON_THEMES}]==])
include([==[${CMAKE_SOURCE_DIR}/packaging/windows/cmake/stage-runtime-deps.cmake]==])
" COMPONENT runtime)

# --- component "symbols" (D11) --------------------------------------------------------------
# The PDB exists only when the build area links with -Wl,--pdb (Release); OPTIONAL keeps Debug
# builds installable. The installer offers it as an unselected optional component.
install(FILES "$<TARGET_FILE_DIR:radios>/$<TARGET_FILE_BASE_NAME:radios>.pdb"
        DESTINATION bin COMPONENT symbols OPTIONAL)

# --- component "python" -----------------------------------------------------------------------
install(CODE "
set(RADIOS_PYTHON_VERSION [==[${RADIOS_PYTHON_VERSION}]==])
set(RADIOS_PYTHON_EMBED_ZIP [==[${RADIOS_PYTHON_EMBED_ZIP}]==])
set(RADIOS_PYTHON_EMBED_SHA256 [==[${RADIOS_PYTHON_EMBED_SHA256}]==])
set(RADIOS_TZDATA_WHEEL [==[${RADIOS_TZDATA_WHEEL}]==])
set(RADIOS_TZDATA_SHA256 [==[${RADIOS_TZDATA_SHA256}]==])
set(RADIOS_PYTHON_WORK_DIR [==[${CMAKE_BINARY_DIR}/radios-python-work]==])
include([==[${CMAKE_SOURCE_DIR}/packaging/windows/cmake/stage-python.cmake]==])
" COMPONENT python)

# --- component "application" ------------------------------------------------------------------
# Python tools started by RadioS.exe (data.c: radios_database.py, admin_ui.c: radios_reliability.py)
# plus the modules they import and the operator's hardware report tool.
set(RADIOS_INSTALLED_TOOLS
    radios_database.py
    radios_database_setup.py
    radios_reliability.py
    radios_platform.py
    radios_hardware_check.py)
set(_radios_tool_paths "")
foreach(_radios_tool IN LISTS RADIOS_INSTALLED_TOOLS)
    set(_radios_path "${CMAKE_SOURCE_DIR}/tools/${_radios_tool}")
    if(NOT EXISTS "${_radios_path}")
        message(FATAL_ERROR "RadioSRuntime: tools/${_radios_tool} yok / missing (RADIOS_INSTALLED_TOOLS).")
    endif()
    list(APPEND _radios_tool_paths "${_radios_path}")
    # Every sibling module (tools/<name>.py) an installed tool imports must be installed too,
    # otherwise the installed tool fails with ModuleNotFoundError on the station PC only.
    file(STRINGS "${_radios_path}" _radios_imports ENCODING UTF-8
         REGEX "^[ \t]*(import[ \t]+[A-Za-z_]|from[ \t]+[A-Za-z_])")
    foreach(_radios_line IN LISTS _radios_imports)
        set(_radios_modules "")
        if(_radios_line MATCHES "^[ \t]*from[ \t]+([A-Za-z_][A-Za-z0-9_]*)")
            set(_radios_modules "${CMAKE_MATCH_1}")
        elseif(_radios_line MATCHES "^[ \t]*import[ \t]+([^#]+)")
            string(REPLACE "," ";" _radios_parts "${CMAKE_MATCH_1}")
            foreach(_radios_part IN LISTS _radios_parts)
                if(_radios_part MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)")
                    list(APPEND _radios_modules "${CMAKE_MATCH_1}")
                endif()
            endforeach()
        endif()
        foreach(_radios_module IN LISTS _radios_modules)
            if(EXISTS "${CMAKE_SOURCE_DIR}/tools/${_radios_module}.py"
               AND NOT "${_radios_module}.py" IN_LIST RADIOS_INSTALLED_TOOLS)
                message(FATAL_ERROR "RadioSRuntime: tools/${_radios_tool} '${_radios_module}' modülünü içe "
                                    "aktarıyor ama kurulmuyor / imports '${_radios_module}' which is not "
                                    "installed; add ${_radios_module}.py to RADIOS_INSTALLED_TOOLS.")
            endif()
        endforeach()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_radios_path}")
endforeach()
unset(_radios_modules)
unset(_radios_parts)
unset(_radios_part)
foreach(_radios_input config/radios.example.ini config/README.md database/SDBASE/pyproject.toml
                      database/SDBASE/README.md database/SDBASE/src/sdbase/__init__.py
                      assets/fonts/InterVariable.ttf assets/fonts/LICENSE.txt)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${_radios_input}")
        message(FATAL_ERROR "RadioSRuntime: ${_radios_input} yok / missing.")
    endif()
endforeach()
install(FILES "${CMAKE_SOURCE_DIR}/config/radios.example.ini" "${CMAKE_SOURCE_DIR}/config/README.md"
        DESTINATION config COMPONENT application)
install(FILES "${CMAKE_SOURCE_DIR}/database/SDBASE/pyproject.toml" "${CMAKE_SOURCE_DIR}/database/SDBASE/README.md"
        DESTINATION database/SDBASE COMPONENT application)
# Only *.py: exactly the files of the SDBASE source digest (D6, spec 5.7).
install(DIRECTORY "${CMAKE_SOURCE_DIR}/database/SDBASE/src/"
        DESTINATION database/SDBASE/src COMPONENT application
        FILES_MATCHING PATTERN "*.py"
        PATTERN "__pycache__" EXCLUDE)
install(FILES ${_radios_tool_paths} DESTINATION tools COMPONENT application)
install(FILES "${CMAKE_SOURCE_DIR}/assets/fonts/InterVariable.ttf" "${CMAKE_SOURCE_DIR}/assets/fonts/LICENSE.txt"
        DESTINATION assets/fonts COMPONENT application)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/themes/"
        DESTINATION assets/themes COMPONENT application
        FILES_MATCHING PATTERN "*.css")
# User documentation: the numbered chapters docs/NN-*.md (the other files and sub-folders are
# development history and stay out of the package).
file(GLOB _radios_docs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/docs/[0-9][0-9]-*.md")
if(NOT _radios_docs)
    message(FATAL_ERROR "RadioSRuntime: docs/NN-*.md yok / missing.")
endif()
install(FILES ${_radios_docs} DESTINATION docs COMPONENT application)
unset(_radios_docs)
# Operator launchers of the database service (tools area). They resolve the interpreter as
# <root>\runtime\python (dev) or <root>\python (installed, D4). Installed when present; the
# installer itself stops the service with python\python.exe directly and does not need them.
set(RADIOS_INSTALLED_LAUNCHERS run-database.cmd run-database.ps1)
set(_radios_launchers "")
foreach(_radios_launcher IN LISTS RADIOS_INSTALLED_LAUNCHERS)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_radios_launcher}")
        list(APPEND _radios_launchers "${CMAKE_SOURCE_DIR}/${_radios_launcher}")
    else()
        message(STATUS "RadioS: ${_radios_launcher} yok, pakete girmez / missing, not packaged")
    endif()
endforeach()
if(_radios_launchers)
    install(FILES ${_radios_launchers} DESTINATION . COMPONENT application)
endif()
unset(_radios_launchers)
unset(_radios_launcher)
foreach(_radios_dir backups cache database endpoints imports logs recordings)
    install(DIRECTORY DESTINATION "data/${_radios_dir}" COMPONENT application)
endforeach()
unset(_radios_tool)
unset(_radios_path)
unset(_radios_imports)
unset(_radios_line)
unset(_radios_module)
unset(_radios_input)
unset(_radios_dir)

# --- dev runtime <root>/runtime (D4) ----------------------------------------------------------
set(RADIOS_DEV_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/runtime")
set(_radios_runtime_stage "${CMAKE_BINARY_DIR}/runtime-stage")
add_custom_target(radios_runtime
    # 1. python/ + share/zoneinfo: create when missing, otherwise verify read-only (never rewritten).
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --component python
            --prefix "${RADIOS_DEV_RUNTIME_DIR}"
    # 2. fresh stage of the runtime component (DLL closure, GTK data, caches).
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_radios_runtime_stage}"
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --component runtime
            --prefix "${_radios_runtime_stage}"
    # 3. in-use-safe all-or-nothing sync into <root>/runtime (refuses while RadioS.exe runs).
    COMMAND ${RADIOS_PACKAGING_PYTHON} "${CMAKE_SOURCE_DIR}/packaging/windows/sync_runtime.py"
            --stage "${_radios_runtime_stage}" --prefix "${RADIOS_DEV_RUNTIME_DIR}"
            --keep python --keep share/zoneinfo
            --version "${PROJECT_VERSION}" --build-type "$<CONFIG>"
    DEPENDS radios
    COMMENT "RadioS çalışma ortamı / runtime: ${RADIOS_DEV_RUNTIME_DIR}"
    USES_TERMINAL
    VERBATIM)
unset(_radios_runtime_stage)

if(BUILD_TESTING)
    # Unit test of the sync tool (in-use refusal, keep trees, rollback) with the interpreter that
    # runs it in production.
    add_test(NAME packaging_sync_runtime
             COMMAND ${RADIOS_PACKAGING_PYTHON} "${CMAKE_SOURCE_DIR}/packaging/windows/tests/test_sync_runtime.py")
    set_tests_properties(packaging_sync_runtime PROPERTIES LABELS "packaging" TIMEOUT 120)
endif()
