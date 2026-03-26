#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <random>

#include <mdbx.h++>

#include "db/storage/storage.h"
#include "db/wire/codec.h"
#include "db/crypto/hash.h"
#include "db/identity/identity.h"
#include "db/engine/engine.h"

#include "db/tests/test_helpers.h"

namespace fs = std::filesystem;

namespace {

using chromatindb::test::TempDir;

/// Create a test BlobData with specified TTL and timestamp.
/// Uses deterministic namespace derived from a counter for test isolation.
/// Timestamp is in microseconds (matching production loadgen behavior).
/// Default 1000000000 = 1000 seconds in microseconds.
chromatindb::wire::BlobData make_test_blob(
    uint8_t ns_byte,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = 1000000000ULL)
{
    chromatindb::wire::BlobData blob;
    blob.namespace_id.fill(ns_byte);
    blob.pubkey.resize(2592, ns_byte);
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = timestamp;
    blob.signature.resize(4627, 0x42);
    return blob;
}

/// Compute the blob hash for a given BlobData.
std::array<uint8_t, 32> compute_hash(const chromatindb::wire::BlobData& blob) {
    auto encoded = chromatindb::wire::encode_blob(blob);
    return chromatindb::wire::blob_hash(encoded);
}

} // anonymous namespace

using chromatindb::storage::Storage;
using chromatindb::storage::StoreResult;

// ============================================================================
// Plan 02-01: Basic storage operations
// ============================================================================

TEST_CASE("Storage opens and closes without error", "[storage]") {
    TempDir tmp;
    { Storage store(tmp.path.string()); }
    // Destructor runs without error
    REQUIRE(true);
}

TEST_CASE("Storage creates data directory if missing", "[storage]") {
    TempDir tmp;
    REQUIRE_FALSE(fs::exists(tmp.path));
    { Storage store(tmp.path.string()); }
    REQUIRE(fs::exists(tmp.path));
}

TEST_CASE("Storage store and retrieve blob round-trip", "[storage]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0x01, "hello chromatindb");
    auto hash = compute_hash(blob);

    auto result = store.store_blob(blob);
    REQUIRE(result.status == StoreResult::Status::Stored);

    auto retrieved = store.get_blob(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(hash));

    REQUIRE(retrieved.has_value());
    REQUIRE(retrieved->namespace_id == blob.namespace_id);
    REQUIRE(retrieved->data == blob.data);
    REQUIRE(retrieved->ttl == blob.ttl);
    REQUIRE(retrieved->timestamp == blob.timestamp);
    REQUIRE(retrieved->pubkey == blob.pubkey);
    REQUIRE(retrieved->signature == blob.signature);
}

TEST_CASE("Storage deduplicates by content hash", "[storage]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0x01, "duplicate me");

    REQUIRE(store.store_blob(blob).status == StoreResult::Status::Stored);
    REQUIRE(store.store_blob(blob).status == StoreResult::Status::Duplicate);
}

TEST_CASE("Storage has_blob returns correct results", "[storage]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0x01, "check existence");
    auto hash = compute_hash(blob);

    REQUIRE_FALSE(store.has_blob(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(hash)));

    store.store_blob(blob);

    REQUIRE(store.has_blob(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(hash)));
}

TEST_CASE("Storage get_blob returns nullopt for unknown blob", "[storage]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> fake_ns{};
    std::array<uint8_t, 32> fake_hash{};
    fake_ns.fill(0xFF);
    fake_hash.fill(0xFF);

    auto result = store.get_blob(
        std::span<const uint8_t, 32>(fake_ns),
        std::span<const uint8_t, 32>(fake_hash));
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Storage crash recovery -- data persists across close/reopen", "[storage]") {
    TempDir tmp;
    auto blob = make_test_blob(0x01, "survive the crash");
    auto hash = compute_hash(blob);

    // Store and close (destroy without explicit close)
    {
        Storage store(tmp.path.string());
        REQUIRE(store.store_blob(blob).status == StoreResult::Status::Stored);
    }

    // Reopen from same path -- data should be intact
    {
        Storage store(tmp.path.string());
        REQUIRE(store.has_blob(
            std::span<const uint8_t, 32>(blob.namespace_id),
            std::span<const uint8_t, 32>(hash)));

        auto retrieved = store.get_blob(
            std::span<const uint8_t, 32>(blob.namespace_id),
            std::span<const uint8_t, 32>(hash));
        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->data == blob.data);
    }
}

TEST_CASE("Storage move constructor works", "[storage]") {
    TempDir tmp;
    Storage store1(tmp.path.string());

    auto blob = make_test_blob(0x01, "move me");
    store1.store_blob(blob);

    Storage store2(std::move(store1));
    auto hash = compute_hash(blob);
    REQUIRE(store2.has_blob(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(hash)));
}

// ============================================================================
// Plan 02-02: Sequence indexing and range queries
// ============================================================================

TEST_CASE("Storage assigns monotonic seq_nums per namespace", "[storage][seq]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob1 = make_test_blob(0x01, "seq-1");
    auto blob2 = make_test_blob(0x01, "seq-2");
    auto blob3 = make_test_blob(0x01, "seq-3");

    REQUIRE(store.store_blob(blob1).status == StoreResult::Status::Stored);
    REQUIRE(store.store_blob(blob2).status == StoreResult::Status::Stored);
    REQUIRE(store.store_blob(blob3).status == StoreResult::Status::Stored);

    // All 3 blobs retrievable via seq range query from 0
    auto results = store.get_blobs_by_seq(
        std::span<const uint8_t, 32>(blob1.namespace_id), 0);
    REQUIRE(results.size() == 3);
    REQUIRE(results[0].data == blob1.data);
    REQUIRE(results[1].data == blob2.data);
    REQUIRE(results[2].data == blob3.data);
}

TEST_CASE("Storage seq_nums are independent per namespace", "[storage][seq]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blobA1 = make_test_blob(0x0A, "ns-A-blob-1");
    auto blobA2 = make_test_blob(0x0A, "ns-A-blob-2");
    auto blobB1 = make_test_blob(0x0B, "ns-B-blob-1");

    store.store_blob(blobA1);
    store.store_blob(blobA2);
    store.store_blob(blobB1);

    auto resultsA = store.get_blobs_by_seq(
        std::span<const uint8_t, 32>(blobA1.namespace_id), 0);
    auto resultsB = store.get_blobs_by_seq(
        std::span<const uint8_t, 32>(blobB1.namespace_id), 0);

    REQUIRE(resultsA.size() == 2);
    REQUIRE(resultsB.size() == 1);
}

TEST_CASE("Storage get_blobs_by_seq with since_seq filters correctly", "[storage][seq]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob1 = make_test_blob(0x01, "range-1");
    auto blob2 = make_test_blob(0x01, "range-2");
    auto blob3 = make_test_blob(0x01, "range-3");
    auto blob4 = make_test_blob(0x01, "range-4");
    auto blob5 = make_test_blob(0x01, "range-5");

    store.store_blob(blob1);
    store.store_blob(blob2);
    store.store_blob(blob3);
    store.store_blob(blob4);
    store.store_blob(blob5);

    // since_seq=3 -> blobs with seq > 3 -> blob4, blob5
    auto results = store.get_blobs_by_seq(
        std::span<const uint8_t, 32>(blob1.namespace_id), 3);
    REQUIRE(results.size() == 2);
    REQUIRE(results[0].data == blob4.data);
    REQUIRE(results[1].data == blob5.data);
}

TEST_CASE("Storage get_blobs_by_seq returns empty for high since_seq", "[storage][seq]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0x01, "only-one");
    store.store_blob(blob);

    auto results = store.get_blobs_by_seq(
        std::span<const uint8_t, 32>(blob.namespace_id), 999);
    REQUIRE(results.empty());
}

TEST_CASE("Storage get_blobs_by_seq returns empty for unknown namespace", "[storage][seq]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> unknown_ns{};
    unknown_ns.fill(0xFF);

    auto results = store.get_blobs_by_seq(
        std::span<const uint8_t, 32>(unknown_ns), 0);
    REQUIRE(results.empty());
}

TEST_CASE("Storage duplicate does not consume seq_num", "[storage][seq]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob1 = make_test_blob(0x01, "unique-1");
    auto blob2 = make_test_blob(0x01, "unique-2");

    store.store_blob(blob1);
    store.store_blob(blob1);  // duplicate
    store.store_blob(blob2);

    // Should be exactly 2 blobs, not 3
    auto results = store.get_blobs_by_seq(
        std::span<const uint8_t, 32>(blob1.namespace_id), 0);
    REQUIRE(results.size() == 2);
}

// ============================================================================
// Plan 02-03: TTL expiry scanning
// ============================================================================

TEST_CASE("Storage expiry scan with no expired blobs returns 0", "[storage][expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    auto blob = make_test_blob(0x01, "not-expired", 604800, 1000000000ULL);
    store.store_blob(blob);

    // Clock is at 1000, blob expires at 1000 + 604800 = 605800
    REQUIRE(store.run_expiry_scan() == 0);
}

TEST_CASE("Storage expiry scan purges expired blob", "[storage][expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    auto blob = make_test_blob(0x01, "will-expire", 100, 1000000000ULL);
    auto hash = compute_hash(blob);
    store.store_blob(blob);

    // Before expiry
    REQUIRE(store.has_blob(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(hash)));
    REQUIRE(store.run_expiry_scan() == 0);

    // Advance past expiry (1000 + 100 = 1100)
    fake_time = 1101;
    REQUIRE(store.run_expiry_scan() == 1);

    // Blob should be gone
    REQUIRE_FALSE(store.has_blob(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(hash)));
    REQUIRE_FALSE(store.get_blob(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(hash)).has_value());
}

