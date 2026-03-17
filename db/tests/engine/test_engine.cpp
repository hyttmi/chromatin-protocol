#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>

#include "db/engine/engine.h"
#include "db/crypto/hash.h"
#include "db/identity/identity.h"
#include "db/net/framing.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

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
               ("chromatindb_test_engine_" + std::to_string(dist(gen)));
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// Build a properly signed BlobData using a NodeIdentity.
chromatindb::wire::BlobData make_signed_blob(
    const chromatindb::identity::NodeIdentity& id,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = 1000)
{
    chromatindb::wire::BlobData blob;
    std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
    blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = timestamp;

    // Build canonical signing input and sign it
    auto signing_input = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}

/// Build a properly signed tombstone BlobData for deleting a blob by its content hash.
chromatindb::wire::BlobData make_signed_tombstone(
    const chromatindb::identity::NodeIdentity& id,
    const std::array<uint8_t, 32>& target_blob_hash,
    uint64_t timestamp = 2000)
{
    chromatindb::wire::BlobData tombstone;
    std::memcpy(tombstone.namespace_id.data(), id.namespace_id().data(), 32);
    tombstone.pubkey.assign(id.public_key().begin(), id.public_key().end());
    tombstone.data = chromatindb::wire::make_tombstone_data(target_blob_hash);
    tombstone.ttl = 0;  // Permanent
    tombstone.timestamp = timestamp;

    auto signing_input = chromatindb::wire::build_signing_input(
        tombstone.namespace_id, tombstone.data, tombstone.ttl, tombstone.timestamp);
    tombstone.signature = id.sign(signing_input);

    return tombstone;
}

/// Build a properly signed delegation BlobData: owner delegates to delegate.
chromatindb::wire::BlobData make_signed_delegation(
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

    auto signing_input = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = owner.sign(signing_input);

    return blob;
}

/// Build a properly signed blob written by a delegate to an owner's namespace.
/// The delegate signs the canonical form with their own key.
chromatindb::wire::BlobData make_delegate_blob(
    const chromatindb::identity::NodeIdentity& owner,
    const chromatindb::identity::NodeIdentity& delegate,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = 4000)
{
    chromatindb::wire::BlobData blob;
    // Target the owner's namespace
    std::memcpy(blob.namespace_id.data(), owner.namespace_id().data(), 32);
    // Signed by delegate's key
    blob.pubkey.assign(delegate.public_key().begin(), delegate.public_key().end());
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = timestamp;

    auto signing_input = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = delegate.sign(signing_input);

    return blob;
}

} // anonymous namespace

using chromatindb::engine::BlobEngine;
using chromatindb::engine::IngestError;
using chromatindb::engine::IngestStatus;
using chromatindb::storage::Storage;

// ============================================================================
// Plan 03-01 Task 2: BlobEngine ingest pipeline
// ============================================================================

TEST_CASE("BlobEngine rejects blob with wrong pubkey size", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "wrong-pubkey-size");

    // Corrupt pubkey size
    blob.pubkey.resize(100);

    auto result = engine.ingest(blob);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::malformed_blob);
}

TEST_CASE("BlobEngine rejects blob with empty signature", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "empty-sig");

    // Clear signature
    blob.signature.clear();

    auto result = engine.ingest(blob);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::malformed_blob);
}

TEST_CASE("BlobEngine rejects namespace mismatch", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "wrong-namespace");

    // Corrupt namespace_id
    blob.namespace_id.fill(0xFF);

    auto result = engine.ingest(blob);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    // With delegation bypass, non-owner without delegation gets no_delegation
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("BlobEngine rejects invalid signature", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "bad-sig");

    // Flip first byte of signature
    blob.signature[0] ^= 0xFF;

    auto result = engine.ingest(blob);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::invalid_signature);
}

TEST_CASE("BlobEngine accepts valid blob", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "valid-blob");

    auto result = engine.ingest(blob);
    REQUIRE(result.accepted);
    REQUIRE(result.ack.has_value());
    REQUIRE(result.ack->seq_num == 1);
    REQUIRE(result.ack->status == IngestStatus::stored);
    REQUIRE(result.ack->replication_count == 1);

    // blob_hash should be non-zero
    std::array<uint8_t, 32> zero{};
    REQUIRE(result.ack->blob_hash != zero);
}

TEST_CASE("BlobEngine duplicate returns duplicate status", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "dup-blob");

    auto first = engine.ingest(blob);
    auto second = engine.ingest(blob);

    REQUIRE(first.accepted);
    REQUIRE(second.accepted);
    REQUIRE(second.ack->status == IngestStatus::duplicate);
    // Dedup short-circuit returns seq_num=0 (skips expensive seq_map scan).
    // This is safe: no consumer requires valid seq_num for duplicate acks.
    REQUIRE(second.ack->seq_num == 0);
    REQUIRE(second.ack->blob_hash == first.ack->blob_hash);
}

