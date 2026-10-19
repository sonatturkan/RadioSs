if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_catalogue_playlist_ui tests/test_catalogue_playlist_ui.c)
    target_link_libraries(test_catalogue_playlist_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_catalogue_playlist COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_catalogue_playlist_ui.py $<TARGET_FILE:test_catalogue_playlist_ui>)
    set_tests_properties(native_catalogue_playlist PROPERTIES TIMEOUT 120
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;G_DEBUG=fatal-warnings")
endif()
