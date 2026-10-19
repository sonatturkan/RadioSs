# Ayarlar › Medya arşivi › Radyo verileri: the workstation INI reader and the
# data service's config/storage.ini handling (tools/radios_database.py). The
# page and its core tests live in MediaArchive.cmake.
if(BUILD_TESTING)
    add_executable(test_local_ini tests/test_local_ini.c)
    target_link_libraries(test_local_ini PRIVATE radios_core radios_warnings)
    add_test(NAME local_ini COMMAND test_local_ini)
    set_tests_properties(local_ini PROPERTIES TIMEOUT 30 ENVIRONMENT "G_DEBUG=fatal-warnings")
    add_test(NAME storage_locations_service COMMAND ${Python3_EXECUTABLE}
             ${CMAKE_SOURCE_DIR}/tests/test_storage_locations.py)
    set_tests_properties(storage_locations_service PROPERTIES TIMEOUT 120
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()
