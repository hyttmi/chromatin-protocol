#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <random>

#include "db/engine/engine.h"
#include "db/crypto/hash.h"
#include "db/identity/identity.h"
#include "db/net/framing.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

#include "db/tests/test_helpers.h"
#include "db/util/hex.h"

namespace fs = std::filesystem;

namespace {

} // anonymous namespace

using chromatindb::test::TempDir;
using chromatindb::test::run_async;
using chromatindb::test::make_signed_blob;
using chromatindb::test::make_signed_tombstone;
using chromatindb::test::make_signed_delegation;
using chromatindb::test::make_delegate_blob;
using chromatindb::test::current_timestamp;
using chromatindb::test::TS_AUTO;
using chromatindb::test::ns_span;
using chromatindb::util::to_hex;

using chromatindb::engine::BlobEngine;
using chromatindb::engine::IngestError;
using chromatindb::engine::IngestStatus;
using chromatindb::storage::Storage;

// ============================================================================
// Plan 03-01 Task 2: BlobEngine ingest pipeline
// ============================================================================

TEST_CASE("BlobEngine rejects blob with empty signature", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
    auto blob = make_signed_blob(id, "empty-sig");

    // Clear signature
    blob.signature.clear();

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::malformed_blob);
}

TEST_CASE("BlobEngine rejects unknown signer_hint (no_delegation)", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Register PUBK so the namespace exists (PUBK-first gate passes).
    auto pubk = chromatindb::test::make_pubk_blob(id);
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), pubk)).accepted);

    auto blob = make_signed_blob(id, "wrong-signer-hint");
    // Corrupt signer_hint so owner_pubkeys lookup misses AND delegation_map misses.
    blob.signer_hint.fill(0xFF);

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("BlobEngine rejects invalid signature", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
    auto blob = make_signed_blob(id, "bad-sig");

    // Flip first byte of signature
    blob.signature[0] ^= 0xFF;

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::invalid_signature);
}

TEST_CASE("BlobEngine accepts valid blob", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
    auto blob = make_signed_blob(id, "valid-blob");

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
    auto blob = make_signed_blob(id, "dup-blob");

    auto first = run_async(pool, engine.ingest(ns_span(id), blob));
    auto second = run_async(pool, engine.ingest(ns_span(id), blob));

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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id1 = chromatindb::identity::NodeIdentity::generate();
    auto id2 = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id1);
    chromatindb::test::register_pubk(store, id2);

    auto blob1 = make_signed_blob(id1, "ns1-blob");
    auto blob2 = make_signed_blob(id2, "ns2-blob");

    auto result1 = run_async(pool, engine.ingest(ns_span(id1), blob1));
    auto result2 = run_async(pool, engine.ingest(ns_span(id2), blob2));

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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Ingest 3 blobs with different data (unique payloads make unique blobs)
    auto blob1 = make_signed_blob(id, "seq-range-1");
    auto blob2 = make_signed_blob(id, "seq-range-2");
    auto blob3 = make_signed_blob(id, "seq-range-3");

    auto r1 = run_async(pool, engine.ingest(ns_span(id), blob1));
    auto r2 = run_async(pool, engine.ingest(ns_span(id), blob2));
    auto r3 = run_async(pool, engine.ingest(ns_span(id), blob3));
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Ingest 5 blobs
    for (int i = 0; i < 5; ++i) {
        auto blob = make_signed_blob(id,
            "max-count-" + std::to_string(i), 604800,
            current_timestamp() + static_cast<uint64_t>(i));
        auto r = run_async(pool, engine.ingest(ns_span(id), blob));
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    // Generate 3 identities, ingest 1 blob each
    auto id1 = chromatindb::identity::NodeIdentity::generate();
    auto id2 = chromatindb::identity::NodeIdentity::generate();
    auto id3 = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id1);
    chromatindb::test::register_pubk(store, id2);
    chromatindb::test::register_pubk(store, id3);

    auto r1 = run_async(pool, engine.ingest(ns_span(id1), make_signed_blob(id1, "ns-list-1")));
    auto r2 = run_async(pool, engine.ingest(ns_span(id2), make_signed_blob(id2, "ns-list-2")));
    auto r3 = run_async(pool, engine.ingest(ns_span(id3), make_signed_blob(id3, "ns-list-3")));
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto idA = chromatindb::identity::NodeIdentity::generate();
    auto idB = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, idA);
    chromatindb::test::register_pubk(store, idB);

    // Ingest 4 blobs in namespace A
    for (int i = 0; i < 4; ++i) {
        auto blob = make_signed_blob(idA,
            "nsA-" + std::to_string(i), 604800,
            current_timestamp() + static_cast<uint64_t>(i));
        REQUIRE(run_async(pool, engine.ingest(ns_span(idA), blob)).accepted);
    }

    // Ingest 2 blobs in namespace B
    for (int i = 0; i < 2; ++i) {
        auto blob = make_signed_blob(idB,
            "nsB-" + std::to_string(i), 604800,
            current_timestamp() + static_cast<uint64_t>(i));
        REQUIRE(run_async(pool, engine.ingest(ns_span(idB), blob)).accepted);
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto namespaces = engine.list_namespaces();
    REQUIRE(namespaces.empty());
}

// --- get_blob tests ---

TEST_CASE("get_blob returns stored blob by hash", "[engine][query]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
    auto blob = make_signed_blob(id, "get-by-hash");
    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    // Generate 2 identities
    auto idA = chromatindb::identity::NodeIdentity::generate();
    auto idB = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, idA);
    chromatindb::test::register_pubk(store, idB);

    // Ingest 3 blobs for identity A
    std::vector<chromatindb::engine::WriteAck> acksA;
    for (int i = 0; i < 3; ++i) {
        auto blob = make_signed_blob(idA,
            "e2e-A-" + std::to_string(i), 604800,
            current_timestamp() + static_cast<uint64_t>(i));
        auto r = run_async(pool, engine.ingest(ns_span(idA), blob));
        REQUIRE(r.accepted);
        REQUIRE(r.ack->status == IngestStatus::stored);
        acksA.push_back(r.ack.value());
    }

    // Ingest 2 blobs for identity B
    std::vector<chromatindb::engine::WriteAck> acksB;
    for (int i = 0; i < 2; ++i) {
        auto blob = make_signed_blob(idB,
            "e2e-B-" + std::to_string(i), 604800,
            current_timestamp() + static_cast<uint64_t>(i));
        auto r = run_async(pool, engine.ingest(ns_span(idB), blob));
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

    // Post-122: B's blob targeted at A's namespace -> unknown signer for that ns.
    auto bad_blob = make_signed_blob(idB, "should-fail");
    auto bad_result = run_async(pool, engine.ingest(ns_span(idA), bad_blob));
    REQUIRE_FALSE(bad_result.accepted);
    REQUIRE(bad_result.error.value() == IngestError::no_delegation);
}

