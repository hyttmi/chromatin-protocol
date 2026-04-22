#include "db/config/config.h"
#include "db/net/framing.h"
#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fstream>
#include <set>
#include <stdexcept>

namespace chromatindb::config {

Config load_config(const std::filesystem::path& path) {
    Config cfg;

    if (!std::filesystem::exists(path)) {
        return cfg;  // Return defaults if file doesn't exist
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return cfg;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error("Invalid JSON in config file '" + path.string() + "': " + e.what());
    }

    try {
        cfg.bind_address = j.value("bind_address", cfg.bind_address);
        cfg.storage_path = j.value("storage_path", cfg.storage_path);
        cfg.data_dir = j.value("data_dir", cfg.data_dir);
        cfg.log_level = j.value("log_level", cfg.log_level);
        cfg.max_peers = j.value("max_peers", cfg.max_peers);
        cfg.max_clients = j.value("max_clients", cfg.max_clients);
        cfg.max_ttl_seconds = j.value("max_ttl_seconds", cfg.max_ttl_seconds);
        cfg.max_subscriptions_per_connection = j.value("max_subscriptions_per_connection", cfg.max_subscriptions_per_connection);
        cfg.safety_net_interval_seconds = j.value("safety_net_interval_seconds", cfg.safety_net_interval_seconds);
        cfg.max_storage_bytes = j.value("max_storage_bytes", cfg.max_storage_bytes);
        cfg.blob_max_bytes = j.value("blob_max_bytes", cfg.blob_max_bytes);
        cfg.rate_limit_bytes_per_sec = j.value("rate_limit_bytes_per_sec", cfg.rate_limit_bytes_per_sec);
        cfg.rate_limit_burst = j.value("rate_limit_burst", cfg.rate_limit_burst);
        cfg.full_resync_interval = j.value("full_resync_interval", cfg.full_resync_interval);
        cfg.cursor_stale_seconds = j.value("cursor_stale_seconds", cfg.cursor_stale_seconds);
        cfg.namespace_quota_bytes = j.value("namespace_quota_bytes", cfg.namespace_quota_bytes);
        cfg.namespace_quota_count = j.value("namespace_quota_count", cfg.namespace_quota_count);
        cfg.worker_threads = j.value("worker_threads", cfg.worker_threads);
        cfg.sync_cooldown_seconds = j.value("sync_cooldown_seconds", cfg.sync_cooldown_seconds);
        cfg.max_sync_sessions = j.value("max_sync_sessions", cfg.max_sync_sessions);
        cfg.log_file = j.value("log_file", cfg.log_file);
        cfg.log_max_size_mb = j.value("log_max_size_mb", cfg.log_max_size_mb);
        cfg.log_max_files = j.value("log_max_files", cfg.log_max_files);
        cfg.log_format = j.value("log_format", cfg.log_format);
        cfg.compaction_interval_hours = j.value("compaction_interval_hours", cfg.compaction_interval_hours);
        cfg.uds_path = j.value("uds_path", cfg.uds_path);
        cfg.metrics_bind = j.value("metrics_bind", cfg.metrics_bind);
        cfg.blob_transfer_timeout = j.value("blob_transfer_timeout", cfg.blob_transfer_timeout);
        cfg.sync_timeout = j.value("sync_timeout", cfg.sync_timeout);
        cfg.pex_interval = j.value("pex_interval", cfg.pex_interval);
        cfg.strike_threshold = j.value("strike_threshold", cfg.strike_threshold);
        cfg.strike_cooldown = j.value("strike_cooldown", cfg.strike_cooldown);
    } catch (const nlohmann::json::type_error& e) {
        throw std::runtime_error(
            std::string("Config type error: ") + e.what() +
            " (check field types in config file)");
    }

    // Warn on unknown config keys (forward compatibility)
    static const std::set<std::string> known_keys = {
        "bind_address", "storage_path", "data_dir", "bootstrap_peers",
        "log_level", "max_peers", "max_clients", "max_ttl_seconds", "max_subscriptions_per_connection",
        "safety_net_interval_seconds", "max_storage_bytes", "blob_max_bytes",
        "rate_limit_bytes_per_sec", "rate_limit_burst", "sync_namespaces",
        "allowed_client_keys", "allowed_peer_keys", "trusted_peers", "full_resync_interval",
        "cursor_stale_seconds", "namespace_quota_bytes", "namespace_quota_count",
        "worker_threads", "sync_cooldown_seconds", "max_sync_sessions",
        "namespace_quotas", "log_file", "log_max_size_mb", "log_max_files",
        "log_format", "compaction_interval_hours",
        "uds_path", "metrics_bind",
        "blob_transfer_timeout", "sync_timeout", "pex_interval",
        "strike_threshold", "strike_cooldown"
    };
    for (const auto& [key, _] : j.items()) {
        if (known_keys.find(key) == known_keys.end()) {
            spdlog::warn("unknown config key '{}' (ignored)", key);
        }
    }

    if (j.contains("namespace_quotas") && j["namespace_quotas"].is_object()) {
        for (auto& [key, val] : j["namespace_quotas"].items()) {
            if (key.size() != 64) {
                throw std::runtime_error(
                    "Invalid namespace_quotas key '" + key +
                    "': expected 64 hex characters");
            }
            validate_allowed_keys({key});
            auto& entry = cfg.namespace_quotas[key];
            if (val.contains("max_bytes")) {
                entry.first = val["max_bytes"].get<uint64_t>();
            }
            if (val.contains("max_count")) {
                entry.second = val["max_count"].get<uint64_t>();
            }
        }
    }

    if (j.contains("bootstrap_peers") && j["bootstrap_peers"].is_array()) {
        for (const auto& peer : j["bootstrap_peers"]) {
            if (peer.is_string()) {
                cfg.bootstrap_peers.push_back(peer.get<std::string>());
            }
        }
    }

    if (j.contains("sync_namespaces") && j["sync_namespaces"].is_array()) {
        for (const auto& ns : j["sync_namespaces"]) {
            if (ns.is_string()) {
                cfg.sync_namespaces.push_back(ns.get<std::string>());
            }
        }
        validate_allowed_keys(cfg.sync_namespaces);  // Same 64-char hex format
    }

    if (j.contains("allowed_client_keys") && j["allowed_client_keys"].is_array()) {
        for (const auto& key : j["allowed_client_keys"]) {
            if (key.is_string()) {
                cfg.allowed_client_keys.push_back(key.get<std::string>());
            }
        }
        validate_allowed_keys(cfg.allowed_client_keys);
    }

    if (j.contains("allowed_peer_keys") && j["allowed_peer_keys"].is_array()) {
        for (const auto& key : j["allowed_peer_keys"]) {
            if (key.is_string()) {
                cfg.allowed_peer_keys.push_back(key.get<std::string>());
            }
        }
        validate_allowed_keys(cfg.allowed_peer_keys);
    }

    if (j.contains("trusted_peers") && j["trusted_peers"].is_array()) {
        for (const auto& peer : j["trusted_peers"]) {
            if (peer.is_string()) {
                cfg.trusted_peers.push_back(peer.get<std::string>());
            }
        }
        validate_trusted_peers(cfg.trusted_peers);
    }

    return cfg;
}

Config parse_args(int argc, const char* argv[], Config base) {
    Config cfg = base;
    std::filesystem::path config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--data-dir" && i + 1 < argc) {
            cfg.data_dir = argv[++i];
        } else if (arg == "--log-level" && i + 1 < argc) {
            cfg.log_level = argv[++i];
        } else {
            throw std::runtime_error("Unknown argument '" + arg +
                "' (run options: --config <path>, --data-dir <path>, --log-level <lvl>)");
        }
    }

    // If --config was provided, load that file first
    if (!config_path.empty()) {
        cfg = load_config(config_path);
        cfg.config_path = config_path;
        // Re-apply CLI overrides that came after --config
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--config") {
                ++i;  // skip value
            } else if (arg == "--data-dir" && i + 1 < argc) {
                cfg.data_dir = argv[++i];
            } else if (arg == "--log-level" && i + 1 < argc) {
                cfg.log_level = argv[++i];
            }
        }
    }

    return cfg;
}

