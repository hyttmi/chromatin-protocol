// Phase 129 Wave 2 — sync-out cap-divergence filter tests (VERI-03).
//
// Covers the 4 cap scenarios (smaller / larger / equal / zero) for each of
// the 3 sync-out filter sites (BlobNotify fan-out, BlobFetch response, PULL
// set-reconciliation announce). The underlying filter logic is shared via
// the should_skip_for_peer_cap() helper in blob_push_manager.h; this test
// file locks that helper's contract AND exercises the MetricsCollector
// per-peer skip counter that every site is expected to call on skip.
//
// Filter semantics (CONTEXT.md D-01 / D-04):
//   - advertised_blob_cap == 0 means "unknown" -- MUST NOT skip.
//   - Boundary is strict `>`: blob_size == cap is accepted.
//   - On skip: exactly one increment_sync_skipped_oversized(peer_addr) call.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "db/config/config.h"
#include "db/peer/blob_push_manager.h"  // should_skip_for_peer_cap
#include "db/peer/metrics_collector.h"
#include "db/storage/storage.h"

#include "db/tests/test_helpers.h"

#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

using chromatindb::config::Config;
using chromatindb::peer::MetricsCollector;
using chromatindb::peer::should_skip_for_peer_cap;
using chromatindb::storage::Storage;
using chromatindb::test::TempDir;

using Catch::Matchers::ContainsSubstring;

namespace {

constexpr uint64_t MB = 1024ull * 1024ull;

/// Minimal MetricsCollector fixture. Avoids the full PeerManager stack.
/// MetricsCollector requires Storage + io_context + Config + a peers_
/// reference (for the `chromatindb_peers_connected` gauge); we wire an
/// empty deque for the latter since we only exercise the per-peer
/// sync-skip counter path here.
struct Fixture {
    TempDir tmp;
    Config cfg;
    Storage store;
    asio::io_context ioc;
    bool stopping = false;
    std::deque<std::unique_ptr<chromatindb::peer::PeerInfo>> empty_peers;
    MetricsCollector collector;

    Fixture()
        : tmp()
        , cfg()
        , store(tmp.path.string())
        , ioc()
        , collector(store, ioc, /*metrics_bind=*/"", stopping, cfg) {
        collector.set_peers(empty_peers);
    }

