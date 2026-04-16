#include "db/version.h"
#include "db/acl/access_control.h"
#include "db/config/config.h"
#include "db/crypto/hash.h"
#include "db/crypto/thread_pool.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/logging/logging.h"
#include "db/net/auth_helpers.h"
#include "db/net/protocol.h"
#include "db/peer/peer_manager.h"
#include "db/storage/storage.h"
#include "db/util/endian.h"
#include "db/util/hex.h"

#include <asio.hpp>
#include <asio/local/stream_protocol.hpp>
#include <nlohmann/json.hpp>
#include <oqs/oqs.h>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <signal.h>
#include <span>
#include <string>
#include <thread>


namespace {

using chromatindb::util::to_hex;

void print_usage(const char* prog) {
    std::cerr << "chromatindb " << VERSION << "\n\n"
              << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  run          Start the daemon\n"
              << "  keygen       Generate identity keypair\n"
              << "  show-key     Print namespace (public key hash)\n"
              << "  backup       Create a live database backup\n"
              << "  add-peer     Add a bootstrap peer to config\n"
              << "  remove-peer  Remove a bootstrap peer from config\n"
              << "  list-peers   Show configured and connected peers\n"
              << "  version      Print version\n\n"
              << "Run options:\n"
              << "  --config <path>     JSON config file\n"
              << "  --data-dir <path>   Data directory (default: ./data)\n"
              << "  --log-level <lvl>   Log level: trace|debug|info|warn|error (default: info)\n\n"
              << "Keygen options:\n"
              << "  --data-dir <path>   Data directory (default: ./data)\n"
              << "  --force             Overwrite existing identity\n\n"
              << "Show-key options:\n"
              << "  --data-dir <path>   Data directory (default: ./data)\n\n"
              << "Peer management options:\n"
              << "  --config <path>     Config file to edit (default: <data-dir>/config.json)\n"
              << "  --data-dir <path>   Data directory (default: ./data)\n";
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

int cmd_show_key(int argc, char* argv[]) {
    std::string data_dir = "./data";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        }
    }

