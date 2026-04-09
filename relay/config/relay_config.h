#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace chromatindb::relay::config {

struct RelayConfig {
    std::string bind_address = "0.0.0.0";
    uint32_t bind_port = 4201;
    std::string uds_path;              // Required -- path to node's UDS
    std::string identity_key_path;     // Required -- path to relay .key file
    std::string log_level = "info";    // trace|debug|info|warn|error
    std::string log_file;              // Empty = console only
    uint32_t max_send_queue = 256;     // Per-client send queue cap
};

/// Load relay config from JSON file.
/// @throws std::runtime_error if file not found, invalid JSON, or missing required fields.
RelayConfig load_relay_config(const std::filesystem::path& path);

/// Validate config field values.
/// @throws std::runtime_error if any validation fails.
void validate_relay_config(const RelayConfig& cfg);

} // namespace chromatindb::relay::config
