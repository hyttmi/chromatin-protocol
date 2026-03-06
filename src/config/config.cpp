#include "config/config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

namespace chromatin::config {

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

    if (j.contains("bootstrap_peers") && j["bootstrap_peers"].is_array()) {
        for (const auto& peer : j["bootstrap_peers"]) {
            if (peer.is_string()) {
                cfg.bootstrap_peers.push_back(peer.get<std::string>());
            }
        }
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

} // namespace chromatin::config
