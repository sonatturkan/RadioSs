# Native song timeline from prompt/4/4.pdf, page 1 / figure 1.
target_sources(radios_ui PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src/ui/neon_progress.c"
    "${CMAKE_CURRENT_LIST_DIR}/../src/ui/header_progress.c")

if(BUILD_TESTING)
    add_executable(test_song_seek "${CMAKE_CURRENT_LIST_DIR}/../tests/test_song_seek.c")
    target_link_libraries(test_song_seek PRIVATE radios_audio radios_warnings m)
    target_compile_definitions(test_song_seek PRIVATE RADIOS_TEST_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
    add_test(NAME song_seek COMMAND test_song_seek)
    set_tests_properties(song_seek PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING AND RADIOS_GUI_TESTS)
    # Keep the widget/controller tests independent of service and audio I/O.
    add_executable(test_neon_progress
        "${CMAKE_CURRENT_LIST_DIR}/../tests/test_neon_progress.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/neon_progress.c")
    target_include_directories(test_neon_progress PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_link_libraries(test_neon_progress PRIVATE PkgConfig::GTK3 radios_warnings m)
    add_test(NAME native_neon_progress COMMAND test_neon_progress)
    set_tests_properties(native_neon_progress PROPERTIES TIMEOUT 30
        ENVIRONMENT "G_DEBUG=fatal-warnings;GDK_BACKEND=x11")

    add_executable(test_header_progress
        "${CMAKE_CURRENT_LIST_DIR}/../tests/test_header_progress.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/header_progress.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/neon_progress.c")
    target_include_directories(test_header_progress PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_link_libraries(test_header_progress PRIVATE
        PkgConfig::GTK3 PkgConfig::JSONGLIB radios_warnings m)
    add_test(NAME native_header_progress COMMAND test_header_progress)
    set_tests_properties(native_header_progress PROPERTIES TIMEOUT 30
        ENVIRONMENT "G_DEBUG=fatal-warnings;GDK_BACKEND=x11")
endif()

