add_executable(chromatin-tests)
target_sources(chromatin-tests PRIVATE
    test_crypto.cpp
    test_kem.cpp
    test_aead.cpp
    test_storage.cpp
    test_node_id.cpp
    test_routing_table.cpp
    test_tcp_transport.cpp
    test_kademlia.cpp
    test_repl_log.cpp
    test_config.cpp
    test_worker_pool.cpp
    test_ws_server.cpp
)
target_link_libraries(chromatin-tests PRIVATE
    chromatin-core
    GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(chromatin-tests)

# Performance benchmark (not a test — standalone binary)
add_executable(chromatin-bench bench_kademlia.cpp)
target_link_libraries(chromatin-bench PRIVATE chromatin-core)
