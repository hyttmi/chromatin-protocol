#include "db/version.h"
#include "relay/config/relay_config.h"
#include "relay/identity/relay_identity.h"
#include "relay/core/relay_session.h"
#include "relay/core/message_filter.h"
#include "db/crypto/hash.h"
#include "db/logging/logging.h"
#include "db/net/connection.h"
#include "db/util/hex.h"

#include <asio/signal_set.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>

namespace {

using chromatindb::util::to_hex;

void print_usage(const char* prog) {
    std::cerr << "chromatindb_relay " << VERSION << "\n\n"
              << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  run        Start the relay\n"
              << "  keygen     Generate relay identity keypair\n"
              << "  show-key   Print public key hash\n"
              << "  version    Print version\n\n"
              << "Run options:\n"
              << "  --config <path>     JSON config file (required)\n\n"
              << "Keygen options:\n"
              << "  --output <path>     Secret key output path (writes <path> + sibling .pub)\n"
              << "  --force             Overwrite existing identity\n\n"
              << "Show-key options:\n"
              << "  --key <path>        Secret key path (reads sibling .pub)\n";
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

int cmd_show_key(int argc, char* argv[]) {
    std::string key_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--key" && i + 1 < argc) {
            key_path = argv[++i];
        }
    }

    if (key_path.empty()) {
        std::cerr << "Error: --key <path> is required" << std::endl;
        return 1;
    }

    try {
        auto identity = chromatindb::relay::identity::RelayIdentity::load_from(key_path);
        std::cout << "Public key hash: " << to_hex(identity.public_key_hash()) << std::endl;
    } catch (const std::runtime_error& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

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

    // Convert RelayIdentity to NodeIdentity for Connection compatibility
    auto node_identity = identity.to_node_identity();

    // Create io_context
    asio::io_context ioc;

    // Session tracking (deque per project decision: coroutine-accessed containers)
    std::deque<chromatindb::relay::core::RelaySession::Ptr> sessions;
    bool draining = false;

    // TCP acceptor setup
    asio::ip::tcp::acceptor acceptor(ioc);
    auto port_str = std::to_string(cfg.bind_port);
    asio::ip::tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(cfg.bind_address, port_str);
    auto endpoint = *endpoints.begin();
    acceptor.open(endpoint.endpoint().protocol());
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint.endpoint());
    acceptor.listen();

    spdlog::info("listening on {}:{}", cfg.bind_address, cfg.bind_port);

    // Accept loop coroutine
    auto accept_loop = [&]() -> asio::awaitable<void> {
        while (!draining) {
            auto [ec, socket] = co_await acceptor.async_accept(
                chromatindb::net::use_nothrow);
            if (ec) {
                if (draining) break;
                spdlog::warn("accept error: {}", ec.message());
                continue;
            }

            std::string client_addr;
            {
                asio::error_code ep_ec;
                auto ep = socket.remote_endpoint(ep_ec);
                if (!ep_ec) client_addr = ep.address().to_string() + ":" + std::to_string(ep.port());
            }

            spdlog::info("client connecting from {}", client_addr);

            // Create inbound connection (PQ handshake as responder, per RELAY-01)
            auto conn = chromatindb::net::Connection::create_inbound(
                std::move(socket), node_identity);

            // After PQ handshake completes, create relay session
            conn->on_ready([&, conn](chromatindb::net::Connection::Ptr c) {
                spdlog::info("client authenticated: {} from {}",
                    [&]() {
                        auto& pk = c->peer_pubkey();
                        auto hash = chromatindb::crypto::sha3_256(pk);
                        return to_hex(std::span<const uint8_t>(hash.data(), hash.size()));
                    }(),
                    c->remote_address());

                // Create relay session (per RELAY-02: dedicated UDS per client)
                auto session = chromatindb::relay::core::RelaySession::create(
                    c, cfg.uds_path, node_identity, ioc);

                session->on_close([&](chromatindb::relay::core::RelaySession::Ptr s) {
                    sessions.erase(
                        std::remove(sessions.begin(), sessions.end(), s),
                        sessions.end());
                });

                sessions.push_back(session);

                // Start session (connect UDS to node, begin forwarding)
                asio::co_spawn(ioc, [session]() -> asio::awaitable<void> {
                    bool ok = co_await session->start();
                    if (!ok) {
                        // UDS connect failed (per D-03: refuse client)
                        spdlog::error("failed to connect to node via UDS for client {}",
                            session->client_address());
                        session->stop();
                    }
                }, asio::detached);
            });

            // If handshake fails, connection closes itself (on_close fires)
            conn->on_close([&, client_addr](chromatindb::net::Connection::Ptr c, bool /*graceful*/) {
                if (!c->is_authenticated()) {
                    spdlog::info("client handshake failed from {}", client_addr);
                }
            });

            // Run connection (PQ handshake + message loop)
            asio::co_spawn(ioc, [conn]() -> asio::awaitable<void> {
                co_await conn->run();
            }, asio::detached);
        }
    };

    // Spawn accept loop
    asio::co_spawn(ioc, accept_loop(), asio::detached);

    // Signal handling (SIGINT/SIGTERM graceful shutdown)
    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](asio::error_code ec, int sig) {
        if (ec) return;
        spdlog::info("signal {} received, shutting down", sig);
        draining = true;

        // Stop accepting
        asio::error_code close_ec;
        acceptor.close(close_ec);

        // Stop all sessions
        auto snapshot = sessions;
        for (auto& s : snapshot) {
            s->stop();
        }
        sessions.clear();

        spdlog::info("relay stopped");
    });

    // Run event loop (blocks until stopped)
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
    if (cmd == "show-key") return cmd_show_key(argc - 1, argv + 1);
    if (cmd == "version" || cmd == "--version" || cmd == "-v") return cmd_version();
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { print_usage(argv[0]); return 0; }

    std::cerr << "Unknown command: " << cmd << std::endl;
    print_usage(argv[0]);
    return 1;
}
