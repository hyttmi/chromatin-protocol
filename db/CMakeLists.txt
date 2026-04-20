cmake_minimum_required(VERSION 3.20)
project(chromatindb-core LANGUAGES C CXX)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/version.h
    @ONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# =============================================================================
# Dependencies (guarded for composability)
# =============================================================================

# -- liboqs 0.15.0 (ML-DSA-87, ML-KEM-1024, SHA3-256)
# Strip all unused algorithms to reduce build time and binary size.
if(NOT TARGET oqs)
  set(OQS_BUILD_ONLY_LIB ON CACHE BOOL "" FORCE)
  set(OQS_MINIMAL_BUILD "" CACHE STRING "" FORCE)
  set(OQS_ENABLE_KEM_BIKE OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_FRODOKEM OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_NTRUPRIME OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_NTRU OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_CLASSIC_MCELIECE OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_HQC OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_KYBER OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_ML_KEM ON CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_ML_DSA ON CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_FALCON OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_SPHINCS OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_MAYO OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_CROSS OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_UOV OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_SNOVA OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_SLH_DSA OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(liboqs
    GIT_REPOSITORY https://github.com/open-quantum-safe/liboqs.git
    GIT_TAG        0.15.0
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(liboqs)
  FetchContent_GetProperties(liboqs)
  target_include_directories(oqs PUBLIC
    $<BUILD_INTERFACE:${liboqs_BINARY_DIR}/include>
  )
endif()

# -- libsodium via cmake wrapper (ChaCha20-Poly1305 AEAD, HKDF-SHA256)
if(NOT TARGET sodium)
  set(SODIUM_DISABLE_TESTS ON CACHE BOOL "" FORCE)
  FetchContent_Declare(sodium
    GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(sodium)
endif()

# -- FlatBuffers 25.2.10 (wire format + codegen)
if(NOT TARGET flatbuffers)
  set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "" FORCE)
  set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v25.2.10
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(flatbuffers)
endif()

# -- spdlog 1.15.1 (structured logging)
if(NOT TARGET spdlog::spdlog)
  set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(spdlog)
endif()

# -- nlohmann/json 3.11.3 (config parsing) - URL download (fast, ~200KB)
if(NOT TARGET nlohmann_json::nlohmann_json)
  FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  )
  FetchContent_MakeAvailable(json)
endif()

# -- libmdbx v0.13.11 (persistent key-value storage)
if(NOT TARGET mdbx-static)
  set(MDBX_BUILD_CXX ON CACHE BOOL "" FORCE)
  set(MDBX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(MDBX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
  set(MDBX_INSTALL_STATIC OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(libmdbx
    GIT_REPOSITORY https://github.com/erthink/libmdbx.git
    GIT_TAG        v0.13.11
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(libmdbx)
endif()

# -- Standalone Asio 1.38.0 (async networking, C++20 coroutines)
if(NOT TARGET asio::asio)
  set(ASIO_TAG "asio-1-38-0" CACHE STRING "" FORCE)
  FetchContent_Declare(asiocmake
    GIT_REPOSITORY https://github.com/OlivierLDff/asio.cmake
    GIT_TAG        main
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(asiocmake)
endif()

# =============================================================================
# FlatBuffers schema compilation
# =============================================================================
set(FLATBUFFERS_SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/schemas)
set(FLATBUFFERS_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/wire)

if(EXISTS ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs)
  add_custom_command(
    OUTPUT ${FLATBUFFERS_GENERATED_DIR}/blob_generated.h
    COMMAND flatc --cpp --gen-object-api
            -o ${FLATBUFFERS_GENERATED_DIR}
            ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs
    DEPENDS ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs flatc
    COMMENT "Generating FlatBuffers blob header"
  )
  add_custom_target(flatbuffers_blob_generated DEPENDS ${FLATBUFFERS_GENERATED_DIR}/blob_generated.h)
endif()

if(EXISTS ${FLATBUFFERS_SCHEMA_DIR}/transport.fbs)
  add_custom_command(
    OUTPUT ${FLATBUFFERS_GENERATED_DIR}/transport_generated.h
    COMMAND flatc --cpp --gen-object-api
            -o ${FLATBUFFERS_GENERATED_DIR}
            ${FLATBUFFERS_SCHEMA_DIR}/transport.fbs
    DEPENDS ${FLATBUFFERS_SCHEMA_DIR}/transport.fbs flatc
    COMMENT "Generating FlatBuffers transport header"
  )
  add_custom_target(flatbuffers_transport_generated DEPENDS ${FLATBUFFERS_GENERATED_DIR}/transport_generated.h)
endif()

# =============================================================================
# Library target
# =============================================================================
add_library(chromatindb_lib STATIC
  crypto/hash.cpp
  crypto/signing.cpp
  crypto/kem.cpp
  crypto/aead.cpp
  crypto/kdf.cpp
  crypto/master_key.cpp
  wire/codec.cpp
  config/config.cpp
  logging/logging.cpp
  identity/identity.cpp
  storage/storage.cpp
  engine/engine.cpp
  net/framing.cpp
  net/protocol.cpp
  net/handshake.cpp
  net/connection.cpp
  net/server.cpp
  net/uds_acceptor.cpp
  sync/sync_protocol.cpp
  sync/reconciliation.cpp
  peer/peer_manager.cpp
  peer/metrics_collector.cpp
  peer/pex_manager.cpp
  peer/connection_manager.cpp
  peer/blob_push_manager.cpp
  peer/sync_orchestrator.cpp
  peer/message_dispatcher.cpp
  acl/access_control.cpp
)

target_include_directories(chromatindb_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

target_link_libraries(chromatindb_lib PUBLIC
  oqs
  sodium
  flatbuffers
  spdlog::spdlog
  nlohmann_json::nlohmann_json
  mdbx-static
  asio::asio
)

# =============================================================================
# Standalone build support
# =============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(CTest)
endif()

# =============================================================================
# Test target
# =============================================================================
if(BUILD_TESTING)
  # -- Catch2 v3.7.1 (testing framework)
  if(NOT TARGET Catch2::Catch2WithMain)
    FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG        v3.7.1
      GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
  endif()

  add_executable(chromatindb_tests
    tests/acl/test_access_control.cpp
    tests/config/test_config.cpp
    tests/crypto/test_aead.cpp
    tests/crypto/test_hash.cpp
    tests/crypto/test_kdf.cpp
    tests/crypto/test_kem.cpp
    tests/crypto/test_master_key.cpp
    tests/crypto/test_signing.cpp
    tests/engine/test_engine.cpp
    tests/engine/test_verify_signer_hint.cpp
    tests/identity/test_identity.cpp
    tests/net/test_connection.cpp
    tests/net/test_framing.cpp
    tests/net/test_handshake.cpp
    tests/net/test_protocol.cpp
    tests/net/test_server.cpp
    tests/net/test_uds.cpp
    tests/peer/test_peer_manager.cpp
    tests/peer/test_event_expiry.cpp
    tests/peer/test_keepalive.cpp
    tests/peer/test_metrics_endpoint.cpp
    tests/peer/test_blob_push.cpp
    tests/storage/test_owner_pubkeys.cpp
    tests/storage/test_storage.cpp
    tests/storage/test_storage_concurrency_tsan.cpp
    tests/sync/test_reconciliation.cpp
    tests/sync/test_sync_protocol.cpp
    tests/wire/test_codec.cpp
    tests/test_daemon.cpp
    tests/test_schema_phase122.cpp
    tests/util/test_endian.cpp
    tests/util/test_blob_helpers.cpp
    tests/util/test_address.cpp
    tests/util/test_storage_thread_check.cpp
    tests/net/test_auth_helpers.cpp
    tests/crypto/test_verify_helpers.cpp
  )

  target_link_libraries(chromatindb_tests PRIVATE
    chromatindb_lib
    Catch2::Catch2WithMain
  )

  include(Catch)
  catch_discover_tests(chromatindb_tests)
endif()
