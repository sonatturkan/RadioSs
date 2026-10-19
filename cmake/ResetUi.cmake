# Ayarlar › Sıfırlama: the native page against an isolated genuine store
# (desktop-reset/preview and apply through the GTK dialogs).
if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_reset_ui tests/test_reset_ui.c)
    target_link_libraries(test_reset_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_reset_ui COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/run_data_ui.py $<TARGET_FILE:test_reset_ui>)
    set_tests_properties(native_reset_ui PROPERTIES TIMEOUT 240
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;G_DEBUG=fatal-warnings;GDK_BACKEND=x11")
endif()
