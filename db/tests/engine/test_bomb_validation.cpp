// BOMB ingest validation coverage (T-123-01..04, D-12, D-13).
//
// Tag taxonomy:
//   [phase123][engine][bomb_ttl]      — D-13(1) ttl != 0 rejection
//   [phase123][engine][bomb_sanity]   — D-13(2) header/size structural rejection
//   [phase123][engine][bomb_accept]   — D-13 positive paths (count==0 no-op, normal)
//   [phase123][engine][bomb_delegate] — D-12 delegate-reject extension
//
// Companion files: test_bomb_side_effect.cpp (Step 3.5 delete), test_name_delegate.cpp,
//                  test_name_overwrite.cpp.

#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"
#include "db/crypto/hash.h"

#include "db/tests/test_helpers.h"

using chromatindb::engine::BlobEngine;
using chromatindb::engine::IngestError;
using chromatindb::identity::NodeIdentity;
using chromatindb::storage::Storage;
using chromatindb::test::TempDir;
using chromatindb::test::current_timestamp;
using chromatindb::test::make_bomb_blob;
using chromatindb::test::make_pubk_blob;
using chromatindb::test::make_signed_blob;
using chromatindb::test::make_signed_delegation;
using chromatindb::test::ns_span;
using chromatindb::test::run_async;

namespace {

/// Build a ttl!=0 BOMB by bypassing make_bomb_blob (which enforces ttl=0).
/// Used exclusively to exercise the Step 1.7 ttl-rejection path.
chromatindb::wire::BlobData build_bomb_with_ttl_nonzero(
    const NodeIdentity& id,
    std::span<const std::array<uint8_t, 32>> targets,
    uint32_t ttl)
{
    chromatindb::wire::BlobData blob;
    auto hint = chromatindb::crypto::sha3_256(id.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data = chromatindb::wire::make_bomb_data(targets);
    blob.ttl = ttl;  // deliberately non-zero
    blob.timestamp = current_timestamp();
    auto si = chromatindb::wire::build_signing_input(
        id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(si);
    return blob;
}

/// Build a structurally malformed BOMB: declared count says 5 targets but
/// payload only carries 3 (size 8 + 3*32 = 104, expected 8 + 5*32 = 168).
/// Signed validly so the rejection is proven to be the Step 1.7 gate, not
/// the signature path.
chromatindb::wire::BlobData build_malformed_bomb(const NodeIdentity& id) {
    chromatindb::wire::BlobData blob;
    auto hint = chromatindb::crypto::sha3_256(id.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);

    // [BOMB:4][count:4 BE = 5][target_hash:32 × 3]  (size = 8 + 96 = 104, not 168)
    blob.data.clear();
    blob.data.insert(blob.data.end(),
                     chromatindb::wire::BOMB_MAGIC.begin(),
                     chromatindb::wire::BOMB_MAGIC.end());
    // count = 5 big-endian
    blob.data.push_back(0x00);
    blob.data.push_back(0x00);
    blob.data.push_back(0x00);
    blob.data.push_back(0x05);
    // 3 target hashes (but count says 5)
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 32; ++j) {
            blob.data.push_back(static_cast<uint8_t>(i));
        }
    }
    blob.ttl = 0;
    blob.timestamp = current_timestamp();
    auto si = chromatindb::wire::build_signing_input(
        id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(si);
    return blob;
}

} // namespace

// T-123-03 / D-13(1): BOMB with ttl != 0 must be rejected with bomb_ttl_nonzero.
// Runs BEFORE crypto::offload (Step 1.7) so the reject path is cheap.
TEST_CASE("BOMB with ttl!=0 is rejected", "[phase123][engine][bomb_ttl]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    // Clear PUBK-first gate so the BOMB reaches Step 1.7.
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    // Single target — content doesn't matter for this test.
    std::array<uint8_t, 32> target{};
    target.fill(0xAB);
    std::vector<std::array<uint8_t, 32>> targets{target};

    auto bomb = build_bomb_with_ttl_nonzero(
        id, std::span<const std::array<uint8_t, 32>>(targets), /*ttl*/3600);

    auto result = run_async(pool, engine.ingest(ns_span(id), bomb));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::bomb_ttl_nonzero);
}

// T-123-02 / D-13(2): Structural sanity — declared count != payload length
// must be rejected with bomb_malformed at Step 1.7 (BEFORE crypto offload).
TEST_CASE("BOMB with wrong payload length is rejected",
          "[phase123][engine][bomb_sanity]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    auto bad = build_malformed_bomb(id);
    auto result = run_async(pool, engine.ingest(ns_span(id), bad));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::bomb_malformed);
}