    try {
        auto identity = chromatindb::identity::NodeIdentity::load_from(data_dir);
        std::cout << "Namespace: " << to_hex(identity.namespace_id()) << std::endl;
    } catch (const std::runtime_error& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

int cmd_backup(int argc, char* argv[]) {
    std::string data_dir = "./data";
    std::string dest_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (dest_path.empty() && arg[0] != '-') {
            dest_path = arg;
        }
    }

    if (dest_path.empty()) {
        std::cerr << "Usage: chromatindb backup <dest-path> [--data-dir <path>]\n"
                  << "Creates a live compacted copy of the database at <dest-path>.\n";
        return 1;
    }

    try {
        chromatindb::storage::Storage storage(data_dir);
        if (storage.backup(dest_path)) {
            std::cout << "Backup written to " << dest_path << std::endl;
            return 0;
        } else {
            std::cerr << "Backup failed." << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

int cmd_add_peer(int argc, char* argv[]) {
    std::string data_dir = "./data";
    std::string config_path;
    std::string address;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (address.empty() && arg[0] != '-') {
            address = arg;
        }
    }

    if (address.empty()) {
        std::cerr << "Usage: chromatindb add-peer <host:port> [--config <path>] [--data-dir <path>]\n";
        return 1;
    }

    // Resolve config path
    if (config_path.empty()) {
        config_path = (std::filesystem::path(data_dir) / "config.json").string();
    }

    try {
        // Read existing config as raw JSON (preserves all fields)
        nlohmann::json j;
        {
            std::ifstream f(config_path);
            if (f.is_open()) {
                j = nlohmann::json::parse(f);
            }
        }

        // Ensure bootstrap_peers is an array
        if (!j.contains("bootstrap_peers") || !j["bootstrap_peers"].is_array()) {
            j["bootstrap_peers"] = nlohmann::json::array();
        }

        // Check for duplicates
        for (const auto& peer : j["bootstrap_peers"]) {
            if (peer.get<std::string>() == address) {
                std::cerr << "Peer " << address << " already in bootstrap_peers" << std::endl;
                return 1;
            }
        }

        // Add the peer
        j["bootstrap_peers"].push_back(address);

        // Write back with indentation
        {
            std::ofstream f(config_path);
            if (!f.is_open()) {
                std::cerr << "Error: cannot write " << config_path << std::endl;
                return 1;
            }
            f << j.dump(4) << std::endl;
        }

        std::cout << "Added peer " << address << " to " << config_path << std::endl;

        // Send SIGHUP to running node if pidfile exists (D-10)
        auto pidfile = std::filesystem::path(data_dir) / "chromatindb.pid";
        if (std::filesystem::exists(pidfile)) {
            std::ifstream pf(pidfile);
            pid_t pid;
            if (pf >> pid && kill(pid, 0) == 0) {
                kill(pid, SIGHUP);
                std::cout << "Sent SIGHUP to node (PID " << pid << ")" << std::endl;
            }
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

int cmd_remove_peer(int argc, char* argv[]) {
    std::string data_dir = "./data";
    std::string config_path;
    std::string address;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (address.empty() && arg[0] != '-') {
            address = arg;
        }
    }

    if (address.empty()) {
        std::cerr << "Usage: chromatindb remove-peer <host:port> [--config <path>] [--data-dir <path>]\n";
        return 1;
    }

    // Resolve config path
    if (config_path.empty()) {
        config_path = (std::filesystem::path(data_dir) / "config.json").string();
    }

    try {
        // Read existing config as raw JSON
        nlohmann::json j;
        {
            std::ifstream f(config_path);
            if (!f.is_open()) {
                std::cerr << "Error: cannot read " << config_path << std::endl;
                return 1;
            }
            j = nlohmann::json::parse(f);
        }

        if (!j.contains("bootstrap_peers") || !j["bootstrap_peers"].is_array()) {
            std::cerr << "No bootstrap_peers array in " << config_path << std::endl;
            return 1;
        }

        // Find and remove the peer
        auto& peers = j["bootstrap_peers"];
        auto it = std::find(peers.begin(), peers.end(), address);
        if (it == peers.end()) {
            std::cerr << "Peer " << address << " not found in bootstrap_peers" << std::endl;
            return 1;
        }
        peers.erase(it);

        // Write back
        {
            std::ofstream f(config_path);
            if (!f.is_open()) {
                std::cerr << "Error: cannot write " << config_path << std::endl;
                return 1;
            }
            f << j.dump(4) << std::endl;
        }

        std::cout << "Removed peer " << address << " from " << config_path << std::endl;

        // Send SIGHUP to running node if pidfile exists (D-10)
        auto pidfile = std::filesystem::path(data_dir) / "chromatindb.pid";
        if (std::filesystem::exists(pidfile)) {
            std::ifstream pf(pidfile);
            pid_t pid;
            if (pf >> pid && kill(pid, 0) == 0) {
                kill(pid, SIGHUP);
                std::cout << "Sent SIGHUP to node (PID " << pid << ")" << std::endl;
            }
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

// Forward declaration -- implemented in Task 2
int cmd_list_peers(int argc, char* argv[]);

int cmd_run(int argc, char* argv[]) {
    // Parse args
    std::vector<const char*> args;
    args.push_back("chromatindb");
    for (int i = 1; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    chromatindb::config::Config config;
    try {
        config = chromatindb::config::parse_args(
            static_cast<int>(args.size()), args.data());
        chromatindb::config::validate_config(config);
    } catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    // Initialize logging
    chromatindb::logging::init(config.log_level,
                                config.log_file,
                                config.log_max_size_mb,
                                config.log_max_files,
                                config.log_format);

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
    spdlog::info("safety-net interval: {}s", config.safety_net_interval_seconds);
    if (config.compaction_interval_hours > 0) {
        spdlog::info("compaction interval: {}h", config.compaction_interval_hours);
    } else {
        spdlog::info("compaction: disabled");
    }
    if (!config.uds_path.empty()) {
        spdlog::info("uds: {}", config.uds_path);
    } else {
        spdlog::info("uds: disabled");
    }
    spdlog::info("blob_transfer_timeout: {}s", config.blob_transfer_timeout);
    spdlog::info("sync_timeout: {}s", config.sync_timeout);
    spdlog::info("pex_interval: {}s", config.pex_interval);
    spdlog::info("strike_threshold: {}", config.strike_threshold);
    spdlog::info("strike_cooldown: {}s", config.strike_cooldown);

    // Resolve and create thread pool for crypto offload
    uint32_t hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 2;
    if (config.worker_threads > hw) {
        spdlog::warn("worker_threads {} exceeds hardware_concurrency {}, clamped",
                     config.worker_threads, hw);
    }
    auto num_workers = chromatindb::crypto::resolve_worker_threads(config.worker_threads);
    spdlog::info("worker threads: {}{}",
                 num_workers,
                 config.worker_threads == 0 ? " (auto-detected)" : " (configured)");

    asio::thread_pool pool(num_workers);

    // Create components
    chromatindb::storage::Storage storage(config.data_dir);
    storage.integrity_scan();

    chromatindb::engine::BlobEngine engine(storage, pool, config.max_storage_bytes,
                                           config.namespace_quota_bytes,
                                           config.namespace_quota_count,
                                           config.max_ttl_seconds);
    if (!config.namespace_quotas.empty()) {
        engine.set_quota_config(config.namespace_quota_bytes,
                                config.namespace_quota_count,
                                config.namespace_quotas);
    }
    asio::io_context ioc;

    chromatindb::acl::AccessControl acl(config.allowed_client_keys, config.allowed_peer_keys, identity.namespace_id());
    chromatindb::peer::PeerManager pm(config, identity, engine, storage, ioc, pool, acl, config.config_path);
    pm.start();

    spdlog::info("daemon started");

    // Write pidfile for peer management subcommands (Phase 118)
    auto pidfile = std::filesystem::path(config.data_dir) / "chromatindb.pid";
    {
        std::ofstream pf(pidfile);
        pf << getpid();
    }

    // Run event loop (expiry scanning now lives in PeerManager)
    ioc.run();

    // Clean up pidfile
    std::filesystem::remove(pidfile);

    // Wait for in-flight crypto operations to complete
    pool.join();

    return pm.exit_code();
}

// Stub -- full implementation in Task 2
int cmd_list_peers(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    std::cerr << "list-peers: not yet implemented\n";
    return 1;
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
    if (cmd == "show-key") return cmd_show_key(argc - 1, argv + 1);
    if (cmd == "backup") return cmd_backup(argc - 1, argv + 1);
    if (cmd == "add-peer") return cmd_add_peer(argc - 1, argv + 1);
    if (cmd == "remove-peer") return cmd_remove_peer(argc - 1, argv + 1);
    if (cmd == "list-peers") return cmd_list_peers(argc - 1, argv + 1);
    if (cmd == "version" || cmd == "--version" || cmd == "-v") return cmd_version();
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { print_usage(argv[0]); return 0; }

    std::cerr << "Unknown command: " << cmd << std::endl;
    print_usage(argv[0]);
    return 1;
}
