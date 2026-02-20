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
