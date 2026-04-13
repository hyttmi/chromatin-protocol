#include "relay/core/metrics_collector.h"

#include <string>

namespace chromatindb::relay::core {

MetricsCollector::MetricsCollector(const std::atomic<bool>& stopping)
    : stopping_(stopping)
    , start_time_(std::chrono::steady_clock::now()) {
}

// =============================================================================
// Uptime
// =============================================================================

uint64_t MetricsCollector::uptime_seconds() const {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
}

// =============================================================================
// Prometheus text exposition format
// =============================================================================

std::string MetricsCollector::format_prometheus() {
    size_t active_conns = 0, active_subs = 0;
    if (gauge_provider_) {
        auto [conns, subs] = gauge_provider_();
        active_conns = conns;
        active_subs = subs;
    }
    return format_prometheus(active_conns, active_subs);
}

std::string MetricsCollector::format_prometheus(size_t active_connections,
                                                 size_t active_subscriptions) {
    std::string out;
    out.reserve(2048);

    // Counters (8 -- all RelayMetrics fields, per D-04)
    out += "# HELP chromatindb_relay_http_connections_total Total HTTP connections since startup.\n"
           "# TYPE chromatindb_relay_http_connections_total counter\n"
           "chromatindb_relay_http_connections_total " +
           std::to_string(metrics_.http_connections_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_http_disconnections_total Total HTTP disconnections since startup.\n"
           "# TYPE chromatindb_relay_http_disconnections_total counter\n"
           "chromatindb_relay_http_disconnections_total " +
           std::to_string(metrics_.http_disconnections_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_messages_received_total Total messages received since startup.\n"
           "# TYPE chromatindb_relay_messages_received_total counter\n"
           "chromatindb_relay_messages_received_total " +
           std::to_string(metrics_.messages_received_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_messages_sent_total Total messages sent since startup.\n"
           "# TYPE chromatindb_relay_messages_sent_total counter\n"
           "chromatindb_relay_messages_sent_total " +
           std::to_string(metrics_.messages_sent_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_auth_failures_total Total authentication failures since startup.\n"
           "# TYPE chromatindb_relay_auth_failures_total counter\n"
           "chromatindb_relay_auth_failures_total " +
           std::to_string(metrics_.auth_failures_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_rate_limited_total Total rate-limited messages since startup.\n"
           "# TYPE chromatindb_relay_rate_limited_total counter\n"
           "chromatindb_relay_rate_limited_total " +
           std::to_string(metrics_.rate_limited_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_errors_total Total errors since startup.\n"
           "# TYPE chromatindb_relay_errors_total counter\n"
           "chromatindb_relay_errors_total " +
           std::to_string(metrics_.errors_total.load(std::memory_order_relaxed)) + "\n\n";

    out += "# HELP chromatindb_relay_request_timeouts_total Total request timeouts since startup.\n"
           "# TYPE chromatindb_relay_request_timeouts_total counter\n"
           "chromatindb_relay_request_timeouts_total " +
           std::to_string(metrics_.request_timeouts_total.load(std::memory_order_relaxed)) + "\n\n";

    // Gauges (3 -- per D-05)
    out += "# HELP chromatindb_relay_http_connections_active Current active HTTP connections.\n"
           "# TYPE chromatindb_relay_http_connections_active gauge\n"
           "chromatindb_relay_http_connections_active " +
           std::to_string(active_connections) + "\n\n";

    out += "# HELP chromatindb_relay_subscriptions_active Current active namespace subscriptions.\n"
           "# TYPE chromatindb_relay_subscriptions_active gauge\n"
           "chromatindb_relay_subscriptions_active " +
           std::to_string(active_subscriptions) + "\n\n";

    out += "# HELP chromatindb_relay_uptime_seconds Relay uptime in seconds.\n"
           "# TYPE chromatindb_relay_uptime_seconds gauge\n"
           "chromatindb_relay_uptime_seconds " +
           std::to_string(uptime_seconds()) + "\n";

    return out;
}

} // namespace chromatindb::relay::core
