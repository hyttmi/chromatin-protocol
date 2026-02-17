add_executable(helix-tests)
target_sources(helix-tests PRIVATE
    placeholder_test.cpp
)
target_link_libraries(helix-tests PRIVATE
    helix-core
    GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(helix-tests)
