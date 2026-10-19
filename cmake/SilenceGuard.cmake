# Acil › Sessizlikte otomatik devralma (AC-09): dead-air detector and its guard.
target_sources(radios_core PRIVATE src/core/silence_detector.c)
target_sources(radios_ui PRIVATE src/ui/silence_guard.c)

if(BUILD_TESTING)
    add_executable(test_silence_detector tests/test_silence_detector.c)
    target_link_libraries(test_silence_detector PRIVATE radios_core radios_warnings m)
    add_test(NAME silence_detector COMMAND test_silence_detector)
    set_tests_properties(silence_detector PROPERTIES TIMEOUT 30)
    if(RADIOS_GUI_TESTS)
        add_executable(test_silence_guard_ui tests/test_silence_guard_ui.c)
        target_link_libraries(test_silence_guard_ui PRIVATE radios_ui radios_warnings m)
        add_test(NAME native_silence_guard COMMAND test_silence_guard_ui)
        set_tests_properties(native_silence_guard PROPERTIES TIMEOUT 90
            SKIP_RETURN_CODE 77 ENVIRONMENT "G_DEBUG=fatal-warnings;GDK_BACKEND=x11")
    endif()
endif()
