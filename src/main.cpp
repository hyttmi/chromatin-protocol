#include <atomic>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#include "config/config.h"
#include "kademlia/kademlia.h"
#include "kademlia/node_id.h"
#include "kademlia/routing_table.h"
#include "kademlia/udp_transport.h"
#include "replication/repl_log.h"
#include "storage/storage.h"

static std::atomic<bool> g_running{true};

static void signal_handler(int /*sig*/) {
    g_running.store(false);
}

static std::string hex(const helix::crypto::Hash& h) {
    std::ostringstream oss;
    for (auto b : h) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

static void print_usage(const char* prog) {
    spdlog::error("usage: {} --config <path>", prog);
}

int main(int argc, char* argv[]) {
    spdlog::info("helix-node v0.1.0 starting");

    // --- 1. Parse --config <path> ---
    std::filesystem::path config_path;
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            config_path = argv[++i];
        }
    }
    if (config_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // --- 2. Load or generate config ---
    if (!std::filesystem::exists(config_path)) {
        helix::config::generate_default_config(config_path);
        spdlog::info("generated config template at {}", config_path.string());
        spdlog::info("edit the file and re-run");
        return 0;
    }

    helix::config::Config cfg;
    try {
        cfg = helix::config::load_config(config_path);
    } catch (const std::exception& e) {
        spdlog::error("failed to load config: {}", e.what());
        return 1;
    }
    spdlog::info("loaded config from {}", config_path.string());

    // --- 3. Create data_dir if needed ---
    std::filesystem::create_directories(cfg.data_dir);

    // --- 4. Load or generate keypair ---
    helix::crypto::KeyPair keypair;
    try {
        keypair = helix::config::load_or_generate_keypair(cfg.data_dir);
    } catch (const std::exception& e) {
        spdlog::error("keypair error: {}", e.what());
        return 1;
    }

    // --- 5. Compute NodeId, build NodeInfo ---
    auto node_id = helix::kademlia::NodeId::from_pubkey(keypair.public_key);
    spdlog::info("node id: {}", hex(node_id.id));

    helix::kademlia::NodeInfo self;
    self.id = node_id;
    self.address = cfg.bind;
    self.udp_port = cfg.udp_port;
    self.ws_port = cfg.ws_port;
    self.pubkey = keypair.public_key;
    self.last_seen = std::chrono::steady_clock::now();

    // --- 6. Create Storage ---
    auto db_path = cfg.data_dir / "helix.mdbx";
    helix::storage::Storage storage(db_path);
    spdlog::info("storage opened at {}", db_path.string());

    // --- 7. Create ReplLog ---
    helix::replication::ReplLog repl_log(storage);

    // --- 8. Create RoutingTable ---
    helix::kademlia::RoutingTable routing_table;

    // --- 9. Create UdpTransport ---
    helix::kademlia::UdpTransport transport(cfg.bind, cfg.udp_port);
    spdlog::info("UDP bound to {}:{}", cfg.bind, transport.local_port());

    // --- 10. Create Kademlia engine ---
    helix::kademlia::Kademlia kademlia(self, transport, routing_table, storage, repl_log, keypair);

    // --- 11. Start UDP recv loop in background thread ---
    std::thread recv_thread([&]() {
        transport.run([&](const helix::kademlia::Message& msg,
                         const std::string& from_addr, uint16_t from_port) {
            kademlia.handle_message(msg, from_addr, from_port);
        });
    });

    // --- 12. Bootstrap ---
    if (!cfg.bootstrap.empty()) {
        spdlog::info("bootstrapping from {} peer(s)", cfg.bootstrap.size());
        kademlia.set_bootstrap_addrs(cfg.bootstrap);
        kademlia.bootstrap(cfg.bootstrap);
    } else {
        spdlog::info("no bootstrap peers — running as standalone bootstrap node");
    }

    // --- 13. Install signal handlers and wait ---
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    spdlog::info("node ready — press Ctrl-C to stop");
    while (g_running.load()) {
        kademlia.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // --- 14. Shutdown ---
    spdlog::info("shutting down...");
    transport.stop();
    recv_thread.join();
    spdlog::info("helix-node shutdown");

    return 0;
}
