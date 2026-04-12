#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "db/engine/chunking.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/tests/test_helpers.h"
#include "db/util/endian.h"
#include "db/util/hex.h"
#include "db/wire/codec.h"

using chromatindb::engine::BlobEngine;
using chromatindb::engine::CHUNK_SIZE;
using chromatindb::engine::ChunkedReadResult;
using chromatindb::engine::IngestStatus;
using chromatindb::engine::MANIFEST_MAGIC;
using chromatindb::engine::MAX_CHUNK_COUNT;
using chromatindb::engine::SignFn;
using chromatindb::engine::is_manifest;
using chromatindb::engine::make_manifest_data;
using chromatindb::engine::parse_manifest;

using chromatindb::storage::PrecomputedBlob;
using chromatindb::storage::Storage;
using chromatindb::storage::StoreResult;
using chromatindb::test::TempDir;
using chromatindb::test::current_timestamp;
using chromatindb::test::make_signed_blob;
using chromatindb::test::run_async;

// ============================================================================
// Manifest format tests
// ============================================================================

TEST_CASE("make_manifest_data with 3 hashes produces correct binary layout", "[chunking]") {
    std::array<uint8_t, 32> h1{}, h2{}, h3{};
    h1.fill(0x11);
    h2.fill(0x22);
    h3.fill(0x33);

    auto data = make_manifest_data({h1, h2, h3});

    // Expected size: 4 (magic) + 4 (count) + 3*32 (hashes) = 104
    REQUIRE(data.size() == 104);

    // Check magic prefix
    REQUIRE(data[0] == 0x43);  // 'C'
    REQUIRE(data[1] == 0x48);  // 'H'
    REQUIRE(data[2] == 0x4E);  // 'N'
    REQUIRE(data[3] == 0x4B);  // 'K'

    // Check chunk_count = 3 (big-endian)
    uint32_t count = chromatindb::util::read_u32_be(
        std::span<const uint8_t>(data).subspan(4, 4));
    REQUIRE(count == 3);

    // Check hash positions
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(data[8 + i] == 0x11);       // hash 1
        REQUIRE(data[8 + 32 + i] == 0x22);  // hash 2
        REQUIRE(data[8 + 64 + i] == 0x33);  // hash 3
    }
}

TEST_CASE("make_manifest_data with 1 hash produces 40 bytes", "[chunking]") {
    std::array<uint8_t, 32> h{};
    h.fill(0xAA);

    auto data = make_manifest_data({h});
    REQUIRE(data.size() == 40);  // 8 header + 32 hash
}

TEST_CASE("parse_manifest round-trips with make_manifest_data", "[chunking]") {
    std::array<uint8_t, 32> h1{}, h2{}, h3{};
    h1.fill(0x01);
    h2.fill(0x02);
    h3.fill(0x03);

    auto data = make_manifest_data({h1, h2, h3});
    auto parsed = parse_manifest(data);

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 3);
    REQUIRE((*parsed)[0] == h1);
    REQUIRE((*parsed)[1] == h2);
    REQUIRE((*parsed)[2] == h3);
}

TEST_CASE("is_manifest returns true for valid manifest data", "[chunking]") {
    std::array<uint8_t, 32> h{};
    h.fill(0xFF);
    auto data = make_manifest_data({h});
    REQUIRE(is_manifest(data));
}

TEST_CASE("is_manifest returns false for tombstone data", "[chunking]") {
    // Tombstone: [0xDE 0xAD 0xBE 0xEF][target_hash:32]
    std::array<uint8_t, 32> target{};
    target.fill(0xCC);
    auto tombstone_data = chromatindb::wire::make_tombstone_data(target);
    REQUIRE_FALSE(is_manifest(tombstone_data));
}

TEST_CASE("is_manifest returns false for empty data", "[chunking]") {
    std::vector<uint8_t> empty;
    REQUIRE_FALSE(is_manifest(empty));
}

TEST_CASE("is_manifest returns false for too-short data", "[chunking]") {
    std::vector<uint8_t> short_data = {0x43, 0x48, 0x4E};  // only 3 bytes
    REQUIRE_FALSE(is_manifest(short_data));
}

TEST_CASE("parse_manifest returns nullopt for bad magic", "[chunking]") {
    std::vector<uint8_t> data(40, 0);  // 8 header + 32 hash size
    data[0] = 0xDE;  // wrong magic
    data[1] = 0xAD;
    data[2] = 0xBE;
    data[3] = 0xEF;
    // count = 1 (BE)
    data[4] = 0; data[5] = 0; data[6] = 0; data[7] = 1;
    REQUIRE_FALSE(parse_manifest(data).has_value());
}

