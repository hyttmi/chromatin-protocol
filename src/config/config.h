#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "crypto/crypto.h"

namespace chromatin::config {

struct Config {
    std::filesystem::path data_dir = ".";
    std::string bind = "0.0.0.0";
    uint16_t tcp_port = 4000;
    uint16_t ws_port = 4001;
    std::vector<std::pair<std::string, uint16_t>> bootstrap;

    // TLS: both must be set, or both empty (empty = plaintext WS)
    std::string tls_cert_path;  // PEM certificate file path
    std::string tls_key_path;   // PEM private key file path

    // Network
    std::string external_address;     // routable address (empty = use bind)
    uint16_t replication_factor = 3;
    uint16_t max_routing_table_size = 256;

    // Timeouts (seconds)
    uint16_t tcp_connect_timeout = 5;
    uint16_t tcp_read_timeout = 5;
    uint16_t ws_idle_timeout = 120;
    uint16_t upload_timeout = 30;

    // Worker pool
    uint16_t worker_pool_threads = 4;
    uint16_t worker_pool_queue_max = 1024;

    // Rate limiter
    double rate_limit_tokens = 50.0;
    double rate_limit_max = 50.0;
    double rate_limit_refill = 10.0;

    // Size limits
    uint64_t max_message_size = 50ULL * 1024 * 1024;  // 50 MiB
    uint32_t max_profile_size = 1024 * 1024;           // 1 MiB
    uint32_t max_request_blob_size = 64 * 1024;        // 64 KiB

    // TTL & maintenance
    uint32_t ttl_days = 7;
    uint32_t compact_interval_minutes = 60;
    uint32_t compact_keep_entries = 10000;

    // PoW
    uint8_t contact_pow_difficulty = 16;
    uint8_t name_pow_difficulty = 28;

    // Sync
    uint16_t sync_interval_seconds = 120;
    uint16_t sync_batch_size = 10;

    // TCP transport
    uint16_t max_tcp_clients = 256;

    // Connection pool
    uint16_t conn_pool_max = 64;
    uint16_t conn_pool_idle_seconds = 60;

    // Storage
    uint64_t mdbx_max_size = 1ULL << 30;  // 1 GB
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
