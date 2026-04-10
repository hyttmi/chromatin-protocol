#pragma once

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace chromatindb::relay::core {

/// Atomic counters for relay-level metrics (per D-04).
/// All fields are atomic for safe concurrent access from multiple sessions.
struct RelayMetrics {
    std::atomic<uint64_t> ws_connections_total{0};
    std::atomic<uint64_t> ws_disconnections_total{0};
    std::atomic<uint64_t> messages_received_total{0};
    std::atomic<uint64_t> messages_sent_total{0};
    std::atomic<uint64_t> auth_failures_total{0};
    std::atomic<uint64_t> rate_limited_total{0};
    std::atomic<uint64_t> errors_total{0};
};

/// Prometheus /metrics HTTP endpoint and relay metrics management.
///
/// Mirrors the node's MetricsCollector pattern (db/peer/metrics_collector.h)
/// but exposes relay-specific counters and gauges with chromatindb_relay_ prefix.
class MetricsCollector {
public:
    MetricsCollector(asio::io_context& ioc, const std::string& metrics_bind,
                     const std::atomic<bool>& stopping);

    /// Start HTTP listener on metrics_bind address (if non-empty).
    void start();

    /// Stop HTTP listener.
    void stop();

    /// SIGHUP reload: stop, update bind, start (per D-15).
    void set_metrics_bind(const std::string& bind);

    /// Access the metrics struct for incrementing counters.
    RelayMetrics& metrics() { return metrics_; }
    const RelayMetrics& metrics() const { return metrics_; }

    /// Gauge provider callback: returns (active_connections, active_subscriptions).
    using GaugeProvider = std::function<std::pair<size_t, size_t>()>;

    /// Set gauge provider for live data at scrape time.
    void set_gauge_provider(GaugeProvider provider) { gauge_provider_ = std::move(provider); }

    /// Uptime in seconds since construction.
    uint64_t uptime_seconds() const;

    /// Format Prometheus text exposition output.
    /// @param active_connections Current WebSocket connection count (gauge).
    /// @param active_subscriptions Current subscription namespace count (gauge).
    std::string format_prometheus(size_t active_connections, size_t active_subscriptions);

private:
    asio::awaitable<void> accept_loop();
    asio::awaitable<void> handle_connection(asio::ip::tcp::socket socket);

    asio::io_context& ioc_;
    const std::atomic<bool>& stopping_;
    RelayMetrics metrics_;
    std::chrono::steady_clock::time_point start_time_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::string metrics_bind_;
    GaugeProvider gauge_provider_;
};

} // namespace chromatindb::relay::core
