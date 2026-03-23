#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace chromatindb::relay::config {

/// Relay configuration loaded from JSON file.
struct RelayConfig {
    std::string bind_address = "0.0.0.0";   ///< Listen address
    uint32_t bind_port = 4201;               ///< Listen port 1-65535 (separate from address per D-05)
    std::string uds_path;                    ///< UDS path to chromatindb node (required)
    std::string identity_key_path;           ///< Path to secret key file (required, per D-07)
    std::string log_level = "info";          ///< trace|debug|info|warn|error|critical
    std::string log_file;                    ///< Empty = console only
};

/// Load relay configuration from a JSON file.
/// Config file is required -- throws if missing (per D-04) or invalid JSON.
/// @throws std::runtime_error if file missing, unreadable, or invalid JSON.
RelayConfig load_relay_config(const std::filesystem::path& path);

/// Validate relay configuration with error accumulation.
/// Collects all errors and throws single runtime_error with all messages (per D-06).
/// @throws std::runtime_error if any validation fails.
void validate_relay_config(const RelayConfig& cfg);

} // namespace chromatindb::relay::config
