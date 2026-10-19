# RadioS: Windows 10/11 x64 build (MSYS2 UCRT64 GCC + Ninja). Use the presets of
# CMakePresets.json: cmake --preset windows-debug; cmake --build --preset windows-debug;
# ctest --preset windows-debug. cmake/README.md describes the modules.
cmake_minimum_required(VERSION 3.25...4.4)
project(RadioS VERSION 0.19.0 LANGUAGES C RC)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(RadioSToolchain)    # toolchain guard, MSYS2 prefix, pkg-config, global flags, build Python
include(RadioSWindowsExe)   # manifest/icon/VERSIONINFO/binary _fmode/8 MiB stack for every program

# ---- Dependencies ------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.66 gio-2.0>=2.66)
pkg_check_modules(GIO_WIN32 REQUIRED IMPORTED_TARGET gio-windows-2.0>=2.66)  # GWin32Input/OutputStream
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.24)
pkg_check_modules(PANGO REQUIRED IMPORTED_TARGET pango>=1.56)               # pango_font_map_add_font_file (D5)
# Fontconfig: src/ui/window.c, tests/test_media_drop_ui.c and tests/test_automation_calendar.c
# still call FcInit/FcConfigAppFontAddFile (each target links it itself). DirectWrite never
# sees fonts added that way (DECISIONS D5): delete this check and the three links with them.
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
set(_radios_fontconfig_users "")
foreach(_radios_file IN ITEMS src/ui/window.c tests/test_media_drop_ui.c tests/test_automation_calendar.c)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_radios_file}")
        file(STRINGS "${CMAKE_SOURCE_DIR}/${_radios_file}" _radios_fc REGEX "fontconfig/fontconfig\\.h")
        if(_radios_fc)
            list(APPEND _radios_fontconfig_users "${_radios_file}")
        endif()
    endif()
endforeach()
if(NOT _radios_fontconfig_users)
    message(WARNING "Hiçbir kaynak fontconfig kullanmıyor (DECISIONS D5): FONTCONFIG denetimini ve "
                    "radios_ui/test_media_drop_ui/test_automation_calendar bağlantılarını silin.")
endif()
pkg_check_modules(JSONGLIB REQUIRED IMPORTED_TARGET json-glib-1.0>=1.10)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl>=7.85)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat>=59.27.100)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec>=59.37.100)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil>=57.28.100)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample>=4.7.100)
pkg_check_modules(AVFILTER REQUIRED IMPORTED_TARGET libavfilter>=8.44.100)
foreach(_radios_package IN ITEMS GLIB GIO_WIN32 GTK3 PANGO FONTCONFIG JSONGLIB CURL)
    radios_require_package_root(${_radios_package} "${RADIOS_MSYS2_PREFIX}")
endforeach()
if(RADIOS_FFMPEG_PREFIX)
    set(_radios_ffmpeg_root "${RADIOS_FFMPEG_PREFIX}")
else()
    set(_radios_ffmpeg_root "${RADIOS_MSYS2_PREFIX}")
endif()
foreach(_radios_package IN ITEMS AVFORMAT AVCODEC AVUTIL SWRESAMPLE AVFILTER)
    radios_require_package_root(${_radios_package} "${_radios_ffmpeg_root}")
endforeach()
message(STATUS "FFmpeg: ${_radios_ffmpeg_root} (libavcodec ${AVCODEC_VERSION})")

