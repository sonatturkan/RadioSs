# RadioS build guard and global compiler settings.
#
# RadioS is built only for Windows 10/11 x64 with the MSYS2 UCRT64 GCC. The guard
# checks the compiler's identity, not its install path, so the same GCC works
# whether it runs natively (C:/msys64/ucrt64, the product build) or through a
# cross-compiling emulator. A WinLibs GCC (C:\MinGW, on the machine PATH) or an
# MSVCRT/32-bit toolchain is refused before any target is defined.
#
# Inputs (cache):
#   RADIOS_MSYS2_PREFIX   MSYS2 UCRT64 prefix (bin/, include/, lib/pkgconfig/).
#                         Default on Windows: the directory above gcc.exe.
#   RADIOS_FFMPEG_PREFIX  Optional LGPL FFmpeg 9.0.2 prefix (DECISIONS D10).
#   RADIOS_BUILD_PYTHON   MSYS2 python.exe for build-time generators.
# Outputs:
#   RADIOS_BUILD_PYTHON_COMMAND  emulator + python + "-X utf8 -B -I".
#   radios_warnings              INTERFACE target with the strict C warnings.
include_guard(GLOBAL)

if(NOT WIN32 OR NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "RadioS yalnız Windows x64 için MSYS2 UCRT64 GCC ile derlenir. "
                        "CMakePresets.json içindeki windows-* ön ayarlarını kullanın "
                        "(cmake --preset windows-debug).")
endif()

# ---- 1. Compiler identity --------------------------------------------------------------
set(RADIOS_GCC_MINIMUM_VERSION 16.2)
set(RADIOS_GCC_VERIFIED_MAJOR 16)
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "Derleyici ${CMAKE_C_COMPILER} (${CMAKE_C_COMPILER_ID}) GCC değil. "
                        "RadioS yalnız MSYS2 UCRT64 GCC ile derlenir.")
endif()
if(CMAKE_C_COMPILER_VERSION VERSION_LESS RADIOS_GCC_MINIMUM_VERSION)
    message(FATAL_ERROR "GCC ${CMAKE_C_COMPILER_VERSION} çok eski (${CMAKE_C_COMPILER}); en az "
                        "${RADIOS_GCC_MINIMUM_VERSION} gerekir (MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-gcc).")
endif()
math(EXPR _radios_next_major "${RADIOS_GCC_VERIFIED_MAJOR} + 1")
if(NOT CMAKE_C_COMPILER_VERSION VERSION_LESS _radios_next_major)
    message(WARNING "GCC ${CMAKE_C_COMPILER_VERSION} doğrulanan sürüm ailesinin "
                    "(${RADIOS_GCC_VERIFIED_MAJOR}.x) dışında; -Werror yeni uyarılarla derlemeyi durdurabilir.")
endif()
foreach(_radios_tool IN ITEMS "${CMAKE_C_COMPILER}" "${CMAKE_RC_COMPILER}")
    file(REAL_PATH "${_radios_tool}" _radios_real)
    foreach(_radios_candidate IN ITEMS "${_radios_tool}" "${_radios_real}")
        string(TOLOWER "${_radios_candidate}" _radios_lower)
        if(_radios_lower MATCHES "^[a-z]:[/\\\\]mingw([/\\\\]|$)")
            message(FATAL_ERROR "${_radios_tool} C:\\MinGW altında (WinLibs). Bu araç zinciri MSYS2 "
                                "kütüphaneleriyle ABI/DLL uyuşmazlığı yaratır; kullanılmaz.")
        endif()
    endforeach()
