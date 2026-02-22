cmake_minimum_required(VERSION 3.20)
project(chromatin-node VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# --- liboqs (PQ crypto) ---
FetchContent_Declare(
    liboqs
    GIT_REPOSITORY https://github.com/open-quantum-safe/liboqs.git
    GIT_TAG        0.15.0
    GIT_SHALLOW    ON
)
set(OQS_BUILD_ONLY_LIB ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# Build ML-DSA-87 (signing) and ML-KEM-1024 (TCP transport encryption).
# SHA3 is part of liboqs common code and always available.
set(OQS_ENABLE_KEM_BIKE            OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_CLASSIC_MCELIECE OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_FRODOKEM        OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_HQC             OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_ML_KEM          ON  CACHE BOOL "" FORCE)
set(OQS_ENABLE_KEM_NTRUPRIME       OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_SIG_ML_DSA          ON  CACHE BOOL "" FORCE)
set(OQS_ENABLE_SIG_FALCON          OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_SIG_SPHINCS         OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_SIG_MAYO            OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_SIG_CROSS           OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_SIG_UOV             OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_SIG_SNOVA           OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_SIG_STFL_LMS        OFF CACHE BOOL "" FORCE)
set(OQS_ENABLE_SIG_STFL_XMSS       OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(liboqs)

# --- libmdbx (storage) ---
FetchContent_Declare(
    libmdbx
    GIT_REPOSITORY https://github.com/erthink/libmdbx.git
    GIT_TAG        v0.13.11
    GIT_SHALLOW    ON
)
set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(MDBX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MDBX_BUILD_CXX ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libmdbx)

# --- spdlog (logging) ---
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.0
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(spdlog)

# --- jsoncpp (JSON) ---
FetchContent_Declare(
    jsoncpp
    GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
    GIT_TAG        1.9.6
    GIT_SHALLOW    ON
)
set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(jsoncpp)

# --- uSockets (networking, no SSL) ---
FetchContent_Declare(
    usockets
    GIT_REPOSITORY https://github.com/uNetworking/uSockets.git
    GIT_TAG        v0.8.8
    GIT_SHALLOW    ON
    GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(usockets)

file(GLOB USOCKETS_SRC
    ${usockets_SOURCE_DIR}/src/*.c
    ${usockets_SOURCE_DIR}/src/eventing/*.c
    ${usockets_SOURCE_DIR}/src/crypto/*.c
)
add_library(uSockets STATIC ${USOCKETS_SRC})
target_include_directories(uSockets PUBLIC ${usockets_SOURCE_DIR}/src)
target_compile_definitions(uSockets
    PUBLIC LIBUS_NO_SSL
    PRIVATE LIBUS_USE_EPOLL
)

# --- uWebSockets (header-only WebSocket) ---
FetchContent_Declare(
    uwebsockets
    GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
    GIT_TAG        v20.74.0
    GIT_SHALLOW    ON
    GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(uwebsockets)

add_library(uWebSockets INTERFACE)
target_include_directories(uWebSockets INTERFACE ${uwebsockets_SOURCE_DIR}/src)
target_compile_definitions(uWebSockets INTERFACE UWS_NO_ZLIB)
target_link_libraries(uWebSockets INTERFACE uSockets)

# --- GoogleTest ---
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    ON
)
FetchContent_MakeAvailable(googletest)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(sodium REQUIRED IMPORTED_TARGET libsodium)

add_subdirectory(src)
enable_testing()
add_subdirectory(tests)
add_subdirectory(tests/integration)

# --- Install: config and systemd ---
include(GNUInstallDirs)
# Only install config if it doesn't already exist (preserve user edits)
install(CODE "
    if(NOT EXISTS \"/etc/chromatin/config.json\")
        file(INSTALL \"${CMAKE_CURRENT_SOURCE_DIR}/dist/config.json\"
             DESTINATION \"/etc/chromatin\")
    else()
        message(STATUS \"Skipping /etc/chromatin/config.json (already exists)\")
    endif()
" COMPONENT config)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/dist/chromatin-node.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/chromatin-node.service
    @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/chromatin-node.service
        DESTINATION /usr/lib/systemd/system
        COMPONENT systemd)