// ============================================================================
// Plan 11-01 Task 2: Oversized blob rejection (Step 0)
// ============================================================================

TEST_CASE("BlobEngine rejects oversized blob data", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    SECTION("blob with data > MAX_BLOB_DATA_SIZE is rejected") {
        chromatindb::wire::BlobData blob;
        auto _blob_hint = chromatindb::crypto::sha3_256(id.public_key());
        std::memcpy(blob.signer_hint.data(), _blob_hint.data(), 32);
        blob.data.resize(chromatindb::net::MAX_BLOB_DATA_SIZE + 1, 0x42);
        blob.ttl = 604800;
        blob.timestamp = current_timestamp();
        blob.signature = {0x01};  // Invalid sig, but we should never reach sig check

        auto result = run_async(pool, engine.ingest(ns_span(id), blob));
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.error.has_value());
        REQUIRE(result.error.value() == IngestError::oversized_blob);
    }

    SECTION("blob with data == MAX_BLOB_DATA_SIZE is not rejected for size") {
        chromatindb::wire::BlobData blob;
        auto _blob_hint = chromatindb::crypto::sha3_256(id.public_key());
        std::memcpy(blob.signer_hint.data(), _blob_hint.data(), 32);
        blob.data.resize(chromatindb::net::MAX_BLOB_DATA_SIZE, 0x42);
        blob.ttl = 604800;
        blob.timestamp = current_timestamp();
        blob.signature = {0x01};  // Invalid sig

        auto result = run_async(pool, engine.ingest(ns_span(id), blob));
        // Should not be oversized_blob -- will fail on later validation step
        if (!result.accepted && result.error.has_value()) {
            REQUIRE(result.error.value() != IngestError::oversized_blob);
        }
    }

    SECTION("blob with empty data is not rejected for size") {
        auto blob = make_signed_blob(id, "");
        auto result = run_async(pool, engine.ingest(ns_span(id), blob));
        // Empty data is valid, should be accepted
        REQUIRE(result.accepted);
    }

    SECTION("oversized_blob rejection happens before signature verification") {
        chromatindb::wire::BlobData blob;
        auto _blob_hint = chromatindb::crypto::sha3_256(id.public_key());
        std::memcpy(blob.signer_hint.data(), _blob_hint.data(), 32);
        blob.data.resize(chromatindb::net::MAX_BLOB_DATA_SIZE + 1, 0x42);
        blob.ttl = 604800;
        blob.timestamp = current_timestamp();
        // Invalid signature -- if size check is first, we get oversized_blob, not invalid_signature
        blob.signature.resize(100, 0xFF);

        auto result = run_async(pool, engine.ingest(ns_span(id), blob));
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.error.value() == IngestError::oversized_blob);
    }

    SECTION("error_detail includes actual size") {
        chromatindb::wire::BlobData blob;
        auto _blob_hint = chromatindb::crypto::sha3_256(id.public_key());
        std::memcpy(blob.signer_hint.data(), _blob_hint.data(), 32);
        blob.data.resize(chromatindb::net::MAX_BLOB_DATA_SIZE + 1, 0x42);
        blob.ttl = 604800;
        blob.timestamp = current_timestamp();
        blob.signature = {0x01};

        auto result = run_async(pool, engine.ingest(ns_span(id), blob));
        REQUIRE_FALSE(result.accepted);
        // error_detail should contain the actual size
        REQUIRE(result.error_detail.find(std::to_string(chromatindb::net::MAX_BLOB_DATA_SIZE + 1)) != std::string::npos);
    }
}

TEST_CASE("BlobEngine validation order: delegation before signature", "[engine]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    REQUIRE(run_async(pool, engine.ingest(ns_span(id),
        chromatindb::test::make_pubk_blob(id))).accepted);

    auto blob = make_signed_blob(id, "order-check");
    // Corrupt BOTH signer_hint AND signature.
    blob.signer_hint.fill(0xFF);
    blob.signature[0] ^= 0xFF;

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(result.accepted);
    // no_delegation (owner/delegation miss before sig verify), not invalid_signature.
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Ingest a blob first
    auto blob = make_signed_blob(id, "to-be-deleted");
    auto ingest_result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(ingest_result.accepted);
    auto target_hash = ingest_result.ack->blob_hash;

    // Verify blob exists
    auto found_before = engine.get_blob(id.namespace_id(), target_hash);
    REQUIRE(found_before.has_value());

    // Delete it
    auto tombstone = make_signed_tombstone(id, target_hash);
    auto delete_result = run_async(pool, engine.delete_blob(ns_span(id), tombstone));
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Delete a blob that was never stored (pre-emptive)
    std::array<uint8_t, 32> fake_hash{};
    fake_hash.fill(0xBB);

    auto tombstone = make_signed_tombstone(id, fake_hash);
    auto result = run_async(pool, engine.delete_blob(ns_span(id), tombstone));
    REQUIRE(result.accepted);
    REQUIRE(result.ack->status == IngestStatus::stored);
}

TEST_CASE("delete_blob is idempotent", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Create a blob and delete it
    auto blob = make_signed_blob(id, "idempotent-delete");
    auto ingest_result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(ingest_result.accepted);
    auto target_hash = ingest_result.ack->blob_hash;

    auto tombstone = make_signed_tombstone(id, target_hash);
    auto first_delete = run_async(pool, engine.delete_blob(ns_span(id), tombstone));
    REQUIRE(first_delete.accepted);
    REQUIRE(first_delete.ack->status == IngestStatus::stored);

    // Delete again -- should return duplicate
    auto second_delete = run_async(pool, engine.delete_blob(ns_span(id), tombstone));
    REQUIRE(second_delete.accepted);
    REQUIRE(second_delete.ack->status == IngestStatus::duplicate);
}