TEST_CASE("parse_manifest returns nullopt for wrong size", "[chunking]") {
    // Make a valid header with count=2 but only provide space for 1 hash
    std::vector<uint8_t> data(40, 0);  // space for 1 hash, but count says 2
    data[0] = 0x43; data[1] = 0x48; data[2] = 0x4E; data[3] = 0x4B;
    data[4] = 0; data[5] = 0; data[6] = 0; data[7] = 2;  // count=2
    REQUIRE_FALSE(parse_manifest(data).has_value());
}

TEST_CASE("parse_manifest returns nullopt for zero chunk_count", "[chunking]") {
    // Just header with count=0
    std::vector<uint8_t> data = {0x43, 0x48, 0x4E, 0x4B, 0, 0, 0, 0};
    REQUIRE_FALSE(parse_manifest(data).has_value());
}

TEST_CASE("parse_manifest returns nullopt for count exceeding MAX_CHUNK_COUNT", "[chunking]") {
    // Build header with count = MAX_CHUNK_COUNT + 1 (but don't allocate full data)
    // Size would need to be 8 + (MAX_CHUNK_COUNT+1)*32, but we only test that
    // the count check triggers before size validation
    uint32_t over_limit = MAX_CHUNK_COUNT + 1;
    std::vector<uint8_t> data(8);
    data[0] = 0x43; data[1] = 0x48; data[2] = 0x4E; data[3] = 0x4B;
    chromatindb::util::store_u32_be(data.data() + 4, over_limit);
    REQUIRE_FALSE(parse_manifest(data).has_value());
}

// ============================================================================
// Atomic storage tests
// ============================================================================

TEST_CASE("store_blobs_atomic stores 3 blobs, all retrievable", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Build 3 pre-computed blobs
    std::vector<PrecomputedBlob> blobs;
    for (int i = 0; i < 3; ++i) {
        auto blob = make_signed_blob(id, "chunk-" + std::to_string(i));
        auto encoded = chromatindb::wire::encode_blob(blob);
        auto hash = chromatindb::wire::blob_hash(encoded);
        blobs.push_back({std::move(blob), hash, std::move(encoded)});
    }

    auto results = store.store_blobs_atomic(blobs);
    REQUIRE(results.size() == 3);

    for (size_t i = 0; i < 3; ++i) {
        REQUIRE(results[i].status == StoreResult::Status::Stored);
        REQUIRE(results[i].blob_hash == blobs[i].content_hash);
    }

    // Verify all retrievable
    for (const auto& b : blobs) {
        auto got = store.get_blob(b.blob.namespace_id, b.content_hash);
        REQUIRE(got.has_value());
        REQUIRE(got->data == b.blob.data);
    }
}

TEST_CASE("store_blobs_atomic returns correct blob_hash for each", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id = chromatindb::identity::NodeIdentity::generate();

    std::vector<PrecomputedBlob> blobs;
    for (int i = 0; i < 2; ++i) {
        auto blob = make_signed_blob(id, "data-" + std::to_string(i));
        auto encoded = chromatindb::wire::encode_blob(blob);
        auto hash = chromatindb::wire::blob_hash(encoded);
        blobs.push_back({std::move(blob), hash, std::move(encoded)});
    }

    auto results = store.store_blobs_atomic(blobs);
    REQUIRE(results.size() == 2);
    REQUIRE(results[0].blob_hash == blobs[0].content_hash);
    REQUIRE(results[1].blob_hash == blobs[1].content_hash);
}

TEST_CASE("store_blobs_atomic with duplicate blob returns Duplicate", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id = chromatindb::identity::NodeIdentity::generate();

    auto blob = make_signed_blob(id, "dup-test");
    auto encoded = chromatindb::wire::encode_blob(blob);
    auto hash = chromatindb::wire::blob_hash(encoded);

    // Store it first via regular store_blob
    auto first = store.store_blob(blob, hash, encoded);
    REQUIRE(first.status == StoreResult::Status::Stored);

    // Now attempt via atomic with the same blob
    std::vector<PrecomputedBlob> batch;
    batch.push_back({blob, hash, encoded});
    auto results = store.store_blobs_atomic(batch);

    REQUIRE(results.size() == 1);
    REQUIRE(results[0].status == StoreResult::Status::Duplicate);
}

