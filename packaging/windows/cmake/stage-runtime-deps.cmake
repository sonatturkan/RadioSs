# RadioS - install-time step of the "runtime" component (runs inside "cmake --install").
#
# Declared last among the runtime install rules by cmake/RadioSRuntime.cmake, so RadioS.exe,
# the gspawn helpers, the SVG pixbuf loader, GTK schemas/icons/locales and settings.ini are
# already in <prefix>. This script then
#   1. copies every non-system DLL needed by <prefix>/bin/*.exe|*.dll and the pixbuf loaders
#      (file(GET_RUNTIME_DEPENDENCIES) over objdump; search order: LGPL FFmpeg prefix, MSYS2
#      UCRT64 bin) into <prefix>/bin - unresolved, conflicting or foreign DLLs are fatal;
#   2. regenerates lib/gdk-pixbuf-2.0/2.10.0/loaders.cache (relative module paths) with
#      gdk-pixbuf-query-loaders.exe run from <prefix>/bin, and validates it;
#   3. compiles share/glib-2.0/schemas (glib-compile-schemas --strict);
#   4. regenerates the icon-theme.cache of every shipped theme (last step).
# Windows programs run through RADIOS_EMULATOR (CMAKE_CROSSCOMPILING_EMULATOR at configure
# time; empty on the Windows build PC).
#
# Variables provided by the install(CODE) preamble in cmake/RadioSRuntime.cmake:
#   RADIOS_MSYS2_PREFIX, RADIOS_FFMPEG_PREFIX (may be empty), RADIOS_OBJDUMP, RADIOS_EMULATOR,
#   RADIOS_SYSTEM_DLLS, RADIOS_FORBIDDEN_DLL_REGEX, RADIOS_ICON_THEMES

include("${CMAKE_CURRENT_LIST_DIR}/RadioSPeClosure.cmake")

foreach(required RADIOS_MSYS2_PREFIX RADIOS_OBJDUMP RADIOS_SYSTEM_DLLS RADIOS_FORBIDDEN_DLL_REGEX RADIOS_ICON_THEMES)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "stage-runtime-deps.cmake: ${required} is not set (include it only from "
                            "the install rules of cmake/RadioSRuntime.cmake).")
    endif()
endforeach()
if(NOT "$ENV{DESTDIR}" STREQUAL "")
    message(FATAL_ERROR "DESTDIR desteklenmez / DESTDIR is not supported; use "
                        "'cmake --install <build> --prefix <dir>'.")
endif()

set(prefix "${CMAKE_INSTALL_PREFIX}")
set(bin "${prefix}/bin")
set(loader_dir "${prefix}/lib/gdk-pixbuf-2.0/2.10.0/loaders")
set(msys2_bin "${RADIOS_MSYS2_PREFIX}/bin")

set(search_dirs "")
set(ffmpeg_bin "")
if(RADIOS_FFMPEG_PREFIX)
    set(ffmpeg_bin "${RADIOS_FFMPEG_PREFIX}/bin")
    if(NOT IS_DIRECTORY "${ffmpeg_bin}")
        message(FATAL_ERROR "RADIOS_FFMPEG_PREFIX/bin yok / does not exist: ${ffmpeg_bin}")
    endif()
    list(APPEND search_dirs "${ffmpeg_bin}")   # must win over MSYS2's GPL FFmpeg (D10)
endif()
list(APPEND search_dirs "${msys2_bin}")

# gdk-pixbuf-query-loaders.exe must run from <prefix>/bin so that it writes module paths
# relative to <prefix>; it is a build-time tool and removed again below (not shipped).
set(query_loaders "${bin}/gdk-pixbuf-query-loaders.exe")
file(COPY "${msys2_bin}/gdk-pixbuf-query-loaders.exe" DESTINATION "${bin}")

