#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "relay/core/metrics_collector.h"

#include <thread>

using chromatindb::relay::core::MetricsCollector;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("MetricsCollector: format prometheus counters", "[metrics_collector]") {
    asio::io_context ioc;
    std::atomic<bool> stopping{false};
    MetricsCollector mc(ioc, "", stopping);

    mc.metrics().ws_connections_total.store(42, std::memory_order_relaxed);
    mc.metrics().ws_disconnections_total.store(7, std::memory_order_relaxed);
    mc.metrics().messages_received_total.store(1000, std::memory_order_relaxed);
    mc.metrics().messages_sent_total.store(800, std::memory_order_relaxed);
    mc.metrics().auth_failures_total.store(3, std::memory_order_relaxed);
    mc.metrics().rate_limited_total.store(15, std::memory_order_relaxed);
    mc.metrics().errors_total.store(2, std::memory_order_relaxed);

    auto text = mc.format_prometheus(0, 0);

    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_ws_connections_total 42"));
    CHECK_THAT(text, ContainsSubstring("# TYPE chromatindb_relay_ws_connections_total counter"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_ws_disconnections_total 7"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_messages_received_total 1000"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_messages_sent_total 800"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_auth_failures_total 3"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_rate_limited_total 15"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_errors_total 2"));
}

TEST_CASE("MetricsCollector: format prometheus gauges", "[metrics_collector]") {
    asio::io_context ioc;
    std::atomic<bool> stopping{false};
    MetricsCollector mc(ioc, "", stopping);

    auto text = mc.format_prometheus(5, 3);

    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_ws_connections_active 5"));
    CHECK_THAT(text, ContainsSubstring("# TYPE chromatindb_relay_ws_connections_active gauge"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_subscriptions_active 3"));
    CHECK_THAT(text, ContainsSubstring("# TYPE chromatindb_relay_subscriptions_active gauge"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_uptime_seconds"));
    CHECK_THAT(text, ContainsSubstring("# TYPE chromatindb_relay_uptime_seconds gauge"));
}

TEST_CASE("MetricsCollector: uptime seconds", "[metrics_collector]") {
    asio::io_context ioc;
    std::atomic<bool> stopping{false};
    MetricsCollector mc(ioc, "", stopping);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // Uptime should be 0 or more seconds (sub-second might still be 0)
    CHECK(mc.uptime_seconds() >= 0);
}

TEST_CASE("MetricsCollector: health provider API", "[metrics_collector]") {
    asio::io_context ioc;
    std::atomic<bool> stopping{false};
    MetricsCollector mc(ioc, "", stopping);

    bool called = false;
    mc.set_health_provider([&called]() { called = true; return true; });
    // HealthProvider is stored -- not called until /health request arrives
    CHECK_FALSE(called);
}

TEST_CASE("MetricsCollector: atomic increments", "[metrics_collector]") {
    asio::io_context ioc;
    std::atomic<bool> stopping{false};
    MetricsCollector mc(ioc, "", stopping);

    for (int i = 0; i < 100; ++i) {
        mc.metrics().messages_received_total.fetch_add(1, std::memory_order_relaxed);
    }
    CHECK(mc.metrics().messages_received_total.load(std::memory_order_relaxed) == 100);
}
