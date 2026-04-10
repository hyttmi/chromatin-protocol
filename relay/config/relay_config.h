#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace chromatindb::relay::config {

struct RelayConfig {
    std::string bind_address = "0.0.0.0";
    uint32_t bind_port = 4201;
    std::string uds_path;              // Required -- path to node's UDS
    std::string identity_key_path;     // Required -- path to relay .key file
    std::string log_level = "info";    // trace|debug|info|warn|error
    std::string log_file;              // Empty = console only
    uint32_t max_send_queue = 256;     // Per-client send queue cap
    std::string cert_path;             // TLS certificate chain file (PEM). Empty = plain WS mode.
    std::string key_path;              // TLS private key file (PEM). Empty = plain WS mode.
    uint32_t max_connections = 1024;                  // Per D-32: SIGHUP-reloadable
    std::vector<std::string> allowed_client_keys;     // Per D-34: array of 64-char hex namespace hashes
    std::string metrics_bind;                          // Empty=disabled, "host:port"=enabled (per D-02)
    uint32_t rate_limit_messages_per_sec = 0;          // 0=disabled (per D-10)

    /// Returns true when both cert_path and key_path are set (WSS mode).
    bool tls_enabled() const { return !cert_path.empty() && !key_path.empty(); }
};

/// Load relay config from JSON file.
/// @throws std::runtime_error if file not found, invalid JSON, or missing required fields.
RelayConfig load_relay_config(const std::filesystem::path& path);

/// Validate config field values.
/// @throws std::runtime_error if any validation fails.
void validate_relay_config(const RelayConfig& cfg);

} // namespace chromatindb::relay::config
