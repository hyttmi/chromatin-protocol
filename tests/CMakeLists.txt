add_executable(helix-tests)
target_sources(helix-tests PRIVATE
    test_crypto.cpp
    test_storage.cpp
    test_node_id.cpp
    test_routing_table.cpp
    test_udp_transport.cpp
    test_kademlia.cpp
    test_repl_log.cpp
)
target_link_libraries(helix-tests PRIVATE
    helix-core
    GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(helix-tests)