# ---- Design CSS embedded into radios_ui ----------------------------------------------------
file(GLOB DESIGN_CSS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/design/reference/tasarrım/*.css")
set(REFERENCE_CSS_C "${CMAKE_BINARY_DIR}/generated/reference-css.c")
add_custom_command(OUTPUT "${REFERENCE_CSS_C}"
    COMMAND ${RADIOS_BUILD_PYTHON_COMMAND} "${CMAKE_SOURCE_DIR}/tools/css_to_gtk.py"
            --output-dir "${CMAKE_BINARY_DIR}/generated"
    MAIN_DEPENDENCY "${CMAKE_SOURCE_DIR}/design/reference/tasarrım/index.html"
    DEPENDS ${DESIGN_CSS} "${CMAKE_SOURCE_DIR}/tools/css_to_gtk.py"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/reference.css"
               "${CMAKE_BINARY_DIR}/generated/css-manifest.json"
    COMMENT "Tasarım CSS'i GTK CSS'ine aktarılıyor (tools/css_to_gtk.py)"
    VERBATIM)
# Its own object library: a custom command of radios_ui would wait for every library
# radios_ui links, and every UI object would wait for it.
add_library(radios_reference_css OBJECT "${REFERENCE_CSS_C}")
target_link_libraries(radios_reference_css PRIVATE PkgConfig::GLIB radios_warnings)

# ---- Libraries (static: tests link them and wrap radios_* functions with --wrap) ---------
# Windows platform layer (DECISIONS D3): files, locks, ACLs, processes, time zones, crash dumps.
add_library(radios_platform STATIC src/core/platform.c src/core/platform_win32.c src/core/platform_tz.c)
target_include_directories(radios_platform PUBLIC include)
target_link_libraries(radios_platform PUBLIC PkgConfig::GLIB PkgConfig::GIO_WIN32
    PRIVATE advapi32 ntdll shell32 ole32 user32 radios_warnings)
# MIDI controllers through WinMM, hotplug through the configuration manager.
add_library(radios_midi STATIC src/core/midi.c)
target_include_directories(radios_midi PUBLIC include)
target_link_libraries(radios_midi PUBLIC PkgConfig::GLIB PRIVATE winmm cfgmgr32 radios_warnings)

add_library(radios_core STATIC src/core/config.c src/core/runtime.c src/core/diagnostics.c
    src/core/ui_preferences.c src/core/data_client.c src/core/recovery.c src/core/external_access.c
    src/core/programme_model.c src/core/automation_calendar_plan.c src/media/analyzer.c src/media/rhythm.c
    src/media/loudness.c)
target_include_directories(radios_core PUBLIC include)
target_link_libraries(radios_core PUBLIC radios_platform radios_midi PkgConfig::GLIB PkgConfig::JSONGLIB
    PRIVATE PkgConfig::CURL PkgConfig::AVFORMAT PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::SWRESAMPLE
    radios_warnings)
target_sources(radios_core PRIVATE src/core/weekly_programme_plan.c src/core/player_topology.c
    src/core/loudness_policy.c src/core/automation_scope.c src/core/transition_preset.c
    src/core/storage_locations.c src/core/local_ini.c src/core/media_archive.c src/core/library_analysis.c
    src/core/routing_graph.c src/playout/copyright_policy.c src/playout/automation_policy.c)

# Audio engine; WASAPI device layer (exactly one TU, wasapi_device.c, instantiates the GUIDs).
add_library(radios_audio STATIC src/audio/decoder.c src/audio/tempo.c src/audio/engine.c
    src/audio/wasapi_backend.c src/audio/wasapi_device.c src/audio/dsp.c src/audio/transition.c
    src/audio/output.c src/audio/patchbay.c src/audio/channel_input.c)
target_include_directories(radios_audio PUBLIC include)
target_link_libraries(radios_audio PUBLIC radios_core PRIVATE PkgConfig::CURL PkgConfig::AVFORMAT
    PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::SWRESAMPLE PkgConfig::AVFILTER
    ole32 uuid avrt winmm mmdevapi propsys ksuser version shlwapi advapi32 radios_warnings)
# RADIOS_AUDIO_BACKEND=virtual|none (DECISIONS D8, tests-c.md 7.2-7.3) when the audio area keeps
# it in its own translation unit. Any other new src/ file fails the configure (coverage guard).
radios_optional_source(_radios_virtual_backend src/audio/virtual_backend.c)
if(_radios_virtual_backend)
    target_sources(radios_audio PRIVATE "${_radios_virtual_backend}")
endif()

add_library(radios_playout STATIC src/playout/planner.c)
target_include_directories(radios_playout PUBLIC include)
target_link_libraries(radios_playout PUBLIC radios_audio PRIVATE radios_warnings)

add_library(radios_ui STATIC src/ui/window.c src/ui/widgets.c src/ui/editors.c src/ui/data.c
    src/ui/header.c src/ui/instruments.c src/ui/broadcast.c src/ui/programme_queue.c src/ui/player_loader.c
    src/ui/programme_lifecycle.c src/ui/programme_prerequisites.c src/ui/playlist.c
    src/ui/automation.c src/ui/automation_calendar.c src/ui/rotation_editor.c src/ui/dj.c src/ui/emergency.c
    src/ui/advert.c src/ui/technical.c src/ui/external_access_ui.c src/ui/media.c src/ui/media_jobs.c
    src/ui/rhythm_jobs.c src/ui/player_waveform.c src/ui/audio_ui.c src/ui/playout_ui.c src/ui/routing.c
    src/ui/advert_editors.c src/ui/advert_ui.c src/ui/broadcast_ui.c src/ui/broadcast_output_ui.c
    src/ui/admin_ui.c $<TARGET_OBJECTS:radios_reference_css>)
target_sources(radios_ui PRIVATE src/ui/catalogue_playlist.c src/ui/schema_settings_ui.c
    src/ui/stream_metadata.c src/ui/local_event_ui.c src/ui/local_event_prepare.c src/ui/autodj_settings.c
    src/ui/event_playout.c src/ui/autodj_marker_jobs.c src/ui/playlist_browser.c src/ui/player_rotation.c
    src/ui/weekly_programme_ui.c src/ui/automation_scope.c src/ui/player_workspace.c src/ui/player_tree_ui.c
    src/ui/automation_workspace_ui.c src/ui/player_scope_execution.c src/ui/media_routing.c src/ui/midi_ui.c
    src/ui/reset_ui.c src/ui/settings_sync.c src/ui/copyright_settings.c src/ui/probability_editor.c
    src/ui/autodj_automation_ui.c src/ui/transition_distribution.c src/ui/workflow_hub.c
    src/ui/media_archive_ui.c src/ui/library_analysis_ui.c src/ui/content_scale.c src/ui/live_control.c
    # Kullanıcı isteği U2: player card "Otomasyon ▾ › Şemadan yükle…".
    src/ui/schema_track_picker.c
    src/ui/transfer_ui.c
    # Kılavuz 18.10 R10.1: Ayarlar › Import › rekordbox import profiles.
    src/ui/import_profiles_ui.c
    src/ui/routing_graph_ui.c src/ui/view_presets_ui.c)
target_compile_definitions(radios_ui PRIVATE RADIOS_VERSION="${PROJECT_VERSION}")
target_link_libraries(radios_ui PUBLIC radios_core radios_audio radios_playout PkgConfig::GTK3 PkgConfig::PANGO
    PRIVATE PkgConfig::FONTCONFIG radios_warnings)

# ---- The application ---------------------------------------------------------------------
add_executable(radios src/app/main.c)
set_target_properties(radios PROPERTIES OUTPUT_NAME RadioS WIN32_EXECUTABLE ON)   # RadioS.exe, -mwindows
target_link_libraries(radios PRIVATE radios_ui radios_warnings)
target_compile_definitions(radios PRIVATE RADIOS_VERSION="${PROJECT_VERSION}")
# CodeView PDB next to RadioS.exe for the crash minidumps (DECISIONS D11); not shipped by default.
target_link_options(radios PRIVATE "$<$<CONFIG:Release,RelWithDebInfo>:LINKER:--pdb=RadioS.pdb>")

# ---- Developer tools built from the static libraries ---------------------------------------
# tools/otomasyon_testleri/e2e.c: the schedule driver of the automation scenarios (and of the
# automation_recovery_stress test); tools/yayin_akisi_724/analiz.c: media import helper.
add_executable(otomasyon_e2e tools/otomasyon_testleri/e2e.c)
target_include_directories(otomasyon_e2e PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(otomasyon_e2e PRIVATE radios_playout radios_warnings)
set_target_properties(otomasyon_e2e PROPERTIES OUTPUT_NAME e2e
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/otomasyon_testleri")
add_executable(yayin_akisi_analiz EXCLUDE_FROM_ALL tools/yayin_akisi_724/analiz.c)
target_link_libraries(yayin_akisi_analiz PRIVATE radios_core radios_warnings)
set_target_properties(yayin_akisi_analiz PROPERTIES OUTPUT_NAME analiz
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/yayin_akisi_724")

# Private-desktop launcher for GUI tests (DECISIONS D9, tests-c.md 6.3).
radios_optional_source(RADIOS_TEST_DESKTOP_SOURCE tools/win32/radios-test-desktop.c)
if(RADIOS_TEST_DESKTOP_SOURCE)
    add_executable(radios_test_desktop "${RADIOS_TEST_DESKTOP_SOURCE}")
    set_target_properties(radios_test_desktop PROPERTIES OUTPUT_NAME radios-test-desktop)
    target_link_libraries(radios_test_desktop PRIVATE user32 radios_warnings)
endif()

include(CTest)
include(RadioSTesting)      # test Python, global test environment, GLib case checks, GUI desktop
if(BUILD_TESTING)
    radios_add_executable_selfcheck()   # exe_manifest, exe_pe_selfcheck, exe_pe_radios
    # Windows platform layer (DECISIONS D3): paths, files, locks, ACLs, processes, time zones.
    add_executable(test_platform tests/test_platform.c)
    target_link_libraries(test_platform PRIVATE radios_platform advapi32 shell32 radios_warnings)
    target_compile_definitions(test_platform PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    # One process per case (31 cases): a failing case cannot hide the others.
    radios_add_glib_program(platform test_platform tests/test_platform.c TESTS_VAR _radios_platform_tests)
    set_tests_properties(${_radios_platform_tests} PROPERTIES TIMEOUT 120 ENVIRONMENT "G_DEBUG=fatal-warnings")
    add_executable(test_automation_scope tests/test_automation_scope.c)
    target_link_libraries(test_automation_scope PRIVATE radios_core radios_warnings)
    add_test(NAME automation_scope COMMAND test_automation_scope)
    set_tests_properties(automation_scope PROPERTIES TIMEOUT 20 ENVIRONMENT "G_DEBUG=fatal-warnings")
    add_executable(test_live_control tests/test_live_control.c)
    target_link_libraries(test_live_control PRIVATE radios_ui radios_warnings)
    add_test(NAME live_control COMMAND test_live_control)
    set_tests_properties(live_control PROPERTIES TIMEOUT 20 ENVIRONMENT "G_DEBUG=fatal-warnings")
    add_executable(test_programme_window tests/test_programme_window.c)
    target_link_libraries(test_programme_window PRIVATE radios_ui radios_warnings)
    add_test(NAME programme_window COMMAND test_programme_window)
    set_tests_properties(programme_window PROPERTIES TIMEOUT 20 ENVIRONMENT "G_DEBUG=fatal-warnings")
    if(RADIOS_GUI_TESTS)
        # Real pointer/keyboard input on the test's desktop (tests-c.md 6.2: posted window
        # messages; SendInput only with RADIOS_TEST_INPUT=sendinput, hence the lock).
        add_executable(test_content_scale_module tests/test_content_scale_module.c src/ui/content_scale.c)
        target_include_directories(test_content_scale_module PRIVATE include)
        target_link_libraries(test_content_scale_module PRIVATE PkgConfig::GTK3 user32 radios_warnings)
        add_test(NAME native_content_scale_module COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_content_scale_module>)
        set_tests_properties(native_content_scale_module PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
            LABELS gui RESOURCE_LOCK native_input)
        add_executable(test_content_scale_popups tests/test_content_scale_popups.c src/ui/content_scale.c)
        target_include_directories(test_content_scale_popups PRIVATE include)
        target_link_libraries(test_content_scale_popups PRIVATE PkgConfig::GTK3 user32 radios_warnings)
        foreach(popup_case context-menus-at-pointer-and-widget context-menus-before-map combo-lists-and-menu-button
                popover-and-tooltip tree-search-entry)
            add_test(NAME native_content_scale_popup_${popup_case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_content_scale_popups>
                -p /content-scale-module/popup-${popup_case})
            set_tests_properties(native_content_scale_popup_${popup_case} PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
                LABELS gui RESOURCE_LOCK native_input)
        endforeach()
        add_executable(test_view_presets_ui tests/test_view_presets_ui.c src/ui/view_presets_ui.c src/ui/settings_sync.c)
        target_link_libraries(test_view_presets_ui PRIVATE radios_core PkgConfig::GTK3 radios_warnings)
        add_test(NAME view_presets_ui COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_view_presets_ui>)
        set_tests_properties(view_presets_ui PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
    endif()
    if(RADIOS_GUI_TESTS)
        add_executable(test_player_tree_ui tests/test_player_tree_ui.c)
        target_link_libraries(test_player_tree_ui PRIVATE radios_ui radios_warnings)
        radios_wrap(test_player_tree_ui radios_player_workspace_ready radios_player_workspace_topology
            radios_player_workspace_replace radios_player_workspace_reload)
        add_test(NAME native_player_tree COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_player_tree_ui>)
        set_tests_properties(native_player_tree PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
            ENVIRONMENT "G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple")
    endif()
    add_executable(test_player_topology tests/test_player_topology.c)
    target_link_libraries(test_player_topology PRIVATE radios_core radios_warnings)
    add_test(NAME player_topology COMMAND test_player_topology)
    set_tests_properties(player_topology PROPERTIES TIMEOUT 20 ENVIRONMENT "G_DEBUG=fatal-warnings")
    add_test(NAME radios_launch COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_radios_launch.py)
    set_tests_properties(radios_launch PROPERTIES TIMEOUT 30 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME desktop_player_topology COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_desktop_player_topology.py)
    set_tests_properties(desktop_player_topology PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME stream_metadata_schema COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_stream_metadata_schema.py)
    set_tests_properties(stream_metadata_schema PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_executable(test_routing_graph tests/test_routing_graph.c)
    target_link_libraries(test_routing_graph PRIVATE radios_core radios_warnings)
    add_test(NAME routing_graph COMMAND test_routing_graph)
    add_test(NAME routing_graph_schema COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_routing_graph_schema.py)
    set_tests_properties(routing_graph_schema PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} TIMEOUT 30)
    add_executable(test_rhythm tests/test_rhythm.c)
    target_link_libraries(test_rhythm PRIVATE radios_core radios_warnings)
    add_test(NAME rhythm COMMAND test_rhythm)
    add_executable(test_library_analysis tests/test_library_analysis.c)
    target_link_libraries(test_library_analysis PRIVATE radios_core radios_warnings)
    add_test(NAME library_analysis COMMAND test_library_analysis)
    add_executable(test_rhythm_upkeep tests/test_rhythm_upkeep.c src/ui/autodj_marker_jobs.c)
    target_include_directories(test_rhythm_upkeep PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_rhythm_upkeep PRIVATE radios_core PkgConfig::GTK3 radios_warnings)
    add_test(NAME rhythm_upkeep COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/run_rhythm_upkeep.py $<TARGET_FILE:test_rhythm_upkeep>)
    set_tests_properties(rhythm_upkeep PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} TIMEOUT 120)
    add_executable(test_loudness_policy tests/test_loudness_policy.c)
    target_link_libraries(test_loudness_policy PRIVATE radios_core radios_warnings)
    add_test(NAME loudness_policy COMMAND test_loudness_policy)
    add_executable(test_loudness tests/test_loudness.c)
    target_link_libraries(test_loudness PRIVATE radios_core radios_warnings)
    add_test(NAME loudness COMMAND test_loudness)
    add_executable(test_loudness_engine tests/test_loudness_engine.c)
    target_link_libraries(test_loudness_engine PRIVATE radios_playout radios_warnings)
    target_compile_definitions(test_loudness_engine PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    add_test(NAME loudness_engine COMMAND test_loudness_engine)
    set_tests_properties(loudness_engine PROPERTIES TIMEOUT 120)
    # MIDI through WinMM: keys/decoding are pure, listing reads the system list only;
    # /midi/virtual-controller needs a loopback port (RADIOS_TEST_MIDI_LOOPBACK_OUT).
    add_executable(test_midi tests/test_midi.c)
    target_link_libraries(test_midi PRIVATE radios_midi winmm radios_warnings)
    add_test(NAME midi COMMAND test_midi)
    set_tests_properties(midi PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 ENVIRONMENT "G_DEBUG=fatal-warnings")
    # R8.4: src/core/midi.c is compiled into the test against an in-process fake WinMM and
    # configuration manager (WinMM input is exclusive: no real controller is ever listed or
    # opened) and drives the window's MIDI service. input-* need no display.
    add_executable(test_midi_hotplug tests/test_midi_hotplug.c)
    target_link_libraries(test_midi_hotplug PRIVATE radios_ui radios_warnings)
    radios_add_glib_cases(native_midi_hotplug_ test_midi_hotplug tests/test_midi_hotplug.c /midi-hotplug
        CASES input-announcements input-liveness-without-announcements input-liveness-from-list
              input-driver-close input-sysex-realtime input-index-shift input-duplicate-names
              input-polling-port input-list-lag input-delivery input-close-during-slow-listing input-open-errors
        TESTS_VAR _radios_midi_tests)
    set_tests_properties(${_radios_midi_tests} PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
        ENVIRONMENT "G_DEBUG=fatal-warnings")
    # The fake must be complete: the test program imports no WinMM input or CM function.
    add_test(NAME native_midi_hotplug_imports_no_device_api
             COMMAND "${CMAKE_COMMAND}" "-DOBJDUMP=${CMAKE_OBJDUMP}" "-DEXE=$<TARGET_FILE:test_midi_hotplug>"
                     "-DFORBIDDEN=midiIn;CM_Register_Notification;CM_Unregister_Notification"
                     -P "${CMAKE_SOURCE_DIR}/cmake/RadioSCheckImports.cmake")
    set_tests_properties(native_midi_hotplug_imports_no_device_api PROPERTIES TIMEOUT 30)
    if(RADIOS_GUI_TESTS)
        radios_add_glib_cases(native_midi_hotplug_ test_midi_hotplug tests/test_midi_hotplug.c /midi-hotplug GUI
            CASES ui-unplug-replug-refresh ui-replug-same-port ui-slow-enumeration ui-teardown-pending-job
                  ui-busy-status ui-learn-and-map
            TESTS_VAR _radios_midi_tests)
        set_tests_properties(${_radios_midi_tests} PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
            ENVIRONMENT "G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple")
    endif()
    add_executable(test_external_access tests/test_external_access.c)
    target_link_libraries(test_external_access PRIVATE radios_core radios_warnings)
    add_test(NAME external_access COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_external_access.py $<TARGET_FILE:test_external_access>)
    set_tests_properties(external_access PROPERTIES TIMEOUT 45
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
    foreach(component config runtime ui_preferences data_client recovery media programme_model)
        add_executable(test_${component} tests/test_${component}.c)
        target_link_libraries(test_${component} PRIVATE radios_core radios_warnings)
        add_test(NAME ${component} COMMAND test_${component})
        set_tests_properties(${component} PROPERTIES TIMEOUT 20)
    endforeach()
    target_compile_definitions(test_data_client PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    set_tests_properties(data_client PROPERTIES TIMEOUT 45)
    target_compile_definitions(test_media PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    set_tests_properties(media PROPERTIES TIMEOUT 60)
    foreach(component audio audio_decoder tempo dsp effect_return transition runner output channel_input)
        add_executable(test_${component} tests/test_${component}.c)
        target_link_libraries(test_${component} PRIVATE radios_audio radios_warnings)
        target_compile_definitions(test_${component} PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
        add_test(NAME ${component} COMMAND test_${component})
        set_tests_properties(${component} PROPERTIES TIMEOUT 60)
    endforeach()
    add_executable(test_playout tests/test_playout.c)
    target_link_libraries(test_playout PRIVATE radios_playout radios_warnings)
    target_compile_definitions(test_playout PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    add_test(NAME playout COMMAND test_playout)
    set_tests_properties(playout PROPERTIES TIMEOUT 30)
    # Device/service loss and recovery on the virtual backend's control API. The Windows twin
    # tests/test_wasapi_recovery.c (tests-c.md 10.4) replaces the Pulse-daemon test when present.
    radios_optional_source(_radios_recovery_twin tests/test_wasapi_recovery.c)
    if(_radios_recovery_twin)
        set(_radios_recovery_target test_wasapi_recovery)
    else()
        set(_radios_recovery_target test_audio_recovery)
    endif()
    add_executable(${_radios_recovery_target} tests/${_radios_recovery_target}.c)
    target_link_libraries(${_radios_recovery_target} PRIVATE radios_audio radios_warnings)
    target_compile_definitions(${_radios_recovery_target} PRIVATE
        RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    add_test(NAME audio_recovery COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
             ${CMAKE_SOURCE_DIR}/tests/run_audio_recovery.py $<TARGET_FILE:${_radios_recovery_target}>)
    set_tests_properties(audio_recovery PROPERTIES TIMEOUT 100 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # Windows audio device layer (WASAPI). Without hardware: COM objects, the device watch,
    # atomic list publishing, hotplug without "Yenile", service loss and reconnect (fake
    # provider). The "hardware" variant (operator's PC only; excluded by the test presets)
    # also plays a -60 dBFS tone through loopback, opens the microphone and runs the engine.
    add_executable(test_wasapi_backend tests/test_wasapi_backend.c)
    target_link_libraries(test_wasapi_backend PRIVATE radios_audio radios_warnings)
    radios_add_glib_program(wasapi_backend test_wasapi_backend tests/test_wasapi_backend.c
        TESTS_VAR _radios_wasapi_tests)
    set_tests_properties(${_radios_wasapi_tests} PROPERTIES TIMEOUT 120 ENVIRONMENT "G_DEBUG=fatal-warnings")
    add_test(NAME wasapi_backend_hardware COMMAND test_wasapi_backend)
    set_tests_properties(wasapi_backend_hardware PROPERTIES TIMEOUT 180 LABELS hardware RUN_SERIAL TRUE
        ENVIRONMENT "G_DEBUG=fatal-warnings;RADIOS_TEST_WASAPI=1")
    # Device hotplug against a fake IMMDeviceEnumerator (tests-c.md 7.3).
    radios_optional_source(_radios_wasapi_hotplug tests/test_wasapi_hotplug.c)
    if(_radios_wasapi_hotplug)
        add_executable(test_wasapi_hotplug tests/test_wasapi_hotplug.c)
        target_link_libraries(test_wasapi_hotplug PRIVATE radios_audio radios_warnings)
        add_test(NAME wasapi_hotplug COMMAND test_wasapi_hotplug)
        set_tests_properties(wasapi_hotplug PROPERTIES TIMEOUT 60 ENVIRONMENT "G_DEBUG=fatal-warnings")
    endif()
    add_executable(test_audio_soak tests/test_audio_soak.c)
    target_link_libraries(test_audio_soak PRIVATE radios_audio psapi radios_warnings)
    add_test(NAME audio_soak COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
             ${CMAKE_SOURCE_DIR}/tests/run_audio_soak.py $<TARGET_FILE:test_audio_soak>)
    set_tests_properties(audio_soak PROPERTIES TIMEOUT 70 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_executable(test_patchbay tests/test_patchbay.c)
    target_link_libraries(test_patchbay PRIVATE radios_audio radios_warnings)
    target_compile_definitions(test_patchbay PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    add_test(NAME patchbay COMMAND test_patchbay)
    set_tests_properties(patchbay PROPERTIES TIMEOUT 120)
    add_test(NAME config_from_other_directory COMMAND radios --check-config
             --project-root ${CMAKE_SOURCE_DIR})
    set_tests_properties(config_from_other_directory PROPERTIES WORKING_DIRECTORY ${RADIOS_TEST_FOREIGN_CWD} TIMEOUT 10)
    add_test(NAME database_setup COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
             ${CMAKE_SOURCE_DIR}/tests/test_database_setup.py)
    set_tests_properties(database_setup PROPERTIES TIMEOUT 120
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME database_recovery COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
             ${CMAKE_SOURCE_DIR}/tests/test_database_recovery.py)
    set_tests_properties(database_recovery PROPERTIES TIMEOUT 30
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    foreach(component playlist_selection playlist_workspace desktop_api desktop_routing desktop_reset desktop_programme
            desktop_local_event desktop_rekordbox desktop_rekordbox_device data_playlists desktop_media desktop_playout
            desktop_advert desktop_broadcast desktop_admin reliability packaging release_install sdbase_platform)
        add_test(NAME ${component} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/test_${component}.py)
        set_tests_properties(${component} PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endforeach()
    if(RADIOS_GUI_TESTS)
        add_executable(test_player_workspace_ui tests/test_player_workspace_ui.c)
        target_link_libraries(test_player_workspace_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_player_workspace COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py $<TARGET_FILE:test_player_workspace_ui>)
        set_tests_properties(native_player_workspace PROPERTIES TIMEOUT 180
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_automation_workspace_ui tests/test_automation_workspace_ui.c)
        target_link_libraries(test_automation_workspace_ui PRIVATE radios_ui radios_warnings)
        radios_wrap(test_automation_workspace_ui radios_player_workspace_ready radios_player_workspace_topology
            radios_player_workspace_replace radios_player_workspace_reload radios_player_workspace_assign
            radios_player_workspace_get_programme radios_player_workspace_projection radios_data_connected
            radios_data_client_acquire radios_data_body radios_data_request radios_automation_scope_fetch
            radios_player_scope_execution_get radios_player_scope_execution_snapshot
            radios_player_scope_execution_play_programme radios_player_scope_execution_play_schema
            radios_player_scope_execution_load_playlist radios_player_scope_execution_action)
        add_test(NAME native_automation_workspace COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_automation_workspace_ui>)
        set_tests_properties(native_automation_workspace PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_automation_workspace_acceptance tests/test_automation_workspace_acceptance.c)
        target_link_libraries(test_automation_workspace_acceptance PRIVATE radios_ui radios_warnings)
        add_test(NAME native_automation_workspace_acceptance COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py $<TARGET_FILE:test_automation_workspace_acceptance>)
        set_tests_properties(native_automation_workspace_acceptance PROPERTIES TIMEOUT 180
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_automation_scope_ui tests/test_automation_scope_ui.c)
        target_link_libraries(test_automation_scope_ui PRIVATE radios_ui radios_warnings)
        radios_wrap(test_automation_scope_ui radios_data_connected radios_data_client_acquire
            radios_data_client_unref radios_data_client_station_id radios_data_request radios_json_int)
        add_test(NAME automation_scope_fetch COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_automation_scope_ui>)
        set_tests_properties(automation_scope_fetch PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings")

        add_executable(test_programme_queue tests/test_programme_queue.c)
        target_link_libraries(test_programme_queue PRIVATE radios_ui radios_warnings)
        radios_wrap(test_programme_queue gtk_drag_finish)
        radios_add_gui_test(NAME native_programme_queue COMMAND test_programme_queue)
        set_tests_properties(native_programme_queue PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_data_connection_ui tests/test_data_connection_ui.c)
        target_link_libraries(test_data_connection_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_data_connection COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_data_connection_ui.py $<TARGET_FILE:test_data_connection_ui>)
        set_tests_properties(native_data_connection PROPERTIES TIMEOUT 240
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_transfer_ui tests/test_transfer_ui.c)
        target_link_libraries(test_transfer_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_transfer_ui COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_transfer_ui.py $<TARGET_FILE:test_transfer_ui>)
        # Kılavuz 18.10 R10.1: import profiles and the profile-based rekordbox
        # XML import on Ayarlar › Import, against a real private service.
        add_executable(test_import_profiles_ui tests/test_import_profiles_ui.c)
        target_link_libraries(test_import_profiles_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_import_profiles COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_admin_ui.py $<TARGET_FILE:test_import_profiles_ui>)
        set_tests_properties(native_import_profiles PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        set_tests_properties(native_transfer_ui PROPERTIES TIMEOUT 90
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_media_drop_ui tests/test_media_drop_ui.c)
        target_link_libraries(test_media_drop_ui PRIVATE radios_ui PkgConfig::FONTCONFIG radios_warnings)
        add_test(NAME native_media_drop COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_data_ui.py $<TARGET_FILE:test_media_drop_ui>)
        set_tests_properties(native_media_drop PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_emergency_transfer_ui tests/test_emergency_transfer_ui.c)
        target_link_libraries(test_emergency_transfer_ui PRIVATE radios_ui radios_warnings)
        add_executable(test_broadcast_catalogue_ui tests/test_broadcast_catalogue_ui.c)
        target_link_libraries(test_broadcast_catalogue_ui PRIVATE radios_ui radios_warnings)
        radios_wrap(test_broadcast_catalogue_ui gtk_drag_finish)
        add_test(NAME native_broadcast_catalogue COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_broadcast_catalogue_ui.py $<TARGET_FILE:test_broadcast_catalogue_ui>)
        set_tests_properties(native_broadcast_catalogue PROPERTIES TIMEOUT 150
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_test(NAME native_emergency_transfer COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_emergency_transfer_ui.py $<TARGET_FILE:test_emergency_transfer_ui>)
        set_tests_properties(native_emergency_transfer PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_programme_ui tests/test_programme_ui.c)
        target_link_libraries(test_programme_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_programme COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_programme_ui.py $<TARGET_FILE:test_programme_ui>)
        set_tests_properties(native_programme PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_rekordbox_ui tests/test_rekordbox_ui.c)
        target_link_libraries(test_rekordbox_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_rekordbox COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_rekordbox_ui.py $<TARGET_FILE:test_rekordbox_ui>)
        set_tests_properties(native_rekordbox PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_application_shutdown src/app/main.c tests/application_shutdown_driver.c)
        target_link_libraries(test_application_shutdown PRIVATE radios_ui radios_warnings)
        radios_wrap(test_application_shutdown radios_window_present)
        add_test(NAME native_application_shutdown COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/test_application_shutdown.py $<TARGET_FILE:test_application_shutdown>)
        set_tests_properties(native_application_shutdown PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_external_access_ui tests/test_external_access_ui.c)
        target_link_libraries(test_external_access_ui PRIVATE radios_ui radios_warnings)
        radios_wrap(test_external_access_ui gtk_show_uri_on_window radios_external_probe_sync)
        add_test(NAME native_external_access COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_admin_ui.py $<TARGET_FILE:test_external_access_ui>)
        set_tests_properties(native_external_access PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_routing_graph_ui tests/test_routing_graph_ui.c)
        target_link_libraries(test_routing_graph_ui PRIVATE radios_ui user32 radios_warnings)
        target_compile_definitions(test_routing_graph_ui PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
        add_test(NAME native_routing_graph COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_routing_graph_ui.py $<TARGET_FILE:test_routing_graph_ui>)
        set_tests_properties(native_routing_graph PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
            LABELS gui RESOURCE_LOCK native_input
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_stream_metadata tests/test_stream_metadata.c)
        target_link_libraries(test_stream_metadata PRIVATE radios_ui radios_warnings)
        add_test(NAME native_stream_metadata COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_stream_metadata_ui.py $<TARGET_FILE:test_stream_metadata>)
        set_tests_properties(native_stream_metadata PROPERTIES TIMEOUT 240
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple")
        add_executable(test_routing_ui tests/test_routing_ui.c)
        target_link_libraries(test_routing_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_routing COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_routing_ui.py $<TARGET_FILE:test_routing_ui>)
        set_tests_properties(native_routing PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
            LABELS gui RESOURCE_LOCK native_input
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_playlist_browser_ui tests/test_playlist_browser_ui.c)
        target_link_libraries(test_playlist_browser_ui PRIVATE radios_ui radios_warnings)
        radios_add_gui_test(NAME playlist_browser_ui COMMAND test_playlist_browser_ui)
        set_tests_properties(playlist_browser_ui PROPERTIES TIMEOUT 30
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_playlist_redesign_ui tests/test_playlist_redesign_ui.c)
        target_link_libraries(test_playlist_redesign_ui PRIVATE radios_ui radios_warnings)
        radios_wrap(test_playlist_redesign_ui radios_media_import_open)
        radios_add_gui_test(NAME native_playlist_redesign COMMAND test_playlist_redesign_ui)
        set_tests_properties(native_playlist_redesign PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_ui tests/test_ui.c)
        target_link_libraries(test_ui PRIVATE radios_ui radios_warnings)
        target_compile_definitions(test_ui PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
        radios_add_gui_test(NAME native_controls COMMAND test_ui)
        set_tests_properties(native_controls PROPERTIES TIMEOUT 30 WORKING_DIRECTORY ${RADIOS_TEST_FOREIGN_CWD}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_data_ui tests/test_data_ui.c)
        target_link_libraries(test_data_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_data COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_native_data_ui.py $<TARGET_FILE:test_data_ui>)
        set_tests_properties(native_data PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_test(NAME native_media COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_data_ui.py $<TARGET_FILE:test_data_ui> -p /native-media)
        set_tests_properties(native_media PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_audio_ui tests/test_audio_ui.c)
        target_link_libraries(test_audio_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_audio COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py $<TARGET_FILE:test_audio_ui>)
        set_tests_properties(native_audio PROPERTIES TIMEOUT 180
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_audio_loudness_policy_ui tests/test_audio_loudness_policy_ui.c)
        target_link_libraries(test_audio_loudness_policy_ui PRIVATE radios_ui radios_warnings)
        foreach(policy_case roundtrip-close-recovery first-read-independent-merge invalid-edit-preserves-revision production-catalogue-names-selection-persistence preview-applies-stored-measurement)
            add_test(NAME native_loudness_policy_${policy_case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py $<TARGET_FILE:test_audio_loudness_policy_ui>
                -p /native-loudness-policy/${policy_case})
            set_tests_properties(native_loudness_policy_${policy_case} PROPERTIES TIMEOUT 90
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                ENVIRONMENT "G_DEBUG=fatal-warnings")
        endforeach()
        add_executable(test_loudness_origin_playout_ui tests/test_loudness_origin_playout_ui.c)
        target_link_libraries(test_loudness_origin_playout_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_loudness_execution_origin COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_loudness_origin_playout_ui>
            -p /native-loudness-origin/assigned-scope-immutable-filter-manual-takeover-MAIN-PCM)
        set_tests_properties(native_loudness_execution_origin PROPERTIES TIMEOUT 90
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_test(NAME native_loudness_scheduled_origin COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_loudness_origin_playout_ui>
            -p /native-loudness-origin/scheduled-prepared-manual-and-explicit-origin)
        set_tests_properties(native_loudness_scheduled_origin PROPERTIES TIMEOUT 90
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_test(NAME native_loudness_scope_execution_origin COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_loudness_origin_playout_ui>
            -p /native-loudness-origin/scope-execution-manual-versus-actual-schema-MAIN-PCM)
        set_tests_properties(native_loudness_scope_execution_origin PROPERTIES TIMEOUT 90
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_effect_return_ui tests/test_effect_return_ui.c)
        target_link_libraries(test_effect_return_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_effect_return COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_dj_ui.py $<TARGET_FILE:test_effect_return_ui>)
        set_tests_properties(native_effect_return PROPERTIES TIMEOUT 180
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_dj_ui tests/test_dj_ui.c)
        target_link_libraries(test_dj_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_dj COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_dj_ui.py $<TARGET_FILE:test_dj_ui>)
        # 18 independent fixtures, each with its own data service: ~200 s on a quiet machine.
        set_tests_properties(native_dj PROPERTIES TIMEOUT 600
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_playout_ui tests/test_playout_ui.c)
        target_link_libraries(test_playout_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_playout COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_playout_ui>)
        # Each scenario owns a fresh service/audio fixture; genuine weekly
        # boundary scenarios also wait through a complete UTC minute.
        set_tests_properties(native_playout PROPERTIES TIMEOUT 1800
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_advert_ui tests/test_advert_ui.c)
        target_link_libraries(test_advert_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_advert COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_advert_ui.py $<TARGET_FILE:test_advert_ui>)
        set_tests_properties(native_advert PROPERTIES TIMEOUT 300
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_broadcast_ui tests/test_broadcast_ui.c)
        target_link_libraries(test_broadcast_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_broadcast COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_broadcast_ui.py $<TARGET_FILE:test_broadcast_ui>)
        set_tests_properties(native_broadcast PROPERTIES TIMEOUT 300
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_executable(test_admin_ui tests/test_admin_ui.c)
        target_link_libraries(test_admin_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_admin COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_admin_ui.py $<TARGET_FILE:test_admin_ui> -p /native-admin)
        # The admin page copies the API token to the system clipboard: one clipboard user at a time.
        set_tests_properties(native_admin PROPERTIES TIMEOUT 180 RESOURCE_LOCK system_clipboard
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        add_test(NAME audio_devices COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py --engine-only $<TARGET_FILE:test_audio>)
        set_tests_properties(audio_devices PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        radios_add_gui_test(NAME native_window COMMAND radios --smoke-test
                 --project-root ${CMAKE_SOURCE_DIR}
                 --capture ${CMAKE_BINARY_DIR}/native-window.png)
        set_tests_properties(native_window PROPERTIES TIMEOUT 20 WORKING_DIRECTORY ${RADIOS_TEST_FOREIGN_CWD})
    endif()
endif()

include(cmake/Prompt7Tests.cmake)

include(cmake/Pdf2HeaderStatus.cmake)

# Prompt 1 native Preferences panel.
include(cmake/Preferences.cmake)
include(cmake/MediaArchive.cmake)
include(cmake/SilenceGuard.cmake)
# Medya arşivi: workstation INI reader test and the data service's storage.ini test.
include(cmake/MediaArchiveService.cmake)
# Ayarlar › Kütüphane analizi: native page test.
include(cmake/LibraryAnalysis.cmake)
# Ayarlar › Sıfırlama: native page test against a private store.
include(cmake/ResetUi.cmake)

# PDF13 calendar acceptance: each test uses a private database.
if(BUILD_TESTING)
    add_test(NAME desktop_automation_calendar COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/test_desktop_automation_calendar.py)
    set_tests_properties(desktop_automation_calendar PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    if(RADIOS_GUI_TESTS)
        add_executable(test_automation_calendar tests/test_automation_calendar.c)
        target_link_libraries(test_automation_calendar PRIVATE radios_ui PkgConfig::FONTCONFIG radios_warnings)
        add_test(NAME native_automation_calendar COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_automation_calendar_ui.py $<TARGET_FILE:test_automation_calendar>)
        set_tests_properties(native_automation_calendar PROPERTIES TIMEOUT 360
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endif()
endif()

# Prompt 5 broadcast navigation and relocated footer information.
target_sources(radios_ui PRIVATE src/ui/header_navigation.c)
include(cmake/Prompt5Tests.cmake)

include(cmake/Prompt10Playlist.cmake)

# PDF4 shared neon song timeline and native behavior acceptance.
include(cmake/Pdf4Neon.cmake)

include(cmake/Pdf12Playlist.cmake)

if(BUILD_TESTING)
    add_executable(test_automation_calendar_plan tests/test_automation_calendar_plan.c)
    target_link_libraries(test_automation_calendar_plan PRIVATE radios_playout radios_warnings)
    add_test(NAME automation_calendar_plan COMMAND test_automation_calendar_plan)
    set_tests_properties(automation_calendar_plan PROPERTIES TIMEOUT 30)
endif()

include(cmake/Prompt8Tests.cmake)

if(BUILD_TESTING)
    # Focused GTK harnesses on the test desktop, without a user installation.
    add_executable(test_schema_settings_ui tests/test_schema_settings_ui.c src/ui/schema_settings_ui.c src/ui/widgets.c)
    target_link_libraries(test_schema_settings_ui PRIVATE radios_playout PkgConfig::GTK3 radios_warnings)
    add_test(NAME schema_settings_ui COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_schema_settings_ui>)
    set_tests_properties(schema_settings_ui PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings")
    add_executable(test_rotation_editor_regressions tests/test_rotation_editor_regressions.c
        src/ui/rotation_editor.c src/ui/probability_editor.c src/ui/autodj_automation_ui.c src/ui/transition_distribution.c
        src/ui/schema_settings_ui.c src/ui/widgets.c)
    target_compile_options(test_rotation_editor_regressions PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_rotation_editor_regressions PRIVATE -Wl,--gc-sections)
    target_link_libraries(test_rotation_editor_regressions PRIVATE radios_playout PkgConfig::GTK3 radios_warnings)
    add_test(NAME rotation_editor_regressions COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_rotation_editor_regressions>)
    set_tests_properties(rotation_editor_regressions PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple")
    add_executable(test_player_waveform_markers tests/test_player_waveform_markers.c)
    target_link_libraries(test_player_waveform_markers PRIVATE radios_ui radios_warnings)
    radios_wrap(test_player_waveform_markers radios_data_client_acquire radios_data_connected
        radios_data_station_id radios_data_register_refresh radios_data_body radios_data_request
        radios_audio_ui_get_snapshot radios_audio_ui_engine_acquire radios_audio_engine_unref
        radios_audio_engine_set_deck_rhythm radios_media_rhythm_save_async radios_media_rhythm_analyze_async)
    add_test(NAME player_waveform_markers COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_player_waveform_markers>)
    set_tests_properties(player_waveform_markers PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple")
endif()

if(BUILD_TESTING)
    add_executable(test_weekly_programme_plan tests/test_weekly_programme_plan.c)
    target_link_libraries(test_weekly_programme_plan PRIVATE radios_playout radios_warnings)
    add_test(NAME weekly_programme_plan COMMAND test_weekly_programme_plan)
    set_tests_properties(weekly_programme_plan PROPERTIES TIMEOUT 30)
    add_test(NAME desktop_weekly_programme COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/test_desktop_weekly_programme.py)
    set_tests_properties(desktop_weekly_programme PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(BUILD_TESTING)
    add_test(NAME desktop_media_rights COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/test_desktop_media_rights.py)
    add_test(NAME desktop_copyright COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/test_desktop_copyright.py)
    set_tests_properties(desktop_copyright PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(desktop_media_rights PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    if(RADIOS_GUI_TESTS)
        add_executable(test_media_rights_ui tests/test_media_rights_ui.c)
        target_link_libraries(test_media_rights_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_media_rights COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_data_ui.py $<TARGET_FILE:test_media_rights_ui>
            -p /native-media-rights/settings-verified-picker-grant-append-reopen-guards)
        add_test(NAME native_copyright_settings COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_data_ui.py $<TARGET_FILE:test_media_rights_ui>
            -p /native-copyright-policy/manual-metadata-dates-toggle-edit-remove-reopen)
        set_tests_properties(native_copyright_settings PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
        set_tests_properties(native_media_rights PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endif()
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_copyright_playback_ui tests/test_copyright_playback_ui.c)
    target_link_libraries(test_copyright_playback_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_copyright_playback COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_copyright_playback_ui>)
    set_tests_properties(native_copyright_playback PROPERTIES TIMEOUT 120
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
    add_executable(test_calendar_regressions tests/test_calendar_regressions.c)
    target_link_libraries(test_calendar_regressions PRIVATE radios_ui radios_warnings)
    radios_add_gui_test(NAME native_calendar_regressions COMMAND test_calendar_regressions)
    set_tests_properties(native_calendar_regressions PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
        ENVIRONMENT "G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple")
endif()

if(BUILD_TESTING)
    add_test(NAME playout_policy COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_playout_policy.py)
    set_tests_properties(playout_policy PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    if(RADIOS_GUI_TESTS)
        add_executable(test_playout_policy_ui tests/test_playout_policy_ui.c)
        target_link_libraries(test_playout_policy_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_playout_policy COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_data_ui.py $<TARGET_FILE:test_playout_policy_ui>)
        set_tests_properties(native_playout_policy PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple")
    endif()
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_live_layout tests/test_live_layout.c)
    target_link_libraries(test_live_layout PRIVATE radios_ui radios_warnings)
    target_compile_definitions(test_live_layout PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    radios_add_gui_test(NAME native_live_layout COMMAND test_live_layout)
    set_tests_properties(native_live_layout PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
        ENVIRONMENT "G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple")
    add_test(NAME native_live_layout_connected COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py $<TARGET_FILE:test_live_layout>)
    set_tests_properties(native_live_layout_connected PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple")
    add_executable(test_general_queue_ui tests/test_general_queue_ui.c)
    target_link_libraries(test_general_queue_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_general_queue COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py $<TARGET_FILE:test_general_queue_ui>
        -p /general-queue/nearest-saved-preview-private-isolation-header-waiting-controls)
    add_test(NAME native_general_schema_entry COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py $<TARGET_FILE:test_general_queue_ui>
        -p /general-queue/schema-entry-weighted-alternative-assigned-transition)
    # Kılavuz §02 "Mevcut şemayı uygula" on a running general programme (akım B).
    add_test(NAME native_general_schema_apply COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py $<TARGET_FILE:test_general_queue_ui>
        -p /general-queue/schema-apply-running-programme)
    add_test(NAME native_general_queue_responsive COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_general_queue_ui>
        -p /general-queue/174-schedules-responsive-current-next-owner-cancel)
    set_tests_properties(native_general_queue native_general_schema_entry native_general_schema_apply native_general_queue_responsive PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple")
endif()



if(BUILD_TESTING)
    add_test(NAME playlist_sweeper_schema COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_playlist_sweeper_schema.py)
    set_tests_properties(playlist_sweeper_schema PROPERTIES TIMEOUT 45 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_automation_probability_ui tests/test_automation_probability_ui.c)
    target_link_libraries(test_automation_probability_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_automation_probability COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/run_automation_probability_ui.py $<TARGET_FILE:test_automation_probability_ui>)
    set_tests_properties(native_automation_probability PROPERTIES TIMEOUT 180 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING)
    add_executable(test_automation_policy tests/test_automation_policy.c)
    target_link_libraries(test_automation_policy PRIVATE radios_core radios_warnings)
    add_test(NAME automation_probability_policy COMMAND test_automation_policy)
    set_tests_properties(automation_probability_policy PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING)
    add_test(NAME desktop_probability_policy COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_probability_policy_schema.py)
    set_tests_properties(desktop_probability_policy PROPERTIES TIMEOUT 60 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(BUILD_TESTING)
    add_executable(test_automation_pdf_plan tests/test_automation_pdf_plan.c)
    target_link_libraries(test_automation_pdf_plan PRIVATE radios_playout radios_warnings)
    add_test(NAME automation_pdf_plan COMMAND test_automation_pdf_plan)
    set_tests_properties(automation_pdf_plan PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING)
    add_executable(test_automation_adjunct tests/test_automation_adjunct.c)
    target_link_libraries(test_automation_adjunct PRIVATE radios_audio radios_warnings)
    target_compile_definitions(test_automation_adjunct PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    add_test(NAME automation_midtrack_jingle COMMAND test_automation_adjunct)
    set_tests_properties(automation_midtrack_jingle PROPERTIES TIMEOUT 90)
endif()

# U1 (Kılavuz §08 R8.7): a pool of N players holds N items in pool order;
# the real-PCM case plays on owned virtual sinks (run_audio_ui.py --engine-only).
if(BUILD_TESTING)
    add_executable(test_runner_pool_preload tests/test_runner_pool_preload.c)
    target_link_libraries(test_runner_pool_preload PRIVATE radios_audio radios_warnings)
    target_compile_definitions(test_runner_pool_preload PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    add_test(NAME runner_pool_preload COMMAND test_runner_pool_preload)
    # U2: rows of the "Şemadan yükle…" window (pure, no display).
    add_executable(test_schema_track_picker tests/test_schema_track_picker.c)
    target_link_libraries(test_schema_track_picker PRIVATE radios_ui radios_warnings)
    add_test(NAME schema_track_picker COMMAND test_schema_track_picker)
    set_tests_properties(runner_pool_preload PROPERTIES TIMEOUT 120)
    if(RADIOS_GUI_TESTS)
        add_test(NAME runner_pool_preload_pcm COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
                 ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py --engine-only $<TARGET_FILE:test_runner_pool_preload>
                 -p /runner-pool-preload/four-player-cluster-realtime-PCM)
        set_tests_properties(runner_pool_preload_pcm PROPERTIES TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endif()

if(BUILD_TESTING)
    add_executable(test_automation_probability_playout tests/test_automation_probability_playout.c)
    target_link_libraries(test_automation_probability_playout PRIVATE radios_playout radios_warnings)
    target_compile_definitions(test_automation_probability_playout PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    add_test(NAME automation_probability_playout COMMAND test_automation_probability_playout)
    set_tests_properties(automation_probability_playout PROPERTIES TIMEOUT 45)
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_player_schema_weekly_ui tests/test_player_schema_weekly_ui.c)
    target_link_libraries(test_player_schema_weekly_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_player_schema_weekly COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/run_automation_calendar_ui.py $<TARGET_FILE:test_player_schema_weekly_ui>)
    set_tests_properties(native_player_schema_weekly PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_playlist_export_ui tests/test_playlist_export_ui.c)
    target_link_libraries(test_playlist_export_ui PRIVATE radios_ui radios_warnings)
    radios_wrap(test_playlist_export_ui radios_media_import_open)
    add_test(NAME native_playlist_export COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_playlist_export_ui>)
    set_tests_properties(native_playlist_export PROPERTIES TIMEOUT 60 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING)
    add_test(NAME desktop_workflow_bridge COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_desktop_workflow_bridge.py)
    set_tests_properties(desktop_workflow_bridge PROPERTIES TIMEOUT 90 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_workflow_hub_ui tests/test_workflow_hub_ui.c)
    target_link_libraries(test_workflow_hub_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_workflow_hub COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/run_data_ui.py $<TARGET_FILE:test_workflow_hub_ui>)
    set_tests_properties(native_workflow_hub PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_test(NAME native_header_live_strip COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_header_playout_ui> -p /prompt2-header/live-strip-exact-owner-next-diagnostics-MAIN-PCM)
    set_tests_properties(native_header_live_strip PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING)
    add_test(NAME desktop_transfers COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_desktop_transfers.py)
    set_tests_properties(desktop_transfers PROPERTIES TIMEOUT 90 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME desktop_transfer_package COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_desktop_transfer_package.py)
    set_tests_properties(desktop_transfer_package PROPERTIES TIMEOUT 180 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME desktop_integrity COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_desktop_integrity.py)
    set_tests_properties(desktop_integrity PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # Kılavuz 18.10 R10.1: every backend operation reaches the GUI (or is
    # classified with its reason); tools/gui_backend_coverage.py.
    add_test(NAME gui_backend_coverage COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/test_gui_backend_coverage.py)
    set_tests_properties(gui_backend_coverage PROPERTIES TIMEOUT 60 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_playout_jingle_ui tests/test_playout_jingle_ui.c)
    target_link_libraries(test_playout_jingle_ui PRIVATE radios_ui radios_warnings)
    foreach(case jingle-empty-unrouted-MAIN-PCM jingle-later-append-unrouted-MAIN-PCM jingle-unavailable-preserves-primary-and-explicit-player-PCM)
        add_test(NAME native_${case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_playout_jingle_ui> -p /native-playout/${case})
        set_tests_properties(native_${case} PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endforeach()
    add_executable(test_autoplay_pending_ui tests/test_autoplay_pending_ui.c tests/autoplay_execution_clock.c)
    target_link_libraries(test_autoplay_pending_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_autoplay_pending COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_autoplay_pending_ui>
        -p /native-playout/pending-deadline-late-reply-retry-foreign-PCM)
    set_tests_properties(native_autoplay_pending PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_autoplay_header_ui tests/test_autoplay_header_ui.c)
    target_link_libraries(test_autoplay_header_ui PRIVATE radios_ui radios_warnings)
    foreach(case scoped-playlist-name-end-advert-pause-stop-PCM schema-current-future-status-clears-preparing-PCM)
        add_test(NAME native_autoplay_header_${case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_autoplay_header_ui> -p /native-autoplay-header/${case})
        set_tests_properties(native_autoplay_header_${case} PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endforeach()
endif()

# U3 (C10): header "Sonraki playlist ▾" playlist picker, each case in its own
# private DB/PCM fixture (run_playout_ui.py checks the exact GLib case).
if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_header_next_playlist_ui tests/test_header_next_playlist_ui.c)
    target_link_libraries(test_header_next_playlist_ui PRIVATE radios_ui radios_warnings)
    foreach(case local-queue general-owner general-not-owner scope-flow)
        add_test(NAME native_header_next_playlist_${case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_header_next_playlist_ui> -p /native-playout/header-next-playlist/${case})
        set_tests_properties(native_header_next_playlist_${case} PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endforeach()
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_autoplay_events_ui tests/test_autoplay_events_ui.c)
    target_link_libraries(test_autoplay_events_ui PRIVATE radios_ui radios_warnings)
    foreach(case actual-next-replacement-stop-scope-readonly aggregate-retains-independent-queue general-owned-pool-filter-readonly future-schema-wait-cancel-clears-events distinct-block-identity-and-planned-jingle)
        add_test(NAME native_autoplay_events_${case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_autoplay_events_ui> -p /autoplay-events/${case})
        set_tests_properties(native_autoplay_events_${case} PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endforeach()
endif()

if(BUILD_TESTING)
    # The schedule driver is the otomasyon_e2e target (no compiler at test time).
    add_test(NAME automation_recovery_stress COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_automation_stress.py --build-dir ${CMAKE_BINARY_DIR}
        --driver $<TARGET_FILE:otomasyon_e2e>)
    set_tests_properties(automation_recovery_stress PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_playout_vocal_jingle_ui tests/test_playout_vocal_jingle_ui.c)
    target_link_libraries(test_playout_vocal_jingle_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_playout_vocal_jingle COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_playout_vocal_jingle_ui> -p /native-playout/vocal-intro-whole-jingle-duck-primary-MAIN-PCM)
    set_tests_properties(native_playout_vocal_jingle PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING)
    add_test(NAME desktop_playout_vocal_cues COMMAND ${RADIOS_TEST_PYTHON_COMMAND} -m unittest test_playout_vocal_cues)
    set_tests_properties(desktop_playout_vocal_cues PROPERTIES TIMEOUT 60 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} ENVIRONMENT_MODIFICATION "PYTHONPATH=path_list_prepend:${CMAKE_SOURCE_DIR}/tests;PYTHONPATH=path_list_prepend:${CMAKE_SOURCE_DIR}/database/SDBASE/src")
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_window_now_playing tests/test_window_now_playing.c)
    target_link_libraries(test_window_now_playing PRIVATE radios_ui radios_warnings)
    foreach(case current-hidden-programme-song-pause-stop-future-PCM long-UTF8-bounded)
        add_test(NAME native_window_title_${case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_window_now_playing> -p /native-window-title/${case})
        set_tests_properties(native_window_title_${case} PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endforeach()
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_broadcast_metadata_ui tests/test_broadcast_metadata_ui.c)
    target_link_libraries(test_broadcast_metadata_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_broadcast_automatic_metadata_filter COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_broadcast_ui.py $<TARGET_FILE:test_broadcast_metadata_ui>
        -p /native-broadcast/automatic-metadata-excludes-jingle-advertisement)
    set_tests_properties(native_broadcast_automatic_metadata_filter PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_mixer_auto_volume_ui tests/test_mixer_auto_volume_ui.c)
    target_link_libraries(test_mixer_auto_volume_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_mixer_auto_volume COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py $<TARGET_FILE:test_mixer_auto_volume_ui>
        -p /native-mixer-auto-volume/real-toggle-manual-takeover-off-baseline-MAIN-PCM)
    set_tests_properties(native_mixer_auto_volume PROPERTIES TIMEOUT 120
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_programme_jingle_hint tests/test_programme_jingle_hint.c)
    target_link_libraries(test_programme_jingle_hint PRIVATE radios_ui radios_warnings)
    radios_wrap(test_programme_jingle_hint gtk_drag_finish)
    radios_add_gui_test(NAME programme_jingle_hint COMMAND test_programme_jingle_hint)
    set_tests_properties(programme_jingle_hint PROPERTIES TIMEOUT 30
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING)
    add_executable(test_playout_event_blocks tests/test_playout_event_blocks.c)
    target_link_libraries(test_playout_event_blocks PRIVATE radios_playout radios_warnings)
    target_compile_definitions(test_playout_event_blocks PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    add_test(NAME playout_event_blocks COMMAND test_playout_event_blocks)
    set_tests_properties(playout_event_blocks PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_event_block_queue_ui tests/test_event_block_queue_ui.c)
    target_link_libraries(test_event_block_queue_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_event_block_queue COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_event_block_queue_ui>
        -p /event-block-queue/mixed-import-player-order-accepted-jingle-PCM)
    set_tests_properties(native_event_block_queue PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
    # Kılavuz 2026-10-18 s.6/s.8/s.15 (A-1, A-2, A-3, A-19): L and R of a live
    # event row end to end with PCM (general queue A -> live talk -> C).
    add_executable(test_live_row_ui tests/test_live_row_ui.c)
    target_link_libraries(test_live_row_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_live_row_talk_manual COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_live_row_ui>
        -p /live-row/talk-manual-L-R-then-continue-PCM)
    add_test(NAME native_live_row_talk_duration COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_live_row_ui>
        -p /live-row/talk-duration-waits-then-continues-PCM)
    add_test(NAME native_live_row_release COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_live_row_ui>
        -p /live-row/R-release-never-skips-PCM)
    add_test(NAME native_live_row_music_schema COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_live_row_ui>
        -p /live-row/music-L-starts-event-schema-PCM)
    add_test(NAME native_live_row_talk_schema COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_live_row_ui>
        -p /live-row/talk-L-opens-channel-and-live-schema-PCM)
    add_test(NAME native_live_row_header_general_queue COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_live_row_ui>
        -p /live-row/header-transport-drives-general-queue-list-PCM)
    add_test(NAME native_live_row_ending_warning COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_live_row_ui>
        -p /live-row/ending-warning-before-transition-PCM)
    add_test(NAME native_live_row_remove_unheard COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_live_row_ui>
        -p /live-row/remove-unheard-content-from-running-general-queue-PCM)
    set_tests_properties(native_live_row_talk_manual native_live_row_talk_duration native_live_row_release native_live_row_music_schema native_live_row_talk_schema native_live_row_header_general_queue native_live_row_ending_warning native_live_row_remove_unheard
        PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
    # Kılavuz 2026-10-18 s.15 (akım B, H4): a live step's end rule; a duration
    # counts from the previous row's actual end (general queue, real PCM).
    add_executable(test_live_step_wait_ui tests/test_live_step_wait_ui.c)
    target_link_libraries(test_live_step_wait_ui PRIVATE radios_ui radios_warnings)
    foreach(case duration-counts-from-actual-end-PCM next-ends-duration-step-early-PCM manual-step-waits-for-next-PCM next-during-row-enters-step-PCM next-during-row-starts-duration-PCM)
        add_test(NAME native_live_step_${case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_live_step_wait_ui> -p /live-step/${case})
        set_tests_properties(native_live_step_${case} PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endforeach()
    # Kılavuz 2026-10-18 §06/§08: view settings of the broadcast flow and the
    # header counter following the selected event; test desktop, no audio device or DB.
    add_executable(test_programme_view_settings_ui tests/test_programme_view_settings_ui.c)
    target_link_libraries(test_programme_view_settings_ui PRIVATE radios_ui radios_warnings)
    radios_wrap(test_programme_view_settings_ui radios_programme_queue_set_window radios_playout_ui_set_programme_window)
    add_test(NAME native_programme_view_settings COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_programme_view_settings_ui>)
    add_executable(test_programme_view_header_ui tests/test_programme_view_header_ui.c)
    target_link_libraries(test_programme_view_header_ui PRIVATE radios_ui radios_warnings)
    radios_wrap(test_programme_view_header_ui radios_broadcast_header_snapshot)
    add_test(NAME native_programme_view_header COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_programme_view_header_ui>)
    set_tests_properties(native_programme_view_settings native_programme_view_header PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
    # Kılavuz 18.10 A-12/A-18: a 2020-content programme keeps a bounded list.
    add_executable(test_programme_queue_scale tests/test_programme_queue_scale.c)
    target_link_libraries(test_programme_queue_scale PRIVATE radios_ui radios_warnings)
    add_test(NAME native_programme_queue_scale COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_programme_queue_scale>)
    set_tests_properties(native_programme_queue_scale PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_local_event_ui tests/test_local_event_ui.c)
    target_link_libraries(test_local_event_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_local_event_builder COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_local_event_ui>
        -p /native-playout/local-event-builder-save-reopen-isolated-PCM)
    add_executable(test_player_content_status_ui tests/test_player_content_status_ui.c)
    target_link_libraries(test_player_content_status_ui PRIVATE radios_ui radios_warnings)
    foreach(case manual-kinds-MAIN-PCM scoped-sweeper-hidden-pause-stop-MAIN-PCM)
        add_test(NAME native_player_content_status_${case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_player_content_status_ui>
            -p /native-playout/player-content-${case})
        set_tests_properties(native_player_content_status_${case} PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endforeach()
    # U1: a four-player scope shows the playing card and three ready cards.
    add_executable(test_cluster_pool_cards_ui tests/test_cluster_pool_cards_ui.c)
    target_link_libraries(test_cluster_pool_cards_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_cluster_pool_cards COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_cluster_pool_cards_ui>
        -p /native-playout/cluster-pool-cards-four-players-PCM)
    set_tests_properties(native_cluster_pool_cards PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
    # U2: "Şemadan yükle…" lists the schema's songs and loads one (read only).
    add_executable(test_schema_track_picker_ui tests/test_schema_track_picker_ui.c)
    target_link_libraries(test_schema_track_picker_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_schema_track_picker COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_schema_track_picker_ui>
        -p /native-playout/schema-track-picker-load-readonly-PCM)
    set_tests_properties(native_schema_track_picker PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
    set_tests_properties(native_local_event_builder PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_local_event_prepare_ui tests/test_local_event_prepare_ui.c)
    target_link_libraries(test_local_event_prepare_ui PRIVATE radios_ui radios_warnings)
    foreach(case materialize-readonly-and-cancel-PCM insert-fences-live-queue-PCM mixed-sources-station-breaks-PCM mixed-flow-manual-wait-stable-PCM schema-from-now-readonly-PCM)
        add_test(NAME native_local_event_${case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_local_event_prepare_ui> -p /native-playout/local-event-${case})
        set_tests_properties(native_local_event_${case} PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endforeach()
endif()

if(BUILD_TESTING)
    add_test(NAME desktop_autodj COMMAND ${RADIOS_TEST_PYTHON_COMMAND} -m unittest test_desktop_autodj)
    set_tests_properties(desktop_autodj PROPERTIES TIMEOUT 90 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT_MODIFICATION "PYTHONPATH=path_list_prepend:${CMAKE_SOURCE_DIR}/tests;PYTHONPATH=path_list_prepend:${CMAKE_SOURCE_DIR}/database/SDBASE/src")
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_autodj_settings_ui tests/test_autodj_settings_ui.c)
    target_link_libraries(test_autodj_settings_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_autodj_settings COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_autodj_settings_ui>
        -p /native-playout/AutoDJ-settings-bulk-markers-playlist-all-persistence-MAIN-PCM)
    set_tests_properties(native_autodj_settings PROPERTIES TIMEOUT 150 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
    add_test(NAME native_autodj_settings_dense_final_section COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_autodj_settings_ui>
        -p /native-playout/AutoDJ-settings-dense-final-section-OUT-not-end)
    set_tests_properties(native_autodj_settings_dense_final_section PROPERTIES TIMEOUT 150 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_event_playout_ui tests/test_event_playout_ui.c)
    target_link_libraries(test_event_playout_ui PRIVATE radios_ui radios_warnings)
    foreach(case local-event-unclustered-SEMI-preserves-queues-PCM event-complete-then-cluster-continuation-PCM event-stop-newload-target-change-fences-PCM event-assigned-schema-continuation-future-stop-PCM event-SEMI-pending-unrelated-peer-replacement-PCM)
        add_test(NAME native_event_playout_${case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_event_playout_ui> -p /native-playout/${case})
        set_tests_properties(native_event_playout_${case} PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endforeach()
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_event_assignment_ui tests/test_event_assignment_ui.c)
    target_link_libraries(test_event_assignment_ui PRIVATE radios_ui radios_warnings)
    foreach(case save-reopen-SEMI-row-PCM stale-target-fence general-assignment-isolated-SEMI-row-PCM)
        if(case STREQUAL "general-assignment-isolated-SEMI-row-PCM")
            set(event_assignment_path "event-${case}")
        else()
            set(event_assignment_path "event-selected-assignment-${case}")
        endif()
        add_test(NAME native_event_assignment_${case} COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_event_assignment_ui>
            -p /native-playout/${event_assignment_path})
        set_tests_properties(native_event_assignment_${case} PROPERTIES TIMEOUT 120 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "G_DEBUG=fatal-warnings")
    endforeach()
endif()

# ---- Runtime prefix (runtime\), package stage, portable ZIP and installer ------------------
# Last: cmake_install.cmake runs the install rules in declaration order and the DLL closure
# of the runtime component reads the files installed before it.
include(RadioSRuntime)
include(RadioSInstaller)