TEST_CASE("BlobEngine accepts blobs from different namespaces", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id1 = chromatindb::identity::NodeIdentity::generate();
    auto id2 = chromatindb::identity::NodeIdentity::generate();

    auto blob1 = make_signed_blob(id1, "ns1-blob");
    auto blob2 = make_signed_blob(id2, "ns2-blob");

    auto result1 = engine.ingest(blob1);
    auto result2 = engine.ingest(blob2);

    REQUIRE(result1.accepted);
    REQUIRE(result2.accepted);
}

// ============================================================================
// Plan 03-02: BlobEngine query methods
// ============================================================================

// --- get_blobs_since tests (QURY-01) ---

TEST_CASE("get_blobs_since returns blobs after seq_num", "[engine][query]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Ingest 3 blobs with different data (unique timestamps make unique blobs)
    auto blob1 = make_signed_blob(id, "seq-range-1", 604800, 1000);
    auto blob2 = make_signed_blob(id, "seq-range-2", 604800, 1001);
    auto blob3 = make_signed_blob(id, "seq-range-3", 604800, 1002);

    auto r1 = engine.ingest(blob1);
    auto r2 = engine.ingest(blob2);
    auto r3 = engine.ingest(blob3);
    REQUIRE(r1.accepted);
    REQUIRE(r2.accepted);
    REQUIRE(r3.accepted);

    // since_seq=0 -> all 3 blobs
    auto all = engine.get_blobs_since(id.namespace_id(), 0);
    REQUIRE(all.size() == 3);

    // since_seq=1 -> blobs with seq 2,3
    auto after_1 = engine.get_blobs_since(id.namespace_id(), 1);
    REQUIRE(after_1.size() == 2);

    // since_seq=3 -> no blobs (nothing after seq 3)
    auto after_3 = engine.get_blobs_since(id.namespace_id(), 3);
    REQUIRE(after_3.size() == 0);
}

TEST_CASE("get_blobs_since with max_count limits results", "[engine][query]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Ingest 5 blobs
    for (int i = 0; i < 5; ++i) {
        auto blob = make_signed_blob(id,
            "max-count-" + std::to_string(i), 604800,
            static_cast<uint64_t>(2000 + i));
        auto r = engine.ingest(blob);
        REQUIRE(r.accepted);
    }

    // max_count=2 should return exactly 2 blobs (the first two by seq order)
    auto limited = engine.get_blobs_since(id.namespace_id(), 0, 2);
    REQUIRE(limited.size() == 2);

    // max_count=0 should return all 5
    auto unlimited = engine.get_blobs_since(id.namespace_id(), 0, 0);
    REQUIRE(unlimited.size() == 5);

    // max_count larger than available returns all available
    auto over = engine.get_blobs_since(id.namespace_id(), 0, 100);
    REQUIRE(over.size() == 5);
}

TEST_CASE("get_blobs_since for unknown namespace returns empty", "[engine][query]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    // Query a namespace that has no blobs stored
    std::array<uint8_t, 32> fake_ns{};
    fake_ns.fill(0xAB);

    auto results = engine.get_blobs_since(fake_ns, 0);
    REQUIRE(results.empty());
}

// --- list_namespaces tests (QURY-02) ---

TEST_CASE("list_namespaces returns all namespaces", "[engine][query]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    // Generate 3 identities, ingest 1 blob each
    auto id1 = chromatindb::identity::NodeIdentity::generate();
    auto id2 = chromatindb::identity::NodeIdentity::generate();
    auto id3 = chromatindb::identity::NodeIdentity::generate();

    auto r1 = engine.ingest(make_signed_blob(id1, "ns-list-1"));
    auto r2 = engine.ingest(make_signed_blob(id2, "ns-list-2"));
    auto r3 = engine.ingest(make_signed_blob(id3, "ns-list-3"));
    REQUIRE(r1.accepted);
    REQUIRE(r2.accepted);
    REQUIRE(r3.accepted);

    auto namespaces = engine.list_namespaces();
    REQUIRE(namespaces.size() == 3);

    // Each should have latest_seq_num=1
    for (const auto& ns_info : namespaces) {
        REQUIRE(ns_info.latest_seq_num == 1);
    }
}

TEST_CASE("list_namespaces shows correct latest seq_num", "[engine][query]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto idA = chromatindb::identity::NodeIdentity::generate();
    auto idB = chromatindb::identity::NodeIdentity::generate();

    // Ingest 4 blobs in namespace A
    for (int i = 0; i < 4; ++i) {
        auto blob = make_signed_blob(idA,
            "nsA-" + std::to_string(i), 604800,
            static_cast<uint64_t>(3000 + i));
        REQUIRE(engine.ingest(blob).accepted);
    }

    // Ingest 2 blobs in namespace B
    for (int i = 0; i < 2; ++i) {
        auto blob = make_signed_blob(idB,
            "nsB-" + std::to_string(i), 604800,
            static_cast<uint64_t>(4000 + i));
        REQUIRE(engine.ingest(blob).accepted);
    }

    auto namespaces = engine.list_namespaces();
    REQUIRE(namespaces.size() == 2);

    // Find each namespace and check seq_num
    for (const auto& ns_info : namespaces) {
        auto nsA_span = idA.namespace_id();
        auto nsB_span = idB.namespace_id();

        if (std::equal(ns_info.namespace_id.begin(), ns_info.namespace_id.end(),
                       nsA_span.begin())) {
            REQUIRE(ns_info.latest_seq_num == 4);
        } else if (std::equal(ns_info.namespace_id.begin(), ns_info.namespace_id.end(),
                              nsB_span.begin())) {
            REQUIRE(ns_info.latest_seq_num == 2);
        } else {
            FAIL("Unexpected namespace in list_namespaces");
        }
    }
}

