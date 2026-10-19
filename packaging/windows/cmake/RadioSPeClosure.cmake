# RadioS - PE (Windows) DLL closure helper shared by the install-time staging script
# (stage-runtime-deps.cmake) and the package verification (verify-stage.cmake).
#
# Host-neutral: it runs inside "cmake --install" / "cmake -P" on the Windows build PC
# (MSYS2 UCRT64) and in any cross environment, because it only uses
# file(GET_RUNTIME_DEPENDENCIES) with the objdump the C toolchain provides (CMAKE_OBJDUMP).
#
# radios_pe_closure(<resolved-var> <unresolved-var>
#     OBJDUMP <objdump executable>
#     [EXECUTABLES <files>...] [MODULES <files>...] [LIBRARIES <files>...]
#     DIRECTORIES <dirs>...            # search dirs, in priority order
#     SYSTEM_DLLS <names>...)          # Windows component DLL names that are never bundled
#
# <resolved-var>   : absolute paths of every non-system DLL the inputs need (recursively).
# <unresolved-var> : non-system DLL names that could not be found anywhere.
# Conflicts (one DLL name found in two directories) are a FATAL_ERROR: mixing two copies
# of a DLL (e.g. MSYS2 FFmpeg next to the LGPL FFmpeg build) is never acceptable.
#
# Windows resolves DLL names case-insensitively. file(GET_RUNTIME_DEPENDENCIES) looks a
# name up with the file system's rules, so on a case-sensitive file system an import such
# as "libLerc.dll" would stay unresolved although the file exists. Such names are matched
# case-insensitively against DIRECTORIES and fed back as LIBRARIES until a fixpoint is
# reached; on Windows this loop never has work to do.

if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)   # forward-slash paths before regex filters (CMake >= 4.3)
endif()

function(radios_system_dll_regex out_var)
    set(alternatives "")
    foreach(name IN LISTS ARGN)
        string(TOLOWER "${name}" name)
        if(NOT name MATCHES "^[a-z0-9_.-]+$")
            message(FATAL_ERROR "Geçersiz sistem DLL adı / invalid system DLL name: '${name}'")
        endif()
        string(REPLACE "." "\\." escaped "${name}")
        list(APPEND alternatives "${escaped}")
    endforeach()
    list(JOIN alternatives "|" joined)
    set(${out_var} "^(${joined})$" PARENT_SCOPE)
endfunction()

