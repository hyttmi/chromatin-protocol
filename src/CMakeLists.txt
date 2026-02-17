add_library(helix-core STATIC)
target_sources(helix-core PRIVATE
    crypto/crypto.cpp
    storage/storage.cpp
    kademlia/node_id.cpp
    kademlia/routing_table.cpp
    kademlia/udp_transport.cpp
)
target_include_directories(helix-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# liboqs SHA3 internal headers are only exposed via the build include dir
target_include_directories(helix-core PRIVATE ${liboqs_BINARY_DIR}/include)
target_link_libraries(helix-core PUBLIC
    oqs
    mdbx-static
    jsoncpp_static
    spdlog::spdlog
    Threads::Threads
)

add_executable(helix-node main.cpp)
target_link_libraries(helix-node PRIVATE helix-core)
