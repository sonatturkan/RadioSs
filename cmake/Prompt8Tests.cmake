# Each player acceptance gets its own database and owned virtual audio sinks.
# Keep these focused checks available alongside the complete playout suite.
if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    set(PROMPT8_PLAYER_CASES
        "native_player_waveform|player-waveform-cues-square-automatic-analysis"
        "native_player_sync|player-SYNC-analysis-persistence-chosen-pair-PCM"
        "native_player_sync_stale|player-SYNC-unknown-and-late-target-preserve-PCM")
    foreach(player_case IN LISTS PROMPT8_PLAYER_CASES)
        string(REPLACE "|" ";" parts "${player_case}")
        list(GET parts 0 test_name)
        list(GET parts 1 glib_case)
        add_test(NAME ${test_name} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/run_playout_ui.py $<TARGET_FILE:test_playout_ui>
            -p /native-playout/${glib_case})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 90 LABELS "prompt8"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;G_DEBUG=fatal-warnings;GDK_BACKEND=x11")
    endforeach()
    unset(PROMPT8_PLAYER_CASES)
endif()