TEST_CASE("Tombstone blocks future ingest of deleted blob", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Create and ingest a blob
    auto blob = make_signed_blob(id, "will-be-blocked");
    auto ingest_result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(ingest_result.accepted);
    auto target_hash = ingest_result.ack->blob_hash;

    // Delete it
    auto tombstone = make_signed_tombstone(id, target_hash);
    auto delete_result = run_async(pool, engine.delete_blob(ns_span(id), tombstone));
    REQUIRE(delete_result.accepted);

    // Try to re-ingest the same blob -- should be blocked
    auto re_ingest = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(re_ingest.accepted);
    REQUIRE(re_ingest.error.has_value());
    REQUIRE(re_ingest.error.value() == IngestError::tombstoned);
}

TEST_CASE("Pre-emptive tombstone blocks first arrival", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Create blob but DON'T ingest it yet
    auto blob = make_signed_blob(id, "pre-emptive-block");

    // Compute what its hash would be
    auto encoded = chromatindb::wire::encode_blob(blob);
    auto blob_content_hash = chromatindb::wire::blob_hash(encoded);

    // Create tombstone for it before it ever arrives
    auto tombstone = make_signed_tombstone(id, blob_content_hash);
    auto delete_result = run_async(pool, engine.delete_blob(ns_span(id), tombstone));
    REQUIRE(delete_result.accepted);

    // Now try to ingest the blob -- should be blocked
    auto ingest_result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(ingest_result.accepted);
    REQUIRE(ingest_result.error.value() == IngestError::tombstoned);
}

TEST_CASE("Tombstone via ingest (sync path) deletes target and stores", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Ingest a regular blob
    auto blob = make_signed_blob(id, "sync-delete-target");
    auto ingest_result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(ingest_result.accepted);
    auto target_hash = ingest_result.ack->blob_hash;

    // Verify it exists
    auto found_before = engine.get_blob(id.namespace_id(), target_hash);
    REQUIRE(found_before.has_value());

    // Create a tombstone and INGEST it (simulating sync receive)
    auto tombstone = make_signed_tombstone(id, target_hash);
    auto tombstone_ingest = run_async(pool, engine.ingest(ns_span(id), tombstone));
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

TEST_CASE("delete_blob rejects unknown signer_hint", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto attacker = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, attacker);

    auto blob = make_signed_blob(owner, "owned-blob");
    auto ingest_result = run_async(pool, engine.ingest(ns_span(owner), blob));
    REQUIRE(ingest_result.accepted);

    // Attacker signs a tombstone; attacker's signer_hint does not resolve to
    // a registered owner_pubkey for owner's namespace, and no delegation exists.
    auto tombstone = make_signed_tombstone(attacker, ingest_result.ack->blob_hash);

    auto delete_result = run_async(pool, engine.delete_blob(ns_span(owner), tombstone));
    REQUIRE_FALSE(delete_result.accepted);
    REQUIRE(delete_result.error.value() == IngestError::no_delegation);
}

TEST_CASE("delete_blob validates signature", "[engine][tombstone]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    std::array<uint8_t, 32> target{};
    target.fill(0xCC);
    auto tombstone = make_signed_tombstone(id, target);

    // Corrupt signature
    tombstone.signature[0] ^= 0xFF;

    auto result = run_async(pool, engine.delete_blob(ns_span(id), tombstone));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::invalid_signature);
}

TEST_CASE("Tombstone survives expiry scan", "[engine][tombstone]") {
    TempDir tmp;
    uint64_t fake_now = 100000;
    Storage store(tmp.path.string(), [&]() { return fake_now; });
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Create and delete a blob
    auto blob = make_signed_blob(id, "expiry-test");
    auto ingest_result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(ingest_result.accepted);

    auto tombstone = make_signed_tombstone(id, ingest_result.ack->blob_hash);
    auto delete_result = run_async(pool, engine.delete_blob(ns_span(id), tombstone));
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    auto deleg = make_signed_delegation(owner, delegate);
    auto result = run_async(pool, engine.ingest(ns_span(owner), deleg));
    REQUIRE(result.accepted);
    REQUIRE(result.ack.has_value());
    REQUIRE(result.ack->status == IngestStatus::stored);
}

TEST_CASE("Delegation blob: has_valid_delegation returns true after ingest", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    auto deleg = make_signed_delegation(owner, delegate);
    auto result = run_async(pool, engine.ingest(ns_span(owner), deleg));
    REQUIRE(result.accepted);

    REQUIRE(store.has_valid_delegation(
        owner.namespace_id(),
        delegate.public_key()));
}

TEST_CASE("Delegation blob duplicate returns IngestStatus::duplicate", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    auto deleg = make_signed_delegation(owner, delegate);
    auto first = run_async(pool, engine.ingest(ns_span(owner), deleg));
    auto second = run_async(pool, engine.ingest(ns_span(owner), deleg));

    REQUIRE(first.accepted);
    REQUIRE(second.accepted);
    REQUIRE(second.ack->status == IngestStatus::duplicate);
}

TEST_CASE("Delegation blob from attacker rejected for owner's namespace", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto attacker = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, attacker);
    chromatindb::test::register_pubk(store, delegate);

    // Attacker builds a delegation for delegate, but re-signs for owner's ns.
    auto deleg = make_signed_delegation(attacker, delegate);
    auto signing_input = chromatindb::wire::build_signing_input(
        owner.namespace_id(), deleg.data, deleg.ttl, deleg.timestamp);
    deleg.signature = attacker.sign(signing_input);

    auto result = run_async(pool, engine.ingest(ns_span(owner), deleg));
    REQUIRE_FALSE(result.accepted);
    // Attacker's signer_hint does not own owner's ns; no delegation exists.
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("Delegation blob with bad signature rejected", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    auto deleg = make_signed_delegation(owner, delegate);
    deleg.signature[0] ^= 0xFF;

    auto result = run_async(pool, engine.ingest(ns_span(owner), deleg));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::invalid_signature);
}

TEST_CASE("DELEG-03: delegation blob retrievable via get_blob (sync compatible)", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    auto deleg = make_signed_delegation(owner, delegate);
    auto result = run_async(pool, engine.ingest(ns_span(owner), deleg));
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    // Owner creates delegation
    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), deleg)).accepted);

    // Delegate writes to owner's namespace
    auto blob = make_delegate_blob(owner, delegate, "delegate-write-1");
    auto result = run_async(pool, engine.ingest(ns_span(owner), blob));
    REQUIRE(result.accepted);
    REQUIRE(result.ack->status == IngestStatus::stored);
}

