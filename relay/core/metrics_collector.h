#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace chromatindb::relay::core {

/// Counters for relay-level metrics (per D-04).
/// All fields accessed from single event loop thread. No synchronization needed.
/// Renamed from ws_ to http_ where transport-specific (Plan 08, D-28).
struct RelayMetrics {
    uint64_t http_connections_total{0};
    uint64_t http_disconnections_total{0};
    uint64_t messages_received_total{0};
    uint64_t messages_sent_total{0};
    uint64_t auth_failures_total{0};
    uint64_t rate_limited_total{0};
    uint64_t errors_total{0};
    uint64_t request_timeouts_total{0};
};

/// Pure metrics formatting class. No longer owns an HTTP accept loop --
/// /metrics and /health are served by the main HttpServer (Plan 08 merge).
///
/// Provides format_prometheus() for Prometheus text exposition and
/// gauge/health provider callbacks for scrape-time live data.
class MetricsCollector {
public:
    explicit MetricsCollector(const std::atomic<bool>& stopping);

    /// Access the metrics struct for incrementing counters.
    RelayMetrics& metrics() { return metrics_; }
    const RelayMetrics& metrics() const { return metrics_; }

    /// Gauge provider callback: returns (active_connections, active_subscriptions).
    using GaugeProvider = std::function<std::pair<size_t, size_t>()>;

    /// Set gauge provider for live data at scrape time.
    void set_gauge_provider(GaugeProvider provider) { gauge_provider_ = std::move(provider); }

    /// Health provider callback: returns true if node UDS is connected.
    using HealthProvider = std::function<bool()>;

    /// Set health provider for /health endpoint.
    void set_health_provider(HealthProvider provider) { health_provider_ = std::move(provider); }

    /// Get health status (for /health route handler).
    bool is_healthy() const { return health_provider_ ? health_provider_() : false; }

    /// Uptime in seconds since construction.
    uint64_t uptime_seconds() const;

    /// Format Prometheus text exposition output.
    /// If gauge_provider_ is set, uses it. Otherwise uses provided values.
    std::string format_prometheus(size_t active_connections, size_t active_subscriptions);

    /// Format Prometheus text exposition output using gauge_provider_ callback.
    std::string format_prometheus();

    /// Get the gauge provider (for /metrics route).
    const GaugeProvider& gauge_provider() const { return gauge_provider_; }

    /// Get the health provider (for /health route).
    const HealthProvider& health_provider() const { return health_provider_; }

private:
    const std::atomic<bool>& stopping_;
    RelayMetrics metrics_;
    std::chrono::steady_clock::time_point start_time_;
    GaugeProvider gauge_provider_;
    HealthProvider health_provider_;
};

} // namespace chromatindb::relay::core