// T-123-04 / A2: BOMB with count=0 is structurally valid and accepted as a no-op.
// The side-effect loop at Step 3.5 iterates zero targets, storage unchanged.
TEST_CASE("BOMB with count=0 is accepted as no-op",
          "[phase123][engine][bomb_accept]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    // Snapshot blob count post-PUBK (PUBK itself occupies one seq slot).
    auto refs_before = store.get_blob_refs_since(ns_span(id), 0, 1024);
    size_t baseline_count = refs_before.size();

    std::vector<std::array<uint8_t, 32>> empty_targets;
    auto bomb = make_bomb_blob(
        id, std::span<const std::array<uint8_t, 32>>(empty_targets));

    auto result = run_async(pool, engine.ingest(ns_span(id), bomb));
    REQUIRE(result.accepted);

    // After ingest: baseline PUBK + 1 BOMB blob itself — the side-effect
    // deleted ZERO targets (count=0 loop), so no existing blob was removed.
    auto refs_after = store.get_blob_refs_since(ns_span(id), 0, 1024);
    REQUIRE(refs_after.size() == baseline_count + 1);
}

// T-123-01 / D-12: delegate-signed BOMB must be rejected with
// bomb_delegate_not_allowed. Engine resolves signer via delegation_map
// (is_delegate=true) and Step 2 trailer trips the new is_bomb branch.
TEST_CASE("Delegate BOMB is rejected",
          "[phase123][engine][bomb_delegate]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = NodeIdentity::generate();
    auto delegate = NodeIdentity::generate();

    // 1. Owner publishes PUBK (clears PUBK-first gate + registers owner_pubkeys).
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), make_pubk_blob(owner))).accepted);

    // 2. Owner publishes a delegation blob for `delegate` in owner's namespace
    //    (engine.store path indexes delegation_map on store).
    auto delegation = make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), delegation)).accepted);

    // 3. Owner publishes a content blob so the delegate has a legitimate target.
    auto content = make_signed_blob(owner, "bomb-target-content");
    auto content_result = run_async(pool, engine.ingest(ns_span(owner), content));
    REQUIRE(content_result.accepted);
    REQUIRE(content_result.ack.has_value());
    std::array<uint8_t, 32> target_hash = content_result.ack->blob_hash;

    // 4. Delegate constructs a BOMB blob targeting the owner's content,
    //    signed by the delegate. target_namespace = owner_ns (D-01).
    //    Engine resolves signer via delegation_map → is_delegate=true →
    //    rejects at Step 2 trailer with bomb_delegate_not_allowed.
    chromatindb::wire::BlobData delegate_bomb;
    auto delegate_hint = chromatindb::crypto::sha3_256(delegate.public_key());
    std::memcpy(delegate_bomb.signer_hint.data(), delegate_hint.data(), 32);
    std::vector<std::array<uint8_t, 32>> targets{target_hash};
    delegate_bomb.data = chromatindb::wire::make_bomb_data(
        std::span<const std::array<uint8_t, 32>>(targets));
    delegate_bomb.ttl = 0;
    delegate_bomb.timestamp = current_timestamp();
    auto si = chromatindb::wire::build_signing_input(
        owner.namespace_id(), delegate_bomb.data, delegate_bomb.ttl,
        delegate_bomb.timestamp);
    delegate_bomb.signature = delegate.sign(si);

    auto result = run_async(pool, engine.ingest(ns_span(owner), delegate_bomb));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::bomb_delegate_not_allowed);
}

// Positive: owner BOMB with ttl=0 and valid structure is accepted.
TEST_CASE("Owner BOMB with valid header and ttl=0 is accepted",
          "[phase123][engine][bomb_accept]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    // Write a content blob A — the BOMB will target it.
    auto content = make_signed_blob(id, "valid-bomb-target");
    auto content_result = run_async(pool, engine.ingest(ns_span(id), content));
    REQUIRE(content_result.accepted);
    REQUIRE(content_result.ack.has_value());
    std::array<uint8_t, 32> target_a = content_result.ack->blob_hash;

    std::vector<std::array<uint8_t, 32>> targets{target_a};
    auto bomb = make_bomb_blob(id, std::span<const std::array<uint8_t, 32>>(targets));

    auto result = run_async(pool, engine.ingest(ns_span(id), bomb));
    REQUIRE(result.accepted);
    REQUIRE(result.ack.has_value());

    // Post-condition: target blob is gone (Step 3.5 side-effect ran).
    REQUIRE_FALSE(store.has_blob(ns_span(id), target_a));
}