TEST_CASE("Storage expiry scan selective purge", "[storage][expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    // 3 blobs with different expiry times
    auto blob1 = make_test_blob(0x01, "expires-first", 100, 1000000000ULL);   // expires 1100
    auto blob2 = make_test_blob(0x01, "expires-second", 200, 1000000000ULL);  // expires 1200
    auto blob3 = make_test_blob(0x01, "expires-last", 500, 1000000000ULL);    // expires 1500

    store.store_blob(blob1);
    store.store_blob(blob2);
    store.store_blob(blob3);

    auto hash1 = compute_hash(blob1);
    auto hash3 = compute_hash(blob3);

    // Advance past first two expiry times
    fake_time = 1201;
    REQUIRE(store.run_expiry_scan() == 2);

    // First two gone, third still here
    REQUIRE_FALSE(store.has_blob(
        std::span<const uint8_t, 32>(blob1.namespace_id),
        std::span<const uint8_t, 32>(hash1)));
    REQUIRE(store.has_blob(
        std::span<const uint8_t, 32>(blob3.namespace_id),
        std::span<const uint8_t, 32>(hash3)));
}

TEST_CASE("Storage expiry scan is idempotent", "[storage][expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    auto blob = make_test_blob(0x01, "purge-once", 100, 1000000000ULL);
    store.store_blob(blob);

    fake_time = 1101;
    REQUIRE(store.run_expiry_scan() == 1);
    REQUIRE(store.run_expiry_scan() == 0);  // Already purged
}

TEST_CASE("Storage TTL=0 blobs are never purged", "[storage][expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    auto permanent = make_test_blob(0x01, "permanent", 0, 1000000000ULL);
    auto hash = compute_hash(permanent);
    store.store_blob(permanent);

    // Advance far into the future
    fake_time = 99999999;
    REQUIRE(store.run_expiry_scan() == 0);

    REQUIRE(store.has_blob(
        std::span<const uint8_t, 32>(permanent.namespace_id),
        std::span<const uint8_t, 32>(hash)));
}

TEST_CASE("Storage mixed TTL=0 and TTL>0 expiry", "[storage][expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    auto permanent = make_test_blob(0x01, "permanent-blob", 0, 1000000000ULL);
    auto ephemeral = make_test_blob(0x01, "ephemeral-blob", 100, 1000000000ULL);

    store.store_blob(permanent);
    store.store_blob(ephemeral);

    auto perm_hash = compute_hash(permanent);
    auto eph_hash = compute_hash(ephemeral);

    fake_time = 1101;
    REQUIRE(store.run_expiry_scan() == 1);  // Only ephemeral

    REQUIRE(store.has_blob(
        std::span<const uint8_t, 32>(permanent.namespace_id),
        std::span<const uint8_t, 32>(perm_hash)));
    REQUIRE_FALSE(store.has_blob(
        std::span<const uint8_t, 32>(ephemeral.namespace_id),
        std::span<const uint8_t, 32>(eph_hash)));
}

TEST_CASE("Storage expiry scan on empty database returns 0", "[storage][expiry]") {
    TempDir tmp;
    uint64_t fake_time = 99999;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    REQUIRE(store.run_expiry_scan() == 0);
}

TEST_CASE("Storage seq entries remain after expiry (gaps expected)", "[storage][expiry][seq]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    auto blob1 = make_test_blob(0x01, "will-expire-seq", 100, 1000000000ULL);
    auto blob2 = make_test_blob(0x01, "will-survive-seq", 604800, 1000000000ULL);

    store.store_blob(blob1);  // seq 1
    store.store_blob(blob2);  // seq 2

    // Expire blob1
    fake_time = 1101;
    REQUIRE(store.run_expiry_scan() == 1);

    // get_blobs_by_seq should skip the gap and return only blob2
    auto results = store.get_blobs_by_seq(
        std::span<const uint8_t, 32>(blob1.namespace_id), 0);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].data == blob2.data);
}

// ============================================================================
// Plan 03-01: StoreResult struct, list_namespaces, duplicate seq_num
// ============================================================================

TEST_CASE("Storage store_blob returns seq_num and hash", "[storage][plan03]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0x01, "seq-hash-test");
    auto result = store.store_blob(blob);

    REQUIRE(result.status == StoreResult::Status::Stored);
    REQUIRE(result.seq_num == 1);

    // blob_hash should be non-zero
    std::array<uint8_t, 32> zero{};
    REQUIRE(result.blob_hash != zero);
}

TEST_CASE("Storage duplicate returns existing seq_num", "[storage][plan03]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0x01, "dup-seq-test");
    auto first = store.store_blob(blob);
    auto second = store.store_blob(blob);

    REQUIRE(first.status == StoreResult::Status::Stored);
    REQUIRE(second.status == StoreResult::Status::Duplicate);
    REQUIRE(second.seq_num == first.seq_num);
    REQUIRE(second.blob_hash == first.blob_hash);
}

TEST_CASE("Storage list_namespaces returns stored namespaces", "[storage][plan03]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Use two distinct identities for proper namespace_ids
    auto id1 = chromatindb::identity::NodeIdentity::generate();
    auto id2 = chromatindb::identity::NodeIdentity::generate();

    // Create blobs with proper namespace IDs
    chromatindb::wire::BlobData blob1;
    std::memcpy(blob1.namespace_id.data(), id1.namespace_id().data(), 32);
    blob1.pubkey.assign(id1.public_key().begin(), id1.public_key().end());
    blob1.data = {'a', 'b', 'c'};
    blob1.ttl = 604800;
    blob1.timestamp = 1000;
    blob1.signature.resize(4627, 0x42);

    chromatindb::wire::BlobData blob1b;
    std::memcpy(blob1b.namespace_id.data(), id1.namespace_id().data(), 32);
    blob1b.pubkey.assign(id1.public_key().begin(), id1.public_key().end());
    blob1b.data = {'d', 'e', 'f'};
    blob1b.ttl = 604800;
    blob1b.timestamp = 1001;
    blob1b.signature.resize(4627, 0x42);

    chromatindb::wire::BlobData blob2;
    std::memcpy(blob2.namespace_id.data(), id2.namespace_id().data(), 32);
    blob2.pubkey.assign(id2.public_key().begin(), id2.public_key().end());
    blob2.data = {'x', 'y', 'z'};
    blob2.ttl = 604800;
    blob2.timestamp = 1000;
    blob2.signature.resize(4627, 0x42);

    store.store_blob(blob1);
    store.store_blob(blob1b);
    store.store_blob(blob2);

    auto namespaces = store.list_namespaces();
    REQUIRE(namespaces.size() == 2);

    // Find each namespace in the result
    bool found_ns1 = false, found_ns2 = false;
    for (const auto& ns_info : namespaces) {
        if (ns_info.namespace_id == blob1.namespace_id) {
            found_ns1 = true;
            REQUIRE(ns_info.latest_seq_num == 2);  // 2 blobs stored
        }
        if (ns_info.namespace_id == blob2.namespace_id) {
            found_ns2 = true;
            REQUIRE(ns_info.latest_seq_num == 1);  // 1 blob stored
        }
    }
    REQUIRE(found_ns1);
    REQUIRE(found_ns2);
}

// ============================================================================
// Plan 11-02: get_hashes_by_namespace
// ============================================================================

TEST_CASE("Storage get_hashes_by_namespace returns empty for unknown namespace", "[storage]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> ns{};
    ns.fill(0xAA);
    auto hashes = store.get_hashes_by_namespace(ns);
    REQUIRE(hashes.empty());
}

TEST_CASE("Storage get_hashes_by_namespace returns correct hashes", "[storage]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob1 = make_test_blob(0x10, "payload-one");
    auto blob2 = make_test_blob(0x10, "payload-two");
    auto blob3 = make_test_blob(0x10, "payload-three");

    auto r1 = store.store_blob(blob1);
    auto r2 = store.store_blob(blob2);
    auto r3 = store.store_blob(blob3);
    REQUIRE(r1.status == StoreResult::Status::Stored);
    REQUIRE(r2.status == StoreResult::Status::Stored);
    REQUIRE(r3.status == StoreResult::Status::Stored);

    auto hashes = store.get_hashes_by_namespace(
        std::span<const uint8_t, 32>(blob1.namespace_id));
    REQUIRE(hashes.size() == 3);

    // Hashes should match store_blob results
    REQUIRE(hashes[0] == r1.blob_hash);
    REQUIRE(hashes[1] == r2.blob_hash);
    REQUIRE(hashes[2] == r3.blob_hash);
}

TEST_CASE("Storage get_hashes_by_namespace returns hashes in seq order", "[storage]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob1 = make_test_blob(0x20, "first");
    auto blob2 = make_test_blob(0x20, "second");
    auto blob3 = make_test_blob(0x20, "third");

    auto r1 = store.store_blob(blob1);
    auto r2 = store.store_blob(blob2);
    auto r3 = store.store_blob(blob3);

    auto hashes = store.get_hashes_by_namespace(
        std::span<const uint8_t, 32>(blob1.namespace_id));
    REQUIRE(hashes.size() == 3);

    // seq order: r1 first, then r2, then r3
    REQUIRE(hashes[0] == r1.blob_hash);
    REQUIRE(hashes[1] == r2.blob_hash);
    REQUIRE(hashes[2] == r3.blob_hash);
}