file(GLOB executables LIST_DIRECTORIES false "${bin}/*.exe")
file(GLOB libraries LIST_DIRECTORIES false "${bin}/*.dll")
file(GLOB loaders LIST_DIRECTORIES false "${loader_dir}/*.dll")
if(NOT EXISTS "${bin}/RadioS.exe")
    message(FATAL_ERROR "${bin}/RadioS.exe yok / missing: the radios target must be installed first.")
endif()
if(NOT loaders)
    message(FATAL_ERROR "Pixbuf yükleyicisi yok / no pixbuf loader in ${loader_dir}")
endif()

radios_pe_closure(resolved unresolved
    OBJDUMP "${RADIOS_OBJDUMP}"
    EXECUTABLES ${executables}
    MODULES ${loaders}
    LIBRARIES ${libraries}
    DIRECTORIES ${search_dirs}
    SYSTEM_DLLS ${RADIOS_SYSTEM_DLLS})

if(unresolved)
    list(JOIN unresolved "\n  " missing)
    message(FATAL_ERROR "Çözülemeyen DLL bağımlılıkları / unresolved DLL dependencies "
                        "(not in ${search_dirs} and not a listed Windows system DLL):\n  ${missing}\n"
                        "Eksik MSYS2 paketini kurun ya da gerçek bir Windows bileşeniyse RADIOS_SYSTEM_DLLS "
                        "listesine ekleyin / install the missing MSYS2 package, or add it to "
                        "RADIOS_SYSTEM_DLLS if it is a Windows component.")
endif()

set(to_copy "")
set(msys2_ffmpeg "")
foreach(dll IN LISTS resolved)
    cmake_path(GET dll FILENAME leaf)
    string(TOLOWER "${leaf}" lower)
    if(lower MATCHES "${RADIOS_FORBIDDEN_DLL_REGEX}")
        message(FATAL_ERROR "Dağıtılmaması gereken DLL / DLL that must never be shipped: ${dll}\n"
                            "(GPL-only codec, Vulkan loader or sanitizer runtime; see DECISIONS D10).")
    endif()
    radios_path_under(in_prefix "${dll}" "${bin}" "${loader_dir}")
    if(in_prefix)
        continue()
    endif()
    radios_path_under(in_search "${dll}" ${search_dirs})
    if(NOT in_search)
        message(FATAL_ERROR "DLL izinli dizinlerin dışından çözüldü / DLL resolved outside the allowed "
                            "directories: ${dll}\nWindows sistem DLL'iyse RADIOS_SYSTEM_DLLS listesine ekleyin; "
                            "değilse PATH/System32'deki yabancı kopyayı kaldırın / if it is a Windows "
                            "component add it to RADIOS_SYSTEM_DLLS, otherwise remove the foreign copy.")
    endif()
    if(lower MATCHES "^(avcodec|avformat|avutil|avfilter|swresample|swscale|avdevice|postproc)-[0-9]+\\.dll$")
        radios_path_under(from_msys2 "${dll}" "${msys2_bin}")
        if(from_msys2)
            list(APPEND msys2_ffmpeg "${leaf}")
        endif()
    endif()
    list(APPEND to_copy "${dll}")
endforeach()
if(msys2_ffmpeg)
    message(WARNING "MSYS2 FFmpeg (GPLv3, ~120 DLL) çalışma ortamına kopyalandı / was bundled: ${msys2_ffmpeg}. "
                    "Yalnız geliştirme içindir; kurulum paketi LGPL FFmpeg ister (RADIOS_FFMPEG_PREFIX, "
                    "packaging/windows/build-ffmpeg.ps1) / development only - the installer and the portable "
                    "package require the LGPL FFmpeg build (DECISIONS D10).")
endif()
list(LENGTH to_copy copy_count)
message(STATUS "RadioS: ${copy_count} DLL -> ${bin}")
if(to_copy)
    file(INSTALL ${to_copy} DESTINATION "${bin}")
endif()

