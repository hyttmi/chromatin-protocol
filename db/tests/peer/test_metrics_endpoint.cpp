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
#include <filesystem>
#include <fstream>

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

/// Helper: scrape /metrics with a caller-supplied Config. Used by value-fidelity tests.
/// Overrides bind_address + data_dir with test-only values regardless of what the
/// caller passed — those are environment-specific, not value-fidelity subjects.
std::string get_prometheus_text_with_cfg(const Config& cfg_override) {
    TempDir tmp;
    Config cfg = cfg_override;
    cfg.bind_address = "127.0.0.1:0";  // test-only bind
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

// =============================================================================
// Phase 128 VERI-04 + METRICS-01/METRICS-02: config gauge coverage,
//   value fidelity at construction, and SIGHUP-reload scrape fidelity.
// =============================================================================

TEST_CASE("prometheus_metrics_text contains chromatindb_config_* gauges for all numeric Config fields", "[metrics][prometheus][config-gauge]") {
    auto text = get_prometheus_text();

    // All 24 numeric fields per plan 128-04 (alphabetical order; trailing space
    // pins the gauge name as a whole token and excludes HELP/TYPE prefixes).
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_blob_max_bytes "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_blob_transfer_timeout "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_compaction_interval_hours "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_cursor_stale_seconds "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_full_resync_interval "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_log_max_files "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_log_max_size_mb "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_max_clients "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_max_peers "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_max_storage_bytes "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_max_subscriptions_per_connection "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_max_sync_sessions "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_max_ttl_seconds "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_namespace_quota_bytes "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_namespace_quota_count "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_pex_interval "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_rate_limit_burst "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_rate_limit_bytes_per_sec "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_safety_net_interval_seconds "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_strike_cooldown "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_strike_threshold "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_sync_cooldown_seconds "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_sync_timeout "));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_worker_threads "));

    // D-07: excluded string / vector / map fields must NOT appear as gauges.
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_bind_address"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_storage_path"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_data_dir"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_log_level"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_bootstrap_peers"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_sync_namespaces"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_allowed_client_keys"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_allowed_peer_keys"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_trusted_peers"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_namespace_quotas"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_log_format"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_log_file"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_uds_path"));
    REQUIRE_THAT(text, !ContainsSubstring("chromatindb_config_metrics_bind"));
}

TEST_CASE("prometheus_metrics_text config gauge values reflect Config at PeerManager construction", "[metrics][prometheus][config-gauge]") {
    // METRICS-01 value fidelity for the construction-time Config.
    // Proves the gauge reads the live Config, not a hardcoded default.
    Config cfg;
    cfg.blob_max_bytes = 8ULL * 1024 * 1024;   // custom 8 MiB (not default 4 MiB)
    cfg.max_peers = 77;                        // custom (not default 32)

    auto text = get_prometheus_text_with_cfg(cfg);

    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_blob_max_bytes 8388608\n"));
    REQUIRE_THAT(text, ContainsSubstring("chromatindb_config_max_peers 77\n"));
}

TEST_CASE("reload_config changes config gauge values without node restart (VERI-01 SIGHUP + METRICS-02 live-scrape)", "[metrics][prometheus][config-gauge][sighup]") {
    // Covers BOTH:
    //   - VERI-01 "SIGHUP reload" (ROADMAP success criterion 5) — exercised via
    //     PeerManager::reload_config(), the public method SIGHUP dispatches to
    //     in production (peer_manager.cpp handle_sighup → reload_config).
    //   - METRICS-02 — gauge value reflects the post-reload Config on the next
    //     scrape, no restart, no cache invalidation required.
    //
    // No signal infrastructure: reload_config() is the public reload entry
    // point; raising SIGHUP would only exercise one extra layer (signal →
    // async handler → reload_config), none of which is under test here.

    TempDir tmp;
    auto cfg_path = tmp.path / "chromatindb_config.json";
    std::filesystem::create_directories(tmp.path);

    // Step 1: write initial config.json with blob_max_bytes = 4 MiB.
    {
        std::ofstream f(cfg_path);
        f << R"({"bind_address": "127.0.0.1:0",)"
          << R"( "data_dir": ")" << tmp.path.string() << R"(",)"
          << R"( "blob_max_bytes": 4194304})";
    }

    // Step 2: construct PeerManager WITH config_path_ set (8th constructor arg).
    // Required — reload_config() reads from config_path_ member, not from an
    // argument, so the constructor must see the path up front.
    auto initial_cfg = chromatindb::config::load_config(cfg_path);
    // Honour test-only network binding so the constructor doesn't try to bind
    // to a production address if the JSON somehow omitted bind_address.
    initial_cfg.bind_address = "127.0.0.1:0";

    auto id = NodeIdentity::load_or_generate(tmp.path);
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    asio::io_context ioc;
    AccessControl acl({}, initial_cfg.allowed_peer_keys, id.namespace_id());
    PeerManager pm(initial_cfg, id, eng, store, ioc, pool, acl, cfg_path);
    //                                                         ^^^^^^^^ 8th arg

    // Step 3: first scrape — gauge reports the initial 4 MiB cap.
    auto pre_text = pm.prometheus_metrics_text();
    REQUIRE_THAT(pre_text, ContainsSubstring("chromatindb_config_blob_max_bytes 4194304\n"));

    // Step 4: overwrite the SAME config.json with blob_max_bytes = 8 MiB.
    // (reload_config always reads from config_path_, so rewrite in-place.)
    {
        std::ofstream f(cfg_path);
        f << R"({"bind_address": "127.0.0.1:0",)"
          << R"( "data_dir": ")" << tmp.path.string() << R"(",)"
          << R"( "blob_max_bytes": 8388608})";
    }

    // Step 5: call reload_config() — the exact method SIGHUP triggers.
    pm.reload_config();

    // Step 6: second scrape — gauge reports the new 8 MiB cap.
    auto post_text = pm.prometheus_metrics_text();
    REQUIRE_THAT(post_text, ContainsSubstring("chromatindb_config_blob_max_bytes 8388608\n"));

    // And the previous value is NO LONGER present.
    REQUIRE_THAT(post_text, !ContainsSubstring("chromatindb_config_blob_max_bytes 4194304\n"));
}
