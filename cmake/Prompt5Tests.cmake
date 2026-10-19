# Prompt 5: real GTK, private HTTP/DB and owned virtual audio outputs.
if(BUILD_TESTING)
    add_executable(test_header_navigation_backend tests/test_header_navigation_backend.c)
    target_link_libraries(test_header_navigation_backend PRIVATE radios_playout PkgConfig::GTK3 radios_warnings)
    target_compile_definitions(test_header_navigation_backend PRIVATE RADIOS_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME header_navigation_backend COMMAND test_header_navigation_backend)
    set_tests_properties(header_navigation_backend PROPERTIES TIMEOUT 60
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()
if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_test(NAME header_navigation_realtime COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_audio_ui.py --engine-only
        $<TARGET_FILE:test_header_navigation_backend>
        -p /prompt5/backend/realtime-audible-rendered-EOF-jump)
    set_tests_properties(header_navigation_realtime PROPERTIES TIMEOUT 45
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;G_DEBUG=fatal-warnings")
    add_executable(test_header_navigation_ui tests/test_header_navigation_ui.c)
    target_link_libraries(test_header_navigation_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_header_navigation COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_header_navigation_ui.py $<TARGET_FILE:test_header_navigation_ui>)
    set_tests_properties(native_header_navigation PROPERTIES TIMEOUT 150
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;G_DEBUG=fatal-warnings;GDK_BACKEND=x11")
endif()
