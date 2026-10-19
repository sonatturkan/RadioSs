# Prompt/2: extend the existing native header without changing other modules.
target_sources(radios_ui PRIVATE src/ui/header_status.c)

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    # The test fakes the clock of src/ui/header_status.c. g_get_real_time is a GLib DLL
    # import, so a plain --wrap would silently not intercept it on Windows. Preferred
    # (tests-c.md 10.2): tests/header_status_clock.c compiles header_status.c with the call
    # renamed. Until that file exists the __imp_ pointer is wrapped: the test must then
    # define __wrap___imp_g_get_real_time, or the link fails (no silently real clock).
    radios_optional_source(_radios_header_clock tests/header_status_clock.c)
    if(_radios_header_clock)
        add_executable(test_header_ui tests/test_header_ui.c tests/header_status_clock.c)
    else()
        add_executable(test_header_ui tests/test_header_ui.c src/ui/header_status.c)
        radios_wrap(test_header_ui DLLIMPORT g_get_real_time)
    endif()
    target_link_libraries(test_header_ui PRIVATE radios_core PkgConfig::GTK3 radios_warnings)
    target_compile_definitions(test_header_ui PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    radios_add_gui_test(NAME native_header_status COMMAND test_header_ui)
    set_tests_properties(native_header_status PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")

    add_executable(test_broadcast_header tests/test_broadcast_header.c)
    target_link_libraries(test_broadcast_header PRIVATE radios_ui radios_warnings)
    radios_wrap(test_broadcast_header radios_audio_ui_engine_acquire radios_audio_engine_snapshot
        radios_audio_engine_unref radios_audio_ui_get_snapshot radios_playout_ui_header_snapshot
        radios_playout_ui_get_snapshot radios_playout_ui_deck_metadata)
    add_test(NAME broadcast_header COMMAND test_broadcast_header)
    set_tests_properties(broadcast_header PROPERTIES TIMEOUT 20 ENVIRONMENT "G_DEBUG=fatal-warnings")

    add_executable(test_header_playout_ui tests/test_header_playout_ui.c)
    target_link_libraries(test_header_playout_ui PRIVATE radios_ui radios_warnings)
    # Each meaningful scenario owns a fresh DB/audio fixture; station quotas
    # and assertions remain unchanged when running the complete P2 suite.
    add_test(NAME native_header_private_programme COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_header_playout_ui>
        -p /prompt2-header/private-programme-PCM-pause-priority-stop)
    add_test(NAME native_header_private_wait COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_header_playout_ui>
        -p /prompt2-header/private-manual-wait-preserves-programme)
    add_test(NAME native_header_private_playlist COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_header_playout_ui>
        -p /prompt2-header/saved-playlist-name-end-pause-resume-stop-MAIN-PCM)
    add_test(NAME native_header_private_transition COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_header_playout_ui>
        -p /prompt2-header/AutoDJ-OUT-audible-track-end-MAIN-PCM)
    set_tests_properties(native_header_private_programme native_header_private_wait native_header_private_playlist native_header_private_transition PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")

    add_executable(header_layout_driver tests/header_layout_driver.c)
    target_link_libraries(header_layout_driver PRIVATE radios_ui radios_warnings)
    target_compile_definitions(header_layout_driver PRIVATE RADIOS_TEST_PROJECT_ROOT="${RADIOS_TEST_SOURCE_DIR}")
    radios_wrap(header_layout_driver radios_broadcast_header_snapshot)
endif()
