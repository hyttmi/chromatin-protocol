#include "db/version.h"
#include "db/acl/access_control.h"
#include "db/config/config.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/logging/logging.h"
#include "db/peer/peer_manager.h"
#include "db/storage/storage.h"

#include <asio.hpp>
#include <spdlog/spdlog.h>

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
    std::cerr << "chromatindb " << VERSION << "\n\n"
              << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  run       Start the daemon\n"
              << "  keygen    Generate identity keypair\n"
              << "  version   Print version\n\n"
              << "Run options:\n"
              << "  --config <path>     JSON config file\n"
              << "  --data-dir <path>   Data directory (default: ./data)\n"
              << "  --log-level <lvl>   Log level: trace|debug|info|warn|error (default: info)\n\n"
              << "Keygen options:\n"
              << "  --data-dir <path>   Data directory (default: ./data)\n"
              << "  --force             Overwrite existing identity\n";
}

int cmd_version() {
    std::cout << "chromatindb " << VERSION << std::endl;
    return 0;
}

int cmd_keygen(int argc, char* argv[]) {
    std::string data_dir = "./data";
    bool force = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--force") {
            force = true;
        }
    }

    namespace fs = std::filesystem;
    auto key_path = fs::path(data_dir) / "node.key";
    auto pub_path = fs::path(data_dir) / "node.pub";

    if (fs::exists(key_path) && !force) {
        std::cerr << "Identity already exists at " << data_dir
                  << "/. Use --force to overwrite." << std::endl;
        return 1;
    }

    fs::create_directories(data_dir);

    auto identity = chromatindb::identity::NodeIdentity::generate();
    identity.save_to(data_dir);

    std::cout << "Generated identity at " << data_dir << "/" << std::endl;
    std::cout << "Namespace: " << to_hex(identity.namespace_id()) << std::endl;

    return 0;
}

int cmd_run(int argc, char* argv[]) {
    // Parse args
    std::vector<const char*> args;
    args.push_back("chromatindb");
    for (int i = 1; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    auto config = chromatindb::config::parse_args(
        static_cast<int>(args.size()), args.data());

    // Initialize logging
    chromatindb::logging::init(config.log_level);

    spdlog::info("chromatindb {}", VERSION);
    spdlog::info("bind: {}", config.bind_address);
    spdlog::info("data: {}", config.data_dir);

    // Create/load identity
    auto identity = chromatindb::identity::NodeIdentity::load_or_generate(config.data_dir);
    spdlog::info("namespace: {}", to_hex(identity.namespace_id()));

    // Log bootstrap peers
    for (const auto& peer : config.bootstrap_peers) {
        spdlog::info("bootstrap: {}", peer);
    }
    if (config.bootstrap_peers.empty()) {
        spdlog::info("no bootstrap peers configured (accepting inbound only)");
    }

    spdlog::info("max peers: {}", config.max_peers);
    spdlog::info("sync interval: {}s", config.sync_interval_seconds);

    // Create components
    chromatindb::storage::Storage storage(config.data_dir);
    chromatindb::engine::BlobEngine engine(storage);
    asio::io_context ioc;

    chromatindb::acl::AccessControl acl(config.allowed_keys, identity.namespace_id());
    chromatindb::peer::PeerManager pm(config, identity, engine, storage, ioc, acl, config.config_path);
    pm.start();

    // Periodic expiry scanner
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        while (true) {
            asio::steady_timer timer(ioc);
            timer.expires_after(std::chrono::seconds(60));
            auto [ec] = co_await timer.async_wait(
                asio::as_tuple(asio::use_awaitable));
            if (ec) co_return;

            auto purged = storage.run_expiry_scan();
            if (purged > 0) {
                spdlog::info("expiry scan: purged {} blobs", purged);
            }
        }
    }(), asio::detached);

    spdlog::info("daemon started");

    // Run event loop
    ioc.run();

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