TEST_CASE("list_namespaces empty storage returns empty", "[engine][query]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto namespaces = engine.list_namespaces();
    REQUIRE(namespaces.empty());
}

// --- get_blob tests ---

TEST_CASE("get_blob returns stored blob by hash", "[engine][query]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "get-by-hash");
    auto result = engine.ingest(blob);
    REQUIRE(result.accepted);

    auto found = engine.get_blob(id.namespace_id(), result.ack->blob_hash);
    REQUIRE(found.has_value());
    REQUIRE(found->data == blob.data);
    REQUIRE(found->ttl == blob.ttl);
    REQUIRE(found->timestamp == blob.timestamp);
}

TEST_CASE("get_blob returns nullopt for non-existent hash", "[engine][query]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Don't ingest anything -- query a fake hash
    std::array<uint8_t, 32> fake_hash{};
    fake_hash.fill(0xDE);

    auto found = engine.get_blob(id.namespace_id(), fake_hash);
    REQUIRE_FALSE(found.has_value());
}

// --- End-to-end integration test ---

TEST_CASE("full ingest-query cycle across namespaces", "[engine][query][integration]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    // Generate 2 identities
    auto idA = chromatindb::identity::NodeIdentity::generate();
    auto idB = chromatindb::identity::NodeIdentity::generate();

    // Ingest 3 blobs for identity A
    std::vector<chromatindb::engine::WriteAck> acksA;
    for (int i = 0; i < 3; ++i) {
        auto blob = make_signed_blob(idA,
            "e2e-A-" + std::to_string(i), 604800,
            static_cast<uint64_t>(5000 + i));
        auto r = engine.ingest(blob);
        REQUIRE(r.accepted);
        REQUIRE(r.ack->status == IngestStatus::stored);
        acksA.push_back(r.ack.value());
    }

    // Ingest 2 blobs for identity B
    std::vector<chromatindb::engine::WriteAck> acksB;
    for (int i = 0; i < 2; ++i) {
        auto blob = make_signed_blob(idB,
            "e2e-B-" + std::to_string(i), 604800,
            static_cast<uint64_t>(6000 + i));
        auto r = engine.ingest(blob);
        REQUIRE(r.accepted);
        REQUIRE(r.ack->status == IngestStatus::stored);
        acksB.push_back(r.ack.value());
    }

    // Verify list_namespaces returns 2 entries with correct seq_nums
    auto namespaces = engine.list_namespaces();
    REQUIRE(namespaces.size() == 2);

    for (const auto& ns_info : namespaces) {
        auto nsA_span = idA.namespace_id();
        auto nsB_span = idB.namespace_id();

        if (std::equal(ns_info.namespace_id.begin(), ns_info.namespace_id.end(),
                       nsA_span.begin())) {
            REQUIRE(ns_info.latest_seq_num == 3);
        } else if (std::equal(ns_info.namespace_id.begin(), ns_info.namespace_id.end(),
                              nsB_span.begin())) {
            REQUIRE(ns_info.latest_seq_num == 2);
        } else {
            FAIL("Unexpected namespace in e2e test");
        }
    }

    // Verify get_blobs_since for A since 0 returns 3 blobs
    auto blobsA = engine.get_blobs_since(idA.namespace_id(), 0);
    REQUIRE(blobsA.size() == 3);

    // Verify get_blobs_since for B since 1 returns 1 blob (seq 2 only)
    auto blobsB = engine.get_blobs_since(idB.namespace_id(), 1);
    REQUIRE(blobsB.size() == 1);

    // Verify get_blob for A's first blob returns correct data
    auto firstA = engine.get_blob(idA.namespace_id(), acksA[0].blob_hash);
    REQUIRE(firstA.has_value());
    std::string expected_data = "e2e-A-0";
    std::vector<uint8_t> expected_vec(expected_data.begin(), expected_data.end());
    REQUIRE(firstA->data == expected_vec);

    // Verify namespace mismatch still works: try ingesting with wrong namespace for B's pubkey
    auto bad_blob = make_signed_blob(idB, "should-fail");
    // Set namespace to A's namespace (wrong for B's pubkey)
    std::memcpy(bad_blob.namespace_id.data(), idA.namespace_id().data(), 32);

    auto bad_result = engine.ingest(bad_blob);
    REQUIRE_FALSE(bad_result.accepted);
    // With delegation bypass, non-owner without delegation gets no_delegation
    REQUIRE(bad_result.error.value() == IngestError::no_delegation);
}

// ============================================================================
// Plan 11-01 Task 2: Oversized blob rejection (Step 0)
// ============================================================================