void validate_allowed_keys(const std::vector<std::string>& keys) {
    for (const auto& key : keys) {
        if (key.size() != 64) {
            throw std::runtime_error(
                "Invalid allowed_key '" + key + "': expected 64 hex characters, got " +
                std::to_string(key.size()));
        }
        for (char c : key) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                throw std::runtime_error(
                    "Invalid allowed_key '" + key + "': non-hex character '" +
                    std::string(1, c) + "'");
            }
        }
    }
}

void validate_trusted_peers(const std::vector<std::string>& peers) {
    for (const auto& peer : peers) {
        asio::error_code ec;
        asio::ip::make_address(peer, ec);
        if (ec) {
            // Check if entry looks like it has a port (common misconfiguration)
            // IPv4 with port: "1.2.3.4:1234", IPv6 with port: "[::1]:1234"
            bool has_port = false;
            if (peer.find("]:") != std::string::npos) {
                has_port = true;  // IPv6 with port: [::1]:4200
            } else {
                // Check for IPv4 with port: last colon followed by digits only
                auto last_colon = peer.rfind(':');
                if (last_colon != std::string::npos) {
                    auto after = peer.substr(last_colon + 1);
                    if (!after.empty() && std::all_of(after.begin(), after.end(), ::isdigit)) {
                        // Try parsing the part before the colon as IPv4
                        asio::error_code ec2;
                        asio::ip::make_address(peer.substr(0, last_colon), ec2);
                        if (!ec2) {
                            has_port = true;
                        }
                    }
                }
            }

            if (has_port) {
                throw std::runtime_error(
                    "Invalid trusted_peer '" + peer +
                    "': not a valid IP address (if specifying a port, remove it "
                    "-- trusted_peers uses plain IPs)");
            } else {
                throw std::runtime_error(
                    "Invalid trusted_peer '" + peer + "': not a valid IP address");
            }
        }
    }
}

