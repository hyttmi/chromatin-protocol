cmake_minimum_required(VERSION 3.20)
project(chromatindb LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE Debug)

# =============================================================================
# Optional sanitizer support
# =============================================================================
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# =============================================================================
# Dependencies via FetchContent
# =============================================================================
include(FetchContent)

# -- liboqs 0.15.0 (ML-DSA-87, ML-KEM-1024, SHA3-256)
# Strip all unused algorithms to reduce build time and binary size.
set(OQS_BUILD_ONLY_LIB ON CACHE BOOL "" FORCE)
set(OQS_MINIMAL_BUILD "" CACHE STRING "" FORCE)

# Disable all KEM algorithms except ML-KEM (used for ML-KEM-1024)
set(OQS_ENABLE_KEM_BIKE OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_FRODOKEM OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_NTRUPRIME OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_NTRU OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_CLASSIC_MCELIECE OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_HQC OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_KYBER OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_ML_KEM ON CACHE BOOL "" FORCE)

# Disable all signature algorithms except ML-DSA (used for ML-DSA-87)
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

# -- Standalone Asio 1.38.0 (async networking, C++20 coroutines)
set(ASIO_TAG "asio-1-38-0" CACHE STRING "" FORCE)
FetchContent_Declare(asiocmake
  GIT_REPOSITORY https://github.com/OlivierLDff/asio.cmake
  GIT_TAG        main
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(liboqs sodium flatbuffers spdlog json libmdbx asiocmake)

# liboqs copies headers to its build dir; expose that path for oqs/sha3.h etc.
FetchContent_GetProperties(liboqs)
target_include_directories(oqs PUBLIC
  $<BUILD_INTERFACE:${liboqs_BINARY_DIR}/include>
)

# =============================================================================
# Library component (guards in db/CMakeLists.txt skip re-fetching)
# =============================================================================
include(CTest)
add_subdirectory(db)

# =============================================================================
# Daemon binary
# =============================================================================
add_executable(chromatindb db/main.cpp)
target_link_libraries(chromatindb PRIVATE chromatindb_lib)

# =============================================================================
# Benchmark binary
# =============================================================================
add_executable(chromatindb_bench bench/bench_main.cpp)
target_link_libraries(chromatindb_bench PRIVATE chromatindb_lib)

# =============================================================================
# Load generator binary
# =============================================================================
add_executable(chromatindb_loadgen loadgen/loadgen_main.cpp)
target_link_libraries(chromatindb_loadgen PRIVATE chromatindb_lib)
