add_library(helix-core STATIC)
target_sources(helix-core PRIVATE
    placeholder.cpp
)
target_include_directories(helix-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(helix-core PUBLIC
    oqs
    mdbx-static
    jsoncpp_static
    spdlog::spdlog
    Threads::Threads
)

add_executable(helix-node main.cpp)
target_link_libraries(helix-node PRIVATE helix-core)
