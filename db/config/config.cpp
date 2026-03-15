#include "db/config/config.h"
#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
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

    cfg.bind_address = j.value("bind_address", cfg.bind_address);
    cfg.storage_path = j.value("storage_path", cfg.storage_path);
    cfg.data_dir = j.value("data_dir", cfg.data_dir);
    cfg.log_level = j.value("log_level", cfg.log_level);
    cfg.max_peers = j.value("max_peers", cfg.max_peers);
    cfg.sync_interval_seconds = j.value("sync_interval_seconds", cfg.sync_interval_seconds);
    cfg.max_storage_bytes = j.value("max_storage_bytes", cfg.max_storage_bytes);
    cfg.rate_limit_bytes_per_sec = j.value("rate_limit_bytes_per_sec", cfg.rate_limit_bytes_per_sec);
    cfg.rate_limit_burst = j.value("rate_limit_burst", cfg.rate_limit_burst);

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

} // namespace chromatindb::config
