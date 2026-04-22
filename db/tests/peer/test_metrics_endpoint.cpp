#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "db/acl/access_control.h"
#include "db/peer/peer_manager.h"
#include "db/config/config.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"

#include "db/tests/test_helpers.h"

#include <asio.hpp>

using chromatindb::test::TempDir;
using chromatindb::acl::AccessControl;
using chromatindb::config::Config;
using chromatindb::engine::BlobEngine;
using chromatindb::identity::NodeIdentity;
using chromatindb::peer::PeerManager;
using chromatindb::storage::Storage;

using Catch::Matchers::ContainsSubstring;

namespace {

/// Helper: create a minimal PeerManager and return its prometheus_metrics_text().
std::string get_prometheus_text() {
    TempDir tmp;

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    auto id = NodeIdentity::load_or_generate(tmp.path);
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, id.namespace_id());
    PeerManager pm(cfg, id, eng, store, ioc, pool, acl);

    return pm.prometheus_metrics_text();
}

} // anonymous namespace

// =============================================================================
// Prometheus /metrics endpoint tests (OPS-02)
// =============================================================================

TEST_CASE("prometheus_metrics_text contains all counter metrics", "[metrics][prometheus]") {
    auto text = get_prometheus_text();

    REQUIRE_THAT(text, ContainsSubstring("chromatindb_ingests_total"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_rejections_total"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_syncs_total"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_rate_limited_total"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_peers_connected_total"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_peers_disconnected_total"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_cursor_hits_total"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_cursor_misses_total"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_full_resyncs_total"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_quota_rejections_total"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_sync_rejections_total"));
}

TEST_CASE("prometheus_metrics_text contains all gauge metrics", "[metrics][prometheus]") {
    auto text = get_prometheus_text();

    REQUIRE_THAT(text, ContainsSubstring("chromatindb_peers_connected "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_blobs_stored "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_storage_bytes "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_namespaces "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_uptime_seconds "));
}

TEST_CASE("prometheus_metrics_text has correct TYPE annotations", "[metrics][prometheus]") {
    auto text = get_prometheus_text();

    // Counters
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_ingests_total counter"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_rejections_total counter"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_syncs_total counter"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_rate_limited_total counter"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_peers_connected_total counter"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_peers_disconnected_total counter"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_cursor_hits_total counter"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_cursor_misses_total counter"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_full_resyncs_total counter"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_quota_rejections_total counter"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_sync_rejections_total counter"));

    // Gauges
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_peers_connected gauge"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_blobs_stored gauge"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_storage_bytes gauge"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_namespaces gauge"));
    REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_uptime_seconds gauge"));
}

TEST_CASE("prometheus_metrics_text has HELP lines for all metrics", "[metrics][prometheus]") {
    auto text = get_prometheus_text();

    // Spot-check HELP lines
    REQUIRE_THAT(text, ContainsSubstring("# HELP chromatindb_ingests_total"));
    REQUIRE_THAT(text, ContainsSubstring("# HELP chromatindb_rejections_total"));
    REQUIRE_THAT(text, ContainsSubstring("# HELP chromatindb_peers_connected"));
    REQUIRE_THAT(text, ContainsSubstring("# HELP chromatindb_blobs_stored"));
    REQUIRE_THAT(text, ContainsSubstring("# HELP chromatindb_storage_bytes"));
    REQUIRE_THAT(text, ContainsSubstring("# HELP chromatindb_namespaces"));
    REQUIRE_THAT(text, ContainsSubstring("# HELP chromatindb_uptime_seconds"));
}

TEST_CASE("prometheus_metrics_text ends with newline", "[metrics][prometheus]") {
    auto text = get_prometheus_text();
    REQUIRE(!text.empty());
    REQUIRE(text.back() == '\n');
}

TEST_CASE("prometheus_metrics_text counter values are zero initially", "[metrics][prometheus]") {
    auto text = get_prometheus_text();

    // Fresh PeerManager: all counters should be 0
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_ingests_total 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_rejections_total 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_syncs_total 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_rate_limited_total 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_peers_connected_total 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_peers_disconnected_total 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_cursor_hits_total 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_cursor_misses_total 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_full_resyncs_total 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_quota_rejections_total 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_sync_rejections_total 0"));

    // Gauges: peers_connected should be 0, blobs_stored 0, namespaces 0
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_peers_connected 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_blobs_stored 0"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_namespaces 0"));
}
