cmake_minimum_required(VERSION 3.20)

# =============================================================================
# Relay tools (UDS tap + WebSocket smoke test)
# =============================================================================

add_executable(relay_uds_tap relay_uds_tap.cpp)
target_link_libraries(relay_uds_tap PRIVATE chromatindb_relay_lib)

add_executable(relay_smoke_test relay_smoke_test.cpp)
target_link_libraries(relay_smoke_test PRIVATE chromatindb_relay_lib)