TEST_CASE("BlobEngine rejects oversized blob data", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    SECTION("blob with data > MAX_BLOB_DATA_SIZE is rejected") {
        chromatindb::wire::BlobData blob;
        std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
        blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
        blob.data.resize(chromatindb::net::MAX_BLOB_DATA_SIZE + 1, 0x42);
        blob.ttl = 604800;
        blob.timestamp = 1000;
        blob.signature = {0x01};  // Invalid sig, but we should never reach sig check

        auto result = engine.ingest(blob);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.error.has_value());
        REQUIRE(result.error.value() == IngestError::oversized_blob);
    }

    SECTION("blob with data == MAX_BLOB_DATA_SIZE is not rejected for size") {
        chromatindb::wire::BlobData blob;
        std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
        blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
        blob.data.resize(chromatindb::net::MAX_BLOB_DATA_SIZE, 0x42);
        blob.ttl = 604800;
        blob.timestamp = 1000;
        blob.signature = {0x01};  // Invalid sig

        auto result = engine.ingest(blob);
        // Should not be oversized_blob -- will fail on later validation step
        if (!result.accepted && result.error.has_value()) {
            REQUIRE(result.error.value() != IngestError::oversized_blob);
        }
    }

    SECTION("blob with empty data is not rejected for size") {
        auto blob = make_signed_blob(id, "");
        auto result = engine.ingest(blob);
        // Empty data is valid, should be accepted
        REQUIRE(result.accepted);
    }

    SECTION("oversized_blob rejection happens before signature verification") {
        chromatindb::wire::BlobData blob;
        std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
        blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
        blob.data.resize(chromatindb::net::MAX_BLOB_DATA_SIZE + 1, 0x42);
        blob.ttl = 604800;
        blob.timestamp = 1000;
        // Invalid signature -- if size check is first, we get oversized_blob, not invalid_signature
        blob.signature.resize(100, 0xFF);

        auto result = engine.ingest(blob);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.error.value() == IngestError::oversized_blob);
    }

    SECTION("error_detail includes actual size") {
        chromatindb::wire::BlobData blob;
        std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
        blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
        blob.data.resize(chromatindb::net::MAX_BLOB_DATA_SIZE + 1, 0x42);
        blob.ttl = 604800;
        blob.timestamp = 1000;
        blob.signature = {0x01};

        auto result = engine.ingest(blob);
        REQUIRE_FALSE(result.accepted);
        // error_detail should contain the actual size
        REQUIRE(result.error_detail.find(std::to_string(chromatindb::net::MAX_BLOB_DATA_SIZE + 1)) != std::string::npos);
    }
}

TEST_CASE("BlobEngine validation order: namespace/delegation before signature", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "order-check");

    // Corrupt BOTH namespace AND signature
    blob.namespace_id.fill(0xFF);
    blob.signature[0] ^= 0xFF;

    auto result = engine.ingest(blob);
    REQUIRE_FALSE(result.accepted);
    // Should be no_delegation (namespace/delegation checked before signature), not invalid_signature
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

// ============================================================================
// Phase 12: Tombstone deletion tests
// ============================================================================

TEST_CASE("Tombstone utility functions", "[engine][tombstone]") {
    SECTION("is_tombstone detects valid tombstone data") {
        std::array<uint8_t, 32> target{};
        target.fill(0xAB);
        auto data = chromatindb::wire::make_tombstone_data(target);
        REQUIRE(chromatindb::wire::is_tombstone(data));
    }

    SECTION("is_tombstone rejects regular blob data") {
        std::vector<uint8_t> data = {0x01, 0x02, 0x03};
        REQUIRE_FALSE(chromatindb::wire::is_tombstone(data));
    }

    SECTION("is_tombstone rejects empty data") {
        std::vector<uint8_t> data;
        REQUIRE_FALSE(chromatindb::wire::is_tombstone(data));
    }

    SECTION("is_tombstone rejects wrong prefix with right size") {
        std::vector<uint8_t> data(36, 0x00);
        REQUIRE_FALSE(chromatindb::wire::is_tombstone(data));
    }

    SECTION("extract_tombstone_target returns correct hash") {
        std::array<uint8_t, 32> target{};
        target.fill(0xCD);
        auto data = chromatindb::wire::make_tombstone_data(target);
        auto extracted = chromatindb::wire::extract_tombstone_target(data);
        REQUIRE(extracted == target);
    }
}

TEST_CASE("delete_blob on existing blob creates tombstone", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Ingest a blob first
    auto blob = make_signed_blob(id, "to-be-deleted");
    auto ingest_result = engine.ingest(blob);
    REQUIRE(ingest_result.accepted);
    auto target_hash = ingest_result.ack->blob_hash;

    // Verify blob exists
    auto found_before = engine.get_blob(id.namespace_id(), target_hash);
    REQUIRE(found_before.has_value());

    // Delete it
    auto tombstone = make_signed_tombstone(id, target_hash);
    auto delete_result = engine.delete_blob(tombstone);
    REQUIRE(delete_result.accepted);
    REQUIRE(delete_result.ack.has_value());
    REQUIRE(delete_result.ack->status == IngestStatus::stored);

    // Original blob should be gone
    auto found_after = engine.get_blob(id.namespace_id(), target_hash);
    REQUIRE_FALSE(found_after.has_value());

    // Tombstone should exist (under its own hash)
    auto tombstone_hash = delete_result.ack->blob_hash;
    auto found_tombstone = engine.get_blob(id.namespace_id(), tombstone_hash);
    REQUIRE(found_tombstone.has_value());
    REQUIRE(chromatindb::wire::is_tombstone(found_tombstone->data));
}