TEST_CASE("Storage get_hashes_by_namespace isolates namespaces", "[storage]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob_a1 = make_test_blob(0x30, "ns-a-one");
    auto blob_a2 = make_test_blob(0x30, "ns-a-two");
    auto blob_b1 = make_test_blob(0x31, "ns-b-one");

    auto ra1 = store.store_blob(blob_a1);
    auto ra2 = store.store_blob(blob_a2);
    auto rb1 = store.store_blob(blob_b1);

    auto hashes_a = store.get_hashes_by_namespace(
        std::span<const uint8_t, 32>(blob_a1.namespace_id));
    auto hashes_b = store.get_hashes_by_namespace(
        std::span<const uint8_t, 32>(blob_b1.namespace_id));

    REQUIRE(hashes_a.size() == 2);
    REQUIRE(hashes_b.size() == 1);
    REQUIRE(hashes_a[0] == ra1.blob_hash);
    REQUIRE(hashes_a[1] == ra2.blob_hash);
    REQUIRE(hashes_b[0] == rb1.blob_hash);
}

TEST_CASE("Storage get_hashes_by_namespace dedup: duplicate blob has one hash entry", "[storage]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0x40, "deduplicate-me");
    auto r1 = store.store_blob(blob);
    auto r2 = store.store_blob(blob);  // duplicate
    REQUIRE(r1.status == StoreResult::Status::Stored);
    REQUIRE(r2.status == StoreResult::Status::Duplicate);

    auto hashes = store.get_hashes_by_namespace(
        std::span<const uint8_t, 32>(blob.namespace_id));
    REQUIRE(hashes.size() == 1);  // Duplicate doesn't add second seq entry
    REQUIRE(hashes[0] == r1.blob_hash);
}

TEST_CASE("Storage get_hashes_by_namespace matches blob_hash computation", "[storage]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0x50, "hash-check");
    auto expected_hash = compute_hash(blob);
    store.store_blob(blob);

    auto hashes = store.get_hashes_by_namespace(
        std::span<const uint8_t, 32>(blob.namespace_id));
    REQUIRE(hashes.size() == 1);
    REQUIRE(hashes[0] == expected_hash);
}

TEST_CASE("Storage list_namespaces empty on fresh storage", "[storage][plan03]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto namespaces = store.list_namespaces();
    REQUIRE(namespaces.empty());
}

// ============================================================================
// Phase 12: Storage tombstone operations
// ============================================================================

TEST_CASE("Storage delete_blob_data removes existing blob", "[storage][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0x60, "to-delete");
    auto result = store.store_blob(blob);
    REQUIRE(result.status == StoreResult::Status::Stored);

    // Verify blob exists
    REQUIRE(store.has_blob(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(result.blob_hash)));

    // Delete it
    bool deleted = store.delete_blob_data(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(result.blob_hash));
    REQUIRE(deleted);

    // Verify blob is gone
    REQUIRE_FALSE(store.has_blob(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(result.blob_hash)));
}

TEST_CASE("Storage delete_blob_data returns false for non-existent blob", "[storage][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> ns{};
    ns.fill(0x61);
    std::array<uint8_t, 32> fake_hash{};
    fake_hash.fill(0xFF);

    bool deleted = store.delete_blob_data(
        std::span<const uint8_t, 32>(ns),
        std::span<const uint8_t, 32>(fake_hash));
    REQUIRE_FALSE(deleted);
}

TEST_CASE("Storage delete_blob_data cleans up expiry index", "[storage][tombstone]") {
    TempDir tmp;
    uint64_t fake_now = 1000;
    Storage store(tmp.path.string(), [&]() { return fake_now; });

    // Store a blob with TTL
    auto blob = make_test_blob(0x62, "expiry-cleanup", 3600, 1000000000ULL);
    auto result = store.store_blob(blob);
    REQUIRE(result.status == StoreResult::Status::Stored);

    // Delete the blob
    bool deleted = store.delete_blob_data(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(result.blob_hash));
    REQUIRE(deleted);

    // Advance past expiry and scan -- should purge 0 (already deleted)
    fake_now = 99999;
    auto purged = store.run_expiry_scan();
    REQUIRE(purged == 0);
}

TEST_CASE("Storage has_tombstone_for finds tombstone", "[storage][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Store a regular blob
    auto blob = make_test_blob(0x63, "regular-blob");
    auto blob_result = store.store_blob(blob);
    REQUIRE(blob_result.status == StoreResult::Status::Stored);

    // No tombstone yet
    REQUIRE_FALSE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(blob_result.blob_hash)));

    // Store a tombstone targeting this blob
    chromatindb::wire::BlobData tombstone;
    tombstone.namespace_id = blob.namespace_id;
    tombstone.pubkey = blob.pubkey;
    tombstone.data = chromatindb::wire::make_tombstone_data(blob_result.blob_hash);
    tombstone.ttl = 0;
    tombstone.timestamp = 2000;
    tombstone.signature.resize(4627, 0x42);
    store.store_blob(tombstone);

    // Now tombstone should be found
    REQUIRE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(blob_result.blob_hash)));
}

TEST_CASE("Storage has_tombstone_for returns false for wrong target", "[storage][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Store a tombstone targeting a specific hash
    std::array<uint8_t, 32> target{};
    target.fill(0xAA);

    chromatindb::wire::BlobData tombstone;
    tombstone.namespace_id.fill(0x64);
    tombstone.pubkey.resize(2592, 0x64);
    tombstone.data = chromatindb::wire::make_tombstone_data(target);
    tombstone.ttl = 0;
    tombstone.timestamp = 2000;
    tombstone.signature.resize(4627, 0x42);
    store.store_blob(tombstone);

    // Query for a different target hash
    std::array<uint8_t, 32> other_target{};
    other_target.fill(0xBB);

    REQUIRE_FALSE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(tombstone.namespace_id),
        std::span<const uint8_t, 32>(other_target)));
}

// ============================================================================
// Phase 13: Delegation index
// ============================================================================

namespace {

/// Create a delegation BlobData: owner delegates to delegate's pubkey.
chromatindb::wire::BlobData make_delegation_blob(
    const chromatindb::identity::NodeIdentity& owner,
    const chromatindb::identity::NodeIdentity& delegate,
    uint64_t timestamp = 3000)
{
    chromatindb::wire::BlobData blob;
    std::memcpy(blob.namespace_id.data(), owner.namespace_id().data(), 32);
    blob.pubkey.assign(owner.public_key().begin(), owner.public_key().end());
    blob.data = chromatindb::wire::make_delegation_data(delegate.public_key());
    blob.ttl = 0;  // Permanent
    blob.timestamp = timestamp;
    blob.signature.resize(4627, 0x42);  // Fake sig for storage-level tests
    return blob;
}

} // anonymous namespace

TEST_CASE("Delegation codec: is_delegation detects valid delegation data", "[storage][delegation]") {
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    auto data = chromatindb::wire::make_delegation_data(delegate.public_key());
    REQUIRE(chromatindb::wire::is_delegation(data));
}

TEST_CASE("Delegation codec: is_delegation rejects non-delegation data", "[storage][delegation]") {
    SECTION("empty data") {
        std::vector<uint8_t> data;
        REQUIRE_FALSE(chromatindb::wire::is_delegation(data));
    }
    SECTION("regular blob data") {
        std::vector<uint8_t> data = {0x01, 0x02, 0x03};
        REQUIRE_FALSE(chromatindb::wire::is_delegation(data));
    }
    SECTION("tombstone data") {
        std::array<uint8_t, 32> target{};
        target.fill(0xAB);
        auto data = chromatindb::wire::make_tombstone_data(target);
        REQUIRE_FALSE(chromatindb::wire::is_delegation(data));
    }
    SECTION("wrong size with right prefix") {
        std::vector<uint8_t> data(100);
        std::memcpy(data.data(), chromatindb::wire::DELEGATION_MAGIC.data(), 4);
        REQUIRE_FALSE(chromatindb::wire::is_delegation(data));
    }
}

TEST_CASE("Delegation codec: extract_delegate_pubkey round-trips", "[storage][delegation]") {
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    auto data = chromatindb::wire::make_delegation_data(delegate.public_key());

    auto extracted = chromatindb::wire::extract_delegate_pubkey(data);
    auto expected = delegate.public_key();
    REQUIRE(extracted.size() == expected.size());
    REQUIRE(std::equal(extracted.begin(), extracted.end(), expected.begin()));
}

TEST_CASE("Delegation index: has_valid_delegation returns true after storing delegation blob", "[storage][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    auto deleg_blob = make_delegation_blob(owner, delegate);
    auto result = store.store_blob(deleg_blob);
    REQUIRE(result.status == StoreResult::Status::Stored);

    REQUIRE(store.has_valid_delegation(
        owner.namespace_id(),
        delegate.public_key()));
}

TEST_CASE("Delegation index: has_valid_delegation returns false for non-existent delegation", "[storage][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    REQUIRE_FALSE(store.has_valid_delegation(
        owner.namespace_id(),
        delegate.public_key()));
}

TEST_CASE("Delegation index: has_valid_delegation returns false for wrong namespace", "[storage][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    auto other_owner = chromatindb::identity::NodeIdentity::generate();

    auto deleg_blob = make_delegation_blob(owner, delegate);
    store.store_blob(deleg_blob);

    REQUIRE_FALSE(store.has_valid_delegation(
        other_owner.namespace_id(),
        delegate.public_key()));
}

TEST_CASE("Delegation index: has_valid_delegation returns false for wrong delegate pubkey", "[storage][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    auto other_delegate = chromatindb::identity::NodeIdentity::generate();

    auto deleg_blob = make_delegation_blob(owner, delegate);
    store.store_blob(deleg_blob);

    REQUIRE_FALSE(store.has_valid_delegation(
        owner.namespace_id(),
        other_delegate.public_key()));
}

