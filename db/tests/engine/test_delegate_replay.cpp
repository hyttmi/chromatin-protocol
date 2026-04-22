// D-13 cross-namespace delegate-replay regression test.
//
// Anchor: VALIDATION.md row
//   phase122/engine: delegate-replay cross-namespace rejected (D-13)
//
// A delegate holding delegations on two namespaces (N_A, N_B) signs a blob
// for N_A. Submitting that exact blob with target_namespace = N_B must fail
// signature verification — build_signing_input absorbs target_namespace as
// its first sponge input, so switching the target produces a different
// digest and verify fails. This is the byte-level defense D-01 is designed
// to provide at the wire / envelope boundary.

#include <catch2/catch_test_macros.hpp>

#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"

#include "db/tests/test_helpers.h"

using chromatindb::engine::BlobEngine;
using chromatindb::engine::IngestError;
using chromatindb::identity::NodeIdentity;
using chromatindb::storage::Storage;
using chromatindb::test::TempDir;
using chromatindb::test::make_delegate_blob;
using chromatindb::test::make_pubk_blob;
using chromatindb::test::make_signed_delegation;
using chromatindb::test::ns_span;
using chromatindb::test::run_async;

// D-13: delegate-replay across namespaces fails signature verify.
// Setup uses two owners (A, B) and one delegate (D). D holds valid
// delegations on both namespaces. A delegate-blob signed for N_A is
// submitted as N_B — the engine resolves the delegate via
// delegation_map[N_B, SHA3(D_pk)] (hit: D is delegated in N_B too), but
// Step 3 verify uses build_signing_input(N_B, ...) which differs from
// what D actually signed (N_A, ...). Verify fails -> invalid_signature.
TEST_CASE("delegate-signed blob for N_A submitted as N_B is rejected (cross-namespace replay)",
          "[phase122][engine][delegate]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner_a = NodeIdentity::generate();
    auto owner_b = NodeIdentity::generate();
    auto delegate = NodeIdentity::generate();

    // Establish both namespaces (clear PUBK-first gate + populate owner_pubkeys).
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner_a), make_pubk_blob(owner_a))).accepted);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner_b), make_pubk_blob(owner_b))).accepted);

    // Each owner delegates to D in their own namespace.
    auto del_a = make_signed_delegation(owner_a, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner_a), del_a)).accepted);
    auto del_b = make_signed_delegation(owner_b, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner_b), del_b)).accepted);

    // Baseline: D writes to N_A with a delegate blob scoped to N_A — accepted.
    // Proves the delegation resolution path works and the baseline (no replay)
    // passes, so any subsequent failure in the replay attack isolates the
    // cross-namespace sponge binding as the cause.
    auto blob_for_a = make_delegate_blob(owner_a, delegate, "for-A");
    auto baseline = run_async(pool, engine.ingest(ns_span(owner_a), blob_for_a));
    REQUIRE(baseline.accepted);

    // Replay attack: rebuild the delegate-blob for A, submit with target = N_B.
    // make_delegate_blob's signing input absorbs owner_a.namespace_id(); the
    // engine will absorb owner_b.namespace_id() into its own build_signing_input
    // on the verify path. The digests differ, so verify fails.
    // (Use a fresh blob rather than the already-accepted baseline — ingest
    // of a duplicate would short-circuit at the dedup check at Step 2.5 and
    // mask the verify failure.)
    auto replay_attempt = make_delegate_blob(owner_a, delegate, "replay-attempt");
    auto result = run_async(pool, engine.ingest(ns_span(owner_b), replay_attempt));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::invalid_signature);

    // Sanity-paired: the same delegate blob submitted with the CORRECT
    // target_namespace (N_A) is accepted, confirming the replay attempt's
    // failure is cross-namespace binding, not a spurious signature bug.
    auto correct = make_delegate_blob(owner_a, delegate, "correct-for-A");
    auto correct_result = run_async(pool, engine.ingest(ns_span(owner_a), correct));
    REQUIRE(correct_result.accepted);
}