TEST_CASE("Delegate write rejected when no delegation exists", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    // No delegation -- delegate tries to write
    auto blob = make_delegate_blob(owner, delegate, "no-delegation");
    auto result = run_async(pool, engine.ingest(ns_span(owner), blob));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("Delegate cannot delete (delete_blob is owner-only)", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), deleg)).accepted);

    auto blob = make_signed_blob(owner, "owner-blob");
    auto ingest_result = run_async(pool, engine.ingest(ns_span(owner), blob));
    REQUIRE(ingest_result.accepted);

    chromatindb::wire::BlobData del_req;
    auto delegate_hint = chromatindb::crypto::sha3_256(delegate.public_key());
    std::memcpy(del_req.signer_hint.data(), delegate_hint.data(), 32);
    del_req.data = chromatindb::wire::make_tombstone_data(ingest_result.ack->blob_hash);
    del_req.ttl = 0;
    del_req.timestamp = current_timestamp();

    auto signing_input = chromatindb::wire::build_signing_input(
        owner.namespace_id(), del_req.data, del_req.ttl, del_req.timestamp);
    del_req.signature = delegate.sign(signing_input);

    auto result = run_async(pool, engine.delete_blob(ns_span(owner), del_req));
    REQUIRE_FALSE(result.accepted);
    // Delegate-signed tombstones rejected (delegate can ingest but not tombstone).
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("Delegate cannot create delegation blobs", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    auto other = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);
    chromatindb::test::register_pubk(store, other);

    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), deleg)).accepted);

    chromatindb::wire::BlobData fake_deleg;
    auto delegate_hint = chromatindb::crypto::sha3_256(delegate.public_key());
    std::memcpy(fake_deleg.signer_hint.data(), delegate_hint.data(), 32);
    fake_deleg.data = chromatindb::wire::make_delegation_data(other.public_key());
    fake_deleg.ttl = 0;
    fake_deleg.timestamp = current_timestamp();

    auto signing_input = chromatindb::wire::build_signing_input(
        owner.namespace_id(), fake_deleg.data, fake_deleg.ttl, fake_deleg.timestamp);
    fake_deleg.signature = delegate.sign(signing_input);

    auto result = run_async(pool, engine.ingest(ns_span(owner), fake_deleg));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("Delegate cannot create tombstone blobs via ingest", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), deleg)).accepted);

    std::array<uint8_t, 32> fake_target{};
    fake_target.fill(0xAA);

    chromatindb::wire::BlobData tomb;
    auto delegate_hint = chromatindb::crypto::sha3_256(delegate.public_key());
    std::memcpy(tomb.signer_hint.data(), delegate_hint.data(), 32);
    tomb.data = chromatindb::wire::make_tombstone_data(fake_target);
    tomb.ttl = 0;
    tomb.timestamp = current_timestamp();

    auto signing_input = chromatindb::wire::build_signing_input(
        owner.namespace_id(), tomb.data, tomb.ttl, tomb.timestamp);
    tomb.signature = delegate.sign(signing_input);

    auto result = run_async(pool, engine.ingest(ns_span(owner), tomb));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("Revocation via tombstone blocks delegate writes", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    // Owner creates delegation
    auto deleg = make_signed_delegation(owner, delegate);
    auto deleg_result = run_async(pool, engine.ingest(ns_span(owner), deleg));
    REQUIRE(deleg_result.accepted);

    // Delegate can write
    auto blob1 = make_delegate_blob(owner, delegate, "before-revocation");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob1)).accepted);

    // Owner revokes by tombstoning the delegation blob
    auto tombstone = make_signed_tombstone(owner, deleg_result.ack->blob_hash);
    auto del_result = run_async(pool, engine.delete_blob(ns_span(owner), tombstone));
    REQUIRE(del_result.accepted);

    // Delegate writes should now be rejected
    auto blob2 = make_delegate_blob(owner, delegate, "after-revocation");
    auto result = run_async(pool, engine.ingest(ns_span(owner), blob2));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == IngestError::no_delegation);
}

TEST_CASE("Re-delegation after revocation allows writes again", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    // 1. Delegate
    auto deleg1 = make_signed_delegation(owner, delegate);
    auto deleg1_result = run_async(pool, engine.ingest(ns_span(owner), deleg1));
    REQUIRE(deleg1_result.accepted);

    // 2. Delegate writes
    auto blob1 = make_delegate_blob(owner, delegate, "round-1");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob1)).accepted);

    // 3. Revoke
    auto tombstone = make_signed_tombstone(owner, deleg1_result.ack->blob_hash);
    REQUIRE(run_async(pool, engine.delete_blob(ns_span(owner), tombstone)).accepted);

    // 4. Delegate blocked
    auto blob2 = make_delegate_blob(owner, delegate, "blocked");
    REQUIRE_FALSE(run_async(pool, engine.ingest(ns_span(owner), blob2)).accepted);

    // 5. Re-delegate (new timestamp -> new blob hash -> not blocked by old tombstone)
    auto deleg2 = make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), deleg2)).accepted);

    // 6. Delegate can write again
    auto blob3 = make_delegate_blob(owner, delegate, "round-2");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob3)).accepted);
}

TEST_CASE("Delegate-written blobs survive revocation", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    // Delegate, write, revoke
    auto deleg = make_signed_delegation(owner, delegate);
    auto deleg_result = run_async(pool, engine.ingest(ns_span(owner), deleg));
    REQUIRE(deleg_result.accepted);

    auto blob = make_delegate_blob(owner, delegate, "persists-after-revoke");
    auto blob_result = run_async(pool, engine.ingest(ns_span(owner), blob));
    REQUIRE(blob_result.accepted);

    auto tombstone = make_signed_tombstone(owner, deleg_result.ack->blob_hash);
    REQUIRE(run_async(pool, engine.delete_blob(ns_span(owner), tombstone)).accepted);

    // Delegate's blob should still be retrievable
    auto found = engine.get_blob(owner.namespace_id(), blob_result.ack->blob_hash);
    REQUIRE(found.has_value());
    REQUIRE(found->data == blob.data);
}