void validate_config(const Config& cfg, bool check_bind_address) {
    std::vector<std::string> errors;

    // Numeric range validation
    if (cfg.max_peers < 1) {
        errors.push_back("max_peers must be >= 1 (got " +
                          std::to_string(cfg.max_peers) + ")");
    }
    if (cfg.safety_net_interval_seconds < 3) {
        errors.push_back("safety_net_interval_seconds must be >= 3 (got " +
                          std::to_string(cfg.safety_net_interval_seconds) + ")");
    }
    if (cfg.max_storage_bytes != 0 && cfg.max_storage_bytes < 1048576) {
        errors.push_back("max_storage_bytes must be 0 (unlimited) or >= 1048576 (1 MiB) (got " +
                          std::to_string(cfg.max_storage_bytes) + ")");
    }
    // BLOB-02: blob_max_bytes bounds [1 MiB, MAX_BLOB_DATA_HARD_CEILING (64 MiB)]
    if (cfg.blob_max_bytes < 1048576ULL) {
        errors.push_back("blob_max_bytes must be >= 1048576 (1 MiB) (got " +
                          std::to_string(cfg.blob_max_bytes) + ")");
    }
    if (cfg.blob_max_bytes > chromatindb::net::MAX_BLOB_DATA_HARD_CEILING) {
        errors.push_back("blob_max_bytes must be <= " +
                          std::to_string(chromatindb::net::MAX_BLOB_DATA_HARD_CEILING) +
                          " (64 MiB hard ceiling) (got " +
                          std::to_string(cfg.blob_max_bytes) + ")");
    }
    if (cfg.rate_limit_bytes_per_sec != 0 && cfg.rate_limit_bytes_per_sec < 1024) {
        errors.push_back("rate_limit_bytes_per_sec must be 0 (disabled) or >= 1024 (got " +
                          std::to_string(cfg.rate_limit_bytes_per_sec) + ")");
    }
    if (cfg.rate_limit_bytes_per_sec > 0 && cfg.rate_limit_burst == 0) {
        errors.push_back("rate_limit_burst must be > 0 when rate limiting is enabled (got burst=0, rate=" +
                          std::to_string(cfg.rate_limit_bytes_per_sec) + ")");
    } else if (cfg.rate_limit_bytes_per_sec > 0 && cfg.rate_limit_burst < cfg.rate_limit_bytes_per_sec) {
        errors.push_back("rate_limit_burst must be >= rate_limit_bytes_per_sec when rate limiting is enabled (got burst=" +
                          std::to_string(cfg.rate_limit_burst) + ", rate=" +
                          std::to_string(cfg.rate_limit_bytes_per_sec) + ")");
    }
    if (cfg.full_resync_interval < 1) {
        errors.push_back("full_resync_interval must be >= 1 (got " +
                          std::to_string(cfg.full_resync_interval) + ")");
    }
    if (cfg.cursor_stale_seconds < 60) {
        errors.push_back("cursor_stale_seconds must be >= 60 (got " +
                          std::to_string(cfg.cursor_stale_seconds) + ")");
    }
    if (cfg.worker_threads > 256) {
        errors.push_back("worker_threads must be 0 (auto-detect) or 1-256 (got " +
                          std::to_string(cfg.worker_threads) + ")");
    }
    if (cfg.max_sync_sessions < 1) {
        errors.push_back("max_sync_sessions must be >= 1 (got " +
                          std::to_string(cfg.max_sync_sessions) + ")");
    }
    if (cfg.blob_transfer_timeout < 10 || cfg.blob_transfer_timeout > 86400) {
        errors.push_back("blob_transfer_timeout must be 10-86400 (got " +
                          std::to_string(cfg.blob_transfer_timeout) + ")");
    }
    if (cfg.sync_timeout < 5 || cfg.sync_timeout > 3600) {
        errors.push_back("sync_timeout must be 5-3600 (got " +
                          std::to_string(cfg.sync_timeout) + ")");
    }
    if (cfg.pex_interval < 10 || cfg.pex_interval > 86400) {
        errors.push_back("pex_interval must be 10-86400 (got " +
                          std::to_string(cfg.pex_interval) + ")");
    }
    if (cfg.strike_threshold < 1 || cfg.strike_threshold > 1000) {
        errors.push_back("strike_threshold must be 1-1000 (got " +
                          std::to_string(cfg.strike_threshold) + ")");
    }
    if (cfg.strike_cooldown > 86400) {
        errors.push_back("strike_cooldown must be 0-86400 (got " +
                          std::to_string(cfg.strike_cooldown) + ")");
    }

    // log_format validation
    static const std::set<std::string> valid_formats = {"text", "json"};
    if (valid_formats.find(cfg.log_format) == valid_formats.end()) {
        errors.push_back("log_format must be one of: text, json (got '" +
                          cfg.log_format + "')");
    }
    if (cfg.log_max_size_mb < 1) {
        errors.push_back("log_max_size_mb must be >= 1 (got " +
                          std::to_string(cfg.log_max_size_mb) + ")");
    }
    if (cfg.log_max_files < 1) {
        errors.push_back("log_max_files must be >= 1 (got " +
                          std::to_string(cfg.log_max_files) + ")");
    }
    // compaction_interval_hours: 0 = disabled, minimum 1 when enabled
    // (uint32_t guarantees non-zero values are >= 1, but document the intent)

    // uds_path validation: if non-empty, must be absolute and within sockaddr_un limit
    if (!cfg.uds_path.empty()) {
        if (cfg.uds_path[0] != '/') {
            errors.push_back("uds_path must be an absolute path (got '" +
                              cfg.uds_path + "')");
        }
        if (cfg.uds_path.size() > 107) {
            errors.push_back("uds_path too long (max 107 chars, got " +
                              std::to_string(cfg.uds_path.size()) + ")");
        }
    }

    // metrics_bind validation: if non-empty, must be host:port with valid port
    if (!cfg.metrics_bind.empty()) {
        auto colon = cfg.metrics_bind.rfind(':');
        if (colon == std::string::npos) {
            errors.push_back("metrics_bind must be host:port format (got '" +
                              cfg.metrics_bind + "')");
        } else {
            auto port_str = cfg.metrics_bind.substr(colon + 1);
            try {
                unsigned long port = std::stoul(port_str);
                if (port < 1 || port > 65535) {
                    errors.push_back("metrics_bind port must be 1-65535 (got " +
                                      std::to_string(port) + ")");
                }
            } catch (...) {
                errors.push_back("metrics_bind port is not a valid number (got '" +
                                  port_str + "')");
            }
        }
    }

    // log_level validation
    static const std::set<std::string> valid_levels = {
        "trace", "debug", "info", "warn", "warning", "error", "err", "critical"
    };
    if (valid_levels.find(cfg.log_level) == valid_levels.end()) {
        errors.push_back("log_level must be one of: trace, debug, info, warn, warning, error, err, critical (got '" +
                          cfg.log_level + "')");
    }

    // bind_address validation (skipped on SIGHUP reload -- bind_address is not
    // reloaded at runtime, so stale values or port 0 in the config file don't
    // matter; checking would just produce misleading reload failures.)
    if (check_bind_address) {
        auto colon_pos = cfg.bind_address.rfind(':');
        if (colon_pos == std::string::npos) {
            errors.push_back("bind_address must contain ':' separating host and port (got '" +
                              cfg.bind_address + "')");
        } else {
            auto port_str = cfg.bind_address.substr(colon_pos + 1);
            try {
                unsigned long port = std::stoul(port_str);
                if (port < 1 || port > 65535) {
                    errors.push_back("bind_address port must be 1-65535 (got " +
                                      std::to_string(port) + ")");
                }
            } catch (...) {
                errors.push_back("bind_address port is not a valid number (got '" +
                                  port_str + "')");
            }
        }
    }

    if (!errors.empty()) {
        std::string msg = "Configuration errors:\n";
        for (const auto& err : errors) {
            msg += "  - " + err + "\n";
        }
        throw std::runtime_error(msg);
    }
}

} // namespace chromatindb::config
