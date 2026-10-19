# RadioS - finalize and verify the package stage (cmake -P, run by target radios_package_stage).
#
#   cmake -DRADIOS_PACKAGE_CONFIG=<build>/packaging/package-config.cmake -P finalize-package-stage.cmake
#
# The stage is the result of "cmake --install --component runtime|python|application" into
# RADIOS_STAGE (+ "symbols" into RADIOS_SYMBOLS_STAGE). This script
#   1. copies the third-party licence texts (packaging/windows/third-party-licenses.txt) into
#      share/licenses/ and writes share/licenses/THIRD-PARTY-NOTICES.txt - every shipped DLL/EXE
#      must have a rule;
#   2. verifies the stage: required runtime/application files, no user data or user
#      configuration, no build-only tools, self-contained DLL closure (every import resolves
#      inside bin/ or to a listed Windows component), package DLL policy (LGPL FFmpeg only,
#      byte-identical to RADIOS_FFMPEG_PREFIX; no GPL-only codecs, no Vulkan loader, no
#      sanitizers), ASCII names, MAX_PATH budget, no case-insensitive duplicates;
#   3. writes the NSIS include files (exact install/uninstall lists) into RADIOS_NSIS_DIR.
# Any problem is a FATAL_ERROR: nothing is packaged from an unverified stage.

if(NOT RADIOS_PACKAGE_CONFIG OR NOT EXISTS "${RADIOS_PACKAGE_CONFIG}")
    message(FATAL_ERROR "finalize-package-stage.cmake: -DRADIOS_PACKAGE_CONFIG=<file> is required.")
endif()
include("${RADIOS_PACKAGE_CONFIG}")
include("${CMAKE_CURRENT_LIST_DIR}/RadioSPeClosure.cmake")

set(stage "${RADIOS_STAGE}")
if(NOT IS_DIRECTORY "${stage}")
    message(FATAL_ERROR "Paket aşaması yok / package stage missing: ${stage}")
endif()

# =========================================================================================
# 1. Licences
# =========================================================================================
file(STRINGS "${RADIOS_LICENSE_RULES}" rule_lines ENCODING UTF-8)
set(rules "")
set(index 0)
foreach(line IN LISTS rule_lines)
    if(line MATCHES "^[ \t]*(#|$)")
        continue()
    endif()
    string(REPLACE " | " ";" fields "${line}")
    list(LENGTH fields n)
    if(NOT n EQUAL 4)
        message(FATAL_ERROR "${RADIOS_LICENSE_RULES}: hatalı satır / malformed rule: ${line}")
    endif()
    list(GET fields 0 rule_match)
    list(GET fields 1 rule_component)
    list(GET fields 2 rule_spdx)
    list(GET fields 3 rule_texts)
    set(rule_${index}_match "${rule_match}")
    set(rule_${index}_component "${rule_component}")
    set(rule_${index}_spdx "${rule_spdx}")
    set(rule_${index}_texts "${rule_texts}")
    list(APPEND rules ${index})
    math(EXPR index "${index} + 1")
endforeach()

set(licenses_dir "${stage}/share/licenses")
file(REMOVE_RECURSE "${licenses_dir}")
file(MAKE_DIRECTORY "${licenses_dir}")
set(wheel_dir "${RADIOS_WORK_DIR}/tzdata-wheel")
file(REMOVE_RECURSE "${wheel_dir}")
file(ARCHIVE_EXTRACT INPUT "${RADIOS_TZDATA_WHEEL}" DESTINATION "${wheel_dir}")