TEST_CASE("Owner can still write after creating delegation", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    // Owner creates delegation
    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), deleg)).accepted);

    // Owner writes a regular blob -- should still work via ownership check
    auto blob = make_signed_blob(owner, "owner-still-writes");
    auto result = run_async(pool, engine.ingest(ns_span(owner), blob));
    REQUIRE(result.accepted);
}

TEST_CASE("Multiple delegates: independent write and revocation", "[engine][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate1 = chromatindb::identity::NodeIdentity::generate();
    auto delegate2 = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate1);
    chromatindb::test::register_pubk(store, delegate2);

    // Delegate both
    auto deleg1 = make_signed_delegation(owner, delegate1);
    auto deleg2 = make_signed_delegation(owner, delegate2);
    auto deleg1_result = run_async(pool, engine.ingest(ns_span(owner), deleg1));
    auto deleg2_result = run_async(pool, engine.ingest(ns_span(owner), deleg2));
    REQUIRE(deleg1_result.accepted);
    REQUIRE(deleg2_result.accepted);

    // Both can write
    auto blob1 = make_delegate_blob(owner, delegate1, "d1-writes");
    auto blob2 = make_delegate_blob(owner, delegate2, "d2-writes");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob1)).accepted);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob2)).accepted);

    // Revoke delegate1 only
    auto tombstone = make_signed_tombstone(owner, deleg1_result.ack->blob_hash);
    REQUIRE(run_async(pool, engine.delete_blob(ns_span(owner), tombstone)).accepted);

    // delegate1 blocked, delegate2 still works
    auto blob3 = make_delegate_blob(owner, delegate1, "d1-blocked");
    REQUIRE_FALSE(run_async(pool, engine.ingest(ns_span(owner), blob3)).accepted);

    auto blob4 = make_delegate_blob(owner, delegate2, "d2-still-works");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob4)).accepted);
}

// ============================================================================
// Plan 16-02: BlobEngine storage capacity enforcement
// ============================================================================

TEST_CASE("BlobEngine rejects ingest when over capacity", "[engine][capacity]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Set max_storage_bytes = 1 byte -- any real database exceeds this immediately
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 1);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
    auto blob = make_signed_blob(id, "capacity-test");

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::storage_full);
    REQUIRE(result.error_detail == "storage capacity exceeded");
}

TEST_CASE("BlobEngine tombstone exempt from capacity check", "[engine][capacity]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Set max_storage_bytes = 1 byte -- database already exceeds this
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 1);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Create a tombstone targeting a random hash
    std::array<uint8_t, 32> random_hash{};
    random_hash.fill(0xAB);
    auto tombstone = make_signed_tombstone(id, random_hash);

    auto result = run_async(pool, engine.ingest(ns_span(id), tombstone));
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
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
    auto blob = make_signed_blob(id, "unlimited-test");

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(result.accepted);
}

TEST_CASE("BlobEngine delete_blob works when over capacity", "[engine][capacity]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Ingest a blob first (with generous limit), then lower limit and delete
    asio::thread_pool pool{1};
    BlobEngine unlimited_engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
    auto blob = make_signed_blob(id, "to-be-deleted");
    auto ingest_result = run_async(pool, unlimited_engine.ingest(ns_span(id), blob));
    REQUIRE(ingest_result.accepted);

    // Now create a capacity-limited engine and delete via tombstone
    BlobEngine limited_engine(store, pool, 1);
    auto tombstone = make_signed_tombstone(id, ingest_result.ack->blob_hash);
    auto result = run_async(pool, limited_engine.delete_blob(ns_span(id), tombstone));
    // delete_blob has no capacity check (it creates tombstones, which are inherently exempt)
    REQUIRE(result.accepted);
}

TEST_CASE("BlobEngine ingest succeeds when under capacity", "[engine][capacity]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Set generous limit: 1 GiB
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 1ULL << 30);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
    auto blob = make_signed_blob(id, "under-capacity-test");

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(result.accepted);
}

// ============================================================================
// Phase 35 Plan 02: BlobEngine namespace quota enforcement
// ============================================================================

TEST_CASE("BlobEngine rejects ingest when namespace byte quota exceeded", "[engine][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Set byte quota to allow exactly one blob (a signed blob with ML-DSA-87 is ~7400 bytes encoded)
    // Use 8000 bytes: enough for one blob, too small for two
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 0, 8000, 0);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // First ingest succeeds (under byte quota)
    auto blob1 = make_signed_blob(id, "fill-quota");
    auto r1 = run_async(pool, engine.ingest(ns_span(id), blob1));
    REQUIRE(r1.accepted);

    // Second blob should be rejected for byte quota
    auto blob2 = make_signed_blob(id, "over-quota");
    auto r2 = run_async(pool, engine.ingest(ns_span(id), blob2));
    REQUIRE_FALSE(r2.accepted);
    REQUIRE(r2.error.has_value());
    REQUIRE(r2.error.value() == IngestError::quota_exceeded);
}

TEST_CASE("BlobEngine rejects ingest when namespace count quota exceeded", "[engine][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Set a count quota of 1 blob per namespace
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 0, 0, 1);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // First blob succeeds (count 0 + 1 <= 1)
    auto blob1 = make_signed_blob(id, "first-blob");
    auto r1 = run_async(pool, engine.ingest(ns_span(id), blob1));
    REQUIRE(r1.accepted);

    // Second blob should be rejected for count quota
    auto blob2 = make_signed_blob(id, "second-blob");
    auto r2 = run_async(pool, engine.ingest(ns_span(id), blob2));
    REQUIRE_FALSE(r2.accepted);
    REQUIRE(r2.error.has_value());
    REQUIRE(r2.error.value() == IngestError::quota_exceeded);
}

TEST_CASE("BlobEngine allows ingest when under quota", "[engine][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Generous quota: 100 MiB bytes, 1000 count
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 0, 100ULL * 1024 * 1024, 1000);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
    auto blob = make_signed_blob(id, "under-quota");
    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(result.accepted);
}

TEST_CASE("BlobEngine tombstone exempt from quota check", "[engine][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Set count quota of 1 -- fill it, then tombstone should still work
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 0, 0, 1);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Fill the count quota
    auto blob = make_signed_blob(id, "fill-count-quota");
    auto r1 = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(r1.accepted);

    // Tombstone should bypass quota check
    auto tombstone = make_signed_tombstone(id, r1.ack->blob_hash);
    auto r2 = run_async(pool, engine.ingest(ns_span(id), tombstone));
    // Tombstone must NOT be rejected with quota_exceeded
    if (!r2.accepted) {
        REQUIRE(r2.error.value() != IngestError::quota_exceeded);
    }
}

