#include "relay/config/relay_config.h"
#include "relay/core/authenticator.h"
#include "relay/identity/relay_identity.h"
#include "relay/ws/ws_acceptor.h"
#include "relay/ws/ws_session.h"
#include "relay/ws/session_manager.h"

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

void print_usage(const char* prog) {
    std::cerr << "chromatindb_relay v3.0.0\n\n"
              << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  --config <path>   JSON config file (required)\n"
              << "  --help            Show this message\n"
              << "  --version         Show version\n";
}

std::string to_hex(std::span<const uint8_t> data) {
    std::string s;
    s.reserve(data.size() * 2);
    for (auto b : data) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        s += buf;
    }
    return s;
}

std::optional<std::vector<uint8_t>> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;

    std::vector<uint8_t> result;
    result.reserve(hex.size() / 2);

    for (size_t i = 0; i < hex.size(); i += 2) {
        uint8_t hi, lo;

        char c = hex[i];
        if (c >= '0' && c <= '9') hi = static_cast<uint8_t>(c - '0');
        else if (c >= 'a' && c <= 'f') hi = static_cast<uint8_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') hi = static_cast<uint8_t>(c - 'A' + 10);
        else return std::nullopt;

        c = hex[i + 1];
        if (c >= '0' && c <= '9') lo = static_cast<uint8_t>(c - '0');
        else if (c >= 'a' && c <= 'f') lo = static_cast<uint8_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') lo = static_cast<uint8_t>(c - 'A' + 10);
        else return std::nullopt;

        result.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    return result;
}

/// Build ACL KeySet from config's allowed_client_keys hex strings.
chromatindb::relay::core::Authenticator::KeySet build_acl(
    const std::vector<std::string>& hex_keys) {
    chromatindb::relay::core::Authenticator::KeySet acl;
    for (const auto& hex_str : hex_keys) {
        auto bytes = from_hex(hex_str);
        if (bytes && bytes->size() == 32) {
            std::array<uint8_t, 32> arr{};
            std::memcpy(arr.data(), bytes->data(), 32);
            acl.insert(arr);
        }
    }
    return acl;
}

void init_logging(const std::string& level, const std::string& log_file) {
    std::vector<spdlog::sink_ptr> sinks;

    // Console sink (always present)
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sinks.push_back(console_sink);

    // Rotating file sink (optional)
    if (!log_file.empty()) {
        try {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file,
                10 * 1024 * 1024,  // 10 MiB
                3);                 // 3 rotated files
            sinks.push_back(file_sink);
        } catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "warning: failed to open log file '" << log_file
                      << "': " << ex.what() << " (falling back to console only)\n";
        }
    }

    auto logger = std::make_shared<spdlog::logger>("", sinks.begin(), sinks.end());

    // Parse level
    if (level == "trace") logger->set_level(spdlog::level::trace);
    else if (level == "debug") logger->set_level(spdlog::level::debug);
    else if (level == "warn") logger->set_level(spdlog::level::warn);
    else if (level == "error") logger->set_level(spdlog::level::err);
    else logger->set_level(spdlog::level::info);

    spdlog::set_default_logger(logger);
}

} // namespace