TEST_CASE("delete_blob on non-existent blob creates pre-emptive tombstone", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Delete a blob that was never stored (pre-emptive)
    std::array<uint8_t, 32> fake_hash{};
    fake_hash.fill(0xBB);

    auto tombstone = make_signed_tombstone(id, fake_hash);
    auto result = engine.delete_blob(tombstone);
    REQUIRE(result.accepted);
    REQUIRE(result.ack->status == IngestStatus::stored);
}

TEST_CASE("delete_blob is idempotent", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Create a blob and delete it
    auto blob = make_signed_blob(id, "idempotent-delete");
    auto ingest_result = engine.ingest(blob);
    REQUIRE(ingest_result.accepted);
    auto target_hash = ingest_result.ack->blob_hash;

    auto tombstone = make_signed_tombstone(id, target_hash);
    auto first_delete = engine.delete_blob(tombstone);
    REQUIRE(first_delete.accepted);
    REQUIRE(first_delete.ack->status == IngestStatus::stored);

    // Delete again -- should return duplicate
    auto second_delete = engine.delete_blob(tombstone);
    REQUIRE(second_delete.accepted);
    REQUIRE(second_delete.ack->status == IngestStatus::duplicate);
}

TEST_CASE("Tombstone blocks future ingest of deleted blob", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Create and ingest a blob
    auto blob = make_signed_blob(id, "will-be-blocked");
    auto ingest_result = engine.ingest(blob);
    REQUIRE(ingest_result.accepted);
    auto target_hash = ingest_result.ack->blob_hash;

    // Delete it
    auto tombstone = make_signed_tombstone(id, target_hash);
    auto delete_result = engine.delete_blob(tombstone);
    REQUIRE(delete_result.accepted);

    // Try to re-ingest the same blob -- should be blocked
    auto re_ingest = engine.ingest(blob);
    REQUIRE_FALSE(re_ingest.accepted);
    REQUIRE(re_ingest.error.has_value());
    REQUIRE(re_ingest.error.value() == IngestError::tombstoned);
}

TEST_CASE("Pre-emptive tombstone blocks first arrival", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Create blob but DON'T ingest it yet
    auto blob = make_signed_blob(id, "pre-emptive-block");

    // Compute what its hash would be
    auto encoded = chromatindb::wire::encode_blob(blob);
    auto blob_content_hash = chromatindb::wire::blob_hash(encoded);

    // Create tombstone for it before it ever arrives
    auto tombstone = make_signed_tombstone(id, blob_content_hash);
    auto delete_result = engine.delete_blob(tombstone);
    REQUIRE(delete_result.accepted);

    // Now try to ingest the blob -- should be blocked
    auto ingest_result = engine.ingest(blob);
    REQUIRE_FALSE(ingest_result.accepted);
    REQUIRE(ingest_result.error.value() == IngestError::tombstoned);
}

TEST_CASE("Tombstone via ingest (sync path) deletes target and stores", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Ingest a regular blob
    auto blob = make_signed_blob(id, "sync-delete-target");
    auto ingest_result = engine.ingest(blob);
    REQUIRE(ingest_result.accepted);
    auto target_hash = ingest_result.ack->blob_hash;

    // Verify it exists
    auto found_before = engine.get_blob(id.namespace_id(), target_hash);
    REQUIRE(found_before.has_value());

    // Create a tombstone and INGEST it (simulating sync receive)
    auto tombstone = make_signed_tombstone(id, target_hash);
    auto tombstone_ingest = engine.ingest(tombstone);
    REQUIRE(tombstone_ingest.accepted);
    REQUIRE(tombstone_ingest.ack->status == IngestStatus::stored);

    // Target blob should be deleted
    auto found_after = engine.get_blob(id.namespace_id(), target_hash);
    REQUIRE_FALSE(found_after.has_value());

    // Tombstone should be stored
    auto tombstone_hash = tombstone_ingest.ack->blob_hash;
    auto found_tombstone = engine.get_blob(id.namespace_id(), tombstone_hash);
    REQUIRE(found_tombstone.has_value());
    REQUIRE(chromatindb::wire::is_tombstone(found_tombstone->data));
}

TEST_CASE("delete_blob validates namespace ownership", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto attacker = chromatindb::identity::NodeIdentity::generate();

    // Ingest a blob owned by 'owner'
    auto blob = make_signed_blob(owner, "owned-blob");
    auto ingest_result = engine.ingest(blob);
    REQUIRE(ingest_result.accepted);

    // Attacker tries to delete it -- tombstone has wrong namespace
    auto tombstone = make_signed_tombstone(attacker, ingest_result.ack->blob_hash);
    // Override namespace to owner's namespace (simulating attack)
    std::memcpy(tombstone.namespace_id.data(), owner.namespace_id().data(), 32);

    auto delete_result = engine.delete_blob(tombstone);
    REQUIRE_FALSE(delete_result.accepted);
    REQUIRE(delete_result.error.value() == IngestError::namespace_mismatch);
}

