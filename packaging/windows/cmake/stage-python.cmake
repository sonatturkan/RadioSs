# RadioS - install-time step of the "python" component (runs inside "cmake --install").
#
# Produces, under <prefix>:
#   python/               official CPython embeddable amd64 (pinned zip, SHA-256 checked), WITHOUT
#                         python3XX._pth (D4: the app runs "python.exe -E -s -B -X utf8 <script>")
#   python/Lib/tzdata/    PyPI tzdata wheel (pinned, SHA-256 checked) - zoneinfo for SDBASE
#   share/zoneinfo/       the same TZif files for the C side (radios_time_zone_new fallback, D3)
#
# Existing trees are never modified: when <prefix>/python or <prefix>/share/zoneinfo already
# exists (the dev runtime <root>/runtime, possibly in use by the SDBASE service) it is
# verified read-only against the pinned inputs and the step fails with instructions on any
# difference. Missing trees are created from the pinned inputs.
#
# Variables provided by the install(CODE) preamble in cmake/RadioSRuntime.cmake:
#   RADIOS_PYTHON_EMBED_ZIP, RADIOS_PYTHON_EMBED_SHA256, RADIOS_PYTHON_VERSION,
#   RADIOS_TZDATA_WHEEL, RADIOS_TZDATA_SHA256, RADIOS_PYTHON_WORK_DIR

foreach(required RADIOS_PYTHON_EMBED_ZIP RADIOS_PYTHON_EMBED_SHA256 RADIOS_PYTHON_VERSION
                 RADIOS_TZDATA_WHEEL RADIOS_TZDATA_SHA256 RADIOS_PYTHON_WORK_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "stage-python.cmake: ${required} is not set (include it only from the "
                            "install rules of cmake/RadioSRuntime.cmake).")
    endif()
endforeach()
if(NOT "$ENV{DESTDIR}" STREQUAL "")
    message(FATAL_ERROR "DESTDIR desteklenmez / DESTDIR is not supported; use "
                        "'cmake --install <build> --prefix <dir>'.")
endif()

function(radios_check_input path expected)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Sabitlenmiş girdi yok / pinned input missing: ${path}\n"
                            "Dosyayı packaging/windows/cache/ altına koyun (bkz. packaging/README.md) / put the "
                            "verified file into packaging/windows/cache/ (see packaging/README.md).")
    endif()
    file(SHA256 "${path}" actual)
    if(NOT actual STREQUAL "${expected}")
        message(FATAL_ERROR "SHA-256 uyuşmuyor / mismatch: ${path}\n  beklenen/expected ${expected}\n"
                            "  bulunan/actual   ${actual}")
    endif()
endfunction()

radios_check_input("${RADIOS_PYTHON_EMBED_ZIP}" "${RADIOS_PYTHON_EMBED_SHA256}")
radios_check_input("${RADIOS_TZDATA_WHEEL}" "${RADIOS_TZDATA_SHA256}")

string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\." _ "${RADIOS_PYTHON_VERSION}")
set(pth_name "python${CMAKE_MATCH_1}${CMAKE_MATCH_2}._pth")

# Extract both inputs into a private work dir (inside the build tree, never the source tree).
set(work "${RADIOS_PYTHON_WORK_DIR}")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/embed" "${work}/wheel")
file(ARCHIVE_EXTRACT INPUT "${RADIOS_PYTHON_EMBED_ZIP}" DESTINATION "${work}/embed")
file(ARCHIVE_EXTRACT INPUT "${RADIOS_TZDATA_WHEEL}" DESTINATION "${work}/wheel")
if(NOT EXISTS "${work}/embed/python.exe" OR NOT EXISTS "${work}/embed/${pth_name}")
    message(FATAL_ERROR "${RADIOS_PYTHON_EMBED_ZIP}: CPython ${RADIOS_PYTHON_VERSION} embeddable değil / "
                        "is not the CPython ${RADIOS_PYTHON_VERSION} embeddable package (${pth_name}).")
endif()
if(NOT EXISTS "${work}/wheel/tzdata/zoneinfo/Europe/Istanbul")
    message(FATAL_ERROR "${RADIOS_TZDATA_WHEEL}: tzdata/zoneinfo yok / missing.")
endif()

# Expected trees as "<relative path>" lists, relative to their roots.
file(GLOB_RECURSE embed_files LIST_DIRECTORIES false RELATIVE "${work}/embed" "${work}/embed/*")
list(REMOVE_ITEM embed_files "${pth_name}")
file(GLOB_RECURSE tzdata_files LIST_DIRECTORIES false RELATIVE "${work}/wheel/tzdata" "${work}/wheel/tzdata/*")
list(FILTER tzdata_files EXCLUDE REGEX "(^|/)__pycache__/")
set(zoneinfo_files "${tzdata_files}")
list(FILTER zoneinfo_files INCLUDE REGEX "^zoneinfo/")
list(FILTER zoneinfo_files EXCLUDE REGEX "(^|/)__init__\\.py$")
list(TRANSFORM zoneinfo_files REPLACE "^zoneinfo/" "")

