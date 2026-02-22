add_library(chromatin-core STATIC)
target_sources(chromatin-core PRIVATE
    crypto/crypto.cpp
    crypto/kem.cpp
    crypto/aead.cpp
    storage/storage.cpp
    kademlia/node_id.cpp
    kademlia/routing_table.cpp
    kademlia/tcp_transport.cpp
    kademlia/tcp_encryption.cpp
    kademlia/kademlia.cpp
    replication/repl_log.cpp
    config/config.cpp
    ws/ws_server.cpp
)
target_include_directories(chromatin-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# liboqs SHA3 internal headers are only exposed via the build include dir
target_include_directories(chromatin-core PRIVATE ${liboqs_BINARY_DIR}/include)
target_link_libraries(chromatin-core PUBLIC
    oqs
    mdbx-static
    jsoncpp_static
    spdlog::spdlog
    uWebSockets
    PkgConfig::sodium
    Threads::Threads
)

add_executable(chromatin-node main.cpp)
target_link_libraries(chromatin-node PRIVATE chromatin-core)

# --- Install rules ---
include(GNUInstallDirs)
install(TARGETS chromatin-node RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
