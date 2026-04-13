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
  test_relay_config.cpp
  test_type_registry.cpp
  test_write_tracker.cpp
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
# HTTP parser tests (Plan 999.9-01)
# ===========================================================================
add_executable(test_http_parser
  test_http_parser.cpp
)
target_link_libraries(test_http_parser PRIVATE
  chromatindb_relay_lib
  Catch2::Catch2WithMain
)
catch_discover_tests(test_http_parser)

# ===========================================================================
# Token store tests (Plan 999.9-01)
# ===========================================================================
add_executable(test_token_store
  test_token_store.cpp
)
target_link_libraries(test_token_store PRIVATE
  chromatindb_relay_lib
  Catch2::Catch2WithMain
)
catch_discover_tests(test_token_store)

# ===========================================================================
# HTTP router tests (Plan 999.9-03)
# ===========================================================================
add_executable(test_http_router
  test_http_router.cpp
)
target_link_libraries(test_http_router PRIVATE
  chromatindb_relay_lib
  Catch2::Catch2WithMain
)
catch_discover_tests(test_http_router)

# ===========================================================================
# Data handler tests (Plan 999.9-05)
# ===========================================================================
add_executable(test_handlers_data
  test_handlers_data.cpp
)
target_link_libraries(test_handlers_data PRIVATE
  chromatindb_relay_lib
  Catch2::Catch2WithMain
)
catch_discover_tests(test_handlers_data)

# ===========================================================================
# Query handler tests (Plan 999.9-06)
# ===========================================================================
add_executable(test_handlers_query
  test_handlers_query.cpp
)
target_link_libraries(test_handlers_query PRIVATE
  chromatindb_relay_lib
  Catch2::Catch2WithMain
)
catch_discover_tests(test_handlers_query)

# ===========================================================================
# Response promise tests (Plan 999.9-04)
# ===========================================================================
add_executable(test_response_promise
  test_response_promise.cpp
)
target_link_libraries(test_response_promise PRIVATE
  chromatindb_relay_lib
  Catch2::Catch2WithMain
)
catch_discover_tests(test_response_promise)

# ===========================================================================
# SSE writer tests (Plan 999.9-07)
# ===========================================================================
add_executable(test_sse_writer
  test_sse_writer.cpp
)
target_link_libraries(test_sse_writer PRIVATE
  chromatindb_relay_lib
  Catch2::Catch2WithMain
)
catch_discover_tests(test_sse_writer)
