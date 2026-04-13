cmake_minimum_required(VERSION 3.20)

# =============================================================================
# Relay tools (UDS tap)
# =============================================================================

add_executable(relay_uds_tap relay_uds_tap.cpp)
target_link_libraries(relay_uds_tap PRIVATE chromatindb_relay_lib)