endforeach()
# The --version banner, the target triplet and the RC compiler are probed once per
# compiler path (each probe starts a process; cached as INTERNAL).
if(NOT RADIOS_TOOLCHAIN_IDENTITY STREQUAL "${CMAKE_C_COMPILER}|${CMAKE_RC_COMPILER}")
    execute_process(COMMAND "${CMAKE_C_COMPILER}" --version
                    OUTPUT_VARIABLE _radios_banner ERROR_VARIABLE _radios_banner_error
                    RESULT_VARIABLE _radios_result)
    if(NOT _radios_result EQUAL 0 OR NOT _radios_banner MATCHES "Built by MSYS2 project")
        string(REGEX REPLACE "\n.*" "" _radios_banner "${_radios_banner}")
        message(FATAL_ERROR "${CMAKE_C_COMPILER} MSYS2 projesinin GCC'si değil (\"${_radios_banner}\"). "
                            "C:/msys64/ucrt64/bin/gcc.exe kullanın.")
    endif()
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                    OUTPUT_VARIABLE _radios_machine OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _radios_result)
    if(NOT _radios_result EQUAL 0 OR NOT _radios_machine STREQUAL "x86_64-w64-mingw32")
        message(FATAL_ERROR "${CMAKE_C_COMPILER} hedefi \"${_radios_machine}\"; x86_64-w64-mingw32 gerekir.")
    endif()
    execute_process(COMMAND "${CMAKE_RC_COMPILER}" --version
                    OUTPUT_VARIABLE _radios_rc_banner ERROR_VARIABLE _radios_rc_error
                    RESULT_VARIABLE _radios_result)
    if(NOT _radios_result EQUAL 0 OR NOT _radios_rc_banner MATCHES "^GNU windres")
        message(FATAL_ERROR "Kaynak derleyicisi ${CMAKE_RC_COMPILER} GNU windres değil. "
                            "C:/msys64/ucrt64/bin/windres.exe kullanın.")
    endif()
    string(REGEX REPLACE "\n.*" "" _radios_banner "${_radios_banner}")
    message(STATUS "RadioS araç zinciri: ${_radios_banner}, hedef ${_radios_machine}")
    set(RADIOS_TOOLCHAIN_IDENTITY "${CMAKE_C_COMPILER}|${CMAKE_RC_COMPILER}" CACHE INTERNAL "")
endif()
include(CheckSymbolExists)
check_symbol_exists(_UCRT "stdio.h" RADIOS_HAVE_UCRT)
if(NOT RADIOS_HAVE_UCRT)
    message(FATAL_ERROR "${CMAKE_C_COMPILER} UCRT çalışma zamanını kullanmıyor (_UCRT tanımsız). "
                        "RadioS MSYS2 UCRT64 ortamını ister (MINGW64/MSVCRT değil).")
endif()

# ---- 2. MSYS2 prefix -------------------------------------------------------------------
set(RADIOS_MSYS2_PREFIX "" CACHE PATH
    "MSYS2 UCRT64 kökü (bin/, include/, lib/pkgconfig/); boşsa gcc.exe'nin üst dizini")
if(NOT RADIOS_MSYS2_PREFIX)
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Çapraz derlemede RADIOS_MSYS2_PREFIX verilmelidir (MSYS2 UCRT64 kökünün "
                            "bu makinedeki yolu).")
    endif()
    get_filename_component(_radios_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_radios_prefix "${_radios_cc_dir}/.." ABSOLUTE)
    set_property(CACHE RADIOS_MSYS2_PREFIX PROPERTY VALUE "${_radios_prefix}")
endif()
file(TO_CMAKE_PATH "${RADIOS_MSYS2_PREFIX}" _radios_prefix)
if(NOT _radios_prefix STREQUAL RADIOS_MSYS2_PREFIX)
    set_property(CACHE RADIOS_MSYS2_PREFIX PROPERTY VALUE "${_radios_prefix}")
endif()
foreach(_radios_marker IN ITEMS include/_mingw.h lib/pkgconfig/glib-2.0.pc lib/pkgconfig/gtk+-3.0.pc
                                bin/glib-compile-schemas.exe)
    if(NOT EXISTS "${RADIOS_MSYS2_PREFIX}/${_radios_marker}")
        message(FATAL_ERROR "${RADIOS_MSYS2_PREFIX} bir MSYS2 UCRT64 kökü değil "
                            "(${_radios_marker} yok). RADIOS_MSYS2_PREFIX'i düzeltin.")
    endif()
endforeach()

# Path comparisons on Windows are case-insensitive.
function(radios_path_is_inside path directory out_var)
    file(TO_CMAKE_PATH "${path}" _path)
    file(TO_CMAKE_PATH "${directory}" _dir)
    string(TOLOWER "${_path}" _path)
    string(TOLOWER "${_dir}/" _dir)
    string(FIND "${_path}" "${_dir}" _pos)
    if(_pos EQUAL 0)
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

