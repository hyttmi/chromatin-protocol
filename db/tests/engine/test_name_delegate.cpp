// Delegate NAME acceptance (D-11).
//
// Tag: [phase123][engine][name_delegate]
//
// Proves: delegates CAN write NAME blobs in the owner's namespace (no reject
// at Step 2 trailer). The NAME blob's signer_hint preserves the delegate's
// identity for audit — the owner can identify who named what.

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
using chromatindb::identity::NodeIdentity;
using chromatindb::storage::Storage;
using chromatindb::test::TempDir;
using chromatindb::test::current_timestamp;
using chromatindb::test::make_pubk_blob;
using chromatindb::test::make_signed_blob;
using chromatindb::test::make_signed_delegation;
using chromatindb::test::ns_span;
using chromatindb::test::run_async;

TEST_CASE("Delegate NAME is accepted and preserves signer_hint",
          "[phase123][engine][name_delegate]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = NodeIdentity::generate();
    auto delegate = NodeIdentity::generate();

    // Establish owner namespace + delegation.
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), make_pubk_blob(owner))).accepted);
    auto delegation = make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), delegation)).accepted);

    // Owner writes a content blob A; delegate will NAME-tag it.
    auto content = make_signed_blob(owner, "name-target-content");
    auto content_result = run_async(pool, engine.ingest(ns_span(owner), content));
    REQUIRE(content_result.accepted);
    REQUIRE(content_result.ack.has_value());
    std::array<uint8_t, 32> target_hash = content_result.ack->blob_hash;

    // Delegate builds a NAME blob for name "foo" → target_hash, signed by the
    // delegate (signer_hint = SHA3(delegate_pk); target_namespace = owner_ns).
    chromatindb::wire::BlobData name_blob;
    auto delegate_hint = chromatindb::crypto::sha3_256(delegate.public_key());
    std::memcpy(name_blob.signer_hint.data(), delegate_hint.data(), 32);

    const std::string name_str = "foo";
    std::span<const uint8_t> name_bytes(
        reinterpret_cast<const uint8_t*>(name_str.data()), name_str.size());
    name_blob.data = chromatindb::wire::make_name_data(
        name_bytes, std::span<const uint8_t, 32>(target_hash));
    name_blob.ttl = 0;  // permanent NAME
    name_blob.timestamp = current_timestamp();
    auto si = chromatindb::wire::build_signing_input(
        owner.namespace_id(), name_blob.data, name_blob.ttl, name_blob.timestamp);
    name_blob.signature = delegate.sign(si);

    // D-11: delegate NAME is ACCEPTED (no is_name entry in Step 2 delegate-reject list).
    auto result = run_async(pool, engine.ingest(ns_span(owner), name_blob));
    REQUIRE(result.accepted);
    REQUIRE(result.ack.has_value());
    auto stored_blob_hash = result.ack->blob_hash;

    // Verify the stored NAME blob preserves the delegate's signer_hint
    // (audit property — owner can see who wrote this NAME).
    auto retrieved = store.get_blob(ns_span(owner),
        std::span<const uint8_t, 32>(stored_blob_hash));
    REQUIRE(retrieved.has_value());
    REQUIRE(std::memcmp(retrieved->signer_hint.data(), delegate_hint.data(), 32) == 0);

    // Sanity: the payload is a NAME (not a generic blob).
    REQUIRE(chromatindb::wire::is_name(retrieved->data));
    auto parsed = chromatindb::wire::parse_name_payload(retrieved->data);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->name.size() == name_str.size());
    REQUIRE(std::memcmp(parsed->name.data(), name_str.data(), name_str.size()) == 0);
    REQUIRE(std::memcmp(parsed->target_hash.data(), target_hash.data(), 32) == 0);
}