TEST_CASE("Delegation index: delete_blob_data removes delegation index entry", "[storage][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    auto deleg_blob = make_delegation_blob(owner, delegate);
    auto result = store.store_blob(deleg_blob);
    REQUIRE(result.status == StoreResult::Status::Stored);

    // Verify delegation exists
    REQUIRE(store.has_valid_delegation(
        owner.namespace_id(),
        delegate.public_key()));

    // Delete the delegation blob
    bool deleted = store.delete_blob_data(
        owner.namespace_id(),
        std::span<const uint8_t, 32>(result.blob_hash));
    REQUIRE(deleted);

    // Delegation index should be cleaned
    REQUIRE_FALSE(store.has_valid_delegation(
        owner.namespace_id(),
        delegate.public_key()));
}

TEST_CASE("Delegation index: multiple delegations in same namespace", "[storage][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate1 = chromatindb::identity::NodeIdentity::generate();
    auto delegate2 = chromatindb::identity::NodeIdentity::generate();

    auto deleg1 = make_delegation_blob(owner, delegate1, 3000);
    auto deleg2 = make_delegation_blob(owner, delegate2, 3001);

    auto r1 = store.store_blob(deleg1);
    auto r2 = store.store_blob(deleg2);
    REQUIRE(r1.status == StoreResult::Status::Stored);
    REQUIRE(r2.status == StoreResult::Status::Stored);

    // Both delegations should be valid
    REQUIRE(store.has_valid_delegation(
        owner.namespace_id(), delegate1.public_key()));
    REQUIRE(store.has_valid_delegation(
        owner.namespace_id(), delegate2.public_key()));

    // Delete only the first delegation
    bool deleted = store.delete_blob_data(
        owner.namespace_id(),
        std::span<const uint8_t, 32>(r1.blob_hash));
    REQUIRE(deleted);

    // First should be gone, second still valid
    REQUIRE_FALSE(store.has_valid_delegation(
        owner.namespace_id(), delegate1.public_key()));
    REQUIRE(store.has_valid_delegation(
        owner.namespace_id(), delegate2.public_key()));
}

// ============================================================================
// Phase 16-01: Tombstone index O(1) and used_bytes
// ============================================================================

TEST_CASE("Storage tombstone index - O(1) lookup finds stored tombstone", "[storage][tombstone-index]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Store a regular blob
    auto blob = make_test_blob(0x70, "target-blob");
    auto blob_result = store.store_blob(blob);
    REQUIRE(blob_result.status == StoreResult::Status::Stored);

    // Store a tombstone targeting that blob
    chromatindb::wire::BlobData tombstone;
    tombstone.namespace_id = blob.namespace_id;
    tombstone.pubkey = blob.pubkey;
    tombstone.data = chromatindb::wire::make_tombstone_data(blob_result.blob_hash);
    tombstone.ttl = 0;
    tombstone.timestamp = 2000;
    tombstone.signature.resize(4627, 0x42);
    store.store_blob(tombstone);

    // O(1) indexed lookup should find it
    REQUIRE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(blob_result.blob_hash)));
}

TEST_CASE("Storage tombstone index - lookup returns false for non-existent target", "[storage][tombstone-index]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> ns{};
    ns.fill(0x71);
    std::array<uint8_t, 32> random_hash{};
    random_hash.fill(0xCC);

    REQUIRE_FALSE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(ns),
        std::span<const uint8_t, 32>(random_hash)));
}

TEST_CASE("Storage tombstone index - cleanup on tombstone deletion", "[storage][tombstone-index]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Store a regular blob
    auto blob = make_test_blob(0x72, "target-for-delete");
    auto blob_result = store.store_blob(blob);
    REQUIRE(blob_result.status == StoreResult::Status::Stored);

    // Store a tombstone targeting that blob
    chromatindb::wire::BlobData tombstone;
    tombstone.namespace_id = blob.namespace_id;
    tombstone.pubkey = blob.pubkey;
    tombstone.data = chromatindb::wire::make_tombstone_data(blob_result.blob_hash);
    tombstone.ttl = 0;
    tombstone.timestamp = 2000;
    tombstone.signature.resize(4627, 0x42);
    auto ts_result = store.store_blob(tombstone);

    // Tombstone should be found
    REQUIRE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(blob_result.blob_hash)));

    // Delete the tombstone blob
    bool deleted = store.delete_blob_data(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(ts_result.blob_hash));
    REQUIRE(deleted);

    // Tombstone index entry should be cleaned
    REQUIRE_FALSE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(blob_result.blob_hash)));
}

TEST_CASE("Storage tombstone index - multiple tombstones in same namespace", "[storage][tombstone-index]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Store two regular blobs
    auto blob1 = make_test_blob(0x74, "multi-target-1");
    auto blob2 = make_test_blob(0x74, "multi-target-2");
    auto r1 = store.store_blob(blob1);
    auto r2 = store.store_blob(blob2);
    REQUIRE(r1.status == StoreResult::Status::Stored);
    REQUIRE(r2.status == StoreResult::Status::Stored);

    // Store tombstones targeting each blob
    chromatindb::wire::BlobData ts1;
    ts1.namespace_id = blob1.namespace_id;
    ts1.pubkey = blob1.pubkey;
    ts1.data = chromatindb::wire::make_tombstone_data(r1.blob_hash);
    ts1.ttl = 0;
    ts1.timestamp = 2000;
    ts1.signature.resize(4627, 0x42);
    auto ts1_result = store.store_blob(ts1);

    chromatindb::wire::BlobData ts2;
    ts2.namespace_id = blob2.namespace_id;
    ts2.pubkey = blob2.pubkey;
    ts2.data = chromatindb::wire::make_tombstone_data(r2.blob_hash);
    ts2.ttl = 0;
    ts2.timestamp = 2001;
    ts2.signature.resize(4627, 0x42);
    store.store_blob(ts2);

    // Both tombstones should be found
    REQUIRE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(blob1.namespace_id),
        std::span<const uint8_t, 32>(r1.blob_hash)));
    REQUIRE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(blob2.namespace_id),
        std::span<const uint8_t, 32>(r2.blob_hash)));

    // Delete only the first tombstone
    bool deleted = store.delete_blob_data(
        std::span<const uint8_t, 32>(blob1.namespace_id),
        std::span<const uint8_t, 32>(ts1_result.blob_hash));
    REQUIRE(deleted);

    // First should be gone, second still present
    REQUIRE_FALSE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(blob1.namespace_id),
        std::span<const uint8_t, 32>(r1.blob_hash)));
    REQUIRE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(blob2.namespace_id),
        std::span<const uint8_t, 32>(r2.blob_hash)));
}

TEST_CASE("Storage tombstone index - cross-namespace isolation", "[storage][tombstone-index]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Same target hash in two different namespaces
    std::array<uint8_t, 32> target{};
    target.fill(0xDD);

    // Tombstone in namespace 0x75
    chromatindb::wire::BlobData ts_a;
    ts_a.namespace_id.fill(0x75);
    ts_a.pubkey.resize(2592, 0x75);
    ts_a.data = chromatindb::wire::make_tombstone_data(target);
    ts_a.ttl = 0;
    ts_a.timestamp = 2000;
    ts_a.signature.resize(4627, 0x42);
    store.store_blob(ts_a);

    // Tombstone in namespace 0x76
    chromatindb::wire::BlobData ts_b;
    ts_b.namespace_id.fill(0x76);
    ts_b.pubkey.resize(2592, 0x76);
    ts_b.data = chromatindb::wire::make_tombstone_data(target);
    ts_b.ttl = 0;
    ts_b.timestamp = 2000;
    ts_b.signature.resize(4627, 0x42);
    store.store_blob(ts_b);

    // Both namespaces should find the tombstone
    REQUIRE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(ts_a.namespace_id),
        std::span<const uint8_t, 32>(target)));
    REQUIRE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(ts_b.namespace_id),
        std::span<const uint8_t, 32>(target)));

    // A third namespace without a tombstone should return false
    std::array<uint8_t, 32> other_ns{};
    other_ns.fill(0x77);
    REQUIRE_FALSE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(other_ns),
        std::span<const uint8_t, 32>(target)));
}

// ============================================================================
// Phase 23-01: Tombstone expiry lifecycle
// ============================================================================

TEST_CASE("Storage tombstone with TTL>0 is expired by expiry scan", "[storage][tombstone-expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    // Store a regular blob
    auto blob = make_test_blob(0x70, "target-blob", 604800, 1000000000ULL);
    auto blob_result = store.store_blob(blob);
    REQUIRE(blob_result.status == StoreResult::Status::Stored);

    // Store a tombstone targeting it with TTL=3600, timestamp=1000
    chromatindb::wire::BlobData tombstone;
    tombstone.namespace_id = blob.namespace_id;
    tombstone.pubkey = blob.pubkey;
    tombstone.data = chromatindb::wire::make_tombstone_data(blob_result.blob_hash);
    tombstone.ttl = 3600;
    tombstone.timestamp = 1000;
    tombstone.signature.resize(4627, 0x42);
    auto ts_result = store.store_blob(tombstone);
    REQUIRE(ts_result.status == StoreResult::Status::Stored);

    // Verify tombstone index is populated
    REQUIRE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(blob_result.blob_hash)));

    // Advance clock past tombstone expiry (1000 + 3600 = 4600, so 4601)
    fake_time = 4601;
    auto purged = store.run_expiry_scan();
    REQUIRE(purged >= 1);

    // tombstone_map should be cleaned
    REQUIRE_FALSE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(blob_result.blob_hash)));
}