TEST_CASE("BlobEngine per-namespace override supersedes global quota", "[engine][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Global quota: 1 byte -- would reject everything
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 0, 1, 0);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Set per-namespace override to allow much more
    auto ns_hex = to_hex(std::span<const uint8_t, 32>(id.namespace_id()));
    std::map<std::string, std::pair<std::optional<uint64_t>, std::optional<uint64_t>>> overrides;
    overrides[ns_hex] = {100ULL * 1024 * 1024, std::nullopt};  // 100 MiB override
    engine.set_quota_config(1, 0, overrides);

    // First blob should succeed (override allows it)
    auto blob1 = make_signed_blob(id, "override-allowed-1");
    auto r1 = run_async(pool, engine.ingest(ns_span(id), blob1));
    REQUIRE(r1.accepted);

    // Second blob should also succeed
    auto blob2 = make_signed_blob(id, "override-allowed-2");
    auto r2 = run_async(pool, engine.ingest(ns_span(id), blob2));
    REQUIRE(r2.accepted);
}

TEST_CASE("BlobEngine zero override exempts namespace from global byte quota", "[engine][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Global byte quota: 1 byte (restrictive)
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 0, 1, 0);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Set override with max_bytes=0 (exempt from byte quota)
    auto ns_hex = to_hex(std::span<const uint8_t, 32>(id.namespace_id()));
    std::map<std::string, std::pair<std::optional<uint64_t>, std::optional<uint64_t>>> overrides;
    overrides[ns_hex] = {uint64_t(0), std::nullopt};  // 0 = unlimited bytes
    engine.set_quota_config(1, 0, overrides);

    // First blob should work
    auto blob1 = make_signed_blob(id, "exempt-blob-1");
    auto r1 = run_async(pool, engine.ingest(ns_span(id), blob1));
    REQUIRE(r1.accepted);

    // Second blob should also work (exempt namespace)
    auto blob2 = make_signed_blob(id, "exempt-blob-2");
    auto r2 = run_async(pool, engine.ingest(ns_span(id), blob2));
    REQUIRE(r2.accepted);
}

TEST_CASE("BlobEngine zero override exempts namespace from global count quota", "[engine][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Global count quota: 1 blob (restrictive)
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 0, 0, 1);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Set override with max_count=0 (exempt from count quota)
    auto ns_hex = to_hex(std::span<const uint8_t, 32>(id.namespace_id()));
    std::map<std::string, std::pair<std::optional<uint64_t>, std::optional<uint64_t>>> overrides;
    overrides[ns_hex] = {std::nullopt, uint64_t(0)};  // 0 = unlimited count
    engine.set_quota_config(0, 1, overrides);

    // First blob succeeds
    auto blob1 = make_signed_blob(id, "exempt-count-1");
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), blob1)).accepted);

    // Second blob also succeeds (exempt from count quota)
    auto blob2 = make_signed_blob(id, "exempt-count-2");
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), blob2)).accepted);
}

TEST_CASE("BlobEngine unlimited quota (0) allows all blobs", "[engine][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Both quotas set to 0 (unlimited) -- default behavior
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 0, 0, 0);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    for (int i = 0; i < 5; ++i) {
        auto blob = make_signed_blob(id, "unlimited-" + std::to_string(i));
        REQUIRE(run_async(pool, engine.ingest(ns_span(id), blob)).accepted);
    }
}

TEST_CASE("BlobEngine set_quota_config updates limits", "[engine][quota]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    // Start with no quota
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool, 0, 0, 0);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // First blob succeeds (no quota)
    auto blob1 = make_signed_blob(id, "before-quota");
    REQUIRE(run_async(pool, engine.ingest(ns_span(id), blob1)).accepted);

    // Now set a count quota of 1 via set_quota_config (simulating SIGHUP)
    std::map<std::string, std::pair<std::optional<uint64_t>, std::optional<uint64_t>>> empty_overrides;
    engine.set_quota_config(0, 1, empty_overrides);

    // Second blob should be rejected (count quota = 1, already have 1)
    auto blob2 = make_signed_blob(id, "after-quota");
    auto r2 = run_async(pool, engine.ingest(ns_span(id), blob2));
    REQUIRE_FALSE(r2.accepted);
    REQUIRE(r2.error.value() == IngestError::quota_exceeded);
}

// ============================================================================
// Phase 45 Plan 01: Delegation quota enforcement verification (STOR-05)
// ============================================================================

TEST_CASE("Delegate write counts against owner namespace quota", "[engine][quota][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Count quota of 3: delegation blob (1) + delegate blob (2) + owner blob (3) = full
    BlobEngine engine(store, pool, 0, 0, 3);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    // Owner creates delegation (counts as blob #1 in owner's namespace)
    auto deleg = make_signed_delegation(owner, delegate);
    auto r0 = run_async(pool, engine.ingest(ns_span(owner), deleg));
    REQUIRE(r0.accepted);

    // Delegate writes blob (counts as blob #2 in OWNER's namespace)
    auto blob1 = make_delegate_blob(owner, delegate, "delegate-data-1");
    auto r1 = run_async(pool, engine.ingest(ns_span(owner), blob1));
    REQUIRE(r1.accepted);

    // Verify quota usage on owner's namespace includes both blobs
    auto quota = store.get_namespace_quota(owner.namespace_id());
    REQUIRE(quota.blob_count == 2);  // delegation blob + delegate's blob
}

TEST_CASE("Owner at count quota rejects delegate write", "[engine][quota][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Count quota of 3
    BlobEngine engine(store, pool, 0, 0, 3);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    // Owner creates delegation (blob #1)
    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), deleg)).accepted);

    // Delegate writes blob (blob #2)
    auto blob1 = make_delegate_blob(owner, delegate, "d1");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob1)).accepted);

    // Owner writes blob (blob #3 -- at quota)
    auto blob2 = make_signed_blob(owner, "o1");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob2)).accepted);

    // Delegate's 2nd write should be rejected (quota full)
    auto blob3 = make_delegate_blob(owner, delegate, "d2");
    auto r3 = run_async(pool, engine.ingest(ns_span(owner), blob3));
    REQUIRE_FALSE(r3.accepted);
    REQUIRE(r3.error.has_value());
    REQUIRE(r3.error.value() == IngestError::quota_exceeded);
}

