#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <random>

#include "db/storage/storage.h"
#include "db/wire/codec.h"
#include "db/crypto/hash.h"
#include "db/identity/identity.h"

namespace fs = std::filesystem;

namespace {

/// Create a unique temporary directory for each test.
struct TempDir {
    fs::path path;

    TempDir() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        path = fs::temp_directory_path() /
               ("chromatindb_test_" + std::to_string(dist(gen)));
        // Don't create -- Storage should create it
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// Create a test BlobData with specified TTL and timestamp.
/// Uses deterministic namespace derived from a counter for test isolation.
chromatindb::wire::BlobData make_test_blob(
    uint8_t ns_byte,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = 1000)
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

    auto blob = make_test_blob(0x01, "not-expired", 604800, 1000);
    store.store_blob(blob);

    // Clock is at 1000, blob expires at 1000 + 604800 = 605800
    REQUIRE(store.run_expiry_scan() == 0);
}

TEST_CASE("Storage expiry scan purges expired blob", "[storage][expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    auto blob = make_test_blob(0x01, "will-expire", 100, 1000);
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
    auto blob1 = make_test_blob(0x01, "expires-first", 100, 1000);   // expires 1100
    auto blob2 = make_test_blob(0x01, "expires-second", 200, 1000);  // expires 1200
    auto blob3 = make_test_blob(0x01, "expires-last", 500, 1000);    // expires 1500

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

    auto blob = make_test_blob(0x01, "purge-once", 100, 1000);
    store.store_blob(blob);

    fake_time = 1101;
    REQUIRE(store.run_expiry_scan() == 1);
    REQUIRE(store.run_expiry_scan() == 0);  // Already purged
}

TEST_CASE("Storage TTL=0 blobs are never purged", "[storage][expiry]") {
    TempDir tmp;
    uint64_t fake_time = 1000;
    Storage store(tmp.path.string(), [&]() -> uint64_t { return fake_time; });

    auto permanent = make_test_blob(0x01, "permanent", 0, 1000);
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

    auto permanent = make_test_blob(0x01, "permanent-blob", 0, 1000);
    auto ephemeral = make_test_blob(0x01, "ephemeral-blob", 100, 1000);

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

    auto blob1 = make_test_blob(0x01, "will-expire-seq", 100, 1000);
    auto blob2 = make_test_blob(0x01, "will-survive-seq", 604800, 1000);

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
    auto blob = make_test_blob(0x62, "expiry-cleanup", 3600, 1000);
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