TEST_CASE("store_blobs_atomic enforces capacity limit", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id = chromatindb::identity::NodeIdentity::generate();

    std::vector<PrecomputedBlob> blobs;
    for (int i = 0; i < 3; ++i) {
        auto blob = make_signed_blob(id, "cap-" + std::to_string(i));
        auto encoded = chromatindb::wire::encode_blob(blob);
        auto hash = chromatindb::wire::blob_hash(encoded);
        blobs.push_back({std::move(blob), hash, std::move(encoded)});
    }

    // Set very low capacity to trigger CapacityExceeded
    auto results = store.store_blobs_atomic(blobs, 1 /* 1 byte max */);
    REQUIRE(results.size() == 3);
    for (const auto& r : results) {
        REQUIRE(r.status == StoreResult::Status::CapacityExceeded);
    }
}

TEST_CASE("store_blobs_atomic enforces quota limit", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id = chromatindb::identity::NodeIdentity::generate();

    std::vector<PrecomputedBlob> blobs;
    for (int i = 0; i < 3; ++i) {
        auto blob = make_signed_blob(id, "quota-" + std::to_string(i));
        auto encoded = chromatindb::wire::encode_blob(blob);
        auto hash = chromatindb::wire::blob_hash(encoded);
        blobs.push_back({std::move(blob), hash, std::move(encoded)});
    }

    // Set very low quota to trigger QuotaExceeded
    auto results = store.store_blobs_atomic(blobs, 0, 1 /* 1 byte quota */);
    REQUIRE(results.size() == 3);
    for (const auto& r : results) {
        REQUIRE(r.status == StoreResult::Status::QuotaExceeded);
    }
}

TEST_CASE("store_blobs_atomic assigns sequential seq_nums", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id = chromatindb::identity::NodeIdentity::generate();

    std::vector<PrecomputedBlob> blobs;
    for (int i = 0; i < 3; ++i) {
        auto blob = make_signed_blob(id, "seq-" + std::to_string(i));
        auto encoded = chromatindb::wire::encode_blob(blob);
        auto hash = chromatindb::wire::blob_hash(encoded);
        blobs.push_back({std::move(blob), hash, std::move(encoded)});
    }

    auto results = store.store_blobs_atomic(blobs);
    REQUIRE(results.size() == 3);

    // All should be Stored with consecutive seq_nums
    REQUIRE(results[0].status == StoreResult::Status::Stored);
    REQUIRE(results[1].status == StoreResult::Status::Stored);
    REQUIRE(results[2].status == StoreResult::Status::Stored);

    REQUIRE(results[0].seq_num == 1);
    REQUIRE(results[1].seq_num == 2);
    REQUIRE(results[2].seq_num == 3);
}

TEST_CASE("store_blobs_atomic creates expiry entries for blobs with TTL", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());

    auto id = chromatindb::identity::NodeIdentity::generate();

    uint64_t now = current_timestamp();
    uint32_t ttl = 3600;  // 1 hour

    auto blob = make_signed_blob(id, "expiry-test", ttl, now);
    auto encoded = chromatindb::wire::encode_blob(blob);
    auto hash = chromatindb::wire::blob_hash(encoded);

    std::vector<PrecomputedBlob> batch;
    batch.push_back({blob, hash, encoded});

    auto results = store.store_blobs_atomic(batch);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].status == StoreResult::Status::Stored);

    // Verify expiry entry was created
    auto earliest = store.get_earliest_expiry();
    REQUIRE(earliest.has_value());
    REQUIRE(*earliest == now + ttl);
}

// ============================================================================
// Engine-level integration tests: store_chunked + read_chunked
// ============================================================================

namespace {

/// Helper: generate random data of given size.
std::vector<uint8_t> make_random_data(size_t size) {
    std::vector<uint8_t> data(size);
    std::mt19937 gen(42);  // Fixed seed for reproducibility
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& b : data) b = static_cast<uint8_t>(dist(gen));
    return data;
}

/// Helper: create a SignFn from a NodeIdentity.
SignFn make_sign_fn(const chromatindb::identity::NodeIdentity& id) {
    return [&id](std::span<const uint8_t> message) -> std::vector<uint8_t> {
        return id.sign(message);
    };
}

} // anonymous namespace

TEST_CASE("store_chunked splits 2.5 MiB into 3 chunks and read_chunked reassembles", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{2};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto data = make_random_data(CHUNK_SIZE * 2 + CHUNK_SIZE / 2);  // 2.5 MiB

    uint64_t ts = current_timestamp();
    uint32_t ttl = 3600;

    auto result = run_async(pool, engine.store_chunked(
        id.namespace_id(), id.public_key(), data, ttl, ts, make_sign_fn(id)));

    REQUIRE(result.accepted);
    REQUIRE(result.ack.has_value());
    auto manifest_hash = result.ack->blob_hash;

    // Read back
    auto read_result = engine.read_chunked(id.namespace_id(), manifest_hash);
    REQUIRE(read_result.success);
    REQUIRE(read_result.data == data);
    REQUIRE(read_result.chunks_expected == 3);
    REQUIRE(read_result.chunks_found == 3);
}

