#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>

#include "engine/engine.h"
#include "identity/identity.h"
#include "storage/storage.h"
#include "wire/codec.h"

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
chromatin::wire::BlobData make_signed_blob(
    const chromatin::identity::NodeIdentity& id,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = 1000)
{
    chromatin::wire::BlobData blob;
    std::memcpy(blob.namespace_id.data(), id.namespace_id().data(), 32);
    blob.pubkey.assign(id.public_key().begin(), id.public_key().end());
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = timestamp;

    // Build canonical signing input and sign it
    auto signing_input = chromatin::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}

} // anonymous namespace

using chromatin::engine::BlobEngine;
using chromatin::engine::IngestError;
using chromatin::engine::IngestStatus;
using chromatin::storage::Storage;

// ============================================================================
// Plan 03-01 Task 2: BlobEngine ingest pipeline
// ============================================================================

TEST_CASE("BlobEngine rejects blob with wrong pubkey size", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatin::identity::NodeIdentity::generate();
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

    auto id = chromatin::identity::NodeIdentity::generate();
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

    auto id = chromatin::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "wrong-namespace");

    // Corrupt namespace_id
    blob.namespace_id.fill(0xFF);

    auto result = engine.ingest(blob);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::namespace_mismatch);
}

TEST_CASE("BlobEngine rejects invalid signature", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatin::identity::NodeIdentity::generate();
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

    auto id = chromatin::identity::NodeIdentity::generate();
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

TEST_CASE("BlobEngine duplicate returns existing seq_num", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatin::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "dup-blob");

    auto first = engine.ingest(blob);
    auto second = engine.ingest(blob);

    REQUIRE(first.accepted);
    REQUIRE(second.accepted);
    REQUIRE(second.ack->status == IngestStatus::duplicate);
    REQUIRE(second.ack->seq_num == first.ack->seq_num);
}

TEST_CASE("BlobEngine accepts blobs from different namespaces", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id1 = chromatin::identity::NodeIdentity::generate();
    auto id2 = chromatin::identity::NodeIdentity::generate();

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

    auto id = chromatin::identity::NodeIdentity::generate();

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

    auto id = chromatin::identity::NodeIdentity::generate();

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
    auto id1 = chromatin::identity::NodeIdentity::generate();
    auto id2 = chromatin::identity::NodeIdentity::generate();
    auto id3 = chromatin::identity::NodeIdentity::generate();

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

    auto idA = chromatin::identity::NodeIdentity::generate();
    auto idB = chromatin::identity::NodeIdentity::generate();

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

    auto id = chromatin::identity::NodeIdentity::generate();
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

    auto id = chromatin::identity::NodeIdentity::generate();
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
    auto idA = chromatin::identity::NodeIdentity::generate();
    auto idB = chromatin::identity::NodeIdentity::generate();

    // Ingest 3 blobs for identity A
    std::vector<chromatin::engine::WriteAck> acksA;
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
    std::vector<chromatin::engine::WriteAck> acksB;
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
    REQUIRE(bad_result.error.value() == IngestError::namespace_mismatch);
}

TEST_CASE("BlobEngine validation order: namespace before signature", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    BlobEngine engine(store);

    auto id = chromatin::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "order-check");

    // Corrupt BOTH namespace AND signature
    blob.namespace_id.fill(0xFF);
    blob.signature[0] ^= 0xFF;

    auto result = engine.ingest(blob);
    REQUIRE_FALSE(result.accepted);
    // Should be namespace_mismatch (checked before signature), not invalid_signature
    REQUIRE(result.error.value() == IngestError::namespace_mismatch);
}
