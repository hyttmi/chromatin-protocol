# Fetch Catch2 if not already available
if(NOT TARGET Catch2::Catch2WithMain)
  include(FetchContent)
  FetchContent_Declare(catch2_relay
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(catch2_relay)
endif()

add_executable(chromatindb_relay_tests
  test_authenticator.cpp
  test_message_filter.cpp
  test_metrics_collector.cpp
  test_rate_limiter.cpp
  test_session.cpp
  test_relay_config.cpp
  test_type_registry.cpp
  test_write_tracker.cpp
  test_ws_frame.cpp
  test_ws_handshake.cpp
  test_base64.cpp
  test_transport_codec.cpp
  test_aead.cpp
  test_request_router.cpp
  test_subscription_tracker.cpp
  test_translator.cpp
)

target_link_libraries(chromatindb_relay_tests PRIVATE
  chromatindb_relay_lib
  Catch2::Catch2WithMain
)

# Ensure Catch2 CMake module path is available
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(chromatindb_relay_tests)

# ===========================================================================
# HTTP parser tests (standalone target for Plan 999.9-01)
# ===========================================================================
add_executable(test_http_parser
  test_http_parser.cpp
)
target_link_libraries(test_http_parser PRIVATE
  chromatindb_relay_lib
  Catch2::Catch2WithMain
)
catch_discover_tests(test_http_parser)