# Text files may legitimately differ only in line endings (a git checkout with
# "* text=auto eol=lf" normalises LICENSE.txt); everything else must be byte-identical.
function(radios_same_file out_var expected actual)
    file(SHA256 "${expected}" a)
    file(SHA256 "${actual}" b)
    if(a STREQUAL b)
        set(${out_var} TRUE PARENT_SCOPE)
        return()
    endif()
    if(expected MATCHES "\\.(txt|py|tab|zi)$" OR expected MATCHES "/(leapseconds|LICENSE[^/]*)$")
        file(READ "${expected}" ta)
        file(READ "${actual}" tb)
        string(REPLACE "\r\n" "\n" ta "${ta}")
        string(REPLACE "\r\n" "\n" tb "${tb}")
        if(ta STREQUAL tb)
            set(${out_var} TRUE PARENT_SCOPE)
            return()
        endif()
    endif()
    set(${out_var} FALSE PARENT_SCOPE)
endfunction()

# radios_verify_tree(<label> <expected root> <actual root> <relative files>...)
function(radios_verify_tree label expected_root actual_root)
    set(problems "")
    foreach(rel IN LISTS ARGN)
        if(NOT EXISTS "${actual_root}/${rel}")
            list(APPEND problems "eksik/missing ${rel}")
            continue()
        endif()
        radios_same_file(same "${expected_root}/${rel}" "${actual_root}/${rel}")
        if(NOT same)
            list(APPEND problems "farklı/different ${rel}")
        endif()
    endforeach()
    if(problems)
        list(LENGTH problems n)
        list(SUBLIST problems 0 20 shown)
        list(JOIN shown "\n  " text)
        message(FATAL_ERROR "${actual_root} sabitlenmiş ${label} ile uyuşmuyor (${n} sorun) / does not match "
                            "the pinned ${label} (${n} problems):\n  ${text}\n"
                            "Mevcut ağaç değiştirilmedi. Veritabanı servisini durdurup (run-database.cmd stop) "
                            "klasörü silin ve kurulumu yineleyin / the existing tree was NOT modified; stop the "
                            "database service, delete the folder and run the install again.")
    endif()
endfunction()

set(prefix "${CMAKE_INSTALL_PREFIX}")

# --- python/ --------------------------------------------------------------------------------
set(python_dir "${prefix}/python")
if(EXISTS "${python_dir}")
    radios_verify_tree("CPython ${RADIOS_PYTHON_VERSION} embeddable" "${work}/embed" "${python_dir}" ${embed_files})
    radios_verify_tree("tzdata" "${work}/wheel/tzdata" "${python_dir}/Lib/tzdata" ${tzdata_files})
    file(GLOB stray_pth LIST_DIRECTORIES false "${python_dir}/*._pth" "${python_dir}/*.pth")
    if(stray_pth)
        message(FATAL_ERROR "${python_dir}: ._pth/.pth dosyası var / found ${stray_pth}; D4 yorumlayıcısı "
                            "betik dizinini sys.path'e almalı / the D4 interpreter must not use ._pth files.")
    endif()
    message(STATUS "RadioS: ${python_dir} doğrulandı, değiştirilmedi / verified, left unchanged")
else()
    file(INSTALL "${work}/embed/" DESTINATION "${python_dir}" PATTERN "${pth_name}" EXCLUDE)
    file(INSTALL "${work}/wheel/tzdata" DESTINATION "${python_dir}/Lib" PATTERN "__pycache__" EXCLUDE)
    radios_verify_tree("CPython ${RADIOS_PYTHON_VERSION} embeddable" "${work}/embed" "${python_dir}" ${embed_files})
    radios_verify_tree("tzdata" "${work}/wheel/tzdata" "${python_dir}/Lib/tzdata" ${tzdata_files})
endif()

# --- share/zoneinfo/ ------------------------------------------------------------------------
set(zone_dir "${prefix}/share/zoneinfo")
if(EXISTS "${zone_dir}")
    radios_verify_tree("tzdata zoneinfo" "${work}/wheel/tzdata/zoneinfo" "${zone_dir}" ${zoneinfo_files})
    message(STATUS "RadioS: ${zone_dir} doğrulandı, değiştirilmedi / verified, left unchanged")
else()
    file(INSTALL "${work}/wheel/tzdata/zoneinfo/" DESTINATION "${zone_dir}"
         PATTERN "__init__.py" EXCLUDE PATTERN "__pycache__" EXCLUDE)
    radios_verify_tree("tzdata zoneinfo" "${work}/wheel/tzdata/zoneinfo" "${zone_dir}" ${zoneinfo_files})
endif()

file(REMOVE_RECURSE "${work}")