TEST_CASE("store_chunked with exactly 1 MiB creates 1 chunk + manifest", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{2};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto data = make_random_data(CHUNK_SIZE);  // Exactly 1 MiB

    uint64_t ts = current_timestamp();
    uint32_t ttl = 3600;

    auto result = run_async(pool, engine.store_chunked(
        id.namespace_id(), id.public_key(), data, ttl, ts, make_sign_fn(id)));

    REQUIRE(result.accepted);

    auto read_result = engine.read_chunked(id.namespace_id(), result.ack->blob_hash);
    REQUIRE(read_result.success);
    REQUIRE(read_result.data == data);
    REQUIRE(read_result.chunks_expected == 1);
    REQUIRE(read_result.chunks_found == 1);
}

TEST_CASE("store_chunked with CHUNK_SIZE + 1 bytes creates 2 chunks", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{2};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto data = make_random_data(CHUNK_SIZE + 1);

    uint64_t ts = current_timestamp();
    uint32_t ttl = 3600;

    auto result = run_async(pool, engine.store_chunked(
        id.namespace_id(), id.public_key(), data, ttl, ts, make_sign_fn(id)));

    REQUIRE(result.accepted);

    auto read_result = engine.read_chunked(id.namespace_id(), result.ack->blob_hash);
    REQUIRE(read_result.success);
    REQUIRE(read_result.data == data);
    REQUIRE(read_result.chunks_expected == 2);
    REQUIRE(read_result.chunks_found == 2);
}

TEST_CASE("read_chunked with non-existent manifest hash returns error", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{2};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    std::array<uint8_t, 32> fake_hash{};
    fake_hash.fill(0xAB);

    auto read_result = engine.read_chunked(id.namespace_id(), fake_hash);
    REQUIRE_FALSE(read_result.success);
    REQUIRE(read_result.error.find("not found") != std::string::npos);
}

TEST_CASE("read_chunked with non-manifest blob returns error", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{2};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Store a regular (non-manifest) blob via ingest
    auto blob = make_signed_blob(id, "regular data, not a manifest");
    auto ingest_result = run_async(pool, engine.ingest(blob));
    REQUIRE(ingest_result.accepted);

    // Try to read_chunked using the regular blob's hash
    auto read_result = engine.read_chunked(id.namespace_id(), ingest_result.ack->blob_hash);
    REQUIRE_FALSE(read_result.success);
    REQUIRE(read_result.error.find("not a manifest") != std::string::npos);
}

TEST_CASE("read_chunked fails with missing chunks when chunk deleted", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{2};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto data = make_random_data(CHUNK_SIZE * 2 + CHUNK_SIZE / 2);  // 2.5 MiB, 3 chunks

    uint64_t ts = current_timestamp();
    uint32_t ttl = 3600;

    auto result = run_async(pool, engine.store_chunked(
        id.namespace_id(), id.public_key(), data, ttl, ts, make_sign_fn(id)));
    REQUIRE(result.accepted);

    // Parse the manifest to get chunk hashes
    auto manifest_blob = engine.get_blob(id.namespace_id(), result.ack->blob_hash);
    REQUIRE(manifest_blob.has_value());
    auto chunk_hashes = parse_manifest(manifest_blob->data);
    REQUIRE(chunk_hashes.has_value());
    REQUIRE(chunk_hashes->size() == 3);

    // Delete the second chunk
    bool deleted = store.delete_blob_data(id.namespace_id(), (*chunk_hashes)[1]);
    REQUIRE(deleted);

    // Now read_chunked should fail
    auto read_result = engine.read_chunked(id.namespace_id(), result.ack->blob_hash);
    REQUIRE_FALSE(read_result.success);
    REQUIRE(read_result.chunks_expected == 3);
    REQUIRE(read_result.chunks_found < 3);
    REQUIRE(read_result.error.find("missing chunk") != std::string::npos);
}

