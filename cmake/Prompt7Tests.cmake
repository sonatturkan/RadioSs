# Live player queue and picker regressions from the prompt/7 audit.
if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_live_players_ui
        "${CMAKE_CURRENT_LIST_DIR}/../tests/test_live_players_ui.c")
    target_link_libraries(test_live_players_ui PRIVATE radios_ui radios_warnings)
    radios_wrap(test_live_players_ui radios_media_library_get radios_media_library_open_for_deck
        radios_player_loader_open radios_audio_ui_get_snapshot radios_audio_ui_load_media radios_audio_ui_play
        radios_audio_ui_pause radios_audio_ui_stop radios_playout_ui_deck_metadata radios_playout_ui_owns_deck)
    radios_add_gui_test(NAME native_live_players COMMAND test_live_players_ui)

    add_executable(test_player_loader_ui
        "${CMAKE_CURRENT_LIST_DIR}/../tests/test_player_loader_ui.c")
    target_link_libraries(test_player_loader_ui PRIVATE radios_ui radios_warnings)
    radios_wrap(test_player_loader_ui radios_data_connected radios_data_request)
    radios_add_gui_test(NAME native_player_loader COMMAND test_player_loader_ui)
    set_tests_properties(native_live_players native_player_loader PROPERTIES
        TIMEOUT 30 SKIP_RETURN_CODE 77
        ENVIRONMENT "G_DEBUG=fatal-warnings")
    # Exercise the last normal deck through real PCM and managed UI APIs.
    add_executable(test_player_mapping_ui
        "${CMAKE_CURRENT_LIST_DIR}/../tests/test_player_mapping_ui.c")
    target_link_libraries(test_player_mapping_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_player_mapping
        COMMAND ${RADIOS_TEST_PYTHON_COMMAND} "${PROJECT_SOURCE_DIR}/tests/run_player_mapping_ui.py"
            "$<TARGET_FILE:test_player_mapping_ui>")
    set_tests_properties(native_player_mapping PROPERTIES
        TIMEOUT 120 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()
