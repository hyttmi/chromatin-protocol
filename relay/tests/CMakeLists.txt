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
  test_session.cpp
  test_relay_config.cpp
  test_type_registry.cpp
  test_ws_frame.cpp
  test_ws_handshake.cpp
  test_base64.cpp
  test_transport_codec.cpp
  test_aead.cpp
  test_request_router.cpp
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