TEST_CASE("store_chunked signs each chunk independently as valid blobs", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{2};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto data = make_random_data(CHUNK_SIZE * 2 + CHUNK_SIZE / 2);  // 3 chunks

    uint64_t ts = current_timestamp();
    uint32_t ttl = 3600;

    auto result = run_async(pool, engine.store_chunked(
        id.namespace_id(), id.public_key(), data, ttl, ts, make_sign_fn(id)));
    REQUIRE(result.accepted);

    // Parse manifest to get chunk hashes
    auto manifest_blob = engine.get_blob(id.namespace_id(), result.ack->blob_hash);
    REQUIRE(manifest_blob.has_value());
    auto chunk_hashes = parse_manifest(manifest_blob->data);
    REQUIRE(chunk_hashes.has_value());

    // Each chunk is a valid, independently-storable signed blob
    for (size_t i = 0; i < chunk_hashes->size(); ++i) {
        auto chunk_blob = engine.get_blob(id.namespace_id(), (*chunk_hashes)[i]);
        REQUIRE(chunk_blob.has_value());
        REQUIRE(std::memcmp(chunk_blob->namespace_id.data(), id.namespace_id().data(), 32) == 0);
        REQUIRE(chunk_blob->pubkey.size() > 0);
        REQUIRE(chunk_blob->signature.size() > 0);

        // Check chunk data is the correct slice
        size_t offset = i * CHUNK_SIZE;
        size_t len = std::min(CHUNK_SIZE, data.size() - offset);
        std::vector<uint8_t> expected_slice(data.begin() + offset, data.begin() + offset + len);
        REQUIRE(chunk_blob->data == expected_slice);
    }
}

TEST_CASE("store_chunked respects TTL -- chunks and manifest share same TTL/timestamp", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{2};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto data = make_random_data(CHUNK_SIZE * 2);  // 2 chunks

    uint64_t ts = current_timestamp();
    uint32_t ttl = 7200;

    auto result = run_async(pool, engine.store_chunked(
        id.namespace_id(), id.public_key(), data, ttl, ts, make_sign_fn(id)));
    REQUIRE(result.accepted);

    // Check manifest blob
    auto manifest_blob = engine.get_blob(id.namespace_id(), result.ack->blob_hash);
    REQUIRE(manifest_blob.has_value());
    REQUIRE(manifest_blob->ttl == ttl);
    REQUIRE(manifest_blob->timestamp == ts);

    // Check chunks
    auto chunk_hashes = parse_manifest(manifest_blob->data);
    REQUIRE(chunk_hashes.has_value());
    for (const auto& hash : *chunk_hashes) {
        auto chunk_blob = engine.get_blob(id.namespace_id(), hash);
        REQUIRE(chunk_blob.has_value());
        REQUIRE(chunk_blob->ttl == ttl);
        REQUIRE(chunk_blob->timestamp == ts);
    }
}

TEST_CASE("store_chunked creates all chunks and manifest in same namespace (D-08)", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{2};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto data = make_random_data(CHUNK_SIZE * 2);  // 2 chunks

    uint64_t ts = current_timestamp();
    uint32_t ttl = 3600;

    auto result = run_async(pool, engine.store_chunked(
        id.namespace_id(), id.public_key(), data, ttl, ts, make_sign_fn(id)));
    REQUIRE(result.accepted);

    // Check manifest namespace
    auto manifest_blob = engine.get_blob(id.namespace_id(), result.ack->blob_hash);
    REQUIRE(manifest_blob.has_value());
    REQUIRE(std::memcmp(manifest_blob->namespace_id.data(), id.namespace_id().data(), 32) == 0);

    // Check all chunks namespace
    auto chunk_hashes = parse_manifest(manifest_blob->data);
    REQUIRE(chunk_hashes.has_value());
    for (const auto& hash : *chunk_hashes) {
        auto chunk_blob = engine.get_blob(id.namespace_id(), hash);
        REQUIRE(chunk_blob.has_value());
        REQUIRE(std::memcmp(chunk_blob->namespace_id.data(), id.namespace_id().data(), 32) == 0);
    }
}

TEST_CASE("store_chunked + read_chunked with 10 MiB (10 chunks) round-trips correctly", "[chunking]") {
    TempDir tmp;
    Storage store(tmp.path.string());
    asio::thread_pool pool{2};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();
    auto data = make_random_data(CHUNK_SIZE * 10);  // 10 MiB

    uint64_t ts = current_timestamp();
    uint32_t ttl = 3600;

    auto result = run_async(pool, engine.store_chunked(
        id.namespace_id(), id.public_key(), data, ttl, ts, make_sign_fn(id)));
    REQUIRE(result.accepted);

    auto read_result = engine.read_chunked(id.namespace_id(), result.ack->blob_hash);
    REQUIRE(read_result.success);
    REQUIRE(read_result.data == data);
    REQUIRE(read_result.chunks_expected == 10);
    REQUIRE(read_result.chunks_found == 10);
}
