#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "crypto/crypto.h"

namespace chromatin::config {

// Protocol constants — must be identical across all nodes.
// Changing these is a breaking protocol change.
namespace protocol {
    constexpr uint16_t REPLICATION_FACTOR     = 3;
    constexpr uint8_t  CONTACT_POW_DIFFICULTY = 16;
    constexpr uint8_t  NAME_POW_DIFFICULTY    = 2;
    constexpr uint64_t MAX_MESSAGE_SIZE       = 50ULL * 1024 * 1024;  // 50 MiB
    constexpr uint32_t MAX_PROFILE_SIZE       = 1024 * 1024;           // 1 MiB
    constexpr uint32_t MAX_REQUEST_BLOB_SIZE  = 64 * 1024;             // 64 KiB
    constexpr uint32_t TTL_DAYS               = 7;

    // Profile sub-field limits
    constexpr uint32_t MAX_BIO_SIZE              = 2048;              // 2 KiB
    constexpr uint32_t MAX_AVATAR_SIZE           = 256 * 1024;        // 256 KiB
    constexpr uint8_t  MAX_SOCIAL_LINKS          = 16;
    constexpr uint8_t  MAX_SOCIAL_PLATFORM_LENGTH = 64;
    constexpr uint8_t  MAX_SOCIAL_HANDLE_LENGTH  = 128;
}

// Operational defaults — safe to differ per node, tuning only.
namespace defaults {
    // Routing
    constexpr uint16_t MAX_ROUTING_TABLE_SIZE = 256;
    constexpr uint16_t MAX_NODES_PER_SUBNET   = 3;

    // Timeouts (seconds)
    constexpr uint16_t TCP_CONNECT_TIMEOUT = 5;
    constexpr uint16_t TCP_READ_TIMEOUT    = 5;
    constexpr uint16_t WS_IDLE_TIMEOUT     = 120;
    constexpr uint16_t UPLOAD_TIMEOUT      = 30;

    // Worker pool
    constexpr uint16_t WORKER_POOL_THREADS   = 4;
    constexpr uint16_t WORKER_POOL_QUEUE_MAX = 1024;

    // Rate limiter
    constexpr double RATE_LIMIT_TOKENS = 50.0;
    constexpr double RATE_LIMIT_MAX    = 50.0;
    constexpr double RATE_LIMIT_REFILL = 10.0;

    // Maintenance
    constexpr uint32_t COMPACT_INTERVAL_MINUTES = 60;
    constexpr uint32_t COMPACT_KEEP_ENTRIES     = 10000;
    constexpr uint32_t COMPACT_MIN_AGE_HOURS    = 168;  // 7 days

    // Sync
    constexpr uint16_t SYNC_INTERVAL_SECONDS = 120;
    constexpr uint16_t SYNC_BATCH_SIZE       = 10;

    // TCP transport
    constexpr uint16_t MAX_TCP_CLIENTS = 256;

    // WebSocket
    constexpr uint16_t MAX_WS_CONNECTIONS_PER_IP = 10;

    // Connection pool
    constexpr uint16_t CONN_POOL_MAX          = 64;
    constexpr uint16_t CONN_POOL_IDLE_SECONDS = 60;

    // Integrity sweep
    constexpr uint32_t INTEGRITY_SWEEP_INTERVAL_HOURS = 6;
    constexpr size_t   INTEGRITY_SWEEP_BATCH_SIZE     = 100;

    // Storage
    constexpr uint64_t MDBX_MAX_SIZE = 1ULL << 30;  // 1 GB
}

// Minimal per-deployment config.  Everything else is hardcoded.
struct Config {
    std::filesystem::path data_dir = ".";
    std::string bind = "0.0.0.0";
    uint16_t tcp_port = 4000;
    uint16_t ws_port = 4001;
    std::vector<std::pair<std::string, uint16_t>> bootstrap;
    std::string external_address;     // routable address (empty = use bind)
    std::string tls_cert;             // path to PEM certificate (empty = no TLS)
    std::string tls_key;              // path to PEM private key (empty = no TLS)
};

// Load config from JSON file. Throws std::runtime_error on parse failure
// or invalid values. Missing fields use defaults.
Config load_config(const std::filesystem::path& path);

// Validate config values. Throws std::runtime_error on invalid values.
void validate_config(const Config& cfg);

// Write a template config file with sensible defaults.
void generate_default_config(const std::filesystem::path& path);

// Load or generate keypair from node.key in data_dir.
// Returns the keypair (generating and saving if the file doesn't exist).
crypto::KeyPair load_or_generate_keypair(const std::filesystem::path& data_dir);

} // namespace chromatin::config
