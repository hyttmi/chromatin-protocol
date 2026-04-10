cmake_minimum_required(VERSION 3.20)

# =============================================================================
# Relay static library
# =============================================================================
add_library(chromatindb_relay_lib STATIC
  asio_ssl.cpp
  config/relay_config.cpp
  core/authenticator.cpp
  core/message_filter.cpp
  core/request_router.cpp
  core/session.cpp
  core/subscription_tracker.cpp
  core/uds_multiplexer.cpp
  wire/blob_codec.cpp
  identity/relay_identity.cpp
  translate/type_registry.cpp
  translate/json_schema.cpp
  translate/translator.cpp
  util/base64.cpp
  wire/transport_codec.cpp
  wire/aead.cpp
  ws/ws_frame.cpp
  ws/ws_handshake.cpp
  ws/ws_session.cpp
  ws/ws_acceptor.cpp
  ws/session_manager.cpp
)

target_include_directories(chromatindb_relay_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

# =============================================================================
# Dependencies (guarded -- reuse if already fetched by root/db CMake)
# =============================================================================
include(FetchContent)

# -- Standalone Asio
if(NOT TARGET asio::asio)
  set(ASIO_TAG "asio-1-38-0" CACHE STRING "" FORCE)
  FetchContent_Declare(asiocmake_relay
    GIT_REPOSITORY https://github.com/OlivierLDff/asio.cmake
    GIT_TAG        main
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(asiocmake_relay)
endif()

# -- spdlog
if(NOT TARGET spdlog::spdlog)
  set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(spdlog_relay
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(spdlog_relay)
endif()

# -- nlohmann/json
if(NOT TARGET nlohmann_json::nlohmann_json)
  FetchContent_Declare(json_relay
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  )
  FetchContent_MakeAvailable(json_relay)
endif()

# -- FlatBuffers (TransportMessage codec)
if(NOT TARGET flatbuffers)
  FetchContent_Declare(flatbuffers_relay
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v25.2.10
    GIT_SHALLOW    TRUE
  )
  set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
  set(FLATBUFFERS_BUILD_FLATC OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(flatbuffers_relay)
endif()

# -- liboqs (ML-DSA-87 identity)
if(NOT TARGET oqs)
  set(OQS_BUILD_ONLY_LIB ON CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_ML_DSA ON CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_FALCON OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_SPHINCS OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_MAYO OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_CROSS OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_UOV OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_SNOVA OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_SLH_DSA OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(liboqs_relay
    GIT_REPOSITORY https://github.com/open-quantum-safe/liboqs.git
    GIT_TAG        0.15.0
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(liboqs_relay)
  FetchContent_GetProperties(liboqs_relay)
  target_include_directories(oqs PUBLIC
    $<BUILD_INTERFACE:${liboqs_relay_BINARY_DIR}/include>
  )
endif()

# =============================================================================
# System OpenSSL (TLS termination + SHA-1/Base64 for WebSocket handshake)
# =============================================================================
find_package(OpenSSL 3.3 REQUIRED)

target_link_libraries(chromatindb_relay_lib PUBLIC
  asio::asio
  spdlog::spdlog
  nlohmann_json::nlohmann_json
  flatbuffers
  oqs
  OpenSSL::SSL
  OpenSSL::Crypto
)

target_compile_features(chromatindb_relay_lib PUBLIC cxx_std_20)

# =============================================================================
# Tests
# =============================================================================
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
