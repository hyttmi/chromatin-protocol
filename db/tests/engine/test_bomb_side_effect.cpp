// BOMB side-effect coverage (Step 3.5 — delete N targets).
//
// Tags:
//   [phase123][engine][bomb_side_effect] — positive side-effect cases
//
// Proves SC#6 (cdb rm batched-tombstone semantics — storage side) and D-14
// (no per-target existence check; absent targets are no-op).

#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

#include "db/tests/test_helpers.h"

using chromatindb::engine::BlobEngine;
using chromatindb::identity::NodeIdentity;
using chromatindb::storage::Storage;
using chromatindb::test::TempDir;
using chromatindb::test::make_bomb_blob;
using chromatindb::test::make_pubk_blob;
using chromatindb::test::make_signed_blob;
using chromatindb::test::ns_span;
using chromatindb::test::run_async;

TEST_CASE("BOMB with N=3 targets tombstones all three content blobs",
          "[phase123][engine][bomb_side_effect]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    // Ingest 3 content blobs A, B, C — capture their content hashes.
    auto blob_a = make_signed_blob(id, "content-A");
    auto blob_b = make_signed_blob(id, "content-B");
    auto blob_c = make_signed_blob(id, "content-C");

    auto r_a = run_async(pool, engine.ingest(ns_span(id), blob_a));
    auto r_b = run_async(pool, engine.ingest(ns_span(id), blob_b));
    auto r_c = run_async(pool, engine.ingest(ns_span(id), blob_c));
    REQUIRE(r_a.accepted); REQUIRE(r_a.ack.has_value());
    REQUIRE(r_b.accepted); REQUIRE(r_b.ack.has_value());
    REQUIRE(r_c.accepted); REQUIRE(r_c.ack.has_value());

    auto h_a = r_a.ack->blob_hash;
    auto h_b = r_b.ack->blob_hash;
    auto h_c = r_c.ack->blob_hash;

    // Pre-condition: all three are present.
    REQUIRE(store.has_blob(ns_span(id), h_a));
    REQUIRE(store.has_blob(ns_span(id), h_b));
    REQUIRE(store.has_blob(ns_span(id), h_c));

    // Single BOMB covering {A, B, C}.
    std::vector<std::array<uint8_t, 32>> targets{h_a, h_b, h_c};
    auto bomb = make_bomb_blob(id, std::span<const std::array<uint8_t, 32>>(targets));

    auto bomb_result = run_async(pool, engine.ingest(ns_span(id), bomb));
    REQUIRE(bomb_result.accepted);

    // Post-condition: all three targets removed by Step 3.5 side-effect.
    REQUIRE_FALSE(store.has_blob(ns_span(id), h_a));
    REQUIRE_FALSE(store.has_blob(ns_span(id), h_b));
    REQUIRE_FALSE(store.has_blob(ns_span(id), h_c));
}

// D-14: BOMB does NOT verify target existence. A BOMB with an unknown target
// is accepted; present targets are tombstoned; absent target is a no-op.
TEST_CASE("BOMB with one missing target succeeds for present targets",
          "[phase123][engine][bomb_side_effect]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    auto blob_a = make_signed_blob(id, "present-A");
    auto blob_b = make_signed_blob(id, "present-B");
    auto r_a = run_async(pool, engine.ingest(ns_span(id), blob_a));
    auto r_b = run_async(pool, engine.ingest(ns_span(id), blob_b));
    REQUIRE(r_a.accepted); REQUIRE(r_b.accepted);
    auto h_a = r_a.ack->blob_hash;
    auto h_b = r_b.ack->blob_hash;

    // Fabricate a target hash that does NOT correspond to any stored blob.
    std::array<uint8_t, 32> h_absent{};
    h_absent.fill(0xCD);
    REQUIRE_FALSE(store.has_blob(ns_span(id), h_absent));

    std::vector<std::array<uint8_t, 32>> targets{h_a, h_absent, h_b};
    auto bomb = make_bomb_blob(id, std::span<const std::array<uint8_t, 32>>(targets));

    auto bomb_result = run_async(pool, engine.ingest(ns_span(id), bomb));
    REQUIRE(bomb_result.accepted);  // D-14: node does not check target existence

    // Present targets deleted; absent target has no effect (still absent).
    REQUIRE_FALSE(store.has_blob(ns_span(id), h_a));
    REQUIRE_FALSE(store.has_blob(ns_span(id), h_b));
    REQUIRE_FALSE(store.has_blob(ns_span(id), h_absent));
}
