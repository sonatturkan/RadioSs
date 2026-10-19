# CTest on Windows: interpreters, environment and registration helpers.
#
#   RADIOS_TEST_PYTHON_COMMAND  the shipped CPython (runtime/python/python.exe, DECISIONS
#                               D4/D7) with "-X utf8 -B -s"; run through the cross
#                               emulator when there is one (the usual CMake idiom).
#   RADIOS_TEST_FOREIGN_CWD     a working directory outside the project files, for
#                               the tests that prove nothing depends on the cwd.
#   radios_add_gui_test()       registers a direct GTK test; with the private desktop
#                               launcher (tools/win32/radios-test-desktop.c, D9) the
#                               program runs on its own desktop.
#   radios_add_glib_cases()     registers GLib cases after checking that every
#                               "-p <path>/<case>" exists in the source.
#   radios_add_glib_program()   registers every case of a GLib test program, one
#                               process per case, read from its g_test_add*() calls.
#   radios_finalize_tests()     deferred global environment, zero-test guard, timeouts.
include_guard(GLOBAL)

option(RADIOS_GUI_TESTS
    "GTK arayüz kabul testleri (etkileşimli Windows masaüstü oturumu gerekir; yayın bilgisayarında koşturmayın)" OFF)
set(RADIOS_TEST_PYTHON "${CMAKE_SOURCE_DIR}/runtime/python/python.exe" CACHE FILEPATH
    "Testlerin Python yorumlayıcısı (gömülü CPython 3.14.8 + tzdata)")
set(RADIOS_ZONEINFO_DIR "${CMAKE_SOURCE_DIR}/runtime/share/zoneinfo" CACHE PATH
    "Testlerin TZDIR'i (tzdata 2026.5, SDBASE servisiyle aynı veri)")
set(RADIOS_TEST_TIMEOUT_SCALE 2 CACHE STRING
    "Windows'ta test zaman aşımı çarpanı (süreç başlatma, Python, GTK ve virüs taraması daha yavaş)")
