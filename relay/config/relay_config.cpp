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
    cfg.cert_path = j.value("cert_path", cfg.cert_path);
    cfg.key_path = j.value("key_path", cfg.key_path);
    cfg.max_connections = j.value("max_connections", cfg.max_connections);

    if (j.contains("allowed_client_keys")) {
        cfg.allowed_client_keys = j.at("allowed_client_keys").get<std::vector<std::string>>();
    }

    cfg.metrics_bind = j.value("metrics_bind", cfg.metrics_bind);
    cfg.rate_limit_messages_per_sec = j.value("rate_limit_messages_per_sec", cfg.rate_limit_messages_per_sec);
    cfg.request_timeout_seconds = j.value("request_timeout_seconds", cfg.request_timeout_seconds);

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

    // TLS fields: both or neither
    bool has_cert = !cfg.cert_path.empty();
    bool has_key = !cfg.key_path.empty();
    if (has_cert != has_key) {
        errors += "cert_path and key_path must both be set or both be empty. ";
    }

    // If both set, verify files exist (per D-01: must load or refuse to start)
    if (has_cert && has_key) {
        if (!std::filesystem::exists(cfg.cert_path)) {
            errors += "cert_path file not found: " + cfg.cert_path + ". ";
        }
        if (!std::filesystem::exists(cfg.key_path)) {
            errors += "key_path file not found: " + cfg.key_path + ". ";
        }
    }

    if (!cfg.metrics_bind.empty()) {
        if (cfg.metrics_bind.rfind(':') == std::string::npos) {
            errors += "metrics_bind must be 'host:port' format, got '" + cfg.metrics_bind + "'. ";
        }
    }

    if (cfg.max_connections < 1) {
        errors += "max_connections must be >= 1, got " + std::to_string(cfg.max_connections) + ". ";
    }

    // Validate allowed_client_keys: each entry must be exactly 64 hex chars
    for (size_t i = 0; i < cfg.allowed_client_keys.size(); ++i) {
        const auto& key = cfg.allowed_client_keys[i];
        if (key.size() != 64) {
            errors += "allowed_client_keys[" + std::to_string(i) +
                      "] must be 64 hex chars, got " + std::to_string(key.size()) + ". ";
            continue;
        }
        for (char c : key) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                errors += "allowed_client_keys[" + std::to_string(i) +
                          "] contains non-hex character '" + std::string(1, c) + "'. ";
                break;
            }
        }
    }

    if (!errors.empty()) {
        throw std::runtime_error("Config validation failed: " + errors);
    }
}

} // namespace chromatindb::relay::config
