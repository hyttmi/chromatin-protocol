#include <csignal>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#include "config/config.h"
#include "kademlia/kademlia.h"
#include "kademlia/node_id.h"
#include "kademlia/routing_table.h"
#include "kademlia/tcp_transport.h"
#include "replication/repl_log.h"
#include "storage/storage.h"
#include "ws/ws_server.h"

static std::string hex(const chromatin::crypto::Hash& h) {
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
    spdlog::info("chromatin-node v0.1.0 starting");

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
        chromatin::config::generate_default_config(config_path);
        spdlog::info("generated config template at {}", config_path.string());
        spdlog::info("edit the file and re-run");
        return 0;
    }

    chromatin::config::Config cfg;
    try {
        cfg = chromatin::config::load_config(config_path);
    } catch (const std::exception& e) {
        spdlog::error("failed to load config: {}", e.what());
        return 1;
    }
    spdlog::info("loaded config from {}", config_path.string());

    // --- 3. Create data_dir if needed ---
    std::filesystem::create_directories(cfg.data_dir);

    // --- 4. Load or generate keypair ---
    chromatin::crypto::KeyPair keypair;
    try {
        keypair = chromatin::config::load_or_generate_keypair(cfg.data_dir);
    } catch (const std::exception& e) {
        spdlog::error("keypair error: {}", e.what());
        return 1;
    }

    // --- 5. Compute NodeId, build NodeInfo ---
    auto node_id = chromatin::kademlia::NodeId::from_pubkey(keypair.public_key);
    spdlog::info("node id: {}", hex(node_id.id));

    chromatin::kademlia::NodeInfo self;
    self.id = node_id;
    self.address = cfg.bind;
    self.tcp_port = cfg.tcp_port;
    self.ws_port = cfg.ws_port;
    self.pubkey = keypair.public_key;
    self.last_seen = std::chrono::steady_clock::now();

    // --- 6. Create Storage ---
    auto db_path = cfg.data_dir / "chromatin.mdbx";
    chromatin::storage::Storage storage(db_path);
    spdlog::info("storage opened at {}", db_path.string());

    // --- 7. Create ReplLog ---
    chromatin::replication::ReplLog repl_log(storage);

    // --- 8. Create RoutingTable ---
    chromatin::kademlia::RoutingTable routing_table;

    // --- 9. Create TcpTransport ---
    chromatin::kademlia::TcpTransport transport(cfg.bind, cfg.tcp_port);

    // --- 10. Create Kademlia engine ---
    chromatin::kademlia::Kademlia kademlia(self, transport, routing_table, storage, repl_log, keypair);

    // --- 11. Start TCP accept loop in background thread ---
    std::thread recv_thread([&]() {
        transport.run([&](const chromatin::kademlia::Message& msg,
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

    // --- 13. WebSocket server is the main event loop ---
    // Use TLS if cert/key paths are configured, otherwise plaintext.
    // Both branches use a type-erased stop function for signal handling.
    static std::function<void()> g_stop;
    struct sigaction sa{};
    sa.sa_handler = [](int) { if (g_stop) g_stop(); };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    auto run_ws = [&](auto& ws) {
        kademlia.set_on_store([&](const chromatin::crypto::Hash& key, uint8_t type,
                                  std::span<const uint8_t> value) {
            ws.on_kademlia_store(key, type, value);
        });
        g_stop = [&ws]() { ws.stop(); };

        spdlog::info("node ready — WS{} on port {}, TCP on port {}",
                     cfg.tls_cert_path.empty() ? "" : " (TLS)",
                     cfg.ws_port, cfg.tcp_port);
        ws.run();  // blocks until signal
    };

    if (!cfg.tls_cert_path.empty()) {
        chromatin::ws::WsServer<true> ws(cfg, kademlia, storage, repl_log, keypair);
        run_ws(ws);
    } else {
        chromatin::ws::WsServer<false> ws(cfg, kademlia, storage, repl_log, keypair);
        run_ws(ws);
    }

    // --- 14. Shutdown ---
    spdlog::info("shutting down...");
    transport.stop();
    recv_thread.join();
    spdlog::info("chromatin-node shutdown");

    return 0;
}
