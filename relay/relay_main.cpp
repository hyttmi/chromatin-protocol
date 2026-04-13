#include "relay/config/relay_config.h"
#include "relay/core/authenticator.h"
#include "relay/core/metrics_collector.h"
#include "relay/core/request_router.h"
#include "relay/core/session_dispatch.h"
#include "relay/core/subscription_tracker.h"
#include "relay/core/uds_multiplexer.h"
#include "relay/http/handlers_data.h"
#include "relay/http/handlers_query.h"
#include "relay/http/handlers_pubsub.h"
#include "relay/http/http_router.h"
#include "relay/http/http_server.h"
#include "relay/http/response_promise.h"
#include "relay/http/sse_writer.h"
#include "relay/http/token_store.h"
#include "relay/identity/relay_identity.h"
#include "relay/util/endian.h"
#include "relay/wire/transport_codec.h"
#include "relay/wire/transport_generated.h"

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
    std::cerr << "chromatindb_relay v3.1.0\n\n"
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
            std::cout << "chromatindb_relay v3.1.0" << std::endl;
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

    // =========================================================================
    // 1. Load config
    // =========================================================================
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

    // =========================================================================
    // 2. Init logging
    // =========================================================================
    init_logging(cfg.log_level, cfg.log_file);

    spdlog::info("chromatindb_relay v3.1.0 starting (HTTP transport)");
    spdlog::info("  bind: {}:{}", cfg.bind_address, cfg.bind_port);
    spdlog::info("  uds: {}", cfg.uds_path);
    spdlog::info("  max_connections: {}", cfg.max_connections);

    // =========================================================================
    // 3. Load identity
    // =========================================================================
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

    // =========================================================================
    // 4. Build Authenticator with ACL from config (per D-07, D-34)
    // =========================================================================
    auto acl = build_acl(cfg.allowed_client_keys);
    chromatindb::relay::core::Authenticator authenticator(std::move(acl));
    spdlog::info("  allowed_client_keys: {}",
                 cfg.allowed_client_keys.empty()
                     ? "open relay"
                     : std::to_string(cfg.allowed_client_keys.size()) + " keys");

    // =========================================================================
    // 5. Create io_context
    // =========================================================================
    asio::io_context ioc;

    // =========================================================================
    // 6. Shared atomics for SIGHUP-reloadable settings
    // =========================================================================
    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> rate_limit_rate{cfg.rate_limit_messages_per_sec};
    std::atomic<uint32_t> request_timeout{cfg.request_timeout_seconds};
    std::atomic<uint32_t> max_blob_size{cfg.max_blob_size_bytes};

    // =========================================================================
    // 7. Create RequestRouter
    // =========================================================================
    chromatindb::relay::core::RequestRouter request_router;

    // =========================================================================
    // 8. Create TokenStore
    // =========================================================================
    chromatindb::relay::http::TokenStore token_store;

    // =========================================================================
    // 9. Create SubscriptionTracker
    // =========================================================================
    chromatindb::relay::core::SubscriptionTracker subscription_tracker;

    // =========================================================================
    // 10. Create MetricsCollector (simplified -- no ioc, no metrics_bind)
    // =========================================================================
    chromatindb::relay::core::MetricsCollector metrics_collector(stopping);
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

    // =========================================================================
    // 11. Create ResponsePromiseMap
    // =========================================================================
    chromatindb::relay::http::ResponsePromiseMap promise_map;

    // =========================================================================
    // 12. Create HttpRouter
    // =========================================================================
    chromatindb::relay::http::HttpRouter router;

    // =========================================================================
    // 13. Register auth + metrics routes (health registered after uds_mux creation)
    // =========================================================================
    // Auth routes (public, no auth required) -- D-04, D-05, D-06
    chromatindb::relay::http::register_auth_routes(router, authenticator, token_store);

    // Metrics route (public, no auth required) -- D-30
    chromatindb::relay::http::register_metrics_route(router, metrics_collector);

    // =========================================================================
    // 14. Create SessionDispatch, UdsMultiplexer, then handlers
    // =========================================================================

    // SessionDispatch for HTTP transport (routes notifications to SSE writers).
    chromatindb::relay::core::SessionDispatch dispatch;

    dispatch.send_json = [&token_store](uint64_t session_id, const nlohmann::json& msg) {
        auto* session = token_store.lookup_by_id(session_id);
        if (!session) return;
        if (session->sse_writer) {
            // Notification: push to SSE stream.
            // Use a session-local counter -- but SseWriter manages its own ID tracking.
            // We use 0 as event_id; SseWriter format_event uses it.
            static std::atomic<uint64_t> notification_counter{0};
            session->sse_writer->push_event(msg,
                notification_counter.fetch_add(1, std::memory_order_relaxed) + 1);
        }
        // If no SSE writer, notification is dropped (client hasn't connected SSE yet)
    };

    dispatch.broadcast = [&token_store](const nlohmann::json& msg) {
        token_store.for_each_mut([&msg](chromatindb::relay::http::HttpSessionState& session) {
            if (session.sse_writer) {
                session.sse_writer->push_broadcast(msg);
            }
        });
    };

    dispatch.send_error = dispatch.send_json;

    // Create UdsMultiplexer with SessionDispatch (per D-25)
    chromatindb::relay::core::UdsMultiplexer uds_mux(
        ioc, cfg.uds_path, identity, request_router, std::move(dispatch));
    uds_mux.set_tracker(&subscription_tracker);
    uds_mux.set_request_timeout(&request_timeout);
    uds_mux.set_metrics(&metrics_collector.metrics());
    uds_mux.set_response_promises(&promise_map);
    uds_mux.start();
    spdlog::info("  uds_mux: started (connecting to {})", cfg.uds_path);

    // Health route (public, no auth required) -- FEAT-03
    // Registered after uds_mux creation so the lambda can capture it.
    chromatindb::relay::http::register_health_route(router, [&uds_mux]() {
        return uds_mux.is_connected();
    });

    // DataHandlers (blob write/read/delete, batch read) -- D-07, D-08, D-09, D-14
    chromatindb::relay::http::DataHandlers data_handlers(
        uds_mux, request_router, promise_map, uds_mux.write_tracker(),
        max_blob_size, request_timeout);
    chromatindb::relay::http::register_data_routes(router, data_handlers);

    // QueryHandlers (all query endpoints) -- D-10 through D-21
    chromatindb::relay::http::QueryHandlerDeps query_deps{
        uds_mux, request_router, promise_map, ioc, &request_timeout};
    chromatindb::relay::http::register_query_routes(router, query_deps);

    // PubSubHandlers (subscribe, unsubscribe, SSE events) -- D-22, D-23, D-24
    chromatindb::relay::http::PubSubHandlers pubsub_handlers(
        subscription_tracker, uds_mux, token_store);
    chromatindb::relay::http::register_pubsub_routes(router, pubsub_handlers);

    // =========================================================================
    // 17. Create HttpServer
    // =========================================================================
    chromatindb::relay::http::HttpServer http_server(
        ioc, router, token_store, subscription_tracker, uds_mux,
        cfg.bind_address, static_cast<uint16_t>(cfg.bind_port),
        cfg.max_connections, stopping);

    // =========================================================================
    // 18. Init TLS on HttpServer
    // =========================================================================
    if (cfg.tls_enabled()) {
        if (!http_server.init_tls(cfg.cert_path, cfg.key_path)) {
            spdlog::error("failed to initialize TLS -- check cert_path and key_path");
            return 1;
        }
        spdlog::info("  tls: enabled (TLS 1.3)");
        spdlog::info("  cert: {}", cfg.cert_path);
        spdlog::info("  key: {}", cfg.key_path);
    } else {
        spdlog::warn("  tls: DISABLED -- plain HTTP mode (use cert_path/key_path for TLS)");
    }

    // =========================================================================
    // 19. Wire metrics, health, gauge providers
    // =========================================================================
    metrics_collector.set_gauge_provider([&http_server, &subscription_tracker]() {
        return std::make_pair(http_server.active_connections(),
                              subscription_tracker.namespace_count());
    });
    metrics_collector.set_health_provider([&uds_mux]() {
        return uds_mux.is_connected();
    });

    // Log metrics_bind compatibility warning (D-38).
    if (!cfg.metrics_bind.empty()) {
        auto expected = cfg.bind_address + ":" + std::to_string(cfg.bind_port);
        if (cfg.metrics_bind != expected) {
            spdlog::warn("metrics_bind '{}' ignored -- /metrics is now served on the main HTTP "
                         "server at {}:{}", cfg.metrics_bind, cfg.bind_address, cfg.bind_port);
        }
    }

    // =========================================================================
    // 20. Spawn HttpServer accept loop
    // =========================================================================
    asio::co_spawn(ioc, http_server.accept_loop(), asio::detached);

    // =========================================================================
    // Idle session reaper (per D-05): periodic cleanup every 60s
    // =========================================================================
    auto idle_reaper = [&]() -> asio::awaitable<void> {
        while (!stopping.load(std::memory_order_relaxed)) {
            asio::steady_timer timer(ioc);
            timer.expires_after(std::chrono::seconds(60));
            auto [ec] = co_await timer.async_wait(
                asio::as_tuple(asio::use_awaitable));
            if (ec || stopping.load(std::memory_order_relaxed)) co_return;

            // Reap sessions idle for more than 10 minutes.
            auto reaped_ids = token_store.reap_idle(std::chrono::seconds(600));
            if (!reaped_ids.empty()) {
                spdlog::info("idle reaper: reaped {} idle session(s)", reaped_ids.size());
                // Clean up subscriptions for reaped sessions.
                for (auto sid : reaped_ids) {
                    auto empty_ns = subscription_tracker.remove_client(sid);
                    if (!empty_ns.empty() && uds_mux.is_connected()) {
                        // Forward Unsubscribe for namespaces that dropped to 0 subscribers.
                        std::vector<uint8_t> payload;
                        payload.reserve(2 + empty_ns.size() * 32);
                        uint8_t count_buf[2];
                        chromatindb::relay::util::store_u16_be(count_buf,
                            static_cast<uint16_t>(empty_ns.size()));
                        payload.insert(payload.end(), count_buf, count_buf + 2);
                        for (const auto& ns : empty_ns) {
                            payload.insert(payload.end(), ns.begin(), ns.end());
                        }
                        auto msg = chromatindb::relay::wire::TransportCodec::encode(
                            chromatindb::wire::TransportMsgType_Unsubscribe, payload, 0);
                        uds_mux.send(std::move(msg));
                        spdlog::debug("idle reaper: forwarded {} unsubscribe(s) to node for reaped session {}",
                                      empty_ns.size(), sid);
                    }
                }
            }
        }
    };
    asio::co_spawn(ioc, idle_reaper(), asio::detached);

    // =========================================================================
    // SIGTERM/SIGINT graceful shutdown
    // =========================================================================
    asio::signal_set term_signals(ioc, SIGTERM, SIGINT);
    term_signals.async_wait([&](const asio::error_code& ec, int sig) {
        if (ec) return;
        stopping.store(true, std::memory_order_relaxed);
        spdlog::info("received signal {}, graceful shutdown ({} sessions)",
                     sig, token_store.count());

        // 1. Stop accepting new connections.
        http_server.stop();

        // 2. Cancel all pending response promises.
        promise_map.cancel_all();

        // 3. Wait 2s for SSE writers to drain, then stop ioc.
        auto drain_timer = std::make_shared<asio::steady_timer>(ioc);
        drain_timer->expires_after(std::chrono::seconds(2));
        drain_timer->async_wait([&ioc, drain_timer](const asio::error_code&) {
            ioc.stop();
        });
    });

    // =========================================================================
    // SIGHUP config reload
    // =========================================================================
    asio::signal_set hup_signal(ioc, SIGHUP);
    std::function<void(const asio::error_code&, int)> hup_handler;
    hup_handler = [&](const asio::error_code& ec, int) {
        if (ec) return;
        spdlog::info("received SIGHUP -- reloading configuration");

        try {
            auto new_cfg = chromatindb::relay::config::load_relay_config(config_path);

            // Reload TLS if enabled
            if (new_cfg.tls_enabled()) {
                if (http_server.reload_tls(new_cfg.cert_path, new_cfg.key_path)) {
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

            // Reload max_connections
            http_server.set_max_connections(new_cfg.max_connections);

            // Reload rate limit
            rate_limit_rate.store(new_cfg.rate_limit_messages_per_sec, std::memory_order_relaxed);
            spdlog::info("rate_limit reloaded: {}",
                         new_cfg.rate_limit_messages_per_sec == 0
                             ? std::string("disabled")
                             : std::to_string(new_cfg.rate_limit_messages_per_sec) + " msg/s");

            // Reload max blob size
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

            // Metrics bind: log warning if changed (now part of main server)
            if (new_cfg.metrics_bind != cfg.metrics_bind && !new_cfg.metrics_bind.empty()) {
                spdlog::warn("metrics_bind changed to '{}' -- restart required (now served by main HTTP server)",
                             new_cfg.metrics_bind);
            }

        } catch (const std::exception& e) {
            spdlog::error("SIGHUP config reload failed: {}", e.what());
        }

        hup_signal.async_wait(hup_handler);
    };
    hup_signal.async_wait(hup_handler);

    // =========================================================================
    // Thread pool (per D-21): hardware_concurrency() threads running ioc.run()
    // =========================================================================
    auto thread_count = std::thread::hardware_concurrency();
    if (thread_count == 0) thread_count = 2;  // Fallback
    spdlog::info("  threads: {}", thread_count);

    spdlog::info("relay listening on {}:{}{}",
                cfg.bind_address, cfg.bind_port,
                cfg.tls_enabled() ? " (HTTPS)" : " (HTTP)");

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
