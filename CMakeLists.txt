cmake_minimum_required(VERSION 3.16)
project(helix
    VERSION 0.1.0
    DESCRIPTION "Helix - Decentralized relay node for encrypted messaging"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(HELIX_BUILD_TESTS "Build tests" ON)

# Dependencies
find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# OpenDHT
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENDHT REQUIRED opendht)

# spdlog
find_package(spdlog REQUIRED)

# nlohmann/json
find_package(nlohmann_json REQUIRED)

# liboqs
find_package(liboqs REQUIRED)

# libmdbx
pkg_check_modules(MDBX REQUIRED libmdbx)

# Helix server
add_executable(helixd
    src/main.cpp
    src/config.cpp
    src/node.cpp
    src/storage.cpp
    src/dht.cpp
    src/api.cpp
    src/auth.cpp
    src/relay.cpp
    src/log.cpp
)

target_include_directories(helixd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OPENDHT_INCLUDE_DIRS}
    ${MDBX_INCLUDE_DIRS}
)

target_link_libraries(helixd PRIVATE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    ${OPENDHT_LIBRARIES}
    ${MDBX_LIBRARIES}
    oqs
)

target_compile_options(helixd PRIVATE
    -Wall -Wextra -Wpedantic
    ${OPENDHT_CFLAGS_OTHER}
    ${MDBX_CFLAGS_OTHER}
)

# Tests
if(HELIX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
