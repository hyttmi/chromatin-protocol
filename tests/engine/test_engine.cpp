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
