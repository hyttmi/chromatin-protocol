# Test target for chromatindb-cli
# Included from cli/CMakeLists.txt via add_subdirectory(tests)

add_executable(cli_tests
  test_identity.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/identity.cpp
)

target_include_directories(cli_tests PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
)

target_compile_features(cli_tests PUBLIC cxx_std_20)

target_link_libraries(cli_tests PRIVATE
  oqs
  PkgConfig::sodium
  flatbuffers
  asio::asio
  spdlog::spdlog
  nlohmann_json::nlohmann_json
  Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(cli_tests)