TEST_CASE("Storage tombstone with TTL=0 is never expired", "[storage][tombstone-expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    // Store a tombstone with TTL=0 (permanent)
    std::array<uint8_t, 32> target{};
    target.fill(0xAA);

    chromatindb::wire::BlobData tombstone;
    tombstone.namespace_id.fill(0x78);
    tombstone.pubkey.resize(2592, 0x78);
    tombstone.data = chromatindb::wire::make_tombstone_data(target);
    tombstone.ttl = 0;
    tombstone.timestamp = 1000;
    tombstone.signature.resize(4627, 0x42);
    store.store_blob(tombstone);

    // Verify tombstone exists
    REQUIRE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(tombstone.namespace_id),
        std::span<const uint8_t, 32>(target)));

    // Advance clock far into the future
    fake_time = 99999999;
    REQUIRE(store.run_expiry_scan() == 0);

    // Tombstone should still be there (permanent)
    REQUIRE(store.has_tombstone_for(
        std::span<const uint8_t, 32>(tombstone.namespace_id),
        std::span<const uint8_t, 32>(target)));
}

TEST_CASE("Storage regular blob expiry unaffected by tombstone scan", "[storage][tombstone-expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    // Store a regular blob with TTL=100
    auto blob = make_test_blob(0x79, "regular-expiry", 100, 1000000000ULL);
    auto hash = compute_hash(blob);
    store.store_blob(blob);

    // Verify blob exists
    REQUIRE(store.has_blob(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(hash)));

    // Advance past expiry
    fake_time = 1101;
    REQUIRE(store.run_expiry_scan() == 1);

    // Blob should be gone
    REQUIRE_FALSE(store.has_blob(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(hash)));
}

TEST_CASE("Storage used_bytes - non-zero after storing data", "[storage][used-bytes]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0x73, "some-data-for-size");
    store.store_blob(blob);

    REQUIRE(store.used_bytes() > 0);
}

TEST_CASE("Storage used_bytes - works on empty database", "[storage][used-bytes]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Should not throw, value >= 0 always true for uint64_t
    auto bytes = store.used_bytes();
    (void)bytes;  // Just verifying it doesn't throw
    REQUIRE(true);
}

// ============================================================================
// Phase 24: Encryption at Rest
// ============================================================================

TEST_CASE("Storage encryption at rest: raw mdbx value has version header", "[storage][encryption-at-rest]") {
    TempDir tmp;
    std::array<uint8_t, 32> hash;

    // Store a blob, then destroy Storage so mdbx env is released
    {
        Storage store(tmp.path.string());
        auto blob = make_test_blob(0xEE, "hello-encryption-test");
        auto result = store.store_blob(blob);
        REQUIRE(result.status == StoreResult::Status::Stored);
        hash = result.blob_hash;
    }

    // Open raw mdbx to inspect encrypted value
    {
        mdbx::env_managed::create_parameters cp;
        cp.use_subdirectory = false;
        mdbx::env::operate_parameters op;
        op.max_maps = 6;
        op.max_readers = 4;
        mdbx::env_managed env(tmp.path.string(), cp, op);

        auto txn = env.start_read();
        auto blobs_map = txn.open_map("blobs");

        // Build the key: [namespace:32][hash:32]
        std::array<uint8_t, 64> blob_key;
        std::memset(blob_key.data(), 0xEE, 32);  // namespace
        std::memcpy(blob_key.data() + 32, hash.data(), 32);

        auto val = txn.get(blobs_map,
            mdbx::slice(blob_key.data(), blob_key.size()));

        // First byte must be encryption version 0x01
        REQUIRE(val.length() > 0);
        REQUIRE(static_cast<const uint8_t*>(val.data())[0] == 0x01);

        // Total size = 1 (version) + 12 (nonce) + plaintext_size + 16 (tag)
        // encoded blob is variable size, but envelope overhead is 29 bytes
        REQUIRE(val.length() > 29);
    }
}

TEST_CASE("Storage encryption at rest: raw mdbx value is not plaintext", "[storage][encryption-at-rest]") {
    TempDir tmp;
    std::array<uint8_t, 32> hash;
    const std::string payload = "hello-encryption-test-payload-visible";

    // Store a blob with known payload
    {
        Storage store(tmp.path.string());
        auto blob = make_test_blob(0xEF, payload);
        auto result = store.store_blob(blob);
        REQUIRE(result.status == StoreResult::Status::Stored);
        hash = result.blob_hash;
    }

    // Open raw mdbx to inspect — payload must not appear in ciphertext
    {
        mdbx::env_managed::create_parameters cp;
        cp.use_subdirectory = false;
        mdbx::env::operate_parameters op;
        op.max_maps = 6;
        op.max_readers = 4;
        mdbx::env_managed env(tmp.path.string(), cp, op);

        auto txn = env.start_read();
        auto blobs_map = txn.open_map("blobs");

        std::array<uint8_t, 64> blob_key;
        std::memset(blob_key.data(), 0xEF, 32);
        std::memcpy(blob_key.data() + 32, hash.data(), 32);

        auto val = txn.get(blobs_map,
            mdbx::slice(blob_key.data(), blob_key.size()));

        // Search for payload substring in raw bytes — must NOT appear
        std::string raw_str(static_cast<const char*>(val.data()), val.length());
        REQUIRE(raw_str.find(payload) == std::string::npos);
    }

    // Verify get_blob returns original plaintext correctly
    {
        Storage store(tmp.path.string());
        std::array<uint8_t, 32> ns;
        ns.fill(0xEF);
        auto retrieved = store.get_blob(
            std::span<const uint8_t, 32>(ns),
            std::span<const uint8_t, 32>(hash));
        REQUIRE(retrieved.has_value());
        std::string retrieved_payload(retrieved->data.begin(), retrieved->data.end());
        REQUIRE(retrieved_payload == payload);
    }
}

TEST_CASE("Storage encryption at rest: round-trip through all read paths", "[storage][encryption-at-rest]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Store a blob
    auto blob = make_test_blob(0xF0, "round-trip-test");
    auto result = store.store_blob(blob);
    REQUIRE(result.status == StoreResult::Status::Stored);

    // get_blob returns correct data
    std::array<uint8_t, 32> ns;
    ns.fill(0xF0);
    auto retrieved = store.get_blob(
        std::span<const uint8_t, 32>(ns),
        std::span<const uint8_t, 32>(result.blob_hash));
    REQUIRE(retrieved.has_value());
    REQUIRE(retrieved->data == blob.data);
    REQUIRE(retrieved->ttl == blob.ttl);
    REQUIRE(retrieved->timestamp == blob.timestamp);

    // get_blobs_by_seq returns correct data
    auto blobs = store.get_blobs_by_seq(
        std::span<const uint8_t, 32>(ns), 0);
    REQUIRE(blobs.size() == 1);
    REQUIRE(blobs[0].data == blob.data);

    // has_blob works
    REQUIRE(store.has_blob(
        std::span<const uint8_t, 32>(ns),
        std::span<const uint8_t, 32>(result.blob_hash)));

    // delete_blob_data works (requires decryption for internal checks)
    REQUIRE(store.delete_blob_data(
        std::span<const uint8_t, 32>(ns),
        std::span<const uint8_t, 32>(result.blob_hash)));

    // Blob is now gone
    REQUIRE_FALSE(store.has_blob(
        std::span<const uint8_t, 32>(ns),
        std::span<const uint8_t, 32>(result.blob_hash)));
}

TEST_CASE("Storage encryption at rest: master key auto-generated on first run", "[storage][encryption-at-rest]") {
    TempDir tmp;

    // Before creating Storage, no master.key
    REQUIRE_FALSE(fs::exists(tmp.path / "master.key"));

    {
        Storage store(tmp.path.string());
    }

    // After creating Storage, master.key exists with correct size
    auto key_path = tmp.path / "master.key";
    REQUIRE(fs::exists(key_path));
    REQUIRE(fs::file_size(key_path) == 32);

    // Verify restricted permissions (0600)
    auto perms = fs::status(key_path).permissions();
    REQUIRE((perms & fs::perms::owner_read) != fs::perms::none);
    REQUIRE((perms & fs::perms::owner_write) != fs::perms::none);
    REQUIRE((perms & fs::perms::group_read) == fs::perms::none);
    REQUIRE((perms & fs::perms::others_read) == fs::perms::none);
}

// =============================================================================
// Sync cursor CRUD tests
// =============================================================================

TEST_CASE("set_sync_cursor then get_sync_cursor returns matching values", "[storage][cursor]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> peer_hash{};
    peer_hash.fill(0xAA);
    std::array<uint8_t, 32> ns_id{};
    ns_id.fill(0x01);

    chromatindb::storage::SyncCursor cursor;
    cursor.seq_num = 42;
    cursor.round_count = 3;
    cursor.last_sync_timestamp = 1700000000;

    store.set_sync_cursor(peer_hash, ns_id, cursor);

    auto result = store.get_sync_cursor(peer_hash, ns_id);
    REQUIRE(result.has_value());
    REQUIRE(result->seq_num == 42);
    REQUIRE(result->round_count == 3);
    REQUIRE(result->last_sync_timestamp == 1700000000);
}

