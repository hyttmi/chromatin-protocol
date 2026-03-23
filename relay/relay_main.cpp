#include "db/version.h"
#include "relay/config/relay_config.h"
#include "relay/identity/relay_identity.h"
#include "db/logging/logging.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>

namespace {

std::string to_hex(std::span<const uint8_t> bytes) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        result += hex_chars[(b >> 4) & 0xF];
        result += hex_chars[b & 0xF];
    }
    return result;
}

void print_usage(const char* prog) {
    std::cerr << "chromatindb_relay " << VERSION << "\n\n"
              << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  run       Start the relay\n"
              << "  keygen    Generate relay identity keypair\n"
              << "  version   Print version\n\n"
              << "Run options:\n"
              << "  --config <path>     JSON config file (required)\n\n"
              << "Keygen options:\n"
              << "  --output <path>     Secret key output path (writes <path> + sibling .pub)\n"
              << "  --force             Overwrite existing identity\n";
}

int cmd_version() {
    std::cout << "chromatindb_relay " << VERSION << std::endl;
    return 0;
}

int cmd_keygen(int argc, char* argv[]) {
    std::string output_path;
    bool force = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--force") {
            force = true;
        }
    }

    if (output_path.empty()) {
        std::cerr << "Error: --output <path> is required" << std::endl;
        return 1;
    }

    namespace fs = std::filesystem;
    auto key_path = fs::path(output_path);
    auto pub_path = chromatindb::relay::identity::pub_path_from_key_path(key_path);

    if (fs::exists(key_path) && !force) {
        std::cerr << "Identity already exists at " << key_path.string()
                  << ". Use --force to overwrite." << std::endl;
        return 1;
    }

    auto identity = chromatindb::relay::identity::RelayIdentity::generate();
    identity.save_to(key_path);

    std::cout << "Generated relay identity" << std::endl;
    std::cout << "Public key hash: " << to_hex(identity.public_key_hash()) << std::endl;

    return 0;
}

int cmd_run(int argc, char* argv[]) {
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    if (config_path.empty()) {
        std::cerr << "Error: --config <path> is required\n\n"
                  << "Usage: chromatindb_relay run --config <path>" << std::endl;
        return 1;
    }

    // Load and validate config
    chromatindb::relay::config::RelayConfig cfg;
    try {
        cfg = chromatindb::relay::config::load_relay_config(config_path);
    } catch (const std::runtime_error& e) {
        std::cerr << "Config error: " << e.what() << std::endl;
        return 1;
    }

    try {
        chromatindb::relay::config::validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        std::cerr << "Config validation error: " << e.what() << std::endl;
        return 1;
    }

    // Initialize logging
    chromatindb::logging::init(cfg.log_level, cfg.log_file);

    spdlog::info("chromatindb_relay {}", VERSION);
    spdlog::info("bind: {}:{}", cfg.bind_address, cfg.bind_port);
    spdlog::info("uds: {}", cfg.uds_path);

    // Load or generate identity
    auto identity = chromatindb::relay::identity::RelayIdentity::load_or_generate(cfg.identity_key_path);
    spdlog::info("public key hash: {}", to_hex(identity.public_key_hash()));

    spdlog::info("relay ready");

    return 0;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "run") return cmd_run(argc - 1, argv + 1);
    if (cmd == "keygen") return cmd_keygen(argc - 1, argv + 1);
    if (cmd == "version" || cmd == "--version" || cmd == "-v") return cmd_version();
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { print_usage(argv[0]); return 0; }

    std::cerr << "Unknown command: " << cmd << std::endl;
    print_usage(argv[0]);
    return 1;
}
