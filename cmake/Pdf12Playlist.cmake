# Acceptance of prompt/12 uses the genuine isolated backend, never user data.
if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    foreach(component playlist drop)
        if(EXISTS "${CMAKE_SOURCE_DIR}/tests/test_prompt12_${component}_ui.c")
            add_executable(test_prompt12_${component}_ui tests/test_prompt12_${component}_ui.c)
            target_link_libraries(test_prompt12_${component}_ui PRIVATE radios_ui radios_warnings)
            add_test(NAME native_prompt12_${component}
                COMMAND ${RADIOS_TEST_PYTHON_COMMAND} ${CMAKE_SOURCE_DIR}/tests/run_data_ui.py
                    $<TARGET_FILE:test_prompt12_${component}_ui>)
            set_tests_properties(native_prompt12_${component} PROPERTIES TIMEOUT 120
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                ENVIRONMENT "G_DEBUG=fatal-warnings;RADIOS_P12_SOURCE_ROOT=${CMAKE_SOURCE_DIR}")
        endif()
    endforeach()
endif()
