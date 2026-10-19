# Prompt/2: extend the existing native header without changing other modules.
target_sources(radios_ui PRIVATE src/ui/header_status.c)

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_header_ui tests/test_header_ui.c src/ui/header_status.c)
    target_link_libraries(test_header_ui PRIVATE radios_core PkgConfig::GTK3 radios_warnings m)
    target_link_options(test_header_ui PRIVATE -Wl,--wrap=g_get_real_time)
    target_compile_definitions(test_header_ui PRIVATE RADIOS_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME native_header_status COMMAND test_header_ui)
    set_tests_properties(native_header_status PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings;GDK_BACKEND=x11")

    add_executable(test_broadcast_header tests/test_broadcast_header.c)
    target_link_libraries(test_broadcast_header PRIVATE radios_ui radios_warnings m)
    foreach(snapshot_function audio_ui_engine_acquire audio_engine_snapshot audio_engine_unref
            audio_ui_get_snapshot playout_ui_header_snapshot playout_ui_get_snapshot
            playout_ui_deck_metadata)
        target_link_options(test_broadcast_header PRIVATE "-Wl,--wrap=radios_${snapshot_function}")
    endforeach()
    add_test(NAME broadcast_header COMMAND test_broadcast_header)
    set_tests_properties(broadcast_header PROPERTIES TIMEOUT 20 ENVIRONMENT "G_DEBUG=fatal-warnings")

    add_executable(test_header_playout_ui tests/test_header_playout_ui.c)
    target_link_libraries(test_header_playout_ui PRIVATE radios_ui radios_warnings)
    # Each meaningful scenario owns a fresh DB/audio fixture; station quotas
    # and assertions remain unchanged when running the complete P2 suite.
    add_test(NAME native_header_private_programme COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_header_playout_ui>
        -p /prompt2-header/private-programme-PCM-pause-priority-stop)
    add_test(NAME native_header_private_wait COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_header_playout_ui>
        -p /prompt2-header/private-manual-wait-preserves-programme)
    add_test(NAME native_header_private_playlist COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_header_playout_ui>
        -p /prompt2-header/saved-playlist-name-end-pause-resume-stop-MAIN-PCM)
    add_test(NAME native_header_private_transition COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_header_playout_ui>
        -p /prompt2-header/AutoDJ-OUT-audible-track-end-MAIN-PCM)
    set_tests_properties(native_header_private_programme native_header_private_wait native_header_private_playlist native_header_private_transition PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;G_DEBUG=fatal-warnings;GDK_BACKEND=x11")

    add_executable(header_layout_driver tests/header_layout_driver.c)
    target_link_libraries(header_layout_driver PRIVATE radios_ui radios_warnings)
    target_compile_definitions(header_layout_driver PRIVATE RADIOS_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_options(header_layout_driver PRIVATE -Wl,--wrap=radios_broadcast_header_snapshot)
endif()
