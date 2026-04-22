// SC#6 coverage: explicit tests for the post-122 engine verify
// path. The pre-122 derived-namespace equality check is gone; Step 2 resolves
// the signing pubkey via owner_pubkeys (owner) or delegation_map (delegate)
// lookup. These tests exercise both resolution paths end-to-end.
//
// Anchor: VALIDATION.md §"phase122/engine: verify path resolves pubkey via
// signer_hint" (SC#6). Tag: [phase122][engine][verify].

#include <catch2/catch_test_macros.hpp>

#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

#include "db/tests/test_helpers.h"

using chromatindb::engine::BlobEngine;
using chromatindb::engine::IngestError;
using chromatindb::identity::NodeIdentity;
using chromatindb::storage::Storage;
using chromatindb::test::TempDir;
using chromatindb::test::run_async;
using chromatindb::test::make_pubk_blob;
using chromatindb::test::make_signed_blob;
using chromatindb::test::make_signed_delegation;
using chromatindb::test::make_delegate_blob;
using chromatindb::test::ns_span;

TEST_CASE("verify path resolves owner pubkey via owner_pubkeys lookup (owner write accepted)",
          "[phase122][engine][verify]") {
    // SC#6 positive-path — owner branch.
    // Flow: PUBK ingest (Step 4.5 registers in owner_pubkeys) → subsequent
    // non-PUBK ingest → Step 2 owner_pubkeys[signer_hint] hit → SHA3(resolved_pubkey)
    // cross-check vs target_namespace passes → Step 3 ML-DSA verify succeeds.
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = NodeIdentity::generate();

    // Step 1: owner publishes PUBK. Engine's Step 4.5 registers the signing
    // pubkey in owner_pubkeys DBI after successful verify.
    auto pubk = make_pubk_blob(id);
    auto pubk_result = run_async(pool, engine.ingest(ns_span(id), pubk));
    REQUIRE(pubk_result.accepted);

    // Sanity: owner_pubkeys DBI was populated.
    REQUIRE(store.has_owner_pubkey(ns_span(id)));
    auto resolved = store.get_owner_pubkey(ns_span(id));
    REQUIRE(resolved.has_value());
    REQUIRE(resolved.value().size() == 2592);

    // Step 2: ingest a regular blob signed by the same identity. The verify
    // path must use owner_pubkeys[signer_hint] to resolve the signing key
    // (there is no delegation for this signer in this namespace).
    auto blob = make_signed_blob(id, "owner-verify-path-data");
    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(result.accepted);
    REQUIRE(result.ack.has_value());
    // seq_num 2 (PUBK was 1, this is the second stored blob in the namespace).
    REQUIRE(result.ack->seq_num >= 2);
}

TEST_CASE("verify path resolves delegate pubkey via delegation_map fallback (delegate write accepted)",
          "[phase122][engine][verify]") {
    // SC#6 positive-path — delegate branch.
    // Flow: owner PUBK ingest → owner publishes delegation → delegate writes
    // to owner's namespace → Step 2 owner_pubkeys[delegate_hint] misses →
    // delegation_map[owner_ns || delegate_hint] hits → resolved_pubkey =
    // delegate_pk → Step 3 ML-DSA verify against delegate_pk succeeds.
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = NodeIdentity::generate();
    auto delegate = NodeIdentity::generate();

    // Step 1: owner publishes PUBK (Step 4.5 registers owner in owner_pubkeys).
    auto owner_pubk = make_pubk_blob(owner);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), owner_pubk)).accepted);

    // Step 2: owner publishes delegation to delegate. This is an owner-signed
    // blob (signer_hint = SHA3(owner_pk), resolved via owner_pubkeys). The
    // engine then indexes it into delegation_map on store.
    auto delegation = make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), delegation)).accepted);

    // Step 3: delegate writes to the OWNER's namespace. signer_hint =
    // SHA3(delegate_pk) which is NOT in owner_pubkeys (no PUBK for delegate
    // was ever published). Engine must fall through to delegation_map lookup
    // keyed by (owner_ns, delegate_hint) to resolve the delegate's signing
    // pubkey. target_namespace = owner_ns (from make_delegate_blob's D-01
    // signing-input binding).
    auto delegate_blob = make_delegate_blob(owner, delegate, "delegate-verify-path-data");
    auto result = run_async(pool, engine.ingest(ns_span(owner), delegate_blob));
    REQUIRE(result.accepted);
    REQUIRE(result.ack.has_value());
}

TEST_CASE("verify path rejects unknown signer_hint with no matching delegation (no_delegation)",
          "[phase122][engine][verify]") {
    // SC#6 negative-path — both resolution paths miss.
    // Flow: owner PUBK ingest (PUBK-first gate cleared) → blob submitted with
    // signer_hint of an unrelated identity + no delegation for that signer →
    // Step 2 owner_pubkeys miss + delegation_map miss → IngestError::no_delegation.
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = NodeIdentity::generate();
    auto stranger = NodeIdentity::generate();

    // Owner publishes PUBK so PUBK-first gate passes for subsequent writes.
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), make_pubk_blob(owner))).accepted);

    // Construct a delegate-shaped blob where the delegate is an unregistered
    // stranger: signer_hint = SHA3(stranger_pk), target = owner_ns.
    // No delegation exists for (owner_ns, stranger_hint), and no PUBK for
    // stranger has been published to owner_ns either.
    auto bogus = make_delegate_blob(owner, stranger, "no-delegation-data");
    auto result = run_async(pool, engine.ingest(ns_span(owner), bogus));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::no_delegation);
}
