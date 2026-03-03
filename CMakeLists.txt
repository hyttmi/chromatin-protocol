cmake_minimum_required(VERSION 3.20)
project(chromatindb LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Dependencies via FetchContent
# =============================================================================
include(FetchContent)

# -- liboqs 0.15.0 (ML-DSA-87, ML-KEM-1024, SHA3-256)
set(OQS_BUILD_ONLY_LIB ON CACHE BOOL "" FORCE)
set(OQS_MINIMAL_BUILD "" CACHE STRING "" FORCE)
FetchContent_Declare(liboqs
  GIT_REPOSITORY https://github.com/open-quantum-safe/liboqs.git
  GIT_TAG        0.15.0
  GIT_SHALLOW    TRUE
)

# -- libsodium via cmake wrapper (ChaCha20-Poly1305 AEAD, HKDF-SHA256)
set(SODIUM_DISABLE_TESTS ON CACHE BOOL "" FORCE)
FetchContent_Declare(sodium
  GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)

# -- FlatBuffers 25.2.10 (wire format)
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
FetchContent_Declare(flatbuffers
  GIT_REPOSITORY https://github.com/google/flatbuffers.git
  GIT_TAG        v25.2.10
  GIT_SHALLOW    TRUE
)

# -- Catch2 v3 (testing framework)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
  GIT_SHALLOW    TRUE
)

# -- spdlog 1.15.1 (structured logging)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.15.1
  GIT_SHALLOW    TRUE
)

# -- nlohmann/json 3.11.3 (config parsing) - URL download (fast, ~200KB)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)

# -- libmdbx v0.13.11 (persistent key-value storage)
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

FetchContent_MakeAvailable(liboqs sodium flatbuffers Catch2 spdlog json libmdbx)

# liboqs copies headers to its build dir; expose that path for oqs/sha3.h etc.
FetchContent_GetProperties(liboqs)
target_include_directories(oqs PUBLIC
  $<BUILD_INTERFACE:${liboqs_BINARY_DIR}/include>
)

# =============================================================================
# FlatBuffers schema compilation
# =============================================================================
set(FLATBUFFERS_SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/schemas)
set(FLATBUFFERS_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/wire)

# Will be enabled once schemas exist
if(EXISTS ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs)
  add_custom_command(
    OUTPUT ${FLATBUFFERS_GENERATED_DIR}/blob_generated.h
    COMMAND flatc --cpp --gen-object-api
            -o ${FLATBUFFERS_GENERATED_DIR}
            ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs
    DEPENDS ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs flatc
    COMMENT "Generating FlatBuffers blob header"
  )
  add_custom_target(flatbuffers_generated DEPENDS ${FLATBUFFERS_GENERATED_DIR}/blob_generated.h)
endif()

# =============================================================================
# Library target
# =============================================================================
add_library(chromatindb_lib STATIC
  src/crypto/hash.cpp
  src/crypto/signing.cpp
  src/crypto/kem.cpp
  src/crypto/aead.cpp
  src/crypto/kdf.cpp
  src/wire/codec.cpp
  src/config/config.cpp
  src/logging/logging.cpp
  src/identity/identity.cpp
  src/storage/storage.cpp
)

target_include_directories(chromatindb_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(chromatindb_lib PUBLIC
  oqs
  sodium
  flatbuffers
  spdlog::spdlog
  nlohmann_json::nlohmann_json
  mdbx-static
)

# =============================================================================
# Test target
# =============================================================================
add_executable(chromatindb_tests
  tests/crypto/test_hash.cpp
  tests/crypto/test_signing.cpp
  tests/crypto/test_kem.cpp
  tests/crypto/test_aead.cpp
  tests/crypto/test_kdf.cpp
  tests/wire/test_codec.cpp
  tests/config/test_config.cpp
  tests/identity/test_identity.cpp
  tests/storage/test_storage.cpp
)

target_link_libraries(chromatindb_tests PRIVATE
  chromatindb_lib
  Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(chromatindb_tests)