if(NOT CMAKE_CROSSCOMPILING)
    # Native build: the compiler, windres and pkg-config must be the prefix's own tools.
    if(NOT PKG_CONFIG_EXECUTABLE)
        set(PKG_CONFIG_EXECUTABLE "${RADIOS_MSYS2_PREFIX}/bin/pkg-config.exe" CACHE FILEPATH "pkg-config")
    endif()
    foreach(_radios_tool IN ITEMS "${CMAKE_C_COMPILER}" "${CMAKE_RC_COMPILER}" "${PKG_CONFIG_EXECUTABLE}")
        radios_path_is_inside("${_radios_tool}" "${RADIOS_MSYS2_PREFIX}/bin" _radios_inside)
        if(NOT _radios_inside)
            message(FATAL_ERROR "${_radios_tool} ${RADIOS_MSYS2_PREFIX}/bin içinde değil. Araçlar aynı MSYS2 "
                                "UCRT64 kökünden gelmelidir (CMakePresets.json).")
        endif()
    endforeach()
endif()

# ---- 3. pkg-config sees only MSYS2 (+ the optional FFmpeg prefix) ----------------------
set(RADIOS_FFMPEG_PREFIX "" CACHE PATH
    "LGPL FFmpeg 9.0.2 kökü (DECISIONS D10; packaging/windows/build-ffmpeg.ps1). Boşsa MSYS2 FFmpeg (yalnız geliştirme).")
if(CMAKE_HOST_WIN32)
    set(_radios_pc_sep ";")
else()
    set(_radios_pc_sep ":")