TEST_CASE("Mixed owner and delegate writes fill quota", "[engine][quota][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Count quota of 4: delegation (1) + owner (2) + delegate (3) + owner (4) = full
    BlobEngine engine(store, pool, 0, 0, 4);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    // Delegation blob (#1)
    auto deleg = make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), deleg)).accepted);

    // Owner writes (#2)
    auto blob1 = make_signed_blob(owner, "owner-1");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob1)).accepted);

    // Delegate writes (#3)
    auto blob2 = make_delegate_blob(owner, delegate, "delegate-1");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob2)).accepted);

    // Owner writes (#4 -- fills quota)
    auto blob3 = make_signed_blob(owner, "owner-2");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob3)).accepted);

    // Next write from owner should be rejected
    auto blob4 = make_signed_blob(owner, "owner-3");
    auto r4 = run_async(pool, engine.ingest(ns_span(owner), blob4));
    REQUIRE_FALSE(r4.accepted);
    REQUIRE(r4.error.value() == IngestError::quota_exceeded);

    // Next write from delegate should also be rejected
    auto blob5 = make_delegate_blob(owner, delegate, "delegate-2");
    auto r5 = run_async(pool, engine.ingest(ns_span(owner), blob5));
    REQUIRE_FALSE(r5.accepted);
    REQUIRE(r5.error.value() == IngestError::quota_exceeded);
}

TEST_CASE("Multiple delegates all count against owner quota", "[engine][quota][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate1 = chromatindb::identity::NodeIdentity::generate();
    auto delegate2 = chromatindb::identity::NodeIdentity::generate();

    // Count quota of 5: 2 delegation blobs + 3 data blobs = full
    BlobEngine engine(store, pool, 0, 0, 5);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate1);
    chromatindb::test::register_pubk(store, delegate2);

    // Delegate both (2 delegation blobs)
    auto deleg1 = make_signed_delegation(owner, delegate1);
    auto deleg2 = make_signed_delegation(owner, delegate2);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), deleg1)).accepted);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), deleg2)).accepted);

    // Each delegate writes one blob (2 data blobs = 4 total)
    auto blob1 = make_delegate_blob(owner, delegate1, "d1-writes");
    auto blob2 = make_delegate_blob(owner, delegate2, "d2-writes");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob1)).accepted);
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob2)).accepted);

    // Owner writes one more (blob #5 = at quota)
    auto blob3 = make_signed_blob(owner, "owner-writes");
    REQUIRE(run_async(pool, engine.ingest(ns_span(owner), blob3)).accepted);

    // Any further write from either delegate should be rejected
    auto blob4 = make_delegate_blob(owner, delegate1, "d1-over");
    auto r4 = run_async(pool, engine.ingest(ns_span(owner), blob4));
    REQUIRE_FALSE(r4.accepted);
    REQUIRE(r4.error.value() == IngestError::quota_exceeded);

    auto blob5 = make_delegate_blob(owner, delegate2, "d2-over");
    auto r5 = run_async(pool, engine.ingest(ns_span(owner), blob5));
    REQUIRE_FALSE(r5.accepted);
    REQUIRE(r5.error.value() == IngestError::quota_exceeded);
}

TEST_CASE("Delegate blob bytes count against owner byte quota", "[engine][quota][delegation]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Set byte quota: allow exactly 2 blobs worth of bytes.
    // A signed blob with ML-DSA-87 is ~7400 bytes encoded. Use 16000 bytes
    // to allow delegation blob + one data blob, but reject a second data blob.
    BlobEngine engine(store, pool, 0, 16000, 0);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate);

    // Owner creates delegation (uses ~7400 bytes of owner's byte quota)
    auto deleg = make_signed_delegation(owner, delegate);
    auto r0 = run_async(pool, engine.ingest(ns_span(owner), deleg));
    REQUIRE(r0.accepted);

    // Delegate writes one blob (uses another ~7400 bytes -- still under 20000)
    auto blob1 = make_delegate_blob(owner, delegate, "delegate-byte-1");
    auto r1 = run_async(pool, engine.ingest(ns_span(owner), blob1));
    REQUIRE(r1.accepted);

    // Third blob should exceed byte quota (3 * ~7400 > 20000)
    auto blob2 = make_delegate_blob(owner, delegate, "delegate-byte-2");
    auto r2 = run_async(pool, engine.ingest(ns_span(owner), blob2));
    REQUIRE_FALSE(r2.accepted);
    REQUIRE(r2.error.has_value());
    REQUIRE(r2.error.value() == IngestError::quota_exceeded);
}

// =============================================================================
// Phase 54: Timestamp validation tests (OPS-02)
// =============================================================================

TEST_CASE("Blob with timestamp 0 rejected as too far in past", "[engine][timestamp]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto id = chromatindb::identity::NodeIdentity::generate();

    BlobEngine engine(store, pool);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Timestamp 0 (Unix epoch) is way past 30 days
    auto blob = make_signed_blob(id, "timestamp-zero", 604800, 0);
    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::timestamp_rejected);
    REQUIRE(result.error_detail.find("past") != std::string::npos);
}

TEST_CASE("Blob with timestamp 1hr+1s in future rejected", "[engine][timestamp]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto id = chromatindb::identity::NodeIdentity::generate();

    BlobEngine engine(store, pool);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    auto now_ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    auto blob = make_signed_blob(id, "future-blob", 604800, now_ts + 3601);
    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::timestamp_rejected);
    REQUIRE(result.error_detail.find("future") != std::string::npos);
}

TEST_CASE("Blob with timestamp exactly now passes timestamp check", "[engine][timestamp]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto id = chromatindb::identity::NodeIdentity::generate();

    BlobEngine engine(store, pool);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    auto now_ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Blob with current timestamp -- should pass timestamp check, then succeed
    // because make_signed_blob produces a validly signed blob.
    auto blob = make_signed_blob(id, "now-blob", 604800, now_ts);
    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    // Should NOT be timestamp_rejected (it passes timestamp check).
    // It should either be accepted or fail for a later reason -- but NOT timestamp.
    if (!result.accepted) {
        REQUIRE(result.error.value() != IngestError::timestamp_rejected);
    }
}

