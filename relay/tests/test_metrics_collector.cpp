#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "relay/core/metrics_collector.h"

#include <thread>

using chromatindb::relay::core::MetricsCollector;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("MetricsCollector: format prometheus counters", "[metrics_collector]") {
    std::atomic<bool> stopping{false};
    MetricsCollector mc(stopping);

    mc.metrics().http_connections_total = 42;
    mc.metrics().http_disconnections_total = 7;
    mc.metrics().messages_received_total = 1000;
    mc.metrics().messages_sent_total = 800;
    mc.metrics().auth_failures_total = 3;
    mc.metrics().rate_limited_total = 15;
    mc.metrics().errors_total = 2;

    auto text = mc.format_prometheus(0, 0);

    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_http_connections_total 42"));
    CHECK_THAT(text, ContainsSubstring("# TYPE chromatindb_relay_http_connections_total counter"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_http_disconnections_total 7"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_messages_received_total 1000"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_messages_sent_total 800"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_auth_failures_total 3"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_rate_limited_total 15"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_errors_total 2"));
}

TEST_CASE("MetricsCollector: format prometheus gauges", "[metrics_collector]") {
    std::atomic<bool> stopping{false};
    MetricsCollector mc(stopping);

    auto text = mc.format_prometheus(5, 3);

    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_http_connections_active 5"));
    CHECK_THAT(text, ContainsSubstring("# TYPE chromatindb_relay_http_connections_active gauge"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_subscriptions_active 3"));
    CHECK_THAT(text, ContainsSubstring("# TYPE chromatindb_relay_subscriptions_active gauge"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_uptime_seconds"));
    CHECK_THAT(text, ContainsSubstring("# TYPE chromatindb_relay_uptime_seconds gauge"));
}

TEST_CASE("MetricsCollector: uptime seconds", "[metrics_collector]") {
    std::atomic<bool> stopping{false};
    MetricsCollector mc(stopping);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // Uptime should be 0 or more seconds (sub-second might still be 0)
    CHECK(mc.uptime_seconds() >= 0);
}

TEST_CASE("MetricsCollector: health provider API", "[metrics_collector]") {
    std::atomic<bool> stopping{false};
    MetricsCollector mc(stopping);

    bool called = false;
    mc.set_health_provider([&called]() { called = true; return true; });
    // HealthProvider is stored -- not called until /health request arrives
    CHECK_FALSE(called);
}

TEST_CASE("MetricsCollector: atomic increments", "[metrics_collector]") {
    std::atomic<bool> stopping{false};
    MetricsCollector mc(stopping);

    for (int i = 0; i < 100; ++i) {
        ++mc.metrics().messages_received_total;
    }
    CHECK(mc.metrics().messages_received_total == 100);
}

TEST_CASE("MetricsCollector: format_prometheus no-arg uses gauge_provider", "[metrics_collector]") {
    std::atomic<bool> stopping{false};
    MetricsCollector mc(stopping);

    mc.set_gauge_provider([]() -> std::pair<size_t, size_t> {
        return {10, 5};
    });

    auto text = mc.format_prometheus();
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_http_connections_active 10"));
    CHECK_THAT(text, ContainsSubstring("chromatindb_relay_subscriptions_active 5"));
}

TEST_CASE("MetricsCollector: is_healthy delegates to health_provider", "[metrics_collector]") {
    std::atomic<bool> stopping{false};
    MetricsCollector mc(stopping);

    // No provider -> false
    CHECK_FALSE(mc.is_healthy());

    mc.set_health_provider([]() { return true; });
    CHECK(mc.is_healthy());

    mc.set_health_provider([]() { return false; });
    CHECK_FALSE(mc.is_healthy());
}
