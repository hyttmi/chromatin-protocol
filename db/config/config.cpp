#include "db/config/config.h"
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
        cfg.sync_interval_seconds = j.value("sync_interval_seconds", cfg.sync_interval_seconds);
        cfg.max_storage_bytes = j.value("max_storage_bytes", cfg.max_storage_bytes);
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
    } catch (const nlohmann::json::type_error& e) {
        throw std::runtime_error(
            std::string("Config type error: ") + e.what() +
            " (check field types in config file)");
    }

    // Warn on unknown config keys (forward compatibility)
    static const std::set<std::string> known_keys = {
        "bind_address", "storage_path", "data_dir", "bootstrap_peers",
        "log_level", "max_peers", "sync_interval_seconds", "max_storage_bytes",
        "rate_limit_bytes_per_sec", "rate_limit_burst", "sync_namespaces",
        "allowed_keys", "trusted_peers", "full_resync_interval",
        "cursor_stale_seconds", "namespace_quota_bytes", "namespace_quota_count",
        "worker_threads", "sync_cooldown_seconds", "max_sync_sessions",
        "namespace_quotas", "log_file", "log_max_size_mb", "log_max_files",
        "log_format"
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

    if (j.contains("allowed_keys") && j["allowed_keys"].is_array()) {
        for (const auto& key : j["allowed_keys"]) {
            if (key.is_string()) {
                cfg.allowed_keys.push_back(key.get<std::string>());
            }
        }
        validate_allowed_keys(cfg.allowed_keys);
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

void validate_config(const Config& cfg) {
    std::vector<std::string> errors;

    // Numeric range validation
    if (cfg.max_peers < 1) {
        errors.push_back("max_peers must be >= 1 (got " +
                          std::to_string(cfg.max_peers) + ")");
    }
    if (cfg.sync_interval_seconds < 1) {
        errors.push_back("sync_interval_seconds must be >= 1 (got " +
                          std::to_string(cfg.sync_interval_seconds) + ")");
    }
    if (cfg.max_storage_bytes != 0 && cfg.max_storage_bytes < 1048576) {
        errors.push_back("max_storage_bytes must be 0 (unlimited) or >= 1048576 (1 MiB) (got " +
                          std::to_string(cfg.max_storage_bytes) + ")");
    }
    if (cfg.rate_limit_bytes_per_sec != 0 && cfg.rate_limit_bytes_per_sec < 1024) {
        errors.push_back("rate_limit_bytes_per_sec must be 0 (disabled) or >= 1024 (got " +
                          std::to_string(cfg.rate_limit_bytes_per_sec) + ")");
    }
    if (cfg.rate_limit_bytes_per_sec > 0 && cfg.rate_limit_burst < cfg.rate_limit_bytes_per_sec) {
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

    // log_level validation
    static const std::set<std::string> valid_levels = {
        "trace", "debug", "info", "warn", "warning", "error", "err", "critical"
    };
    if (valid_levels.find(cfg.log_level) == valid_levels.end()) {
        errors.push_back("log_level must be one of: trace, debug, info, warn, warning, error, err, critical (got '" +
                          cfg.log_level + "')");
    }

    // bind_address validation
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

    if (!errors.empty()) {
        std::string msg = "Configuration errors:\n";
        for (const auto& err : errors) {
            msg += "  - " + err + "\n";
        }
        throw std::runtime_error(msg);
    }
}

} // namespace chromatindb::config