# --- GdkPixbuf loaders.cache --------------------------------------------------------------
set(cache "${prefix}/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache")
file(REMOVE "${cache}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=GDK_PIXBUF_MODULEDIR --unset=GDK_PIXBUF_MODULE_FILE
            ${RADIOS_EMULATOR} "${query_loaders}" --update-cache
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(REMOVE "${query_loaders}")
if(NOT rc EQUAL 0 OR NOT EXISTS "${cache}")
    message(FATAL_ERROR "gdk-pixbuf-query-loaders başarısız / failed (exit ${rc}), ${cache} yazılmadı:\n${out}${err}")
endif()
file(READ "${cache}" cache_text)
# Drop the informational "# LoaderDir = <absolute build path>" comment: the package must not
# carry build-machine paths (reproducibility); gdk-pixbuf ignores comment lines.
string(REGEX REPLACE "(^|\n)# LoaderDir = [^\n]*\n" "\\1" cache_text "${cache_text}")
string(REPLACE "\r\n" "\n" cache_text "${cache_text}")
file(WRITE "${cache}" "${cache_text}")
foreach(loader IN LISTS loaders)
    cmake_path(GET loader FILENAME leaf)
    string(FIND "${cache_text}" "\\\\loaders\\\\${leaf}\"" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "${cache}: '${leaf}' göreli yol olarak kayıtlı değil / is not registered with a "
                            "relative path:\n${cache_text}")
    endif()
endforeach()
if(cache_text MATCHES "\n\"([A-Za-z]:|/|\\\\\\\\)")
    message(FATAL_ERROR "${cache}: mutlak modül yolu içeriyor / contains an absolute module path "
                        "(the runtime would not be relocatable):\n${cache_text}")
endif()
if(NOT cache_text MATCHES "\n\"svg\" ")
    message(FATAL_ERROR "${cache}: SVG biçimi kayıtlı değil / SVG format missing (symbolic icons would vanish):\n${cache_text}")
endif()
if(NOT cache_text MATCHES "\r?\n\r?\n$")
    # gdk-pixbuf registers a module block only when an empty line terminates it.
    message(FATAL_ERROR "${cache}: son modül bloğu boş satırla bitmiyor / last module block is not "
                        "terminated by an empty line.")
endif()

# --- GSettings schemas -------------------------------------------------------------------
set(schema_dir "${prefix}/share/glib-2.0/schemas")
file(REMOVE "${schema_dir}/gschemas.compiled")
radios_run_windows_tool("glib-compile-schemas" "${msys2_bin}/glib-compile-schemas.exe" --strict "${schema_dir}")
if(NOT EXISTS "${schema_dir}/gschemas.compiled")
    message(FATAL_ERROR "${schema_dir}/gschemas.compiled oluşmadı / was not created.")
endif()

# --- Icon theme caches (last: GTK ignores a cache older than its theme directory) ----------
foreach(theme IN LISTS RADIOS_ICON_THEMES)
    set(theme_dir "${prefix}/share/icons/${theme}")
    if(NOT EXISTS "${theme_dir}/index.theme")
        message(FATAL_ERROR "${theme_dir}/index.theme yok / missing.")
    endif()
    file(REMOVE "${theme_dir}/icon-theme.cache")
    file(GLOB_RECURSE theme_files LIST_DIRECTORIES false RELATIVE "${theme_dir}" "${theme_dir}/*")
    list(REMOVE_ITEM theme_files "index.theme")
    if(NOT theme_files)
        # hicolor ships only its index (fallback theme); gtk-update-icon-cache writes no cache
        # for a theme without icons and GTK needs none.
        continue()
    endif()
    radios_run_windows_tool("gtk-update-icon-cache (${theme})" "${msys2_bin}/gtk-update-icon-cache.exe"
                            -f "${theme_dir}")
    if(NOT EXISTS "${theme_dir}/icon-theme.cache")
        message(FATAL_ERROR "${theme_dir}/icon-theme.cache oluşmadı / was not created.")
    endif()
endforeach()