int main(int argc, char* argv[]) {
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            std::cout << "chromatindb_relay v3.0.0" << std::endl;
            return 0;
        }
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    if (config_path.empty()) {
        std::cerr << "error: --config <path> is required\n\n";
        print_usage(argv[0]);
        return 1;
    }

    // Load config
    chromatindb::relay::config::RelayConfig cfg;
    try {
        cfg = chromatindb::relay::config::load_relay_config(config_path);
    } catch (const std::runtime_error& e) {
        std::cerr << "error: failed to load config: " << e.what() << "\n";
        return 1;
    }

    // Validate config
    try {
        chromatindb::relay::config::validate_relay_config(cfg);
    } catch (const std::runtime_error& e) {
        std::cerr << "error: config validation failed: " << e.what() << "\n";
        return 1;
    }

    // Initialize logging
    init_logging(cfg.log_level, cfg.log_file);

    spdlog::info("chromatindb_relay starting");
    spdlog::info("  bind: {}:{}", cfg.bind_address, cfg.bind_port);
    spdlog::info("  uds: {}", cfg.uds_path);
    spdlog::info("  max_send_queue: {}", cfg.max_send_queue);
    spdlog::info("  max_connections: {}", cfg.max_connections);

    // Load or generate identity
    try {
        auto identity = chromatindb::relay::identity::RelayIdentity::load_or_generate(
            cfg.identity_key_path);
        spdlog::info("  identity: {}", to_hex(identity.public_key_hash()));
    } catch (const std::runtime_error& e) {
        spdlog::error("failed to load identity: {}", e.what());
        return 1;
    }

    // Build Authenticator with ACL from config (per D-07, D-34)
    auto acl = build_acl(cfg.allowed_client_keys);
    chromatindb::relay::core::Authenticator authenticator(std::move(acl));
    spdlog::info("  allowed_client_keys: {}",
                 cfg.allowed_client_keys.empty()
                     ? "open relay"
                     : std::to_string(cfg.allowed_client_keys.size()) + " keys");

    // Create io_context
    asio::io_context ioc;

    // Create SessionManager and WsAcceptor
    chromatindb::relay::ws::SessionManager session_manager;
    chromatindb::relay::ws::WsAcceptor acceptor(
        ioc, session_manager,
        cfg.bind_address, static_cast<uint16_t>(cfg.bind_port),
        cfg.max_send_queue, cfg.max_connections,
        authenticator);

    // Initialize TLS if configured (per D-01)
    if (cfg.tls_enabled()) {
        if (!acceptor.init_tls(cfg.cert_path, cfg.key_path)) {
            spdlog::error("failed to initialize TLS -- check cert_path and key_path");
            return 1;
        }
        spdlog::info("  tls: enabled (TLS 1.3)");
        spdlog::info("  cert: {}", cfg.cert_path);
        spdlog::info("  key: {}", cfg.key_path);
    } else {
        spdlog::warn("  tls: DISABLED -- plain WebSocket mode (use cert_path/key_path for TLS)");
    }

    // Spawn accept loop
    asio::co_spawn(ioc, acceptor.accept_loop(), asio::detached);

    // SIGTERM/SIGINT graceful shutdown (per D-25)
    asio::signal_set term_signals(ioc, SIGTERM, SIGINT);
    term_signals.async_wait([&](const asio::error_code& ec, int sig) {
        if (ec) return;
        spdlog::info("received signal {}, shutting down with {} active sessions",
                    sig, session_manager.count());
        acceptor.stop();

        // Best-effort Close(1001 Going Away) to all active sessions (per D-25)
        session_manager.for_each([](uint64_t /*id*/, const auto& session) {
            session->close(1001, "server shutting down");
        });

        // Give 2 seconds for close frames to send, then force stop
        auto shutdown_timer = std::make_shared<asio::steady_timer>(ioc);
        shutdown_timer->expires_after(std::chrono::seconds(2));
        shutdown_timer->async_wait([&ioc, shutdown_timer](const asio::error_code&) {
            ioc.stop();
        });
    });

    // SIGHUP config reload (per D-19, D-23, D-32, D-34)
    asio::signal_set hup_signal(ioc, SIGHUP);
    std::function<void(const asio::error_code&, int)> hup_handler;
    hup_handler = [&](const asio::error_code& ec, int) {
        if (ec) return;
        spdlog::info("received SIGHUP -- reloading configuration");

        // Re-read config file
        try {
            auto new_cfg = chromatindb::relay::config::load_relay_config(config_path);

            // Reload TLS if enabled
            if (new_cfg.tls_enabled()) {
                if (acceptor.reload_tls(new_cfg.cert_path, new_cfg.key_path)) {
                    spdlog::info("TLS context reloaded successfully");
                } else {
                    spdlog::error("TLS reload failed -- keeping previous context");
                }
            }

            // Reload allowed_client_keys (per D-34)
            auto new_acl = build_acl(new_cfg.allowed_client_keys);
            authenticator.reload_allowed_keys(std::move(new_acl));
            spdlog::info("allowed_client_keys reloaded: {}",
                         new_cfg.allowed_client_keys.empty()
                             ? "open relay"
                             : std::to_string(new_cfg.allowed_client_keys.size()) + " keys");

            // Reload max_connections (per D-32)
            acceptor.set_max_connections(new_cfg.max_connections);

        } catch (const std::exception& e) {
            spdlog::error("SIGHUP config reload failed: {}", e.what());
        }

        hup_signal.async_wait(hup_handler);
    };
    hup_signal.async_wait(hup_handler);

    // Thread pool (per D-21): hardware_concurrency() threads running ioc.run()
    auto thread_count = std::thread::hardware_concurrency();
    if (thread_count == 0) thread_count = 2;  // Fallback
    spdlog::info("  threads: {}", thread_count);

    spdlog::info("relay listening on {}:{}{}",
                cfg.bind_address, cfg.bind_port,
                cfg.tls_enabled() ? " (WSS)" : " (WS)");

    std::vector<std::thread> threads;
    threads.reserve(thread_count - 1);
    for (unsigned i = 1; i < thread_count; ++i) {
        threads.emplace_back([&ioc]() { ioc.run(); });
    }
    ioc.run();  // Main thread also runs

    for (auto& t : threads) {
        t.join();
    }

    spdlog::info("relay stopped");
    return 0;
}
