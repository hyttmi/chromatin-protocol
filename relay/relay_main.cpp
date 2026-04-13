#include "relay/config/relay_config.h"
#include "relay/core/authenticator.h"
#include "relay/core/metrics_collector.h"
#include "relay/core/request_router.h"
#include "relay/core/subscription_tracker.h"
#include "relay/core/uds_multiplexer.h"
#include "relay/identity/relay_identity.h"
#include "relay/util/endian.h"
#include "relay/wire/transport_codec.h"
#include "relay/wire/transport_generated.h"
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
    chromatindb::relay::identity::RelayIdentity identity =
        chromatindb::relay::identity::RelayIdentity::generate();
    try {
        identity = chromatindb::relay::identity::RelayIdentity::load_or_generate(
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

    // Shared state for graceful shutdown and rate limiting
    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> rate_limit_rate{cfg.rate_limit_messages_per_sec};
    std::atomic<uint32_t> request_timeout{cfg.request_timeout_seconds};
    std::atomic<uint32_t> max_blob_size{cfg.max_blob_size_bytes};

    // Create RequestRouter, SessionManager, SubscriptionTracker
    chromatindb::relay::core::RequestRouter request_router;
    chromatindb::relay::ws::SessionManager session_manager;
    chromatindb::relay::core::SubscriptionTracker subscription_tracker;

    // Create MetricsCollector (Prometheus /metrics endpoint)
    chromatindb::relay::core::MetricsCollector metrics_collector(ioc, cfg.metrics_bind, stopping);
    metrics_collector.start();
    if (!cfg.metrics_bind.empty()) {
        spdlog::info("  metrics: http://{}/metrics", cfg.metrics_bind);
    } else {
        spdlog::info("  metrics: disabled (set metrics_bind to enable)");
    }
    if (cfg.rate_limit_messages_per_sec > 0) {
        spdlog::info("  rate_limit: {} msg/s", cfg.rate_limit_messages_per_sec);
    } else {
        spdlog::info("  rate_limit: disabled");
    }
    if (cfg.request_timeout_seconds > 0) {
        spdlog::info("  request_timeout: {}s", cfg.request_timeout_seconds);
    } else {
        spdlog::info("  request_timeout: disabled");
    }
    if (cfg.max_blob_size_bytes > 0) {
        spdlog::info("  max_blob_size: {} bytes", cfg.max_blob_size_bytes);
    } else {
        spdlog::info("  max_blob_size: disabled (no limit)");
    }

    // Bridge SessionDispatch to existing ws::SessionManager for WS transport
    chromatindb::relay::core::SessionDispatch dispatch;
    dispatch.send_json = [&session_manager](uint64_t id, const nlohmann::json& msg) {
        auto session = session_manager.get_session(id);
        if (session) session->send_json(msg);
    };
    dispatch.broadcast = [&session_manager](const nlohmann::json& msg) {
        session_manager.for_each([&msg](uint64_t, const auto& session) {
            if (session) session->send_json(msg);
        });
    };
    dispatch.send_error = dispatch.send_json;  // Same behavior for WS transport

    chromatindb::relay::core::UdsMultiplexer uds_mux(
        ioc, cfg.uds_path, identity, request_router, std::move(dispatch));
    uds_mux.set_tracker(&subscription_tracker);
    uds_mux.set_request_timeout(&request_timeout);
    uds_mux.set_metrics(&metrics_collector.metrics());
    uds_mux.start();
    spdlog::info("  uds_mux: started (connecting to {})", cfg.uds_path);

    // Wire subscription tracker into session manager for disconnect cleanup (D-05)
    session_manager.set_tracker(&subscription_tracker);
    // FEAT-01: Wire write tracker for source exclusion disconnect cleanup
    session_manager.set_write_tracker(&uds_mux.write_tracker());
    session_manager.set_on_namespaces_empty(
        [&uds_mux](const std::vector<std::array<uint8_t, 32>>& namespaces) {
            if (namespaces.empty() || !uds_mux.is_connected()) return;
            // Build u16BE namespace list payload
            std::vector<uint8_t> payload;
            payload.reserve(2 + namespaces.size() * 32);
            uint8_t buf[2];
            chromatindb::relay::util::store_u16_be(buf, static_cast<uint16_t>(namespaces.size()));
            payload.insert(payload.end(), buf, buf + 2);
            for (const auto& ns : namespaces) {
                payload.insert(payload.end(), ns.begin(), ns.end());
            }
            auto msg = chromatindb::relay::wire::TransportCodec::encode(
                chromatindb::wire::TransportMsgType_Unsubscribe, payload, 0);
            uds_mux.send(std::move(msg));
        });

    // Create WsAcceptor
    chromatindb::relay::ws::WsAcceptor acceptor(
        ioc, session_manager,
        cfg.bind_address, static_cast<uint16_t>(cfg.bind_port),
        cfg.max_send_queue, cfg.max_connections,
        authenticator, &uds_mux, &request_router, &subscription_tracker);

    // Wire metrics and rate limit atomics into components
    session_manager.set_metrics(&metrics_collector.metrics());
    acceptor.set_metrics(&metrics_collector.metrics());
    acceptor.set_shared_rate(&rate_limit_rate);
    acceptor.set_max_blob_size(&max_blob_size);
    metrics_collector.set_gauge_provider([&session_manager, &subscription_tracker]() {
        return std::make_pair(session_manager.count(), subscription_tracker.namespace_count());
    });
    metrics_collector.set_health_provider([&uds_mux]() {
        return uds_mux.is_connected();
    });

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

    // SIGTERM/SIGINT drain-first graceful shutdown (per D-16, D-17)
    asio::signal_set term_signals(ioc, SIGTERM, SIGINT);
    term_signals.async_wait([&](const asio::error_code& ec, int sig) {
        if (ec) return;
        stopping.store(true, std::memory_order_relaxed);
        spdlog::info("received signal {}, graceful shutdown ({} sessions)",
                     sig, session_manager.count());

        // 1. Stop accepting new connections (per D-16 step 1)
        acceptor.stop();

        // 2. Stop metrics endpoint
        metrics_collector.stop();

        // 3. Wait up to 5s for send queues to drain (per D-17)
        auto drain_timer = std::make_shared<asio::steady_timer>(ioc);
        drain_timer->expires_after(std::chrono::seconds(5));
        drain_timer->async_wait([&, drain_timer](const asio::error_code&) {
            // 4. Send Close(1001) to all remaining sessions (per D-16 step 3)
            spdlog::info("drain timeout, closing {} sessions", session_manager.count());
            session_manager.for_each([](uint64_t, const auto& session) {
                session->close(1001, "server shutting down");
            });

            // 5. Wait 2s for close handshake echo (per D-16 step 4)
            auto close_timer = std::make_shared<asio::steady_timer>(ioc);
            close_timer->expires_after(std::chrono::seconds(2));
            close_timer->async_wait([&ioc, close_timer](const asio::error_code&) {
                ioc.stop();
            });
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

            // Reload rate limit (per D-13, D-14)
            rate_limit_rate.store(new_cfg.rate_limit_messages_per_sec, std::memory_order_relaxed);
            spdlog::info("rate_limit reloaded: {}",
                         new_cfg.rate_limit_messages_per_sec == 0
                             ? std::string("disabled")
                             : std::to_string(new_cfg.rate_limit_messages_per_sec) + " msg/s");

            // Reload max blob size (per FEAT-02)
            max_blob_size.store(new_cfg.max_blob_size_bytes, std::memory_order_relaxed);
            spdlog::info("max_blob_size reloaded: {}",
                         new_cfg.max_blob_size_bytes == 0
                             ? std::string("disabled (no limit)")
                             : std::to_string(new_cfg.max_blob_size_bytes) + " bytes");

            // Reload request timeout
            request_timeout.store(new_cfg.request_timeout_seconds, std::memory_order_relaxed);
            spdlog::info("request_timeout reloaded: {}",
                         new_cfg.request_timeout_seconds == 0
                             ? std::string("disabled")
                             : std::to_string(new_cfg.request_timeout_seconds) + "s");

            // Reload metrics_bind (per D-15)
            metrics_collector.set_metrics_bind(new_cfg.metrics_bind);

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