TEST_CASE("get_sync_cursor for nonexistent key returns nullopt", "[storage][cursor]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> peer_hash{};
    peer_hash.fill(0xBB);
    std::array<uint8_t, 32> ns_id{};
    ns_id.fill(0x02);

    auto result = store.get_sync_cursor(peer_hash, ns_id);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("delete_sync_cursor removes entry, subsequent get returns nullopt", "[storage][cursor]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> peer_hash{};
    peer_hash.fill(0xCC);
    std::array<uint8_t, 32> ns_id{};
    ns_id.fill(0x03);

    chromatindb::storage::SyncCursor cursor;
    cursor.seq_num = 100;
    cursor.round_count = 5;
    cursor.last_sync_timestamp = 1700000001;

    store.set_sync_cursor(peer_hash, ns_id, cursor);
    REQUIRE(store.get_sync_cursor(peer_hash, ns_id).has_value());

    store.delete_sync_cursor(peer_hash, ns_id);
    REQUIRE_FALSE(store.get_sync_cursor(peer_hash, ns_id).has_value());
}

TEST_CASE("delete_peer_cursors removes all entries for that peer across namespaces", "[storage][cursor]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> peer_hash{};
    peer_hash.fill(0xDD);

    std::array<uint8_t, 32> ns1{};
    ns1.fill(0x10);
    std::array<uint8_t, 32> ns2{};
    ns2.fill(0x20);
    std::array<uint8_t, 32> ns3{};
    ns3.fill(0x30);

    chromatindb::storage::SyncCursor cursor;
    cursor.seq_num = 10;
    cursor.round_count = 1;
    cursor.last_sync_timestamp = 1700000002;

    store.set_sync_cursor(peer_hash, ns1, cursor);
    store.set_sync_cursor(peer_hash, ns2, cursor);
    store.set_sync_cursor(peer_hash, ns3, cursor);

    // Also set a cursor for a different peer to ensure it's not affected
    std::array<uint8_t, 32> other_peer{};
    other_peer.fill(0xEE);
    store.set_sync_cursor(other_peer, ns1, cursor);

    size_t deleted = store.delete_peer_cursors(peer_hash);
    REQUIRE(deleted == 3);

    REQUIRE_FALSE(store.get_sync_cursor(peer_hash, ns1).has_value());
    REQUIRE_FALSE(store.get_sync_cursor(peer_hash, ns2).has_value());
    REQUIRE_FALSE(store.get_sync_cursor(peer_hash, ns3).has_value());

    // Other peer's cursor should still exist
    REQUIRE(store.get_sync_cursor(other_peer, ns1).has_value());
}

TEST_CASE("list_cursor_peers returns unique peer hashes", "[storage][cursor]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> peer1{};
    peer1.fill(0x11);
    std::array<uint8_t, 32> peer2{};
    peer2.fill(0x22);

    std::array<uint8_t, 32> ns1{};
    ns1.fill(0xA0);
    std::array<uint8_t, 32> ns2{};
    ns2.fill(0xB0);

    chromatindb::storage::SyncCursor cursor;
    cursor.seq_num = 1;
    cursor.round_count = 0;
    cursor.last_sync_timestamp = 1700000003;

    // peer1 has cursors for two namespaces, peer2 for one
    store.set_sync_cursor(peer1, ns1, cursor);
    store.set_sync_cursor(peer1, ns2, cursor);
    store.set_sync_cursor(peer2, ns1, cursor);

    auto peers = store.list_cursor_peers();
    REQUIRE(peers.size() == 2);

    // Verify both peers are in the result (order may vary)
    bool found_peer1 = false, found_peer2 = false;
    for (const auto& p : peers) {
        if (p == peer1) found_peer1 = true;
        if (p == peer2) found_peer2 = true;
    }
    REQUIRE(found_peer1);
    REQUIRE(found_peer2);
}

TEST_CASE("reset_all_round_counters zeroes round_count, preserves seq_num and timestamp", "[storage][cursor]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> peer1{};
    peer1.fill(0x33);
    std::array<uint8_t, 32> peer2{};
    peer2.fill(0x44);
    std::array<uint8_t, 32> ns1{};
    ns1.fill(0xC0);

    chromatindb::storage::SyncCursor c1;
    c1.seq_num = 50;
    c1.round_count = 7;
    c1.last_sync_timestamp = 1700000004;

    chromatindb::storage::SyncCursor c2;
    c2.seq_num = 100;
    c2.round_count = 12;
    c2.last_sync_timestamp = 1700000005;

    store.set_sync_cursor(peer1, ns1, c1);
    store.set_sync_cursor(peer2, ns1, c2);

    size_t reset_count = store.reset_all_round_counters();
    REQUIRE(reset_count == 2);

    auto r1 = store.get_sync_cursor(peer1, ns1);
    REQUIRE(r1.has_value());
    REQUIRE(r1->seq_num == 50);
    REQUIRE(r1->round_count == 0);
    REQUIRE(r1->last_sync_timestamp == 1700000004);

    auto r2 = store.get_sync_cursor(peer2, ns1);
    REQUIRE(r2.has_value());
    REQUIRE(r2->seq_num == 100);
    REQUIRE(r2->round_count == 0);
    REQUIRE(r2->last_sync_timestamp == 1700000005);
}

TEST_CASE("Cursor survives Storage reopen", "[storage][cursor]") {
    TempDir tmp;

    std::array<uint8_t, 32> peer_hash{};
    peer_hash.fill(0x55);
    std::array<uint8_t, 32> ns_id{};
    ns_id.fill(0xD0);

    chromatindb::storage::SyncCursor cursor;
    cursor.seq_num = 999;
    cursor.round_count = 42;
    cursor.last_sync_timestamp = 1700000006;

    // Create cursor and close storage
    {
        Storage store(tmp.path.string());
        store.set_sync_cursor(peer_hash, ns_id, cursor);
    }

    // Reopen and verify cursor persisted
    {
        Storage store(tmp.path.string());
        auto result = store.get_sync_cursor(peer_hash, ns_id);
        REQUIRE(result.has_value());
        REQUIRE(result->seq_num == 999);
        REQUIRE(result->round_count == 42);
        REQUIRE(result->last_sync_timestamp == 1700000006);
    }
}

TEST_CASE("cleanup_stale_cursors deletes cursors for unknown peers", "[storage][cursor]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> known_peer{};
    known_peer.fill(0x66);
    std::array<uint8_t, 32> stale_peer1{};
    stale_peer1.fill(0x77);
    std::array<uint8_t, 32> stale_peer2{};
    stale_peer2.fill(0x88);

    std::array<uint8_t, 32> ns1{};
    ns1.fill(0xE0);

    chromatindb::storage::SyncCursor cursor;
    cursor.seq_num = 1;
    cursor.round_count = 0;
    cursor.last_sync_timestamp = 1700000007;

    store.set_sync_cursor(known_peer, ns1, cursor);
    store.set_sync_cursor(stale_peer1, ns1, cursor);
    store.set_sync_cursor(stale_peer2, ns1, cursor);

    // Only known_peer is in the known set
    std::vector<std::array<uint8_t, 32>> known_set = {known_peer};
    size_t deleted = store.cleanup_stale_cursors(known_set);
    REQUIRE(deleted == 2);

    REQUIRE(store.get_sync_cursor(known_peer, ns1).has_value());
    REQUIRE_FALSE(store.get_sync_cursor(stale_peer1, ns1).has_value());
    REQUIRE_FALSE(store.get_sync_cursor(stale_peer2, ns1).has_value());
}

// =============================================================================
// Phase 35: Namespace quota aggregate tests
// =============================================================================

TEST_CASE("get_namespace_quota returns zero for empty namespace", "[storage][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    std::array<uint8_t, 32> ns{};
    ns.fill(0xA0);

    auto quota = store.get_namespace_quota(std::span<const uint8_t, 32>(ns));
    REQUIRE(quota.total_bytes == 0);
    REQUIRE(quota.blob_count == 0);
}

TEST_CASE("store_blob increments quota aggregate", "[storage][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0xA1, "quota-test-data");
    auto result = store.store_blob(blob);
    REQUIRE(result.status == StoreResult::Status::Stored);

    auto quota = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob.namespace_id));
    REQUIRE(quota.blob_count == 1);
    REQUIRE(quota.total_bytes > 0);

    // Store a second blob in the same namespace
    auto blob2 = make_test_blob(0xA1, "quota-test-data-2");
    auto result2 = store.store_blob(blob2);
    REQUIRE(result2.status == StoreResult::Status::Stored);

    auto quota2 = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob.namespace_id));
    REQUIRE(quota2.blob_count == 2);
    REQUIRE(quota2.total_bytes > quota.total_bytes);
}

TEST_CASE("store_blob duplicate does not increment quota", "[storage][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0xA2, "dedup-quota-test");
    auto r1 = store.store_blob(blob);
    REQUIRE(r1.status == StoreResult::Status::Stored);

    auto quota_after_first = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob.namespace_id));

    // Store same blob again (dedup)
    auto r2 = store.store_blob(blob);
    REQUIRE(r2.status == StoreResult::Status::Duplicate);

    auto quota_after_dup = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob.namespace_id));
    REQUIRE(quota_after_dup.blob_count == quota_after_first.blob_count);
    REQUIRE(quota_after_dup.total_bytes == quota_after_first.total_bytes);
}

