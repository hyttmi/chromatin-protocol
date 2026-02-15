find_package(GTest REQUIRED)

add_executable(helix_tests
    test_storage.cpp
)

target_include_directories(helix_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(helix_tests PRIVATE
    GTest::gtest_main
    ${MDBX_LIBRARIES}
)

gtest_discover_tests(helix_tests)