# copy_texts(<rule index> <out: list of shipped text paths>)
function(copy_texts rule out_var)
    set(shipped "")
    string(REPLACE "," ";" texts "${rule_${rule}_texts}")
    foreach(text IN LISTS texts)
        string(STRIP "${text}" text)
        if(text STREQUAL "-")
            continue()
        endif()
        if(text MATCHES "^package:(.+)$")
            if(NOT EXISTS "${stage}/${CMAKE_MATCH_1}")
                message(FATAL_ERROR "Lisans metni pakette yok / licence text missing in the package: ${CMAKE_MATCH_1}")
            endif()
            list(APPEND shipped "${CMAKE_MATCH_1}")
            continue()
        endif()
        if(text MATCHES "^msys2:(.+)$")
            set(rel "${CMAKE_MATCH_1}")
            set(source_root "${RADIOS_MSYS2_PREFIX}/share/licenses")
            set(dest_rel "${rel}")
        elseif(text MATCHES "^ffmpeg:(.+)$")
            set(rel "${CMAKE_MATCH_1}")
            if(NOT RADIOS_FFMPEG_PREFIX)
                message(FATAL_ERROR "FFmpeg lisansı istendi ama RADIOS_FFMPEG_PREFIX boş / FFmpeg licence requested "
                                    "but RADIOS_FFMPEG_PREFIX is empty.")
            endif()
            set(source_root "${RADIOS_FFMPEG_PREFIX}")
            cmake_path(GET rel FILENAME leaf)
            set(dest_rel "ffmpeg/${leaf}")
        elseif(text MATCHES "^wheel:(.+)$")
            set(rel "${CMAKE_MATCH_1}")
            set(source_root "${wheel_dir}")
            cmake_path(GET rel FILENAME leaf)
            set(dest_rel "tzdata/${leaf}")
        else()
            message(FATAL_ERROR "${RADIOS_LICENSE_RULES}: bilinmeyen kaynak / unknown text source '${text}'")
        endif()
        if(rel MATCHES "^(.*)/\\*$")
            set(folder "${CMAKE_MATCH_1}")
            file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${source_root}/${folder}" "${source_root}/${folder}/*")
            if(NOT files)
                message(FATAL_ERROR "Lisans klasörü boş veya yok / licence folder empty or missing: ${source_root}/${folder}")
            endif()
            string(REGEX REPLACE "/\\*$" "" dest_folder "${dest_rel}")
            if(text MATCHES "^ffmpeg:")
                set(dest_folder "ffmpeg")
            endif()
            foreach(f IN LISTS files)
                configure_file("${source_root}/${folder}/${f}" "${licenses_dir}/${dest_folder}/${f}" COPYONLY)
                list(APPEND shipped "share/licenses/${dest_folder}/${f}")
            endforeach()
        else()
            if(NOT EXISTS "${source_root}/${rel}")
                message(FATAL_ERROR "Lisans metni yok / licence text missing: ${source_root}/${rel}")
            endif()
            configure_file("${source_root}/${rel}" "${licenses_dir}/${dest_rel}" COPYONLY)
            list(APPEND shipped "share/licenses/${dest_rel}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES shipped)
    set(${out_var} "${shipped}" PARENT_SCOPE)
endfunction()

file(GLOB shipped_binaries LIST_DIRECTORIES false
     "${stage}/bin/*.dll" "${stage}/bin/*.exe" "${stage}/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll")
list(SORT shipped_binaries CASE INSENSITIVE)
set(notice "RadioS ${RADIOS_VERSION} - üçüncü taraf bileşenler / third-party components\n")
string(APPEND notice "Bu paket aşağıdaki bileşenleri içerir; lisans metinleri share/licenses/ altındadır.\n")
string(APPEND notice "This package contains the components below; licence texts are in share/licenses/.\n")
string(APPEND notice "LGPL kütüphaneleri ayrı DLL olarak gelir ve aynı ABI'li sürümleriyle değiştirilebilir.\n")
string(APPEND notice "LGPL libraries are separate DLLs and may be replaced by ABI-compatible builds.\n")
string(APPEND notice "Kaynak / sources: MSYS2 UCRT64 packages listed in msys2-packages.lock "
                     "(https://packages.msys2.org), FFmpeg ${RADIOS_FFMPEG_VERSION_NOTE}.\n\n")
set(unmatched "")
foreach(binary IN LISTS shipped_binaries)
    cmake_path(GET binary FILENAME leaf)
    string(TOLOWER "${leaf}" lower)
    if(lower STREQUAL "radios.exe")
        continue()
    endif()
    set(found "")
    foreach(rule IN LISTS rules)
        if(NOT rule_${rule}_match STREQUAL "@always" AND lower MATCHES "${rule_${rule}_match}")
            set(found "${rule}")
            break()
        endif()
    endforeach()
    if(found STREQUAL "")
        list(APPEND unmatched "${leaf}")
        continue()
    endif()
    copy_texts(${found} texts)
    list(JOIN texts ", " texts)
    if(texts STREQUAL "")
        set(texts "-")
    endif()
    string(APPEND notice "${leaf}\n  ${rule_${found}_component}; ${rule_${found}_spdx}\n  ${texts}\n")
endforeach()
if(unmatched)
    list(JOIN unmatched "\n  " unmatched)
    message(FATAL_ERROR "Lisansı kaydedilmemiş bileşen / component without a licence rule in "
                        "${RADIOS_LICENSE_RULES}:\n  ${unmatched}")
endif()
string(APPEND notice "\n")
foreach(rule IN LISTS rules)
    if(rule_${rule}_match STREQUAL "@always")
        copy_texts(${rule} texts)
        list(JOIN texts ", " texts)
        if(texts STREQUAL "")
            set(texts "-")
        endif()
        string(APPEND notice "${rule_${rule}_component}\n  ${rule_${rule}_spdx}\n  ${texts}\n")
    endif()
endforeach()
file(WRITE "${licenses_dir}/THIRD-PARTY-NOTICES.txt" "${notice}")
# The exact MSYS2 package versions (and package-file SHA-256) the bundled DLLs come from: the
# "corresponding source" reference for the LGPL/GPL components.
if(NOT RADIOS_MSYS2_LOCK OR NOT EXISTS "${RADIOS_MSYS2_LOCK}")
    message(FATAL_ERROR "MSYS2 paket kilidi yok / package lock missing: '${RADIOS_MSYS2_LOCK}'")
endif()
configure_file("${RADIOS_MSYS2_LOCK}" "${licenses_dir}/msys2-packages.lock" COPYONLY)
file(REMOVE_RECURSE "${wheel_dir}")

# =========================================================================================
# 2. Verification
# =========================================================================================
set(problems "")
macro(problem text)
    list(APPEND problems "${text}")
endmacro()

set(required
    bin/RadioS.exe
    bin/gspawn-win64-helper.exe
    bin/gspawn-win64-helper-console.exe
    lib/gdk-pixbuf-2.0/2.10.0/loaders.cache
    lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll
    share/glib-2.0/schemas/gschemas.compiled
    share/locale/tr/LC_MESSAGES/gtk30.mo
    share/locale/tr/LC_MESSAGES/glib20.mo
    share/locale/tr/LC_MESSAGES/gdk-pixbuf.mo
    share/locale/tr/LC_MESSAGES/json-glib-1.0.mo
    share/zoneinfo/Europe/Istanbul
    share/zoneinfo/UTC
    share/licenses/THIRD-PARTY-NOTICES.txt
    share/licenses/msys2-packages.lock
    etc/gtk-3.0/settings.ini
    etc/ssl/certs/ca-bundle.crt
    python/python.exe
    python/python${RADIOS_PYTHON_ABI}.dll
    python/python${RADIOS_PYTHON_ABI}.zip
    python/Lib/tzdata/__init__.py
    python/Lib/tzdata/zoneinfo/Europe/Istanbul
    config/radios.example.ini
    database/SDBASE/pyproject.toml
    database/SDBASE/src/sdbase/__init__.py
    tools/radios_database.py
    assets/fonts/InterVariable.ttf
    assets/fonts/LICENSE.txt)
foreach(theme IN LISTS RADIOS_ICON_THEMES)
    list(APPEND required "share/icons/${theme}/index.theme")
    file(GLOB theme_entries LIST_DIRECTORIES true "${stage}/share/icons/${theme}/*")
    list(LENGTH theme_entries theme_entry_count)
    if(theme_entry_count GREATER 1)   # more than index.theme: a cache is mandatory
        list(APPEND required "share/icons/${theme}/icon-theme.cache")
    endif()
endforeach()
foreach(rel IN LISTS required)
    if(NOT EXISTS "${stage}/${rel}")
        problem("eksik/missing: ${rel}")
    endif()
endforeach()
foreach(dir backups cache database endpoints imports logs recordings)
    if(NOT IS_DIRECTORY "${stage}/data/${dir}")
        problem("eksik boş klasör/missing empty directory: data/${dir}")
    endif()
endforeach()

file(GLOB_RECURSE all_files LIST_DIRECTORIES false RELATIVE "${stage}" "${stage}/*")
file(GLOB_RECURSE all_entries LIST_DIRECTORIES true RELATIVE "${stage}" "${stage}/*")
list(SORT all_files)
list(SORT all_entries)
string(TOLOWER "${all_entries}" folded)
list(SORT folded)
set(previous "")
foreach(lower IN LISTS folded)
    if(lower STREQUAL previous)
        problem("büyük/küçük harf çakışması/case-insensitive duplicate: ${lower}")
    endif()
    set(previous "${lower}")
endforeach()
foreach(rel IN LISTS all_entries)
    if(IS_SYMLINK "${stage}/${rel}")
        problem("bağlantı/link in package (not supported by the installer): ${rel}")
    endif()
    if(NOT rel MATCHES "^[ -~]+$")
        problem("ASCII olmayan ad/non-ASCII name: ${rel}")
    endif()
    string(LENGTH "${rel}" length)
    if(length GREATER 180)
        problem("yol çok uzun/path too long (${length} > 180, MAX_PATH): ${rel}")
    endif()
endforeach()
foreach(rel IN LISTS all_files)
    string(TOLOWER "${rel}" lower)
    if(lower MATCHES "^data/")
        problem("kullanıcı verisi/user data in package: ${rel}")
    elseif(lower MATCHES "^config/" AND NOT lower MATCHES "^config/(radios\\.example\\.ini|readme\\.md)$")
        problem("kullanıcı yapılandırması/user configuration in package: ${rel}")
    elseif(lower MATCHES "(^|/)__pycache__/|\\.py[co]$")
        problem("derlenmiş Python/compiled Python in package: ${rel}")
    elseif(lower MATCHES "^python/.*\\._?pth$")
        problem("._pth/.pth in python/ (D4): ${rel}")
    elseif(lower MATCHES "^bin/(gdk-pixbuf-query-loaders|glib-compile-schemas|gtk-update-icon-cache|gdbus|objdump|makensis)\\.exe$")
        problem("derleme aracı/build-time tool in package: ${rel}")
    elseif(lower MATCHES "\\.(pdb|a|la|h|pc|cmake)$")
        problem("geliştirme dosyası/development file in package: ${rel}")
    endif()
endforeach()

# Self-contained DLL closure: search only the stage's bin.
file(GLOB stage_exes LIST_DIRECTORIES false "${stage}/bin/*.exe")
file(GLOB stage_dlls LIST_DIRECTORIES false "${stage}/bin/*.dll")
file(GLOB stage_loaders LIST_DIRECTORIES false "${stage}/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll")
radios_pe_closure(resolved unresolved
    OBJDUMP "${RADIOS_OBJDUMP}"
    EXECUTABLES ${stage_exes}
    MODULES ${stage_loaders}
    LIBRARIES ${stage_dlls}
    DIRECTORIES "${stage}/bin"
    SYSTEM_DLLS ${RADIOS_SYSTEM_DLLS})
foreach(name IN LISTS unresolved)
    problem("paket içinde çözülemeyen DLL/DLL not resolvable inside the package: ${name}")
endforeach()
foreach(dll IN LISTS resolved)
    radios_path_under(inside "${dll}" "${stage}/bin")
    if(NOT inside)
        problem("paket dışından çözülen DLL/DLL resolved outside the package: ${dll}")
    endif()
endforeach()

foreach(dll IN LISTS stage_dlls)
    cmake_path(GET dll FILENAME leaf)
    string(TOLOWER "${leaf}" lower)
    if(lower MATCHES "${RADIOS_PACKAGE_FORBIDDEN_DLL_REGEX}")
        problem("dağıtılamaz DLL/DLL that must not be distributed (D10): ${leaf}")
    endif()
    if(lower MATCHES "^(avcodec|avformat|avutil|avfilter|swresample|swscale|avdevice|postproc)-[0-9]+\\.dll$")
        if(NOT RADIOS_FFMPEG_PREFIX)
            problem("FFmpeg DLL ${leaf}: LGPL FFmpeg (RADIOS_FFMPEG_PREFIX, D10) olmadan paketlenemez / cannot be "
                    "packaged without the LGPL FFmpeg build (packaging/windows/build-ffmpeg.ps1)")
        elseif(NOT EXISTS "${RADIOS_FFMPEG_PREFIX}/bin/${leaf}")
            problem("FFmpeg DLL ${leaf} LGPL yapısından gelmiyor / does not come from the LGPL build ${RADIOS_FFMPEG_PREFIX}")
        else()
            file(SHA256 "${dll}" a)
            file(SHA256 "${RADIOS_FFMPEG_PREFIX}/bin/${leaf}" b)
            if(NOT a STREQUAL b)
                problem("FFmpeg DLL ${leaf} LGPL yapısıyla aynı değil / differs from the LGPL build")
            endif()
        endif()
    endif()
endforeach()

if(problems)
    list(LENGTH problems count)
    list(JOIN problems "\n  " text)
    message(FATAL_ERROR "Paket aşaması doğrulanamadı (${count} sorun) / package stage verification failed "
                        "(${count} problems):\n  ${text}")
endif()

# =========================================================================================
# 3. NSIS include files (exact lists; paths relative to the stage = makensis working dir)
# =========================================================================================
function(nsis_path out_var rel)
    string(REPLACE "/" "\\" rel "${rel}")
    string(REPLACE "$" "$$" rel "${rel}")
    set(${out_var} "${rel}" PARENT_SCOPE)
endfunction()

# write_lists(<root> <source prefix (relative to the stage)> <install out> <uninstall out>)
function(write_lists root source_prefix install_out uninstall_out)
    file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${root}" "${root}/*")
    file(GLOB_RECURSE entries LIST_DIRECTORIES true RELATIVE "${root}" "${root}/*")
    list(SORT files)
    set(dirs "")
    foreach(entry IN LISTS entries)
        if(IS_DIRECTORY "${root}/${entry}")
            list(APPEND dirs "${entry}")
        endif()
    endforeach()
    list(SORT dirs)
    set(install "; Generated by finalize-package-stage.cmake - do not edit.\n")
    set(uninstall "${install}")
    set(caches "")
    set(current_dir "<none>")
    foreach(rel IN LISTS files)
        if(rel MATCHES "^share/icons/[^/]+/icon-theme\\.cache$")
            list(APPEND caches "${rel}")
            continue()
        endif()
        cmake_path(GET rel PARENT_PATH dir)
        if(NOT dir STREQUAL current_dir)
            nsis_path(win_dir "${dir}")
            if(dir STREQUAL "")
                string(APPEND install "SetOutPath \"$INSTDIR\"\n")
            else()
                string(APPEND install "SetOutPath \"$INSTDIR\\${win_dir}\"\n")
            endif()
            set(current_dir "${dir}")
        endif()
        nsis_path(win_source "${source_prefix}${rel}")
        string(APPEND install "File \"${win_source}\"\n")
    endforeach()
    # Icon caches last and stamped with the install time: GTK ignores a cache that is older
    # than its theme directory.
    if(caches)
        string(APPEND install "SetDateSave off\n")
        foreach(rel IN LISTS caches)
            cmake_path(GET rel PARENT_PATH dir)
            nsis_path(win_dir "${dir}")
            nsis_path(win_source "${source_prefix}${rel}")
            string(APPEND install "SetOutPath \"$INSTDIR\\${win_dir}\"\nFile \"${win_source}\"\n")
        endforeach()
        string(APPEND install "SetDateSave on\n")
    endif()
    foreach(dir IN LISTS dirs)
        file(GLOB children "${root}/${dir}/*")
        if(NOT children)
            nsis_path(win_dir "${dir}")
            string(APPEND install "CreateDirectory \"$INSTDIR\\${win_dir}\"\n")
        endif()
    endforeach()
    foreach(rel IN LISTS files)
        nsis_path(win_rel "${rel}")
        string(APPEND uninstall "Delete \"$INSTDIR\\${win_rel}\"\n")
    endforeach()
    # Directories deepest first; RMDir without /r only removes empty ones. data\ is never
    # touched (station data), config\ stays as long as user configuration is in it.
    set(by_depth "")
    foreach(dir IN LISTS dirs)
        string(TOLOWER "${dir}" lower)
        if(lower STREQUAL "data" OR lower MATCHES "^data/")
            continue()
        endif()
        string(REGEX MATCHALL "/" slashes "${dir}")
        list(LENGTH slashes depth)
        math(EXPR key "50 - ${depth}")   # two digits for depth < 40: string order = deepest first
        list(APPEND by_depth "${key}|${dir}")
    endforeach()
    list(SORT by_depth)
    foreach(item IN LISTS by_depth)
        string(REGEX REPLACE "^[0-9]+\\|" "" dir "${item}")
        nsis_path(win_dir "${dir}")
        string(APPEND uninstall "RMDir \"$INSTDIR\\${win_dir}\"\n")
    endforeach()
    file(WRITE "${install_out}" "${install}")
    file(WRITE "${uninstall_out}" "${uninstall}")
endfunction()

file(MAKE_DIRECTORY "${RADIOS_NSIS_DIR}")
write_lists("${stage}" "" "${RADIOS_NSIS_DIR}/files-install.nsh" "${RADIOS_NSIS_DIR}/files-uninstall.nsh")

set(defines "; Generated by finalize-package-stage.cmake - do not edit.\n")
set(total 0)
foreach(rel IN LISTS all_files)
    file(SIZE "${stage}/${rel}" size)
    math(EXPR total "${total} + ${size}")
endforeach()
set(have_symbols FALSE)
if(RADIOS_SYMBOLS_STAGE AND IS_DIRECTORY "${RADIOS_SYMBOLS_STAGE}")
    file(GLOB_RECURSE symbol_files LIST_DIRECTORIES false "${RADIOS_SYMBOLS_STAGE}/*")
    if(symbol_files)
        set(have_symbols TRUE)
    endif()
endif()
if(have_symbols)
    cmake_path(RELATIVE_PATH RADIOS_SYMBOLS_STAGE BASE_DIRECTORY "${stage}" OUTPUT_VARIABLE symbols_rel)
    write_lists("${RADIOS_SYMBOLS_STAGE}" "${symbols_rel}/" "${RADIOS_NSIS_DIR}/files-symbols-install.nsh"
                "${RADIOS_NSIS_DIR}/files-symbols-uninstall.nsh")
    string(APPEND defines "!define RADIOS_HAVE_SYMBOLS\n")
else()
    file(REMOVE "${RADIOS_NSIS_DIR}/files-symbols-install.nsh" "${RADIOS_NSIS_DIR}/files-symbols-uninstall.nsh")
endif()
math(EXPR total_kb "(${total} + 1023) / 1024")
string(APPEND defines "!define RADIOS_ESTIMATED_SIZE_KB ${total_kb}\n")
file(WRITE "${RADIOS_NSIS_DIR}/package-defines.nsh" "${defines}")

list(LENGTH all_files file_count)
message(STATUS "RadioS paket aşaması doğrulandı / package stage verified: ${stage} "
               "(${file_count} dosya/files, ${total_kb} KiB)")