TEST_CASE("store_blob tombstone does not increment quota", "[storage][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Store a regular blob first
    auto blob = make_test_blob(0xA3, "tombstone-quota-test");
    auto blob_result = store.store_blob(blob);
    REQUIRE(blob_result.status == StoreResult::Status::Stored);

    auto quota_before = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob.namespace_id));

    // Store a tombstone targeting the blob
    chromatindb::wire::BlobData tombstone;
    tombstone.namespace_id = blob.namespace_id;
    tombstone.pubkey = blob.pubkey;
    tombstone.data = chromatindb::wire::make_tombstone_data(blob_result.blob_hash);
    tombstone.ttl = 0;
    tombstone.timestamp = 2000;
    tombstone.signature.resize(4627, 0x42);
    auto ts_result = store.store_blob(tombstone);
    REQUIRE(ts_result.status == StoreResult::Status::Stored);

    auto quota_after = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob.namespace_id));
    // Tombstone should NOT increase quota
    REQUIRE(quota_after.blob_count == quota_before.blob_count);
    REQUIRE(quota_after.total_bytes == quota_before.total_bytes);
}

TEST_CASE("delete_blob_data decrements quota aggregate", "[storage][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob = make_test_blob(0xA4, "delete-quota-test");
    auto result = store.store_blob(blob);
    REQUIRE(result.status == StoreResult::Status::Stored);

    auto quota_before = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob.namespace_id));
    REQUIRE(quota_before.blob_count == 1);
    REQUIRE(quota_before.total_bytes > 0);

    // Delete the blob
    REQUIRE(store.delete_blob_data(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(result.blob_hash)));

    auto quota_after = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob.namespace_id));
    REQUIRE(quota_after.blob_count == 0);
    REQUIRE(quota_after.total_bytes == 0);
}

TEST_CASE("run_expiry_scan decrements quota aggregate", "[storage][quota]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    // Store a blob with TTL=100
    auto blob = make_test_blob(0xA5, "expiry-quota-test", 100, 1000000000ULL);
    auto result = store.store_blob(blob);
    REQUIRE(result.status == StoreResult::Status::Stored);

    auto quota_before = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob.namespace_id));
    REQUIRE(quota_before.blob_count == 1);
    REQUIRE(quota_before.total_bytes > 0);

    // Advance clock past expiry
    fake_time = 1101;
    REQUIRE(store.run_expiry_scan() == 1);

    auto quota_after = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob.namespace_id));
    REQUIRE(quota_after.blob_count == 0);
    REQUIRE(quota_after.total_bytes == 0);
}

TEST_CASE("rebuild_quota_aggregates matches actual storage", "[storage][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Store multiple blobs
    auto blob1 = make_test_blob(0xA6, "rebuild-test-1");
    auto blob2 = make_test_blob(0xA6, "rebuild-test-2");
    store.store_blob(blob1);
    store.store_blob(blob2);

    // Get quota from normal increment path
    auto quota_incremental = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob1.namespace_id));

    // Rebuild from scratch
    store.rebuild_quota_aggregates();

    // Should match
    auto quota_rebuilt = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob1.namespace_id));
    REQUIRE(quota_rebuilt.blob_count == quota_incremental.blob_count);
    REQUIRE(quota_rebuilt.total_bytes == quota_incremental.total_bytes);
}

TEST_CASE("rebuild_quota_aggregates clears stale entries", "[storage][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    // Store and delete a blob
    auto blob = make_test_blob(0xA7, "stale-rebuild-test");
    auto result = store.store_blob(blob);
    REQUIRE(result.status == StoreResult::Status::Stored);

    REQUIRE(store.delete_blob_data(
        std::span<const uint8_t, 32>(blob.namespace_id),
        std::span<const uint8_t, 32>(result.blob_hash)));

    // Rebuild
    store.rebuild_quota_aggregates();

    // Namespace with no blobs should have zero quota
    auto quota = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob.namespace_id));
    REQUIRE(quota.blob_count == 0);
    REQUIRE(quota.total_bytes == 0);
}

TEST_CASE("multiple namespaces tracked independently", "[storage][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto blob_ns1 = make_test_blob(0xB0, "ns1-data");
    auto blob_ns2 = make_test_blob(0xB1, "ns2-data-1");
    auto blob_ns2b = make_test_blob(0xB1, "ns2-data-2");

    store.store_blob(blob_ns1);
    store.store_blob(blob_ns2);
    store.store_blob(blob_ns2b);

    auto quota_ns1 = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob_ns1.namespace_id));
    auto quota_ns2 = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob_ns2.namespace_id));

    REQUIRE(quota_ns1.blob_count == 1);
    REQUIRE(quota_ns2.blob_count == 2);

    // Deleting from ns1 shouldn't affect ns2
    auto hash = compute_hash(blob_ns1);
    store.delete_blob_data(
        std::span<const uint8_t, 32>(blob_ns1.namespace_id),
        std::span<const uint8_t, 32>(hash));

    auto quota_ns1_after = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob_ns1.namespace_id));
    auto quota_ns2_after = store.get_namespace_quota(
        std::span<const uint8_t, 32>(blob_ns2.namespace_id));

    REQUIRE(quota_ns1_after.blob_count == 0);
    REQUIRE(quota_ns2_after.blob_count == 2);
}

// ============================================================================
// Phase 43-02: Integrity scan + tombstone GC verification
// ============================================================================

TEST_CASE("integrity_scan on empty storage reports zero counts", "[storage][integrity]") {
    TempDir dir;
    Storage store(dir.path.string());

    // Should not throw and should log zero counts for all sub-databases
    REQUIRE_NOTHROW(store.integrity_scan());
}

TEST_CASE("integrity_scan on populated storage reports correct entry counts", "[storage][integrity]") {
    TempDir dir;
    Storage store(dir.path.string());

    // Store a few blobs in different namespaces
    auto blob1 = make_test_blob(0x10, "integrity-blob-1");
    auto blob2 = make_test_blob(0x10, "integrity-blob-2");
    auto blob3 = make_test_blob(0x20, "integrity-blob-3");

    REQUIRE(store.store_blob(blob1).status == StoreResult::Status::Stored);
    REQUIRE(store.store_blob(blob2).status == StoreResult::Status::Stored);
    REQUIRE(store.store_blob(blob3).status == StoreResult::Status::Stored);

    // integrity_scan should report blobs=3, seq=3 (at minimum)
    REQUIRE_NOTHROW(store.integrity_scan());
}

TEST_CASE("expiry scan decreases entry counts (GC correctness)", "[storage][integrity]") {
    TempDir dir;
    uint64_t fake_now = 10000;
    Storage store(dir.path.string(), [&]() { return fake_now; });

    // Store blobs with short TTL (expires at timestamp + ttl)
    auto blob1 = make_test_blob(0x30, "gc-test-1", 100, 9000);  // expires at 9100
    auto blob2 = make_test_blob(0x30, "gc-test-2", 100, 9000);  // expires at 9100
    auto blob3 = make_test_blob(0x30, "gc-test-3", 0, 9000);    // TTL=0 = permanent

    REQUIRE(store.store_blob(blob1).status == StoreResult::Status::Stored);
    REQUIRE(store.store_blob(blob2).status == StoreResult::Status::Stored);
    REQUIRE(store.store_blob(blob3).status == StoreResult::Status::Stored);

    // Advance clock past expiry and run GC
    fake_now = 10000;  // 9000 + 100 = 9100, so 10000 > 9100
    auto purged = store.run_expiry_scan();
    REQUIRE(purged == 2);  // Two blobs expired, one permanent

    // After GC, blobs_map should have fewer entries
    // (blob data was actually deleted, not just pages freed)
    auto ns = std::array<uint8_t, 32>{};
    ns.fill(0x30);
    auto remaining = store.get_blobs_by_seq(
        std::span<const uint8_t, 32>(ns), 0);
    // Only permanent blob should remain
    REQUIRE(remaining.size() == 1);
}

TEST_CASE("used_bytes returns mmap geometry size", "[storage][integrity]") {
    TempDir dir;
    Storage store(dir.path.string());

    // used_bytes() should return a positive value even on empty storage
    // (mmap geometry is always >= lower bound)
    auto bytes = store.used_bytes();
    REQUIRE(bytes > 0);

    // used_data_bytes() should also return a value, but represents actual data pages
    auto data_bytes = store.used_data_bytes();
    REQUIRE(data_bytes > 0);

    // Mmap geometry (used_bytes) should be >= actual data usage
    REQUIRE(bytes >= data_bytes);
}

// =============================================================================
// Phase 55: Storage compaction tests (COMP-01)
// =============================================================================

TEST_CASE("Storage::compact() on empty DB succeeds", "[storage][compact]") {
    TempDir dir;
    Storage store(dir.path.string());

    auto result = store.compact();
    REQUIRE(result.success);
    REQUIRE(result.before_bytes > 0);
    REQUIRE(result.after_bytes > 0);
    REQUIRE(result.after_bytes <= result.before_bytes);
}

TEST_CASE("Storage::compact() on DB with data produces valid result", "[storage][compact]") {
    TempDir dir;
    Storage store(dir.path.string());

    // Store some blobs to increase DB size
    for (int i = 0; i < 20; ++i) {
        auto blob = make_test_blob(0x50, "compact-data-" + std::to_string(i));
        REQUIRE(store.store_blob(blob).status == StoreResult::Status::Stored);
    }

    auto before = store.used_bytes();
    auto result = store.compact();
    REQUIRE(result.success);
    REQUIRE(result.before_bytes > 0);
    REQUIRE(result.after_bytes > 0);
    REQUIRE(result.after_bytes <= result.before_bytes);
    REQUIRE(result.duration_ms < 60000);  // Should finish quickly
}

