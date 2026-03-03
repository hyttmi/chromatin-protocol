#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace chromatin::config {

/// Node configuration loaded from JSON file and/or CLI arguments.
struct Config {
    std::string bind_address = "0.0.0.0:4200";
    std::string storage_path = "./data/blobs";
    std::string data_dir = "./data";
    std::vector<std::string> bootstrap_peers;
    uint32_t default_ttl = 604800;  // 7 days in seconds
    std::string log_level = "info";
};

/// Load configuration from a JSON file.
/// Returns default Config if file doesn't exist.
/// @throws std::runtime_error if file exists but is invalid JSON.
Config load_config(const std::filesystem::path& path);

/// Parse CLI arguments and override base config.
/// Supports: --config <path>, --data-dir <path>, --log-level <level>
Config parse_args(int argc, const char* argv[], Config base = {});

} // namespace chromatin::config
