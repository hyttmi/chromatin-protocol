// PUBK-first invariant coverage — D-12 (a, c, d, e).
//
// Anchors: VALIDATION.md rows
//   phase122/engine: PUBK-first rejects non-PUBK on fresh namespace (D-12a)
//   phase122/engine: non-PUBK after PUBK succeeds (D-12c)
//   phase122/engine: PUBK after PUBK idempotent when signing key matches (D-12d)
//   phase122/engine: PUBK with different signing key rejected with PUBK_MISMATCH (D-12e)
//
// Tag: [phase122][pubk_first][engine]. Scope is direct-ingest via
// BlobEngine::ingest. D-12(b) sync-path and D-12(f) TSAN race live in
// sibling files (test_pubk_first_sync.cpp, test_pubk_first_tsan.cpp).

#include <array>
#include <cstring>

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
using chromatindb::test::make_pubk_blob;
using chromatindb::test::make_signed_blob;
using chromatindb::test::ns_span;
using chromatindb::test::run_async;

// D-12(a): A non-PUBK first write to a fresh namespace must be rejected
// BEFORE any crypto offload. Step 1.5 gate in engine.cpp fires on
// `!storage_.has_owner_pubkey(target_namespace) && !wire::is_pubkey_blob(blob.data)`.
TEST_CASE("non-PUBK first write to fresh namespace rejected with pubk_first_violation",
          "[phase122][pubk_first][engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    // Deliberately DO NOT register_pubk or ingest a PUBK first.
    auto blob = make_signed_blob(id, "no-pubk-first");

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));

    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::pubk_first_violation);
    REQUIRE(store.count_owner_pubkeys() == 0);
}

// D-12(c): After a PUBK establishes the namespace, subsequent non-PUBK
// writes from the same owner are accepted.
TEST_CASE("PUBK then regular blob both accepted in fresh namespace",
          "[phase122][pubk_first][engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();

    // PUBK first clears the gate (Step 4.5 registers in owner_pubkeys).
    auto pubk = make_pubk_blob(id);
    auto pubk_result = run_async(pool, engine.ingest(ns_span(id), pubk));
    REQUIRE(pubk_result.accepted);
    REQUIRE(store.has_owner_pubkey(ns_span(id)));
    REQUIRE(store.count_owner_pubkeys() == 1);

    // Subsequent regular write succeeds — Step 2 resolves via owner_pubkeys.
    auto blob = make_signed_blob(id, "after-pubk");
    auto blob_result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(blob_result.accepted);

    REQUIRE(store.count_owner_pubkeys() == 1);
}

// D-12(d): Re-ingesting a PUBK with the SAME signing pubkey is idempotent.
// Storage::register_owner_pubkey silently accepts identical bytes; the
// main-DBI store path dedups on content_hash for bit-identical bodies.
// Re-ingesting with a rotated KEM (different KEM bytes but same signing_pk)
// is accepted: signing_pk bytes in owner_pubkeys unchanged, main-DBI blob
// is a new entry (different content_hash).
TEST_CASE("PUBK with matching signing pubkey — idempotent / KEM rotation accepted",
          "[phase122][pubk_first][engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();

    // First PUBK (zero-filled KEM — the default make_pubk_blob KEM).
    auto pubk1 = make_pubk_blob(id);
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), pubk1)).accepted);
    REQUIRE(store.count_owner_pubkeys() == 1);

    // Bit-identical re-ingest must be accepted (idempotent at register,
    // dedup at store). count_owner_pubkeys unchanged.
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), pubk1)).accepted);
    REQUIRE(store.count_owner_pubkeys() == 1);

    // KEM rotation: same signing_pk, different KEM bytes. Accepted:
    // owner_pubkeys is keyed by signer_hint (SHA3(signing_pk)); the KEM
    // portion sits in the PUBK body (main-DBI blob). Different KEM bytes
    // produce a different content_hash, so Step 2.5 dedup misses and the
    // new PUBK enters storage — but register_owner_pubkey sees identical
    // signing_pk bytes and returns silently.
    std::array<uint8_t, 1568> kem_rotated;
    kem_rotated.fill(0x22);
    auto pubk2 = make_pubk_blob(id, kem_rotated);
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), pubk2)).accepted);
    REQUIRE(store.count_owner_pubkeys() == 1);

    // Owner pubkey stored for this signer_hint is still the matching 2592-byte
    // signing_pk (unchanged by KEM rotation).
    auto stored = store.get_owner_pubkey(ns_span(id));
    REQUIRE(stored.has_value());
    REQUIRE(stored->size() == 2592);
    auto id_pk = id.public_key();
    REQUIRE(std::memcmp(stored->data(), id_pk.data(), 2592) == 0);
}

