add_library(chromatindb_relay_lib STATIC
  config/relay_config.cpp
  identity/relay_identity.cpp
)

target_include_directories(chromatindb_relay_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

target_link_libraries(chromatindb_relay_lib PUBLIC chromatindb_lib)
