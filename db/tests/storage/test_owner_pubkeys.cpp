// Phase 122-02: Storage owner_pubkeys DBI + delegation-hint resolver tests.
//
// Covers the Storage surface that Plan 04 (engine verify path refactor) will
// consume:
//   * register_owner_pubkey / get_owner_pubkey / has_owner_pubkey /
//     count_owner_pubkeys  -- the 4 owner_pubkeys DBI accessors.
//   * get_delegate_pubkey_by_hint  -- D-09 fallback path used when the
//     owner_pubkeys lookup misses (delegate write).
//
// D-04 enforcement primitive:
//   register_owner_pubkey must THROW when a different signing pubkey is
//   registered for the same signer_hint. Engine ingest relies on the throw to
//   surface ErrorCode::PUBK_MISMATCH at the protocol layer.

#include <array>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include "db/crypto/hash.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

#include "db/tests/test_helpers.h"

using chromatindb::test::TempDir;
using chromatindb::storage::Storage;
using chromatindb::storage::StoreResult;

namespace {

/// View a NodeIdentity's 2592-byte signing pubkey as a fixed-extent span.
/// NodeIdentity::public_key() returns a dynamic-extent span; Storage expects
/// std::span<const uint8_t, 2592>.
std::span<const uint8_t, 2592> pubkey_2592(
    const chromatindb::identity::NodeIdentity& id) {
    auto pk = id.public_key();
    REQUIRE(pk.size() == 2592);
    return std::span<const uint8_t, 2592>(pk.data(), 2592);
}

/// Build a delegation BlobData that exercises the same path the engine uses
/// (store_blob -> delegation_map upsert). Storage-level tests use a fake
/// signature; verify is done at the engine layer.
chromatindb::wire::BlobData make_delegation_blob_local(
    const chromatindb::identity::NodeIdentity& owner,
    const chromatindb::identity::NodeIdentity& delegate,
    uint64_t timestamp = 3000) {
    chromatindb::wire::BlobData blob;
    // Post-122: signer_hint = SHA3(owner_pk); target_namespace is call-site param.
    auto hint = chromatindb::crypto::sha3_256(owner.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data = chromatindb::wire::make_delegation_data(delegate.public_key());
    blob.ttl = 0;  // Permanent.
    blob.timestamp = timestamp;
    blob.signature.resize(4627, 0x42);  // Fake sig -- storage does not verify.
    return blob;
}

}  // namespace

// ============================================================================
// Test 1: register + has + get round-trip.
// ============================================================================

TEST_CASE("owner_pubkeys: register + has_owner_pubkey round-trip",
          "[phase122][storage][owner_pubkeys]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto pk_span = pubkey_2592(id);
    auto signer_hint = chromatindb::crypto::sha3_256(id.public_key());

    // Empty DB -> hint is unknown.
    REQUIRE_FALSE(store.has_owner_pubkey(
        std::span<const uint8_t, 32>(signer_hint)));

    // Register; has_owner_pubkey flips to true.
    store.register_owner_pubkey(
        std::span<const uint8_t, 32>(signer_hint), pk_span);
    REQUIRE(store.has_owner_pubkey(
        std::span<const uint8_t, 32>(signer_hint)));
}

// ============================================================================
// Test 2: register_owner_pubkey is idempotent on matching value.
// ============================================================================

TEST_CASE("owner_pubkeys: register_owner_pubkey is idempotent on matching value",
          "[phase122][storage][owner_pubkeys]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto pk_span = pubkey_2592(id);
    auto signer_hint = chromatindb::crypto::sha3_256(id.public_key());

    store.register_owner_pubkey(
        std::span<const uint8_t, 32>(signer_hint), pk_span);
    REQUIRE(store.count_owner_pubkeys() == 1);

    // Same (hint, pubkey) again -- must not throw, must not duplicate.
    REQUIRE_NOTHROW(store.register_owner_pubkey(
        std::span<const uint8_t, 32>(signer_hint), pk_span));
    REQUIRE(store.count_owner_pubkeys() == 1);
}

// ============================================================================
// Test 3: register_owner_pubkey THROWS on mismatched value (D-04 primitive).
// ============================================================================

TEST_CASE("owner_pubkeys: register_owner_pubkey THROWS on mismatched value",
          "[phase122][storage][owner_pubkeys]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id_a = chromatindb::identity::NodeIdentity::generate();
    auto id_b = chromatindb::identity::NodeIdentity::generate();

    auto pk_a = pubkey_2592(id_a);
    auto pk_b = pubkey_2592(id_b);
    auto hint_a = chromatindb::crypto::sha3_256(id_a.public_key());

    // First registration wins.
    store.register_owner_pubkey(
        std::span<const uint8_t, 32>(hint_a), pk_a);

    // Attempting to register a DIFFERENT pubkey under the SAME hint must
    // throw -- D-04 requires the engine to be able to catch the throw and
    // surface PUBK_MISMATCH at the protocol layer.
    REQUIRE_THROWS_AS(
        store.register_owner_pubkey(
            std::span<const uint8_t, 32>(hint_a), pk_b),
        std::runtime_error);

    // Count remains 1; original entry is not overwritten.
    REQUIRE(store.count_owner_pubkeys() == 1);
}

// ============================================================================
// Test 4: get_owner_pubkey returns nullopt for unknown signer_hint.
// ============================================================================

TEST_CASE("owner_pubkeys: get_owner_pubkey returns nullopt for unknown signer_hint",
          "[phase122][storage][owner_pubkeys]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> random_hint{};
    random_hint.fill(0x5A);

    auto result = store.get_owner_pubkey(
        std::span<const uint8_t, 32>(random_hint));
    REQUIRE_FALSE(result.has_value());
}

// ============================================================================
// Test 5: get_owner_pubkey returns the registered pubkey bytes.
// ============================================================================

TEST_CASE("owner_pubkeys: get_owner_pubkey returns the registered pubkey bytes",
          "[phase122][storage][owner_pubkeys]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto pk_span = pubkey_2592(id);
    auto signer_hint = chromatindb::crypto::sha3_256(id.public_key());

    store.register_owner_pubkey(
        std::span<const uint8_t, 32>(signer_hint), pk_span);

    auto retrieved = store.get_owner_pubkey(
        std::span<const uint8_t, 32>(signer_hint));
    REQUIRE(retrieved.has_value());
    REQUIRE(std::memcmp(retrieved->data(), pk_span.data(), 2592) == 0);
}

// ============================================================================
// Test 6: count_owner_pubkeys reflects insertions (and idempotent noop).
// ============================================================================

TEST_CASE("owner_pubkeys: count_owner_pubkeys reflects insertions",
          "[phase122][storage][owner_pubkeys]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    REQUIRE(store.count_owner_pubkeys() == 0);

    auto id_1 = chromatindb::identity::NodeIdentity::generate();
    auto id_2 = chromatindb::identity::NodeIdentity::generate();
    auto id_3 = chromatindb::identity::NodeIdentity::generate();

    auto pk_1 = pubkey_2592(id_1);
    auto pk_2 = pubkey_2592(id_2);
    auto pk_3 = pubkey_2592(id_3);

    auto hint_1 = chromatindb::crypto::sha3_256(id_1.public_key());
    auto hint_2 = chromatindb::crypto::sha3_256(id_2.public_key());
    auto hint_3 = chromatindb::crypto::sha3_256(id_3.public_key());

    store.register_owner_pubkey(std::span<const uint8_t, 32>(hint_1), pk_1);
    REQUIRE(store.count_owner_pubkeys() == 1);

    store.register_owner_pubkey(std::span<const uint8_t, 32>(hint_2), pk_2);
    REQUIRE(store.count_owner_pubkeys() == 2);

    store.register_owner_pubkey(std::span<const uint8_t, 32>(hint_3), pk_3);
    REQUIRE(store.count_owner_pubkeys() == 3);

    // Idempotent re-registration must NOT bump the count.
    store.register_owner_pubkey(std::span<const uint8_t, 32>(hint_2), pk_2);
    REQUIRE(store.count_owner_pubkeys() == 3);
}

// ============================================================================
// Test 7: get_delegate_pubkey_by_hint resolves on hit; nullopt on miss.
// D-09 fallback coverage at the storage layer.
// ============================================================================

TEST_CASE("owner_pubkeys: get_delegate_pubkey_by_hint resolves hit + miss + wrong_ns",
          "[phase122][storage][owner_pubkeys]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Stage: use the existing public API (store_blob with a delegation blob)
    // to populate delegation_map. This is the SAME code path the engine takes
    // when ingesting a delegation; do NOT bypass via raw MDBX upsert.
    auto deleg_blob = make_delegation_blob_local(owner, delegate);
    auto r = store.store_blob(std::span<const uint8_t, 32>(owner.namespace_id()), deleg_blob);
    REQUIRE(r.status == StoreResult::Status::Stored);

    auto ns_a = owner.namespace_id();
    auto delegate_pk_hash =
        chromatindb::crypto::sha3_256(delegate.public_key());

    SECTION("hit: correct namespace + signer_hint resolves to delegate pubkey") {
        auto resolved = store.get_delegate_pubkey_by_hint(
            ns_a, std::span<const uint8_t, 32>(delegate_pk_hash));
        REQUIRE(resolved.has_value());
        REQUIRE(resolved->size() == 2592);
        REQUIRE(std::memcmp(resolved->data(),
                            delegate.public_key().data(), 2592) == 0);
    }

    SECTION("miss: unknown signer_hint in correct namespace returns nullopt") {
        auto other = chromatindb::identity::NodeIdentity::generate();
        auto other_hint =
            chromatindb::crypto::sha3_256(other.public_key());
        auto resolved = store.get_delegate_pubkey_by_hint(
            ns_a, std::span<const uint8_t, 32>(other_hint));
        REQUIRE_FALSE(resolved.has_value());
    }

    SECTION("miss: correct signer_hint but wrong namespace returns nullopt -- D-13 lookup is ns-scoped") {
        auto other_owner = chromatindb::identity::NodeIdentity::generate();
        auto wrong_ns = other_owner.namespace_id();
        auto resolved = store.get_delegate_pubkey_by_hint(
            wrong_ns, std::span<const uint8_t, 32>(delegate_pk_hash));
        REQUIRE_FALSE(resolved.has_value());
    }
}