if(NOT RADIOS_TEST_TIMEOUT_SCALE MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "RADIOS_TEST_TIMEOUT_SCALE pozitif bir tam sayı olmalı: '${RADIOS_TEST_TIMEOUT_SCALE}'.")
endif()
set(RADIOS_TEST_PYTHON_COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${RADIOS_TEST_PYTHON}" -X utf8 -B -s)
set(RADIOS_TEST_FOREIGN_CWD "${CMAKE_BINARY_DIR}/test-cwd")
# Compiled into tests as RADIOS_TEST_PROJECT_ROOT. Arguments and environment reach the test
# programs through the emulator's path mapping, compiled-in strings do not: a cross build
# whose tests run through CMAKE_CROSSCOMPILING_EMULATOR gives the tree's path as the test
# programs see it. A native build always uses CMAKE_SOURCE_DIR.
set(RADIOS_TEST_SOURCE_DIR "${CMAKE_SOURCE_DIR}" CACHE STRING
    "Kaynak ağacının test programlarınca görülen yolu (yalnız emülatörle koşan çapraz derlemede farklı)")
if(NOT CMAKE_CROSSCOMPILING AND NOT RADIOS_TEST_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "RADIOS_TEST_SOURCE_DIR (${RADIOS_TEST_SOURCE_DIR}) yalnız çapraz derlemede "
                        "CMAKE_SOURCE_DIR'den (${CMAKE_SOURCE_DIR}) farklı olabilir.")
endif()

if(BUILD_TESTING)
    if(NOT EXISTS "${RADIOS_TEST_PYTHON}")
        message(FATAL_ERROR "${RADIOS_TEST_PYTHON} yok. Testler gömülü CPython 3.14.8 + tzdata çalışma ortamını "
                            "ister: cmake --preset windows-release ve cmake --build --preset windows-runtime "
                            "(runtime\\python ve runtime\\share\\zoneinfo kurulur).")
    endif()
    if(NOT EXISTS "${RADIOS_ZONEINFO_DIR}/Europe/Istanbul")
        message(FATAL_ERROR "${RADIOS_ZONEINFO_DIR} tzdata içermiyor (Europe/Istanbul yok). runtime\\share\\zoneinfo "
                            "tzdata 2026.5 tekerleğinden kurulur (cmake --build --preset windows-runtime).")
    endif()
    if(NOT RADIOS_TEST_PYTHON_CHECKED STREQUAL "${RADIOS_TEST_PYTHON}")
        execute_process(COMMAND ${RADIOS_TEST_PYTHON_COMMAND} -c
                                "import sys, zoneinfo, tzdata; print('%d.%d.%d' % sys.version_info[:3], tzdata.IANA_VERSION, sys.flags.utf8_mode)"
                        OUTPUT_VARIABLE _radios_python OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_VARIABLE _radios_python_error RESULT_VARIABLE _radios_result)
        if(NOT _radios_result EQUAL 0 OR NOT _radios_python MATCHES "^(3\\.14\\.[0-9]+) ([0-9a-z]+) 1$")
            message(FATAL_ERROR "${RADIOS_TEST_PYTHON} CPython 3.14 + tzdata olarak çalışmadı: "
                                "${_radios_python} ${_radios_python_error}")
        endif()
        message(STATUS "Test Python'u: ${RADIOS_TEST_PYTHON} ${CMAKE_MATCH_1}, tzdata ${CMAKE_MATCH_2}")
        set(RADIOS_TEST_PYTHON_CHECKED "${RADIOS_TEST_PYTHON}" CACHE INTERNAL "")
    endif()
    file(MAKE_DIRECTORY "${RADIOS_TEST_FOREIGN_CWD}")

    # Shared test-process support from the tests area (see RadioSWindowsExe.cmake).
    if(RADIOS_TEST_RUNTIME_SOURCE)
        add_library(radios_test_runtime OBJECT "${RADIOS_TEST_RUNTIME_SOURCE}")
        target_link_libraries(radios_test_runtime PUBLIC winmm PRIVATE radios_warnings)
    endif()
    if(RADIOS_TEST_SUPPORT_SOURCE)
        add_library(radios_test_support STATIC "${RADIOS_TEST_SUPPORT_SOURCE}")
        target_include_directories(radios_test_support PUBLIC "${CMAKE_SOURCE_DIR}/tests")
        target_link_libraries(radios_test_support PUBLIC radios_platform PkgConfig::GTK3
                              PRIVATE user32 psapi advapi32 radios_warnings)
        # rt_project_root() canonicalizes this (tests-c.md 4.5 and 5).
        target_compile_definitions(radios_test_support PRIVATE
                                   RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    endif()
endif()

# A test process that runs exactly one GLib case reports a g_test_skip() as CTest "Skipped"
# instead of "Passed" (TAP line "ok 1 /path # SKIP reason"). Only for one-case processes:
# CTest ranks a skip match above a failure.
set(RADIOS_GLIB_SKIP_REGEX "(^|[\r\n])ok [0-9]+ [^\r\n]*# SKIP")

# radios_add_gui_test(NAME <name> COMMAND <target> [<argument>...])
# A GTK program run directly by CTest. With the private desktop launcher (D9) it runs
# on its own desktop: it cannot steal focus or receive the operator's input.
function(radios_add_gui_test)
    cmake_parse_arguments(PARSE_ARGV 0 _gui "" "NAME" "COMMAND")
    if(NOT _gui_NAME OR NOT _gui_COMMAND)
        message(FATAL_ERROR "radios_add_gui_test: NAME ve COMMAND gerekir.")
    endif()
    list(POP_FRONT _gui_COMMAND _program)
    if(NOT TARGET ${_program})
        message(FATAL_ERROR "radios_add_gui_test(${_gui_NAME}): ${_program} bir CMake hedefi değil.")
    endif()
    if(TARGET radios_test_desktop)
        add_test(NAME ${_gui_NAME} COMMAND radios_test_desktop "$<TARGET_FILE:${_program}>" ${_gui_COMMAND})
    else()
        add_test(NAME ${_gui_NAME} COMMAND ${_program} ${_gui_COMMAND})
    endif()
    set_tests_properties(${_gui_NAME} PROPERTIES LABELS gui)
endfunction()

# radios_add_glib_cases(<test-prefix> <target> <source> <path>
#                       [RUNNER <tests/runner.py>] [GUI] CASES <case>... [TESTS_VAR <var>])
# Registers <test-prefix><case> = "<target> -p <path>/<case>". GLib runs ZERO tests and
# exits 0 for an unknown -p path, so every "<path>/<case>" must appear literally in
# <source> (relative to the source dir), else the configure fails.
function(radios_add_glib_cases prefix target source path)
    cmake_parse_arguments(PARSE_ARGV 4 _glib "GUI" "RUNNER;TESTS_VAR" "CASES")
    if(NOT _glib_CASES)
        message(FATAL_ERROR "radios_add_glib_cases(${prefix}): CASES boş.")
    endif()
    file(READ "${CMAKE_SOURCE_DIR}/${source}" _text)
    set(_names "")
    foreach(_case IN LISTS _glib_CASES)
        string(FIND "${_text}" "\"${path}/${_case}\"" _found)
        if(_found EQUAL -1)
            message(FATAL_ERROR "${source}: \"${path}/${_case}\" GLib testi yok (eski/yanlış vaka adı 0 test "
                                "koşturup başarılı görünür).")
        endif()
        set(_name "${prefix}${_case}")
        if(_glib_RUNNER)
            add_test(NAME ${_name} COMMAND ${RADIOS_TEST_PYTHON_COMMAND} "${CMAKE_SOURCE_DIR}/${_glib_RUNNER}"
                     "$<TARGET_FILE:${target}>" -p "${path}/${_case}")
            if(_glib_GUI)
                set_tests_properties(${_name} PROPERTIES LABELS gui)
            endif()
        elseif(_glib_GUI)
            radios_add_gui_test(NAME ${_name} COMMAND ${target} -p "${path}/${_case}")
        else()
            add_test(NAME ${_name} COMMAND ${target} -p "${path}/${_case}")
        endif()
        set_tests_properties(${_name} PROPERTIES SKIP_REGULAR_EXPRESSION "${RADIOS_GLIB_SKIP_REGEX}")
        list(APPEND _names ${_name})
    endforeach()
    if(_glib_TESTS_VAR)
        set(${_glib_TESTS_VAR} ${_names} PARENT_SCOPE)
    endif()
endfunction()

# radios_add_glib_program(<name> <target> <source> [TESTS_VAR <var>])
# Registers every GLib case of <source> (relative to the source dir) as its own CTest
# test "<name>:<path without the leading slash and without a first component equal to
# <name>>" = "<target> -p <path>": one process per
# case, so a failing or crashing case cannot hide the result of the others. The cases are
# read from the literal g_test_add*("/path", ...) calls; a call with a computed path
# fails the configure (list such cases with radios_add_glib_cases instead). The source
# is a configure dependency: adding or renaming a case re-runs CMake by itself.
function(radios_add_glib_program name target source)
    cmake_parse_arguments(PARSE_ARGV 3 _program "" "TESTS_VAR" "")
    set(_file "${CMAKE_SOURCE_DIR}/${source}")
    file(READ "${_file}" _text)
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_file}")
    string(REGEX MATCHALL "g_test_add[A-Za-z_]*[ \t\r\n]*\\(" _calls "${_text}")
    string(REGEX MATCHALL "g_test_add[A-Za-z_]*[ \t\r\n]*\\([ \t\r\n]*\"/[^\"]+\"" _literal "${_text}")
    list(LENGTH _calls _call_count)
    list(LENGTH _literal _literal_count)
    if(_call_count EQUAL 0 OR NOT _call_count EQUAL _literal_count)
        message(FATAL_ERROR "${source}: ${_call_count} g_test_add çağrısının ${_literal_count} tanesi sabit "
                            "\"/yol\" ile başlıyor. Hesaplanan vaka yolları radios_add_glib_cases ile tek tek "
                            "kaydedilmelidir.")
    endif()
    set(_names "")
    set(_paths "")
    foreach(_call IN LISTS _literal)
        string(REGEX REPLACE "^[^\"]*\"(/[^\"]+)\"$" "\\1" _path "${_call}")
        string(SUBSTRING "${_path}" 1 -1 _leaf)
        string(FIND "${_leaf}" "${name}/" _position)
        if(_position EQUAL 0)   # "platform" + "/platform/fs/links" -> "platform:fs/links"
            string(LENGTH "${name}/" _length)
            string(SUBSTRING "${_leaf}" ${_length} -1 _leaf)
        endif()
        set(_test "${name}:${_leaf}")
        if(_test IN_LIST _names)
            message(FATAL_ERROR "${source}: GLib vakası \"${_path}\" iki kez kaydedilmiş.")
        endif()
        add_test(NAME "${_test}" COMMAND ${target} -p "${_path}")
        set_tests_properties("${_test}" PROPERTIES SKIP_REGULAR_EXPRESSION "${RADIOS_GLIB_SKIP_REGEX}")
        list(APPEND _names "${_test}")
        list(APPEND _paths "${_path}")
    endforeach()
    # "-p /a/b" also runs "/a/b/c": such a pair would run a case twice.
    foreach(_path IN LISTS _paths)
        foreach(_other IN LISTS _paths)
            string(FIND "${_other}" "${_path}/" _position)
            if(_position EQUAL 0)
                message(FATAL_ERROR "${source}: \"${_path}\" bir üst yol (\"${_other}\" de koşar); vakalar "
                                    "radios_add_glib_cases ile kaydedilmelidir.")
            endif()
        endforeach()
    endforeach()
    if(_program_TESTS_VAR)
        set(${_program_TESTS_VAR} ${_names} PARENT_SCOPE)
    endif()
endfunction()

# Called once from the deferred radios_finalize_targets() (RadioSWindowsExe.cmake).
function(radios_finalize_tests)
    get_property(_tests DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY TESTS)
    if(NOT _tests)
        return()
    endif()
    # Applied before each test's own ENVIRONMENT_MODIFICATION entries; a variable the
    # test sets itself in ENVIRONMENT is left alone.
    set(_global
        "PATH=path_list_prepend:${RADIOS_MSYS2_PREFIX}/bin"       # GTK/GLib/FFmpeg DLLs, gspawn helpers, ffmpeg.exe
        "PYTHONUTF8=set:1" "PYTHONIOENCODING=set:utf-8" "PYTHONNOUSERSITE=set:1" "PYTHONDONTWRITEBYTECODE=set:1"
        "PYTHONHOME=unset:" "PYTHONPATH=unset:" "PYTHONSTARTUP=unset:"
        "TZDIR=set:${RADIOS_ZONEINFO_DIR}"
        "GDK_BACKEND=unset:" "GTK_THEME=unset:" "GTK_CSD=unset:" "PANGOCAIRO_BACKEND=unset:"
        "RADIOS_SDBASE_TOKEN=unset:" "RADIOS_AUDIO_SERVER=unset:")
    if(RADIOS_FFMPEG_PREFIX)
        list(APPEND _global "PATH=path_list_prepend:${RADIOS_FFMPEG_PREFIX}/bin")   # applied last: first in PATH
    endif()
    if(TARGET radios_test_desktop)
        list(APPEND _global "RADIOS_TEST_DESKTOP=set:$<TARGET_FILE:radios_test_desktop>")
    endif()
    foreach(_test IN LISTS _tests)
        get_test_property(${_test} ENVIRONMENT _environment)
        get_test_property(${_test} ENVIRONMENT_MODIFICATION _modifications)
        get_test_property(${_test} LABELS _labels)
        if(NOT _environment)
            set(_environment "")
        endif()
        if(NOT _modifications)
            set(_modifications "")
        endif()
        set(_mine "")
        foreach(_entry IN LISTS _global)
            string(REGEX MATCH "^[^=]+" _variable "${_entry}")
            if(NOT _variable STREQUAL "PATH" AND _environment MATCHES "(^|;)${_variable}=")
                continue()
            endif()
            list(APPEND _mine "${_entry}")
        endforeach()
        # DECISIONS D8: no test reaches a real audio device unless it says so: the
        # default backend is "none"; a test (or its runner) that selects one, and
        # the hardware-labelled tests, keep their choice.
        if(NOT "hardware" IN_LIST _labels AND NOT _environment MATCHES "(^|;)RADIOS_AUDIO_BACKEND="
           AND NOT _modifications MATCHES "(^|;)RADIOS_AUDIO_BACKEND=")
            list(APPEND _mine "RADIOS_AUDIO_BACKEND=set:none")
        endif()
        list(APPEND _mine ${_modifications})
        set_tests_properties(${_test} PROPERTIES ENVIRONMENT_MODIFICATION "${_mine}")
        # GLib: "-p /renamed/case" prints the TAP plan "1..0" and exits 0 -> a failure here.
        set_property(TEST ${_test} APPEND PROPERTY FAIL_REGULAR_EXPRESSION "(^|[\r\n])1\\.\\.0([\r\n]|$)")
        get_test_property(${_test} TIMEOUT _timeout)
        if(_timeout)
            math(EXPR _scaled "${_timeout} * ${RADIOS_TEST_TIMEOUT_SCALE}")
            set_tests_properties(${_test} PROPERTIES TIMEOUT ${_scaled})
        endif()
    endforeach()
endfunction()