// D-12(e), storage-direct: the throw-on-mismatch invariant at the layer
// where it lives. This mirrors Plan 02 Task 3 case 3 coverage deliberately,
// making Plan 06's test_pubk_first.cpp self-contained for D-12(e) regression-grading.
TEST_CASE("Storage::register_owner_pubkey throws on mismatched signing key",
          "[phase122][pubk_first][engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id_a = NodeIdentity::generate();
    auto id_b = NodeIdentity::generate();

    auto pk_a = id_a.public_key();
    auto pk_b = id_b.public_key();
    auto hint_a = chromatindb::crypto::sha3_256(pk_a);

    // First registration — succeeds.
    REQUIRE_NOTHROW(store.register_owner_pubkey(
        std::span<const uint8_t, 32>(hint_a),
        std::span<const uint8_t, 2592>(pk_a.data(), 2592)));

    // Same hint, DIFFERENT pubkey bytes — must throw (Plan 02 D-04 invariant).
    REQUIRE_THROWS_AS(store.register_owner_pubkey(
                         std::span<const uint8_t, 32>(hint_a),
                         std::span<const uint8_t, 2592>(pk_b.data(), 2592)),
                      std::runtime_error);

    // Bit-identical re-registration — silent no-op (idempotent).
    REQUIRE_NOTHROW(store.register_owner_pubkey(
        std::span<const uint8_t, 32>(hint_a),
        std::span<const uint8_t, 2592>(pk_a.data(), 2592)));

    REQUIRE(store.count_owner_pubkeys() == 1);
}

// D-12(e), engine end-to-end: the catch-throw-emit-pubk_mismatch path
// through Step 4.5. Construction requires a LEGITIMATELY-signed PUBK whose
// embedded signing pubkey differs from the already-registered one for this
// signer_hint — i.e. a collusion scenario where the legitimate namespace
// owner tries to overwrite their registered signing_pk with a different one.
// The signature verify passes (signed by id_a, resolves to pk_a), then Step
// 4.5 invokes register_owner_pubkey(hint_a, pk_b) which throws; engine
// translates to IngestError::pubk_mismatch.
TEST_CASE("Engine ingest: PUBK with different embedded signing pubkey rejected with pubk_mismatch",
          "[phase122][pubk_first][engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id_a = NodeIdentity::generate();
    auto id_b = NodeIdentity::generate();

    // 1. Establish A's namespace normally.
    auto pubk_a = make_pubk_blob(id_a);
    REQUIRE(run_async(pool, engine.ingest(ns_span(id_a), pubk_a)).accepted);
    REQUIRE(store.count_owner_pubkeys() == 1);

    // 2. Build a PUBK for ns_a whose body embeds B's signing pubkey.
    //    signer_hint still points to A (so owner_pubkeys lookup resolves
    //    pk_a); signature is A's (so verify passes against pk_a). Step 4.5
    //    then calls register_owner_pubkey(hint_a, pk_b) -> throws ->
    //    IngestError::pubk_mismatch.
    //
    //    CRITICAL: Step 2's fresh-namespace PUBK branch only triggers when
    //    has_owner_pubkey is FALSE (i.e., it's a fresh namespace). Since
    //    we already registered pk_a, Step 2 goes through the owner_pubkeys
    //    lookup + integrity cross-check path (SHA3(pk_a) == ns_a ✓), sets
    //    resolved_pubkey = pk_a. Step 3 verifies A's signature against pk_a.
    //    Step 4.5 extracts pk_b from the body and attempts to register it.
    auto pk_b = id_b.public_key();
    auto target_ns_a_span = id_a.namespace_id();

    chromatindb::wire::BlobData rotated;
    // signer_hint = A's hint (so lookup resolves pk_a).
    {
        auto hint_a = chromatindb::crypto::sha3_256(id_a.public_key());
        std::memcpy(rotated.signer_hint.data(), hint_a.data(), 32);
    }
    // Build PUBK body embedding pk_b:
    //   [magic:4][signing_pk:2592][kem_pk:1568] = PUBKEY_DATA_SIZE
    rotated.data.reserve(chromatindb::wire::PUBKEY_DATA_SIZE);
    rotated.data.insert(rotated.data.end(),
                        chromatindb::wire::PUBKEY_MAGIC.begin(),
                        chromatindb::wire::PUBKEY_MAGIC.end());
    rotated.data.insert(rotated.data.end(), pk_b.begin(), pk_b.end());
    rotated.data.resize(chromatindb::wire::PUBKEY_DATA_SIZE, 0);  // zero-fill KEM
    rotated.ttl = 0;
    rotated.timestamp = current_timestamp();

    // A (legitimate owner of ns_a) signs — so Step 3 verify succeeds against pk_a.
    auto si = chromatindb::wire::build_signing_input(
        target_ns_a_span, rotated.data, rotated.ttl, rotated.timestamp);
    rotated.signature = id_a.sign(si);

    auto result = run_async(pool, engine.ingest(ns_span(id_a), rotated));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::pubk_mismatch);

    // Sanity: A's entry is unchanged; B did NOT supplant it.
    REQUIRE(store.count_owner_pubkeys() == 1);
    auto still_a = store.get_owner_pubkey(ns_span(id_a));
    REQUIRE(still_a.has_value());
    auto pk_a = id_a.public_key();
    REQUIRE(std::memcmp(still_a->data(), pk_a.data(), 2592) == 0);
}

// D-12(c) reinforcement: after a PUBK establishes the namespace, a longer
// sequence of non-PUBK writes all succeed. Defensive sanity — confirms the
// gate is not re-tripped by subsequent writes and no state regression.
TEST_CASE("non-PUBK after PUBK in same namespace — long write sequence accepted",
          "[phase122][pubk_first][engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), make_pubk_blob(id))).accepted);

    for (int i = 0; i < 3; ++i) {
        auto blob = make_signed_blob(id, "payload-" + std::to_string(i));
        auto r = run_async(pool, engine.ingest(ns_span(id), blob));
        REQUIRE(r.accepted);
    }

    REQUIRE(store.count_owner_pubkeys() == 1);
}
