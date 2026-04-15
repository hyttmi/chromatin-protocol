cmake_minimum_required(VERSION 3.20)
project(chromatindb-cli LANGUAGES C CXX)

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
set(OQS_USE_OPENSSL OFF CACHE BOOL "" FORCE)
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

# -- libsodium (system, via pkg-config)
find_package(PkgConfig REQUIRED)
pkg_check_modules(sodium REQUIRED IMPORTED_TARGET libsodium)

# -- FlatBuffers 25.2.10
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
FetchContent_Declare(flatbuffers
  GIT_REPOSITORY https://github.com/google/flatbuffers.git
  GIT_TAG        v25.2.10
  GIT_SHALLOW    TRUE
)

# -- Standalone Asio 1.38.0
set(ASIO_TAG "asio-1-38-0" CACHE STRING "" FORCE)
FetchContent_Declare(asiocmake
  GIT_REPOSITORY https://github.com/OlivierLDff/asio.cmake
  GIT_TAG        main
  GIT_SHALLOW    TRUE
)

# -- spdlog 1.15.1
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.15.1
  GIT_SHALLOW    TRUE
)

# -- nlohmann/json 3.11.3
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)

FetchContent_MakeAvailable(liboqs flatbuffers asiocmake spdlog json)

# liboqs copies headers to its build dir; expose that path for oqs/sha3.h etc.
FetchContent_GetProperties(liboqs)
target_include_directories(oqs PUBLIC
  $<BUILD_INTERFACE:${liboqs_BINARY_DIR}/include>
)

# =============================================================================
# CLI executable
# =============================================================================
add_executable(chromatindb-cli
  src/main.cpp
  src/identity.cpp
)

target_include_directories(chromatindb-cli PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

target_compile_features(chromatindb-cli PUBLIC cxx_std_20)

target_link_libraries(chromatindb-cli PRIVATE
  oqs
  PkgConfig::sodium
  flatbuffers
  asio::asio
  spdlog::spdlog
  nlohmann_json::nlohmann_json
)

# =============================================================================
# Tests
# =============================================================================
include(CTest)

if(BUILD_TESTING)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(Catch2)

  add_subdirectory(tests)
endif()