endif()
set(ENV{PKG_CONFIG_LIBDIR} "${RADIOS_MSYS2_PREFIX}/lib/pkgconfig${_radios_pc_sep}${RADIOS_MSYS2_PREFIX}/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
if(RADIOS_FFMPEG_PREFIX)
    file(TO_CMAKE_PATH "${RADIOS_FFMPEG_PREFIX}" _radios_ffmpeg)
    if(NOT _radios_ffmpeg STREQUAL RADIOS_FFMPEG_PREFIX)
        set_property(CACHE RADIOS_FFMPEG_PREFIX PROPERTY VALUE "${_radios_ffmpeg}")
    endif()
    foreach(_radios_pc IN ITEMS libavformat libavcodec libavutil libswresample libavfilter)
        if(NOT EXISTS "${RADIOS_FFMPEG_PREFIX}/lib/pkgconfig/${_radios_pc}.pc")
            message(FATAL_ERROR "RADIOS_FFMPEG_PREFIX=${RADIOS_FFMPEG_PREFIX} içinde lib/pkgconfig/${_radios_pc}.pc yok. "
                                "Önce LGPL FFmpeg'i derleyin: packaging/windows/build-ffmpeg.ps1")
        endif()
    endforeach()
    set(ENV{PKG_CONFIG_PATH} "${RADIOS_FFMPEG_PREFIX}/lib/pkgconfig")
endif()

# After pkg_check_modules(<prefix> ...): every existing include and library directory must
# come from <root> (no silent mix of pkg-config trees, no fallback to the GPL FFmpeg).
# A directory that does not exist cannot supply headers or import libraries.
function(radios_require_package_root prefix root)
    set(_dirs ${${prefix}_INCLUDE_DIRS} ${${prefix}_LIBRARY_DIRS})
    foreach(_dir IN LISTS _dirs)
        if(NOT IS_DIRECTORY "${_dir}")
            continue()
        endif()
        radios_path_is_inside("${_dir}/" "${root}" _inside)
        if(NOT _inside)
            message(FATAL_ERROR "${prefix}: ${_dir} beklenen kökün (${root}) dışında. pkg-config yanlış "
                                ".pc dosyalarını buluyor (PKG_CONFIG_PATH/PKG_CONFIG_LIBDIR).")
        endif()
    endforeach()
endfunction()

# ---- 4. Paths: as/ld/ar/windres/objdump are narrow-API tools, MAX_PATH applies ---------
foreach(_radios_dir IN ITEMS "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
    if(_radios_dir MATCHES "[^ -~]")
        message(WARNING "ASCII dışı yol: ${_radios_dir}. as/ld/ar/windres UTF-8 yol desteklemez; "
                        "kaynak ve derleme dizinlerini ASCII tutun.")
    endif()
endforeach()
string(LENGTH "${CMAKE_BINARY_DIR}" _radios_length)
if(_radios_length GREATER 90)
    message(WARNING "Derleme dizini çok uzun (${_radios_length} karakter); nesne yolları MAX_PATH "
                    "sınırına (260) yaklaşır. Kısa bir yol kullanın.")
endif()

# ---- 5. Language level, definitions, flags (DECISIONS D7, D11) -------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
foreach(_radios_definition IN ITEMS _FILE_OFFSET_BITS=64 _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE
                                     NOMINMAX GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_66)
    add_compile_definitions("$<$<COMPILE_LANGUAGE:C>:${_radios_definition}>")
endforeach()
# __USE_MINGW_ANSI_STDIO stays undefined: UCRT printf is C99-correct (build-system.md 4.3.2).
# Release: -O2 (D11), CodeView debug info + PDB for minidumps, hardening, reproducible paths.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -DNDEBUG")
set(_radios_optimized "$<AND:$<CONFIG:Release,RelWithDebInfo>,$<COMPILE_LANGUAGE:C>>")
add_compile_options(
    "$<${_radios_optimized}:-gcodeview>"
    "$<${_radios_optimized}:-fstack-protector-strong>"
    "$<${_radios_optimized}:-D_FORTIFY_SOURCE=2>"
    "$<${_radios_optimized}:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.>"
    "$<${_radios_optimized}:-ffile-prefix-map=${CMAKE_BINARY_DIR}=build>")
add_link_options("$<$<CONFIG:Release,RelWithDebInfo>:LINKER:--no-insert-timestamp>")
# Bound parallel links (about 145 executables, each linking GTK/FFmpeg import libraries).
set_property(GLOBAL APPEND PROPERTY JOB_POOLS radios_link=4)
set(CMAKE_JOB_POOL_LINK radios_link)

option(RADIOS_STRICT_WARNINGS "Derleyici uyarılarını hata say (-Werror)" ON)
add_library(radios_warnings INTERFACE)
target_compile_options(radios_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic>")
if(RADIOS_STRICT_WARNINGS)
    target_compile_options(radios_warnings INTERFACE "$<$<COMPILE_LANGUAGE:C>:-Werror>")
endif()

# ---- 6. Build-time Python (generators only; tests use RADIOS_TEST_PYTHON) --------------
# Pinned to the MSYS2 interpreter: find_package(Python3) would pick a registry/PATH
# Python (PyManager 3.15 on the target machine). Run isolated (-I), UTF-8 mode
# (the design files are UTF-8, the ANSI code page is cp1254), without .pyc (-B).
set(RADIOS_BUILD_PYTHON "${RADIOS_MSYS2_PREFIX}/bin/python.exe" CACHE FILEPATH
    "Derleme zamanı üreteçlerinin Python yorumlayıcısı (MSYS2 UCRT64)")
if(NOT EXISTS "${RADIOS_BUILD_PYTHON}")
    message(FATAL_ERROR "${RADIOS_BUILD_PYTHON} yok (pacman -S mingw-w64-ucrt-x86_64-python).")
endif()
set(RADIOS_BUILD_PYTHON_COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${RADIOS_BUILD_PYTHON}" -X utf8 -B -I)
if(NOT RADIOS_BUILD_PYTHON_CHECKED STREQUAL "${RADIOS_BUILD_PYTHON}")
    execute_process(COMMAND ${RADIOS_BUILD_PYTHON_COMMAND} -c
                            "import sys; print('%d.%d.%d' % sys.version_info[:3], sys.flags.utf8_mode)"
                    OUTPUT_VARIABLE _radios_python OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_VARIABLE _radios_python_error RESULT_VARIABLE _radios_result)
    if(NOT _radios_result EQUAL 0 OR NOT _radios_python MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+) 1$")
        message(FATAL_ERROR "${RADIOS_BUILD_PYTHON} çalışmadı veya UTF-8 kipine geçmedi: "
                            "${_radios_python} ${_radios_python_error}")
    endif()
    if(CMAKE_MATCH_1 VERSION_LESS 3.11)
        message(FATAL_ERROR "${RADIOS_BUILD_PYTHON} ${CMAKE_MATCH_1}; en az 3.11 gerekir.")
    endif()
    message(STATUS "Derleme Python'u: ${RADIOS_BUILD_PYTHON} ${CMAKE_MATCH_1}")
    set(RADIOS_BUILD_PYTHON_CHECKED "${RADIOS_BUILD_PYTHON}" CACHE INTERNAL "")
endif()
