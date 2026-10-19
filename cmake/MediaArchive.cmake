# Ayarlar › Medya arşivi: archive folders, storage locations and the native page.
if(BUILD_TESTING)
    add_executable(test_media_archive tests/test_media_archive.c)
    target_link_libraries(test_media_archive PRIVATE radios_core radios_warnings)
    add_test(NAME media_archive COMMAND test_media_archive)
    set_tests_properties(media_archive PROPERTIES TIMEOUT 60)
    add_executable(test_storage_locations tests/test_storage_locations.c)
    target_link_libraries(test_storage_locations PRIVATE radios_core radios_warnings)
    add_test(NAME storage_locations COMMAND test_storage_locations)
    set_tests_properties(storage_locations PROPERTIES TIMEOUT 30)
endif()
if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    add_executable(test_media_archive_ui tests/test_media_archive_ui.c)
    target_link_libraries(test_media_archive_ui PRIVATE radios_ui radios_warnings)
    add_test(NAME native_media_archive COMMAND ${RADIOS_TEST_PYTHON_COMMAND}
        ${CMAKE_SOURCE_DIR}/tests/run_data_ui.py $<TARGET_FILE:test_media_archive_ui>)
    set_tests_properties(native_media_archive PROPERTIES TIMEOUT 240
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()
