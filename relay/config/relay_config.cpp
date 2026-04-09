#include "relay/config/relay_config.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

namespace chromatindb::relay::config {

RelayConfig load_relay_config(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Config file not found: " + path.string());
    }

    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Cannot open config file: " + path.string());
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error("Invalid JSON in config file: " + std::string(e.what()));
    }

    RelayConfig cfg;

    // Required fields -- throw if missing
    if (!j.contains("uds_path")) {
        throw std::runtime_error("Missing required config field: uds_path");
    }
    cfg.uds_path = j.at("uds_path").get<std::string>();

    if (!j.contains("identity_key_path")) {
        throw std::runtime_error("Missing required config field: identity_key_path");
    }
    cfg.identity_key_path = j.at("identity_key_path").get<std::string>();

    // Optional fields with defaults
    cfg.bind_address = j.value("bind_address", cfg.bind_address);
    cfg.bind_port = j.value("bind_port", cfg.bind_port);
    cfg.log_level = j.value("log_level", cfg.log_level);
    cfg.log_file = j.value("log_file", cfg.log_file);
    cfg.max_send_queue = j.value("max_send_queue", cfg.max_send_queue);

    return cfg;
}

void validate_relay_config(const RelayConfig& cfg) {
    std::string errors;

    if (cfg.bind_port == 0 || cfg.bind_port > 65535) {
        errors += "bind_port must be in [1, 65535], got " + std::to_string(cfg.bind_port) + ". ";
    }

    if (cfg.uds_path.empty()) {
        errors += "uds_path must not be empty. ";
    }

    if (cfg.identity_key_path.empty()) {
        errors += "identity_key_path must not be empty. ";
    }

    if (!errors.empty()) {
        throw std::runtime_error("Config validation failed: " + errors);
    }
}

} // namespace chromatindb::relay::config