function(radios_pe_closure out_resolved out_unresolved)
    cmake_parse_arguments(PARSE_ARGV 2 A "" "OBJDUMP" "EXECUTABLES;MODULES;LIBRARIES;DIRECTORIES;SYSTEM_DLLS")
    if(A_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "radios_pe_closure: unknown arguments: ${A_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT A_OBJDUMP OR NOT EXISTS "${A_OBJDUMP}")
        message(FATAL_ERROR "objdump bulunamadı / objdump not found: '${A_OBJDUMP}' "
                            "(CMAKE_OBJDUMP of the MSYS2 UCRT64 toolchain).")
    endif()
    if(NOT A_SYSTEM_DLLS)
        message(FATAL_ERROR "radios_pe_closure: SYSTEM_DLLS list is empty.")
    endif()
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${A_OBJDUMP}")
    radios_system_dll_regex(system_regex ${A_SYSTEM_DLLS})

    # Case-insensitive index of the search directories (first directory wins).
    set(indexed "")
    foreach(dir IN LISTS A_DIRECTORIES)
        file(GLOB entries LIST_DIRECTORIES false "${dir}/*.dll" "${dir}/*.DLL" "${dir}/*.Dll")
        foreach(entry IN LISTS entries)
            cmake_path(GET entry FILENAME leaf)
            string(TOLOWER "${leaf}" lower)
            if(NOT lower IN_LIST indexed)
                list(APPEND indexed "${lower}")
                set("_radios_ci_${lower}" "${entry}")
            endif()
        endforeach()
    endforeach()

    set(extra_libraries "")
    set(iteration 0)
    while(TRUE)
        math(EXPR iteration "${iteration} + 1")
        if(iteration GREATER 32)
            message(FATAL_ERROR "radios_pe_closure: no fixpoint after 32 iterations.")
        endif()
        set(args "")
        if(A_EXECUTABLES)
            list(APPEND args EXECUTABLES ${A_EXECUTABLES})
        endif()
        if(A_MODULES)
            list(APPEND args MODULES ${A_MODULES})
        endif()
        if(A_LIBRARIES OR extra_libraries)
            list(APPEND args LIBRARIES ${A_LIBRARIES} ${extra_libraries})
        endif()
        file(GET_RUNTIME_DEPENDENCIES
            ${args}
            DIRECTORIES ${A_DIRECTORIES}
            PRE_EXCLUDE_REGEXES "${system_regex}" "^api-ms-win-.*" "^ext-ms-.*"
            RESOLVED_DEPENDENCIES_VAR resolved
            UNRESOLVED_DEPENDENCIES_VAR unresolved
            CONFLICTING_DEPENDENCIES_PREFIX conflict)
        if(conflict_FILENAMES)
            set(details "")
            foreach(name IN LISTS conflict_FILENAMES)
                string(APPEND details "\n  ${name}: ${conflict_${name}}")
            endforeach()
            message(FATAL_ERROR "Aynı DLL birden fazla dizinde bulundu / DLL found in more than one "
                                "directory (mixed runtimes are not allowed):${details}")
        endif()
        set(added FALSE)
        set(still_unresolved "")
        foreach(name IN LISTS unresolved)
            string(TOLOWER "${name}" lower)
            if(DEFINED "_radios_ci_${lower}")
                set(candidate "${_radios_ci_${lower}}")
                if(NOT candidate IN_LIST extra_libraries)
                    list(APPEND extra_libraries "${candidate}")
                    set(added TRUE)
                endif()
            else()
                list(APPEND still_unresolved "${name}")
            endif()
        endforeach()
        if(NOT added)
            break()
        endif()
    endwhile()

    list(APPEND resolved ${extra_libraries})
    list(REMOVE_DUPLICATES resolved)
    list(SORT resolved CASE INSENSITIVE)
    list(REMOVE_DUPLICATES still_unresolved)
    set(${out_resolved} "${resolved}" PARENT_SCOPE)
    set(${out_unresolved} "${still_unresolved}" PARENT_SCOPE)
endfunction()

# radios_path_under(<out-var> <path> <dir>...): TRUE when <path> lies inside one of <dir>
# (compared case-insensitively, as Windows does).
function(radios_path_under out_var path)
    cmake_path(NORMAL_PATH path)
    string(TOLOWER "${path}" lower_path)
    foreach(dir IN LISTS ARGN)
        cmake_path(NORMAL_PATH dir)
        string(TOLOWER "${dir}" lower_dir)
        string(REGEX REPLACE "/+$" "" lower_dir "${lower_dir}")
        string(LENGTH "${lower_dir}/" n)
        string(SUBSTRING "${lower_path}" 0 ${n} head)
        if(head STREQUAL "${lower_dir}/")
            set(${out_var} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} FALSE PARENT_SCOPE)
endfunction()

# radios_run_windows_tool(<description> <tool.exe> <args>...)
# Runs a Windows program of the build host (MSYS2 tool or a staged exe) through
# RADIOS_EMULATOR (CMAKE_CROSSCOMPILING_EMULATOR; empty on the Windows build PC) and fails
# with its full output when it does not exit with 0.
function(radios_run_windows_tool description tool)
    if(NOT EXISTS "${tool}")
        message(FATAL_ERROR "${description}: program bulunamadı / program not found: ${tool}")
    endif()
    execute_process(
        COMMAND ${RADIOS_EMULATOR} "${tool}" ${ARGN}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${description} başarısız / failed (exit ${rc}):\n"
                            "  ${tool} ${ARGN}\n${out}${err}")
    endif()
endfunction()
