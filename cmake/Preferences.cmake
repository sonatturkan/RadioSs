# Native Preferences panel from prompt/1. Keep the module isolated so parallel
# source tasks can add their own targets without rewriting the main build list.
target_sources(radios_core PRIVATE src/core/preferences_storage.c)
target_sources(radios_audio PRIVATE src/audio/output_resampler.c)
target_sources(radios_ui PRIVATE src/ui/preferences.c src/ui/loudness_settings.c)

if(BUILD_TESTING)
    add_executable(test_preferences_output tests/test_preferences_output.c)
    target_link_libraries(test_preferences_output PRIVATE radios_audio radios_warnings m)
    add_test(NAME preferences_output COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_preferences_output.py $<TARGET_FILE:test_preferences_output>)
    set_tests_properties(preferences_output PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;G_DEBUG=fatal-warnings")
    add_executable(test_output_resampler tests/test_output_resampler.c)
    target_link_libraries(test_output_resampler PRIVATE radios_audio radios_warnings m)
    add_test(NAME output_resampler COMMAND test_output_resampler)
    set_tests_properties(output_resampler PROPERTIES TIMEOUT 30)
    add_executable(test_preferences_storage tests/test_preferences_storage.c)
    target_link_libraries(test_preferences_storage PRIVATE radios_core radios_warnings)
    add_test(NAME preferences_storage COMMAND test_preferences_storage)
    set_tests_properties(preferences_storage PROPERTIES TIMEOUT 20)
    if(RADIOS_GUI_TESTS)
        add_executable(test_preferences_ui tests/test_preferences_ui.c)
        target_link_libraries(test_preferences_ui PRIVATE radios_ui radios_warnings)
        add_test(NAME native_preferences COMMAND test_preferences_ui)
        set_tests_properties(native_preferences PROPERTIES TIMEOUT 45
            SKIP_RETURN_CODE 77 ENVIRONMENT "G_DEBUG=fatal-warnings")
    endif()
endif()

# Production LUFS scope controls, isolated preference adapter and private GTK display.
if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_loudness_settings tests/test_loudness_settings.c src/ui/loudness_settings.c)
    target_link_libraries(test_loudness_settings PRIVATE radios_core PkgConfig::GTK3 radios_warnings)
    add_test(NAME loudness_settings COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_loudness_settings>)
    set_tests_properties(loudness_settings PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple;NO_AT_BRIDGE=1")
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    if(NOT TARGET PkgConfig::XTST)
        pkg_check_modules(XTST REQUIRED IMPORTED_TARGET xtst x11)
    endif()
    add_executable(test_content_scale_ui tests/test_content_scale_ui.c)
    target_link_libraries(test_content_scale_ui PRIVATE radios_ui PkgConfig::XTST radios_warnings m)
    target_compile_definitions(test_content_scale_ui PRIVATE RADIOS_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME content_scale_ui COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_content_scale_ui.py $<TARGET_FILE:test_content_scale_ui>)
    set_tests_properties(content_scale_ui PROPERTIES TIMEOUT 110 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple;NO_AT_BRIDGE=1")
endif()

# Private real-file settings roundtrip, with document transport isolated.
if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_settings_sync tests/test_settings_sync.c src/ui/settings_sync.c)
    target_link_libraries(test_settings_sync PRIVATE radios_core PkgConfig::GTK3 radios_warnings)
    add_test(NAME settings_sync COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_rotation_editor_regressions.py $<TARGET_FILE:test_settings_sync>)
    set_tests_properties(settings_sync PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;G_DEBUG=fatal-warnings;GTK_IM_MODULE=gtk-im-context-simple;NO_AT_BRIDGE=1")
endif()