TEST_CASE("Storage::compact() after deletion produces smaller file", "[storage][compact]") {
    TempDir dir;
    Storage store(dir.path.string());

    std::array<uint8_t, 32> ns{};
    ns.fill(0x51);

    // Store enough blobs with large payloads to push DB well past 1 MiB minimum geometry.
    // Each blob: ~2592 (pubkey) + ~4627 (signature) + payload + overhead = ~8KB + payload.
    // With 10 KB payload, each blob is ~18 KB encrypted. 200 blobs = ~3.6 MB.
    std::string large_payload(10000, 'X');
    std::vector<std::array<uint8_t, 32>> hashes;
    for (int i = 0; i < 200; ++i) {
        auto blob = make_test_blob(0x51, large_payload + std::to_string(i));
        auto result = store.store_blob(blob);
        REQUIRE(result.status == StoreResult::Status::Stored);
        hashes.push_back(result.blob_hash);
    }

    auto before_compact = store.used_bytes();
    // Confirm DB grew past minimum 1 MiB
    REQUIRE(before_compact > 1048576);

    // Delete most blobs (180 of 200)
    for (size_t i = 0; i < 180; ++i) {
        REQUIRE(store.delete_blob_data(
            std::span<const uint8_t, 32>(ns),
            std::span<const uint8_t, 32>(hashes[i])));
    }

    // Compact should reclaim space from deleted blobs
    auto result = store.compact();
    REQUIRE(result.success);
    REQUIRE(result.before_bytes > 0);
    REQUIRE(result.after_bytes > 0);
    // After compaction of a DB with mostly deleted data, the
    // compacted file should be measurably smaller
    REQUIRE(result.after_bytes < result.before_bytes);
}

TEST_CASE("Storage::compact() DB is still functional after compaction", "[storage][compact]") {
    TempDir dir;
    Storage store(dir.path.string());

    std::array<uint8_t, 32> ns{};
    ns.fill(0x52);

    // Store blobs before compaction
    auto blob1 = make_test_blob(0x52, "before-compact-1");
    auto r1 = store.store_blob(blob1);
    REQUIRE(r1.status == StoreResult::Status::Stored);

    // Compact
    auto result = store.compact();
    REQUIRE(result.success);

    // Verify existing data survives compaction
    auto retrieved = store.get_blob(
        std::span<const uint8_t, 32>(ns),
        std::span<const uint8_t, 32>(r1.blob_hash));
    REQUIRE(retrieved.has_value());
    REQUIRE(retrieved->data == blob1.data);

    // Verify new writes work after compaction
    auto blob2 = make_test_blob(0x52, "after-compact-1");
    auto r2 = store.store_blob(blob2);
    REQUIRE(r2.status == StoreResult::Status::Stored);

    // Verify new blob is retrievable
    auto retrieved2 = store.get_blob(
        std::span<const uint8_t, 32>(ns),
        std::span<const uint8_t, 32>(r2.blob_hash));
    REQUIRE(retrieved2.has_value());
    REQUIRE(retrieved2->data == blob2.data);
}

// ============================================================================
// count_tombstones / count_delegations tests
// ============================================================================

using chromatindb::test::make_signed_blob;
using chromatindb::test::make_signed_tombstone;
using chromatindb::test::make_signed_delegation;
using chromatindb::test::run_async;
using chromatindb::engine::BlobEngine;

TEST_CASE("count_tombstones returns 0 for empty storage", "[storage][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    CHECK(store.count_tombstones() == 0);
}

TEST_CASE("count_tombstones counts tombstone entries", "[storage][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();

    // Store 2 regular blobs, then tombstone them
    auto blob1 = make_signed_blob(owner, "tombstone-test-1");
    auto r1 = run_async(pool, eng.ingest(blob1));
    REQUIRE(r1.accepted);

    auto blob2 = make_signed_blob(owner, "tombstone-test-2");
    auto r2 = run_async(pool, eng.ingest(blob2));
    REQUIRE(r2.accepted);

    // Store tombstones for both
    auto ts1 = make_signed_tombstone(owner, r1.ack->blob_hash);
    auto tr1 = run_async(pool, eng.delete_blob(ts1));
    REQUIRE(tr1.accepted);

    auto ts2 = make_signed_tombstone(owner, r2.ack->blob_hash);
    auto tr2 = run_async(pool, eng.delete_blob(ts2));
    REQUIRE(tr2.accepted);

    CHECK(store.count_tombstones() == 2);

    // Store a regular blob -- should NOT affect tombstone count
    auto blob3 = make_signed_blob(owner, "regular-blob");
    auto r3 = run_async(pool, eng.ingest(blob3));
    REQUIRE(r3.accepted);
    CHECK(store.count_tombstones() == 2);
}

TEST_CASE("count_delegations returns 0 for empty storage", "[storage][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    auto owner = chromatindb::identity::NodeIdentity::generate();
    CHECK(store.count_delegations(owner.namespace_id()) == 0);
}

TEST_CASE("count_delegations counts per-namespace delegations", "[storage][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);

    auto owner1 = chromatindb::identity::NodeIdentity::generate();
    auto owner2 = chromatindb::identity::NodeIdentity::generate();
    auto delegate1 = chromatindb::identity::NodeIdentity::generate();
    auto delegate2 = chromatindb::identity::NodeIdentity::generate();

    // owner1 delegates to delegate1 and delegate2
    auto d1 = make_signed_delegation(owner1, delegate1);
    auto dr1 = run_async(pool, eng.ingest(d1));
    REQUIRE(dr1.accepted);

    auto d2 = make_signed_delegation(owner1, delegate2);
    auto dr2 = run_async(pool, eng.ingest(d2));
    REQUIRE(dr2.accepted);

    // owner2 delegates to delegate1 only
    auto d3 = make_signed_delegation(owner2, delegate1);
    auto dr3 = run_async(pool, eng.ingest(d3));
    REQUIRE(dr3.accepted);

    CHECK(store.count_delegations(owner1.namespace_id()) == 2);
    CHECK(store.count_delegations(owner2.namespace_id()) == 1);

    // Unknown namespace returns 0
    auto unknown = chromatindb::identity::NodeIdentity::generate();
    CHECK(store.count_delegations(unknown.namespace_id()) == 0);
}

// ============================================================================
// list_delegations tests
// ============================================================================

TEST_CASE("list_delegations returns empty for namespace with no delegations", "[storage][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    auto owner = chromatindb::identity::NodeIdentity::generate();

    auto entries = store.list_delegations(owner.namespace_id());
    CHECK(entries.empty());
}

TEST_CASE("list_delegations returns entries for namespace with delegations", "[storage][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate1 = chromatindb::identity::NodeIdentity::generate();
    auto delegate2 = chromatindb::identity::NodeIdentity::generate();

    // Store 2 delegations
    auto d1 = make_signed_delegation(owner, delegate1);
    auto dr1 = run_async(pool, eng.ingest(d1));
    REQUIRE(dr1.accepted);

    auto d2 = make_signed_delegation(owner, delegate2);
    auto dr2 = run_async(pool, eng.ingest(d2));
    REQUIRE(dr2.accepted);

    auto entries = store.list_delegations(owner.namespace_id());
    REQUIRE(entries.size() == 2);

    // Compute expected delegate_pk_hash values
    auto expected_hash1 = chromatindb::crypto::sha3_256(
        delegate1.public_key().data(), delegate1.public_key().size());
    auto expected_hash2 = chromatindb::crypto::sha3_256(
        delegate2.public_key().data(), delegate2.public_key().size());

    // Compute expected delegation_blob_hash values
    auto encoded1 = chromatindb::wire::encode_blob(d1);
    auto expected_blob_hash1 = chromatindb::wire::blob_hash(encoded1);
    auto encoded2 = chromatindb::wire::encode_blob(d2);
    auto expected_blob_hash2 = chromatindb::wire::blob_hash(encoded2);

    // Entries may be in any order; collect into a set for matching
    bool found1 = false, found2 = false;
    for (const auto& e : entries) {
        if (e.delegate_pk_hash == expected_hash1) {
            CHECK(e.delegation_blob_hash == expected_blob_hash1);
            found1 = true;
        } else if (e.delegate_pk_hash == expected_hash2) {
            CHECK(e.delegation_blob_hash == expected_blob_hash2);
            found2 = true;
        }
    }
    CHECK(found1);
    CHECK(found2);
}

TEST_CASE("list_delegations does not return delegations from other namespaces", "[storage][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);

    auto owner1 = chromatindb::identity::NodeIdentity::generate();
    auto owner2 = chromatindb::identity::NodeIdentity::generate();
    auto delegate1 = chromatindb::identity::NodeIdentity::generate();
    auto delegate2 = chromatindb::identity::NodeIdentity::generate();

    // owner1 delegates to delegate1
    auto d1 = make_signed_delegation(owner1, delegate1);
    auto dr1 = run_async(pool, eng.ingest(d1));
    REQUIRE(dr1.accepted);

    // owner2 delegates to delegate2
    auto d2 = make_signed_delegation(owner2, delegate2);
    auto dr2 = run_async(pool, eng.ingest(d2));
    REQUIRE(dr2.accepted);

    // list_delegations(owner1) should only return delegate1
    auto entries1 = store.list_delegations(owner1.namespace_id());
    REQUIRE(entries1.size() == 1);

    auto expected_hash1 = chromatindb::crypto::sha3_256(
        delegate1.public_key().data(), delegate1.public_key().size());
    CHECK(entries1[0].delegate_pk_hash == expected_hash1);

    // list_delegations(owner2) should only return delegate2
    auto entries2 = store.list_delegations(owner2.namespace_id());
    REQUIRE(entries2.size() == 1);

    auto expected_hash2 = chromatindb::crypto::sha3_256(
        delegate2.public_key().data(), delegate2.public_key().size());
    CHECK(entries2[0].delegate_pk_hash == expected_hash2);
}
