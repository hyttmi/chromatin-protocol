add_executable(helix-tests)
target_sources(helix-tests PRIVATE
    test_crypto.cpp
)
target_link_libraries(helix-tests PRIVATE
    helix-core
    GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(helix-tests)