TEST_CASE("Blob with timestamp 29 days in past passes timestamp check", "[engine][timestamp]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto id = chromatindb::identity::NodeIdentity::generate();

    BlobEngine engine(store, pool);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    auto now_ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Use TTL long enough that the blob is NOT yet expired (29d old + 30d TTL = expires tomorrow)
    auto blob = make_signed_blob(id, "29-days-ago", 30 * 24 * 3600, now_ts - 29 * 24 * 3600);
    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    // Should NOT be timestamp_rejected (within 30-day window and not yet expired)
    if (!result.accepted) {
        REQUIRE(result.error.value() != IngestError::timestamp_rejected);
    }
}

TEST_CASE("Blob with timestamp 31 days in past rejected", "[engine][timestamp]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto id = chromatindb::identity::NodeIdentity::generate();

    BlobEngine engine(store, pool);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    auto now_ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    auto blob = make_signed_blob(id, "31-days-ago", 604800, now_ts - 31 * 24 * 3600);
    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::timestamp_rejected);
    REQUIRE(result.error_detail.find("past") != std::string::npos);
}

TEST_CASE("Blob with timestamp 59 minutes in future passes timestamp check", "[engine][timestamp]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto id = chromatindb::identity::NodeIdentity::generate();

    BlobEngine engine(store, pool);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    auto now_ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    auto blob = make_signed_blob(id, "59-min-future", 604800, now_ts + 59 * 60);
    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    // Should NOT be timestamp_rejected
    if (!result.accepted) {
        REQUIRE(result.error.value() != IngestError::timestamp_rejected);
    }
}

TEST_CASE("Delete request with timestamp too far in future rejected", "[engine][timestamp]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto id = chromatindb::identity::NodeIdentity::generate();

    BlobEngine engine(store, pool);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    auto now_ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // First store a blob so we have a target hash
    auto blob = make_signed_blob(id, "to-delete-future", 604800, now_ts);
    auto r0 = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(r0.accepted);

    // Create tombstone with timestamp far in future
    auto tombstone = make_signed_tombstone(id, r0.ack->blob_hash, now_ts + 3601);
    auto result = run_async(pool, engine.delete_blob(ns_span(id), tombstone));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::timestamp_rejected);
    REQUIRE(result.error_detail.find("future") != std::string::npos);
}

TEST_CASE("Delete request with timestamp too far in past rejected", "[engine][timestamp]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    auto id = chromatindb::identity::NodeIdentity::generate();

    BlobEngine engine(store, pool);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    auto now_ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // First store a blob so we have a target hash
    auto blob = make_signed_blob(id, "to-delete-past", 604800, now_ts);
    auto r0 = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(r0.accepted);

    // Create tombstone with timestamp far in past
    auto tombstone = make_signed_tombstone(id, r0.ack->blob_hash, 0);
    auto result = run_async(pool, engine.delete_blob(ns_span(id), tombstone));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::timestamp_rejected);
    REQUIRE(result.error_detail.find("past") != std::string::npos);
}

// ============================================================================
// TTL enforcement (Phase 98-01 Task 2)
// ============================================================================

TEST_CASE("Tombstone with TTL > 0 rejected at ingest", "[engine][ttl]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // First ingest a valid blob to get a target hash for the tombstone
    auto blob = make_signed_blob(id, "target-blob");
    auto ingest_result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(ingest_result.accepted);
    auto target_hash = ingest_result.ack->blob_hash;

    // Manually construct a tombstone-format blob with non-zero TTL
    chromatindb::wire::BlobData bad_tombstone;
    auto _bad_tombstone_hint = chromatindb::crypto::sha3_256(id.public_key());
        std::memcpy(bad_tombstone.signer_hint.data(), _bad_tombstone_hint.data(), 32);
    bad_tombstone.data = chromatindb::wire::make_tombstone_data(target_hash);
    bad_tombstone.ttl = 3600;  // Non-zero TTL -- invalid for tombstone
    bad_tombstone.timestamp = current_timestamp();

    auto signing_input = chromatindb::wire::build_signing_input(
        id.namespace_id(), bad_tombstone.data,
        bad_tombstone.ttl, bad_tombstone.timestamp);
    bad_tombstone.signature = id.sign(signing_input);

    auto result = run_async(pool, engine.delete_blob(ns_span(id), bad_tombstone));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::invalid_ttl);
    REQUIRE(result.error_detail.find("tombstone must have TTL=0") != std::string::npos);
}

TEST_CASE("Already-expired blob rejected at ingest", "[engine][ttl]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Create a blob with timestamp=now-1000, ttl=100 => expired 900 seconds ago
    uint64_t now = current_timestamp();
    auto blob = make_signed_blob(id, "expired-data", 100, now - 1000);

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::timestamp_rejected);
    REQUIRE(result.error_detail.find("blob already expired") != std::string::npos);
}

TEST_CASE("Valid non-expired blob accepted at ingest", "[engine][ttl]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    auto blob = make_signed_blob(id, "valid-data", 86400, current_timestamp());

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(result.accepted);
}

TEST_CASE("Tombstone with TTL=0 accepted at delete_blob", "[engine][ttl]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Ingest a blob, then delete with proper tombstone (ttl=0)
    auto blob = make_signed_blob(id, "to-delete");
    auto ingest_result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(ingest_result.accepted);

    auto tombstone = make_signed_tombstone(id, ingest_result.ack->blob_hash);
    auto result = run_async(pool, engine.delete_blob(ns_span(id), tombstone));
    REQUIRE(result.accepted);
}

// ============================================================================
// Phase 99 Plan 02: Atomic capacity/quota check via store_blob (RES-03)
// ============================================================================

TEST_CASE("BlobEngine ingest rejects at capacity via store_blob", "[engine][resource]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    // Set max_storage_bytes=1 so any blob exceeds capacity
    BlobEngine engine(store, pool, 1);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);
    auto blob = make_signed_blob(id, "capacity-atomic-test");

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error.value() == IngestError::storage_full);
}

TEST_CASE("Permanent blob accepted regardless of timestamp age", "[engine][ttl]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    // Permanent blob (ttl=0) with recent timestamp should always be accepted
    auto blob = make_signed_blob(id, "permanent-data", 0, current_timestamp());

    auto result = run_async(pool, engine.ingest(ns_span(id), blob));
    REQUIRE(result.accepted);
}
