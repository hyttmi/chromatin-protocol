#include "relay/config/relay_config.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fstream>
#include <set>
#include <stdexcept>

namespace chromatindb::relay::config {

RelayConfig load_relay_config(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Config file not found: " + path.string());
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Config file not readable: " + path.string());
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error("Invalid JSON in config file '" + path.string() + "': " + e.what());
    }

    RelayConfig cfg;
    try {
        cfg.bind_address = j.value("bind_address", cfg.bind_address);
        cfg.bind_port = j.value("bind_port", cfg.bind_port);
        cfg.uds_path = j.value("uds_path", cfg.uds_path);
        cfg.identity_key_path = j.value("identity_key_path", cfg.identity_key_path);
        cfg.log_level = j.value("log_level", cfg.log_level);
        cfg.log_file = j.value("log_file", cfg.log_file);
    } catch (const nlohmann::json::type_error& e) {
        throw std::runtime_error(
            std::string("Relay config type error: ") + e.what() +
            " (check field types in config file)");
    }

    // Warn on unknown config keys
    static const std::set<std::string> known_keys = {
        "bind_address", "bind_port", "uds_path",
        "identity_key_path", "log_level", "log_file"
    };
    for (const auto& [key, _] : j.items()) {
        if (known_keys.find(key) == known_keys.end()) {
            spdlog::warn("unknown relay config key '{}' (ignored)", key);
        }
    }

    return cfg;
}

void validate_relay_config(const RelayConfig& cfg) {
    std::vector<std::string> errors;

    // bind_address must not be empty
    if (cfg.bind_address.empty()) {
        errors.push_back("bind_address must not be empty");
    }

    // bind_port must be 1-65535
    if (cfg.bind_port < 1 || cfg.bind_port > 65535) {
        errors.push_back("bind_port must be 1-65535 (got " +
                          std::to_string(cfg.bind_port) + ")");
    }

    // uds_path: required, must start with '/', max 107 chars
    if (cfg.uds_path.empty()) {
        errors.push_back("uds_path must not be empty (required)");
    } else {
        if (cfg.uds_path[0] != '/') {
            errors.push_back("uds_path must be an absolute path starting with '/' (got '" +
                              cfg.uds_path + "')");
        }
        if (cfg.uds_path.size() > 107) {
            errors.push_back("uds_path too long (max 107 chars, got " +
                              std::to_string(cfg.uds_path.size()) + ")");
        }
    }

    // identity_key_path: required, must end with ".key"
    if (cfg.identity_key_path.empty()) {
        errors.push_back("identity_key_path must not be empty (required)");
    } else if (cfg.identity_key_path.size() < 4 ||
               cfg.identity_key_path.substr(cfg.identity_key_path.size() - 4) != ".key") {
        errors.push_back("identity_key_path must end with '.key' (got '" +
                          cfg.identity_key_path + "')");
    }

    // log_level: must be a valid spdlog level
    static const std::set<std::string> valid_levels = {
        "trace", "debug", "info", "warn", "warning", "error", "err", "critical"
    };
    if (valid_levels.find(cfg.log_level) == valid_levels.end()) {
        errors.push_back("log_level must be one of: trace, debug, info, warn, warning, error, err, critical (got '" +
                          cfg.log_level + "')");
    }

    // log_file: no validation (empty = console only)

    if (!errors.empty()) {
        std::string msg = "Relay configuration errors:\n";
        for (const auto& err : errors) {
            msg += "  - " + err + "\n";
        }
        throw std::runtime_error(msg);
    }
}

} // namespace chromatindb::relay::config