TEST_CASE("delete_blob validates signature", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    std::array<uint8_t, 32> target{};
    target.fill(0xCC);
    auto tombstone = make_signed_tombstone(id, target);

    // Corrupt signature
    tombstone.signature[0] ^= 0xFF;

    auto result = engine.delete_blob(tombstone);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::invalid_signature);
}

TEST_CASE("Tombstone survives expiry scan", "[engine][tombstone]") {
    TempDir tmp;
    uint64_t fake_now = 100000;
    Storage store(tmp.path.string(), [&]() { return fake_now; });
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Create and delete a blob
    auto blob = make_signed_blob(id, "expiry-test", 604800, 1000);
    auto ingest_result = engine.ingest(blob);
    REQUIRE(ingest_result.accepted);

    auto tombstone = make_signed_tombstone(id, ingest_result.ack->blob_hash);
    auto delete_result = engine.delete_blob(tombstone);
    REQUIRE(delete_result.accepted);

    // Advance time far past any TTL
    fake_now = 99999999;
    store.run_expiry_scan();

    // Tombstone should still exist (TTL=0 means permanent)
    auto tombstone_hash = delete_result.ack->blob_hash;
    auto found = engine.get_blob(id.namespace_id(), tombstone_hash);
    REQUIRE(found.has_value());
    REQUIRE(chromatindb::wire::is_tombstone(found->data));
}

// ============================================================================
// Phase 13: Delegation blob creation via ingest
// ============================================================================

TEST_CASE("Delegation blob created via ingest succeeds", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    auto deleg = make_signed_delegation(owner, delegate);
    auto result = engine.ingest(deleg);
    REQUIRE(result.accepted);
    REQUIRE(result.ack.has_value());
    REQUIRE(result.ack->status == IngestStatus::stored);
}

TEST_CASE("Delegation blob: has_valid_delegation returns true after ingest", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    auto deleg = make_signed_delegation(owner, delegate);
    auto result = engine.ingest(deleg);
    REQUIRE(result.accepted);

    REQUIRE(store.has_valid_delegation(
        owner.namespace_id(),
        delegate.public_key()));
}

TEST_CASE("Delegation blob duplicate returns IngestStatus::duplicate", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    auto deleg = make_signed_delegation(owner, delegate);
    auto first = engine.ingest(deleg);
    auto second = engine.ingest(deleg);

    REQUIRE(first.accepted);
    REQUIRE(second.accepted);
    REQUIRE(second.ack->status == IngestStatus::duplicate);
}

TEST_CASE("Delegation blob with wrong namespace rejected", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto attacker = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Attacker tries to create a delegation in owner's namespace
    auto deleg = make_signed_delegation(attacker, delegate);
    std::memcpy(deleg.namespace_id.data(), owner.namespace_id().data(), 32);

    auto result = engine.ingest(deleg);
    REQUIRE_FALSE(result.accepted);
    // Attacker's pubkey doesn't own the namespace and has no delegation
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("Delegation blob with bad signature rejected", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    auto deleg = make_signed_delegation(owner, delegate);
    deleg.signature[0] ^= 0xFF;

    auto result = engine.ingest(deleg);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::invalid_signature);
}

TEST_CASE("DELEG-03: delegation blob retrievable via get_blob (sync compatible)", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    auto deleg = make_signed_delegation(owner, delegate);
    auto result = engine.ingest(deleg);
    REQUIRE(result.accepted);

    // Retrieve via get_blob -- proves it's a regular blob in storage
    auto found = engine.get_blob(owner.namespace_id(), result.ack->blob_hash);
    REQUIRE(found.has_value());
    REQUIRE(chromatindb::wire::is_delegation(found->data));

    // Verify the delegate pubkey in the retrieved blob
    auto extracted_pk = chromatindb::wire::extract_delegate_pubkey(found->data);
    auto expected_pk = delegate.public_key();
    REQUIRE(std::equal(extracted_pk.begin(), extracted_pk.end(), expected_pk.begin()));
}

// ============================================================================
// Phase 13: Delegate write acceptance (DELEG-02, DELEG-04)
// ============================================================================

TEST_CASE("Delegate write accepted when delegation exists", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Owner creates delegation
    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(engine.ingest(deleg).accepted);

    // Delegate writes to owner's namespace
    auto blob = make_delegate_blob(owner, delegate, "delegate-write-1");
    auto result = engine.ingest(blob);
    REQUIRE(result.accepted);
    REQUIRE(result.ack->status == IngestStatus::stored);
}

