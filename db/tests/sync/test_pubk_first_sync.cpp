// D-12(b) sync-replicated PUBK-first invariant coverage.
//
// Anchor: VALIDATION.md row
//   phase122/sync: PUBK-first rejects sync-replicated non-PUBK on fresh
//                  namespace (D-12b / SC#4 sync path)
//
// The PUBK-first gate lives in ONE place — engine.cpp Step 1.5. The sync
// path delegates per-blob to engine.ingest via SyncProtocol::ingest_blobs,
// so the single-site gate covers replicated writes without a parallel
// check in sync_protocol.cpp. These tests prove that architectural choice
// holds end-to-end by exercising the sync receive path:
//   Test 1 — non-PUBK first sync arrival rejected before any PUBK exists.
//   Test 2 — PUBK sync arrival registers the namespace, subsequent
//            non-PUBK sync arrival accepted.
//
// Tag: [phase122][pubk_first][sync].

#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>

#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/sync/sync_protocol.h"

#include "db/tests/test_helpers.h"

using chromatindb::engine::BlobEngine;
using chromatindb::identity::NodeIdentity;
using chromatindb::storage::Storage;
using chromatindb::sync::NamespacedBlob;
using chromatindb::sync::SyncProtocol;
using chromatindb::sync::SyncStats;
using chromatindb::test::TempDir;
using chromatindb::test::make_pubk_blob;
using chromatindb::test::make_signed_blob;
using chromatindb::test::ns_span;
using chromatindb::test::run_async;

namespace {

// Build a single-element NamespacedBlob vector for ingest_blobs.
std::vector<NamespacedBlob> one_ns_blob(std::span<const uint8_t, 32> ns,
                                        const chromatindb::wire::BlobData& blob) {
    std::array<uint8_t, 32> ns_arr;
    std::memcpy(ns_arr.data(), ns.data(), 32);
    return {NamespacedBlob{ns_arr, blob}};
}

} // namespace

// D-12(b): sync-replicated non-PUBK-first arrival must be rejected. The
// engine's Step 1.5 PUBK-first gate fires through the sync delegation path;
// no parallel check in sync_protocol.cpp is needed.
TEST_CASE("sync-replicated non-PUBK-first to fresh namespace rejected",
          "[phase122][pubk_first][sync]") {
    TempDir tmp1, tmp2;
    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    // Both engines share the helper register_pubk path only for store1 so
    // engine1.ingest accepts the blob we want to "send" over sync. store2
    // remains PRISTINE (no PUBK for this namespace) — the sync arrival is
    // the adversarial scenario under test.
    auto id = NodeIdentity::generate();
    chromatindb::test::register_pubk(store1, id);

    // Populate store1 with a regular blob that would normally sync onward.
    auto blob = make_signed_blob(id, "no-pubk-on-store2");
    REQUIRE(run_async(pool, engine1.ingest(ns_span(id), blob)).accepted);

    // Receiver side: sync the blob onto store2 via SyncProtocol::ingest_blobs.
    // store2 has no owner_pubkey for id.namespace_id(); Step 1.5 MUST reject.
    SyncProtocol sync2(engine2, store2, pool);
    auto ns_blobs = one_ns_blob(ns_span(id), blob);
    auto stats = run_async(pool, sync2.ingest_blobs(ns_blobs));

    // Engine rejected with pubk_first_violation -> blobs_received stays 0.
    REQUIRE(stats.blobs_received == 0);
    // The owner_pubkeys DBI on store2 must remain empty — the rejected
    // blob never triggered registration.
    REQUIRE_FALSE(store2.has_owner_pubkey(ns_span(id)));
    REQUIRE(store2.count_owner_pubkeys() == 0);
}

// Paired success: when a PUBK syncs FIRST, the namespace is established on
// the receiver; the subsequent non-PUBK sync arrival is accepted.
TEST_CASE("PUBK syncs first then regular blob both accepted on receiver",
          "[phase122][pubk_first][sync]") {
    TempDir tmp1, tmp2;
    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    auto id = NodeIdentity::generate();

    // store1 is the sender side; engine1 establishes the namespace normally
    // (PUBK ingested) so we have a real PUBK blob to ship over sync.
    auto pubk = make_pubk_blob(id);
    REQUIRE(run_async(pool, engine1.ingest(ns_span(id), pubk)).accepted);

    auto blob = make_signed_blob(id, "post-pubk");
    REQUIRE(run_async(pool, engine1.ingest(ns_span(id), blob)).accepted);

    SyncProtocol sync2(engine2, store2, pool);

    // Step 1: sync the PUBK — Step 4.5 registration on store2 passes,
    //         owner_pubkeys[ns] now populated.
    auto pubk_ns_blobs = one_ns_blob(ns_span(id), pubk);
    auto stats_pubk = run_async(pool, sync2.ingest_blobs(pubk_ns_blobs));
    REQUIRE(stats_pubk.blobs_received == 1);
    REQUIRE(store2.has_owner_pubkey(ns_span(id)));

    // Step 2: sync the regular blob — Step 1.5 gate passes (owner_pubkey
    //         present), Step 2 resolves via owner_pubkeys, Step 3 verify
    //         succeeds, blob stored.
    auto blob_ns_blobs = one_ns_blob(ns_span(id), blob);
    auto stats_blob = run_async(pool, sync2.ingest_blobs(blob_ns_blobs));
    REQUIRE(stats_blob.blobs_received == 1);
    REQUIRE(store2.count_owner_pubkeys() == 1);
}