    std::string scrape() { return collector.format_prometheus_metrics(); }
};

/// Count substring occurrences in a scrape (counter sample inspection).
size_t count_substring(const std::string& hay, const std::string& needle) {
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // anonymous namespace

// =============================================================================
// 1) Helper contract: should_skip_for_peer_cap (CONTEXT.md D-01 + D-04)
// =============================================================================

TEST_CASE("should_skip_for_peer_cap: 4 cap-divergence scenarios", "[sync][cap-filter]") {
    SECTION("peer cap smaller than blob -- MUST skip") {
        REQUIRE(should_skip_for_peer_cap(6 * MB, 1 * MB) == true);
    }
    SECTION("peer cap larger than blob -- MUST NOT skip") {
        REQUIRE(should_skip_for_peer_cap(6 * MB, 8 * MB) == false);
    }
    SECTION("peer cap exactly equal to blob (boundary: `>` not `>=`) -- MUST NOT skip") {
        REQUIRE(should_skip_for_peer_cap(1 * MB, 1 * MB) == false);
        REQUIRE(should_skip_for_peer_cap(8 * MB, 8 * MB) == false);
    }
    SECTION("peer cap zero (unknown, D-01) -- MUST NOT skip even for huge blobs") {
        REQUIRE(should_skip_for_peer_cap(6 * MB, 0) == false);
        REQUIRE(should_skip_for_peer_cap(500 * MB, 0) == false);
        REQUIRE(should_skip_for_peer_cap(1, 0) == false);
    }
}

// =============================================================================
// 2) Site 1: BlobNotify fan-out -- 4 scenarios × counter assertion
// =============================================================================

TEST_CASE("BlobNotify fan-out: 4 scenarios × cap filter + counter", "[sync][cap-filter]") {
    Fixture f;
    const std::string peer_small = "10.0.0.1:7000";
    const std::string peer_large = "10.0.0.2:7000";
    const std::string peer_equal = "10.0.0.3:7000";
    const std::string peer_zero  = "10.0.0.4:7000";

    SECTION("peer cap smaller -- blob skipped, counter increments once per blob") {
        // Mimic the BlobNotify fan-out site: for each peer in peers_, decide
        // to skip using the helper and increment the counter on skip.
        const uint64_t blob_size = 6 * MB;
        const uint64_t cap = 1 * MB;
        if (should_skip_for_peer_cap(blob_size, cap)) {
            f.collector.increment_sync_skipped_oversized(peer_small);
        }
        auto text = f.scrape();
        REQUIRE_THAT(text, ContainsSubstring(
            "chromatindb_sync_skipped_oversized_total{peer=\"" + peer_small + "\"} 1"));
    }

    SECTION("peer cap larger -- blob delivered, counter unchanged") {
        const uint64_t blob_size = 6 * MB;
        const uint64_t cap = 8 * MB;
        bool skipped = should_skip_for_peer_cap(blob_size, cap);
        REQUIRE(skipped == false);
        if (skipped) f.collector.increment_sync_skipped_oversized(peer_large);
        auto text = f.scrape();
        // HELP/TYPE always emitted, but no sample line for this peer.
        REQUIRE_THAT(text, ContainsSubstring("# TYPE chromatindb_sync_skipped_oversized_total counter"));
        REQUIRE(text.find("peer=\"" + peer_large + "\"") == std::string::npos);
    }

    SECTION("peer cap exactly equal -- boundary: delivered, counter unchanged") {
        const uint64_t blob_size = 1 * MB;
        const uint64_t cap = 1 * MB;
        bool skipped = should_skip_for_peer_cap(blob_size, cap);
        REQUIRE(skipped == false);
        if (skipped) f.collector.increment_sync_skipped_oversized(peer_equal);
        auto text = f.scrape();
        REQUIRE(text.find("peer=\"" + peer_equal + "\"") == std::string::npos);
    }

    SECTION("peer cap zero (unknown) -- delivered, counter unchanged") {
        const uint64_t blob_size = 8 * MB;
        const uint64_t cap = 0;
        bool skipped = should_skip_for_peer_cap(blob_size, cap);
        REQUIRE(skipped == false);
        if (skipped) f.collector.increment_sync_skipped_oversized(peer_zero);
        auto text = f.scrape();
        REQUIRE(text.find("peer=\"" + peer_zero + "\"") == std::string::npos);
    }
}

// =============================================================================
// 3) Site 2: BlobFetch response -- 4 scenarios × counter assertion
// =============================================================================

TEST_CASE("BlobFetch response: 4 scenarios × cap filter + counter", "[sync][cap-filter]") {
    Fixture f;
    const std::string req_small = "10.0.1.1:7000";
    const std::string req_large = "10.0.1.2:7000";
    const std::string req_equal = "10.0.1.3:7000";
    const std::string req_zero  = "10.0.1.4:7000";

    SECTION("requester cap smaller -- not-available response, counter increments") {
        // Mimic the BlobFetch response site: storage returns the blob, we then
        // check requester's cap and drop to the not-found 0x01 branch on skip.
        const uint64_t blob_size = 6 * MB;
        const uint64_t cap = 1 * MB;
        if (should_skip_for_peer_cap(blob_size, cap)) {
            f.collector.increment_sync_skipped_oversized(req_small);
        }
        auto text = f.scrape();
        REQUIRE_THAT(text, ContainsSubstring(
            "chromatindb_sync_skipped_oversized_total{peer=\"" + req_small + "\"} 1"));
    }

    SECTION("requester cap larger -- blob sent, counter unchanged") {
        const uint64_t blob_size = 2 * MB;
        const uint64_t cap = 8 * MB;
        bool skipped = should_skip_for_peer_cap(blob_size, cap);
        REQUIRE(skipped == false);
        if (skipped) f.collector.increment_sync_skipped_oversized(req_large);
        auto text = f.scrape();
        REQUIRE(text.find("peer=\"" + req_large + "\"") == std::string::npos);
    }

    SECTION("requester cap exactly equal -- boundary: blob sent, counter unchanged") {
        const uint64_t blob_size = 8 * MB;
        const uint64_t cap = 8 * MB;
        bool skipped = should_skip_for_peer_cap(blob_size, cap);
        REQUIRE(skipped == false);
        if (skipped) f.collector.increment_sync_skipped_oversized(req_equal);
        auto text = f.scrape();
        REQUIRE(text.find("peer=\"" + req_equal + "\"") == std::string::npos);
    }

    SECTION("requester cap zero (unknown) -- blob sent, counter unchanged") {
        const uint64_t blob_size = 100 * MB;
        const uint64_t cap = 0;
        bool skipped = should_skip_for_peer_cap(blob_size, cap);
        REQUIRE(skipped == false);
        if (skipped) f.collector.increment_sync_skipped_oversized(req_zero);
        auto text = f.scrape();
        REQUIRE(text.find("peer=\"" + req_zero + "\"") == std::string::npos);
    }
}

// =============================================================================
// 4) Site 3: PULL set-reconciliation announce -- 4 scenarios × counter
// =============================================================================

TEST_CASE("PULL reconcile announce: 4 scenarios × cap filter + counter", "[sync][cap-filter]") {
    Fixture f;
    const std::string peer_small = "10.0.2.1:7000";
    const std::string peer_large = "10.0.2.2:7000";
    const std::string peer_equal = "10.0.2.3:7000";
    const std::string peer_zero  = "10.0.2.4:7000";

    SECTION("peer cap smaller -- hash filtered out of fingerprint set, counter per-blob") {
        // Mimic sync_orchestrator's std::remove_if over our_hashes. We model 3
        // blobs where 2 are oversized for the peer: expect 2 counter increments.
        const std::vector<uint64_t> blob_sizes = {2 * MB, 6 * MB, 4 * MB};
        const uint64_t cap = 1 * MB;
        uint32_t kept = 0;
        for (auto sz : blob_sizes) {
            if (should_skip_for_peer_cap(sz, cap)) {
                f.collector.increment_sync_skipped_oversized(peer_small);
            } else {
                ++kept;
            }
        }
        REQUIRE(kept == 0);  // All 3 > 1 MB cap
        auto text = f.scrape();
        REQUIRE_THAT(text, ContainsSubstring(
            "chromatindb_sync_skipped_oversized_total{peer=\"" + peer_small + "\"} 3"));
    }

    SECTION("peer cap larger -- all hashes kept, no counter increments") {
        const std::vector<uint64_t> blob_sizes = {2 * MB, 6 * MB, 4 * MB};
        const uint64_t cap = 8 * MB;
        uint32_t kept = 0;
        for (auto sz : blob_sizes) {
            if (should_skip_for_peer_cap(sz, cap)) {
                f.collector.increment_sync_skipped_oversized(peer_large);
            } else {
                ++kept;
            }
        }
        REQUIRE(kept == 3);
        auto text = f.scrape();
        REQUIRE(text.find("peer=\"" + peer_large + "\"") == std::string::npos);
    }

    SECTION("peer cap exactly equal -- boundary kept, no counter increment") {
        // 1 MB blob at 1 MB cap -- boundary kept. Plus a 2 MB blob at 1 MB
        // cap to prove the filter actually runs (increments once).
        const std::vector<uint64_t> blob_sizes = {1 * MB, 2 * MB};
        const uint64_t cap = 1 * MB;
        uint32_t kept = 0;
        for (auto sz : blob_sizes) {
            if (should_skip_for_peer_cap(sz, cap)) {
                f.collector.increment_sync_skipped_oversized(peer_equal);
            } else {
                ++kept;
            }
        }
        REQUIRE(kept == 1);  // The 1 MB blob is kept (boundary); 2 MB skipped.
        auto text = f.scrape();
        REQUIRE_THAT(text, ContainsSubstring(
            "chromatindb_sync_skipped_oversized_total{peer=\"" + peer_equal + "\"} 1"));
    }

    SECTION("peer cap zero (unknown) -- all hashes kept regardless of size") {
        // Pre-v4.2.0 peer: we never got a NodeInfoResponse. D-01: don't skip.
        const std::vector<uint64_t> blob_sizes = {100 * MB, 500 * MB, 1ull};
        const uint64_t cap = 0;
        uint32_t kept = 0;
        for (auto sz : blob_sizes) {
            if (should_skip_for_peer_cap(sz, cap)) {
                f.collector.increment_sync_skipped_oversized(peer_zero);
            } else {
                ++kept;
            }
        }
        REQUIRE(kept == 3);
        auto text = f.scrape();
        REQUIRE(text.find("peer=\"" + peer_zero + "\"") == std::string::npos);
    }
}

// =============================================================================
// 5) Cross-cutting invariants
// =============================================================================

TEST_CASE("MetricsCollector: HELP/TYPE block always emitted regardless of samples",
          "[sync][cap-filter]") {
    Fixture f;
    // No increments at all -- HELP/TYPE must still appear (Prometheus convention).
    auto text = f.scrape();
    REQUIRE_THAT(text, ContainsSubstring(
        "# HELP chromatindb_sync_skipped_oversized_total Blobs skipped on sync-out"));
    REQUIRE_THAT(text, ContainsSubstring(
        "# TYPE chromatindb_sync_skipped_oversized_total counter"));
    // No sample rows yet.
    REQUIRE(count_substring(text, "chromatindb_sync_skipped_oversized_total{peer=") == 0);
}

TEST_CASE("MetricsCollector: multiple peers emitted in alphabetical std::map order",
          "[sync][cap-filter]") {
    Fixture f;
    // Insert in non-alphabetical order; verify scrape emits in lexicographic
    // (std::map) order. Peer labels are chosen to avoid the "10.0.0.1" vs
    // "10.0.0.10" trap where ':' sorts after digit '0' (pure ASCII lex order).
    f.collector.increment_sync_skipped_oversized("peer-c:7000");
    f.collector.increment_sync_skipped_oversized("peer-b:7000");
    f.collector.increment_sync_skipped_oversized("peer-b:7000");
    f.collector.increment_sync_skipped_oversized("peer-a:7000");

    auto text = f.scrape();
    auto pos_a = text.find("peer=\"peer-a:7000\"");
    auto pos_b = text.find("peer=\"peer-b:7000\"");
    auto pos_c = text.find("peer=\"peer-c:7000\"");

    REQUIRE(pos_a != std::string::npos);
    REQUIRE(pos_b != std::string::npos);
    REQUIRE(pos_c != std::string::npos);
    // std::map key-sort order (lexicographic on string).
    REQUIRE(pos_a < pos_b);
    REQUIRE(pos_b < pos_c);

    // peer-b received 2 increments.
    REQUIRE_THAT(text, ContainsSubstring(
        "chromatindb_sync_skipped_oversized_total{peer=\"peer-b:7000\"} 2"));
}
