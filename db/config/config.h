#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace chromatindb::config {

/// Node configuration loaded from JSON file and/or CLI arguments.
struct Config {
    std::string bind_address = "0.0.0.0:4200";
    std::string storage_path = "./data/blobs";
    std::string data_dir = "./data";
    std::vector<std::string> bootstrap_peers;
    std::string log_level = "info";
    uint32_t max_peers = 32;
    uint32_t sync_interval_seconds = 60;
    uint64_t max_storage_bytes = 0;                 // 0 = unlimited (no capacity limit)
    uint64_t rate_limit_bytes_per_sec = 0;          // 0 = disabled (no rate limiting)
    uint64_t rate_limit_burst = 0;                  // Burst capacity in bytes (0 = disabled)
    std::vector<std::string> sync_namespaces;       // Hex namespace hashes to replicate (empty = all)
    std::vector<std::string> allowed_keys;          // Hex namespace hashes (64 chars each)
    std::vector<std::string> trusted_peers;         // IP addresses for lightweight handshake
    uint32_t full_resync_interval = 10;             // Full resync every Nth sync round
    uint64_t cursor_stale_seconds = 3600;           // Force full resync after this gap (seconds)
    uint64_t namespace_quota_bytes = 0;             // Global default byte limit (0 = unlimited)
    uint64_t namespace_quota_count = 0;             // Global default count limit (0 = unlimited)
    uint32_t worker_threads = 0;                    // Thread pool size (0 = auto-detect: hardware_concurrency)
    // Per-namespace overrides: key is 64-char hex namespace hash.
    // Value: {optional max_bytes, optional max_count}
    // If optional has value: that value overrides global (0 = explicitly exempt)
    // If optional has no value: inherit from global default
    std::map<std::string, std::pair<std::optional<uint64_t>, std::optional<uint64_t>>> namespace_quotas;
    std::filesystem::path config_path;              // Path to config file (for SIGHUP reload)
};

/// Load configuration from a JSON file.
/// Returns default Config if file doesn't exist.
/// @throws std::runtime_error if file exists but is invalid JSON.
Config load_config(const std::filesystem::path& path);

/// Parse CLI arguments and override base config.
/// Supports: --config <path>, --data-dir <path>, --log-level <level>
Config parse_args(int argc, const char* argv[], Config base = {});

/// Validate allowed_keys: each must be exactly 64 hex characters.
/// @throws std::runtime_error if any key is malformed.
void validate_allowed_keys(const std::vector<std::string>& keys);

/// Validate trusted_peers: each must be a valid IP address (no ports).
/// @throws std::runtime_error if any entry is malformed.
void validate_trusted_peers(const std::vector<std::string>& peers);

} // namespace chromatindb::config
