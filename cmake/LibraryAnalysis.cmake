# Ayarlar › Kütüphane analizi: the core settings/runner test is registered next
# to the other core tests in CMakeLists.txt; this is the native page.
if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_library_analysis_ui tests/test_library_analysis_ui.c)
    target_link_libraries(test_library_analysis_ui PRIVATE radios_ui radios_warnings)
    radios_add_gui_test(NAME native_library_analysis COMMAND test_library_analysis_ui)
    set_tests_properties(native_library_analysis PROPERTIES TIMEOUT 120
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()