TEST_CASE("Delegate write rejected when no delegation exists", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // No delegation -- delegate tries to write
    auto blob = make_delegate_blob(owner, delegate, "no-delegation");
    auto result = engine.ingest(blob);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("Delegate cannot delete (delete_blob is owner-only)", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Owner creates delegation and a blob
    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(engine.ingest(deleg).accepted);

    auto blob = make_signed_blob(owner, "owner-blob");
    auto ingest_result = engine.ingest(blob);
    REQUIRE(ingest_result.accepted);

    // Delegate tries to delete -- uses delete_blob with delegate's key
    chromatindb::wire::BlobData del_req;
    std::memcpy(del_req.namespace_id.data(), owner.namespace_id().data(), 32);
    del_req.pubkey.assign(delegate.public_key().begin(), delegate.public_key().end());
    del_req.data = chromatindb::wire::make_tombstone_data(ingest_result.ack->blob_hash);
    del_req.ttl = 0;
    del_req.timestamp = 5000;

    auto signing_input = chromatindb::wire::build_signing_input(
        del_req.namespace_id, del_req.data, del_req.ttl, del_req.timestamp);
    del_req.signature = delegate.sign(signing_input);

    auto result = engine.delete_blob(del_req);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::namespace_mismatch);
}

TEST_CASE("Delegate cannot create delegation blobs", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    auto other = chromatindb::identity::NodeIdentity::generate();

    // Owner delegates to delegate
    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(engine.ingest(deleg).accepted);

    // Delegate tries to create a delegation blob for 'other' in owner's namespace
    chromatindb::wire::BlobData fake_deleg;
    std::memcpy(fake_deleg.namespace_id.data(), owner.namespace_id().data(), 32);
    fake_deleg.pubkey.assign(delegate.public_key().begin(), delegate.public_key().end());
    fake_deleg.data = chromatindb::wire::make_delegation_data(other.public_key());
    fake_deleg.ttl = 0;
    fake_deleg.timestamp = 5000;

    auto signing_input = chromatindb::wire::build_signing_input(
        fake_deleg.namespace_id, fake_deleg.data, fake_deleg.ttl, fake_deleg.timestamp);
    fake_deleg.signature = delegate.sign(signing_input);

    auto result = engine.ingest(fake_deleg);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("Delegate cannot create tombstone blobs via ingest", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Owner delegates
    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(engine.ingest(deleg).accepted);

    // Delegate tries to ingest a tombstone blob in owner's namespace
    std::array<uint8_t, 32> fake_target{};
    fake_target.fill(0xAA);

    chromatindb::wire::BlobData tomb;
    std::memcpy(tomb.namespace_id.data(), owner.namespace_id().data(), 32);
    tomb.pubkey.assign(delegate.public_key().begin(), delegate.public_key().end());
    tomb.data = chromatindb::wire::make_tombstone_data(fake_target);
    tomb.ttl = 0;
    tomb.timestamp = 5000;

    auto signing_input = chromatindb::wire::build_signing_input(
        tomb.namespace_id, tomb.data, tomb.ttl, tomb.timestamp);
    tomb.signature = delegate.sign(signing_input);

    auto result = engine.ingest(tomb);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("Revocation via tombstone blocks delegate writes", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Owner creates delegation
    auto deleg = make_signed_delegation(owner, delegate);
    auto deleg_result = engine.ingest(deleg);
    REQUIRE(deleg_result.accepted);

    // Delegate can write
    auto blob1 = make_delegate_blob(owner, delegate, "before-revocation", 604800, 4000);
    REQUIRE(engine.ingest(blob1).accepted);

    // Owner revokes by tombstoning the delegation blob
    auto tombstone = make_signed_tombstone(owner, deleg_result.ack->blob_hash, 5000);
    auto del_result = engine.delete_blob(tombstone);
    REQUIRE(del_result.accepted);

    // Delegate writes should now be rejected
    auto blob2 = make_delegate_blob(owner, delegate, "after-revocation", 604800, 6000);
    auto result = engine.ingest(blob2);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("Re-delegation after revocation allows writes again", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // 1. Delegate
    auto deleg1 = make_signed_delegation(owner, delegate, 3000);
    auto deleg1_result = engine.ingest(deleg1);
    REQUIRE(deleg1_result.accepted);

    // 2. Delegate writes
    auto blob1 = make_delegate_blob(owner, delegate, "round-1", 604800, 4000);
    REQUIRE(engine.ingest(blob1).accepted);

    // 3. Revoke
    auto tombstone = make_signed_tombstone(owner, deleg1_result.ack->blob_hash, 5000);
    REQUIRE(engine.delete_blob(tombstone).accepted);

    // 4. Delegate blocked
    auto blob2 = make_delegate_blob(owner, delegate, "blocked", 604800, 6000);
    REQUIRE_FALSE(engine.ingest(blob2).accepted);

    // 5. Re-delegate (new timestamp -> new blob hash -> not blocked by old tombstone)
    auto deleg2 = make_signed_delegation(owner, delegate, 7000);
    REQUIRE(engine.ingest(deleg2).accepted);

    // 6. Delegate can write again
    auto blob3 = make_delegate_blob(owner, delegate, "round-2", 604800, 8000);
    REQUIRE(engine.ingest(blob3).accepted);
}

TEST_CASE("Delegate-written blobs survive revocation", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Delegate, write, revoke
    auto deleg = make_signed_delegation(owner, delegate);
    auto deleg_result = engine.ingest(deleg);
    REQUIRE(deleg_result.accepted);

    auto blob = make_delegate_blob(owner, delegate, "persists-after-revoke");
    auto blob_result = engine.ingest(blob);
    REQUIRE(blob_result.accepted);

    auto tombstone = make_signed_tombstone(owner, deleg_result.ack->blob_hash, 5000);
    REQUIRE(engine.delete_blob(tombstone).accepted);

    // Delegate's blob should still be retrievable
    auto found = engine.get_blob(owner.namespace_id(), blob_result.ack->blob_hash);
    REQUIRE(found.has_value());
    REQUIRE(found->data == blob.data);
}

TEST_CASE("Owner can still write after creating delegation", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Owner creates delegation
    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(engine.ingest(deleg).accepted);

    // Owner writes a regular blob -- should still work via ownership check
    auto blob = make_signed_blob(owner, "owner-still-writes");
    auto result = engine.ingest(blob);
    REQUIRE(result.accepted);
}

TEST_CASE("Multiple delegates: independent write and revocation", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate1 = chromatindb::identity::NodeIdentity::generate();
    auto delegate2 = chromatindb::identity::NodeIdentity::generate();

    // Delegate both
    auto deleg1 = make_signed_delegation(owner, delegate1, 3000);
    auto deleg2 = make_signed_delegation(owner, delegate2, 3001);
    auto deleg1_result = engine.ingest(deleg1);
    auto deleg2_result = engine.ingest(deleg2);
    REQUIRE(deleg1_result.accepted);
    REQUIRE(deleg2_result.accepted);

    // Both can write
    auto blob1 = make_delegate_blob(owner, delegate1, "d1-writes", 604800, 4000);
    auto blob2 = make_delegate_blob(owner, delegate2, "d2-writes", 604800, 4001);
    REQUIRE(engine.ingest(blob1).accepted);
    REQUIRE(engine.ingest(blob2).accepted);

    // Revoke delegate1 only
    auto tombstone = make_signed_tombstone(owner, deleg1_result.ack->blob_hash, 5000);
    REQUIRE(engine.delete_blob(tombstone).accepted);

    // delegate1 blocked, delegate2 still works
    auto blob3 = make_delegate_blob(owner, delegate1, "d1-blocked", 604800, 6000);
    REQUIRE_FALSE(engine.ingest(blob3).accepted);

    auto blob4 = make_delegate_blob(owner, delegate2, "d2-still-works", 604800, 6001);
    REQUIRE(engine.ingest(blob4).accepted);
}

// ============================================================================
// Plan 16-02: BlobEngine storage capacity enforcement
// ============================================================================

TEST_CASE("BlobEngine rejects ingest when over capacity", "[engine][capacity]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Set max_storage_bytes = 1 byte -- any real database exceeds this immediately
    BlobEngine engine(store, 1);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "capacity-test");

    auto result = engine.ingest(blob);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::storage_full);
    REQUIRE(result.error_detail == "storage capacity exceeded");
}

TEST_CASE("BlobEngine tombstone exempt from capacity check", "[engine][capacity]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Set max_storage_bytes = 1 byte -- database already exceeds this
    BlobEngine engine(store, 1);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Create a tombstone targeting a random hash
    std::array<uint8_t, 32> random_hash{};
    random_hash.fill(0xAB);
    auto tombstone = make_signed_tombstone(id, random_hash);

    auto result = engine.ingest(tombstone);
    // Tombstone should NOT be rejected with storage_full.
    // It may fail for other reasons (like the target not existing), but
    // the key assertion is that it does NOT fail with storage_full.
    if (!result.accepted) {
        REQUIRE(result.error.value() != IngestError::storage_full);
    }
}

TEST_CASE("BlobEngine ingest succeeds when unlimited (max_storage_bytes=0)", "[engine][capacity]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Default: max_storage_bytes = 0 (unlimited)
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "unlimited-test");

    auto result = engine.ingest(blob);
    REQUIRE(result.accepted);
}

TEST_CASE("BlobEngine delete_blob works when over capacity", "[engine][capacity]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Ingest a blob first (with generous limit), then lower limit and delete
    BlobEngine unlimited_engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "to-be-deleted");
    auto ingest_result = unlimited_engine.ingest(blob);
    REQUIRE(ingest_result.accepted);

    // Now create a capacity-limited engine and delete via tombstone
    BlobEngine limited_engine(store, 1);
    auto tombstone = make_signed_tombstone(id, ingest_result.ack->blob_hash);
    auto result = limited_engine.delete_blob(tombstone);
    // delete_blob has no capacity check (it creates tombstones, which are inherently exempt)
    REQUIRE(result.accepted);
}

TEST_CASE("BlobEngine ingest succeeds when under capacity", "[engine][capacity]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Set generous limit: 1 GiB
    BlobEngine engine(store, 1ULL << 30);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "under-capacity-test");

    auto result = engine.ingest(blob);
    REQUIRE(result.accepted);
}
