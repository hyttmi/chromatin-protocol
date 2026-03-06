#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <cstring>

#include "db/sync/sync_protocol.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"
#include "db/config/config.h"

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
               ("chromatindb_test_sync_" + std::to_string(dist(gen)));
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

/// Fixed clock for deterministic tests.
uint64_t test_clock_value = 10000;
uint64_t test_clock() { return test_clock_value; }

} // anonymous namespace

using chromatindb::config::Config;
using chromatindb::engine::BlobEngine;
using chromatindb::storage::Storage;
using chromatindb::storage::NamespaceInfo;
using chromatindb::sync::SyncProtocol;
using chromatindb::sync::SyncStats;

// ============================================================================
// Config Phase 5 fields
// ============================================================================

TEST_CASE("Config Phase 5 defaults", "[sync][config]") {
    Config cfg;
    REQUIRE(cfg.max_peers == 32);
    REQUIRE(cfg.sync_interval_seconds == 60);
}

TEST_CASE("Config Phase 5 custom values from JSON", "[sync][config]") {
    TempDir tmp;
    auto config_file = tmp.path / "config.json";

    // Create temp config file
    fs::create_directories(tmp.path);
    {
        std::ofstream f(config_file);
        f << R"({"max_peers": 64, "sync_interval_seconds": 30})";
    }

    auto cfg = chromatindb::config::load_config(config_file);
    REQUIRE(cfg.max_peers == 64);
    REQUIRE(cfg.sync_interval_seconds == 30);
}

// ============================================================================
// is_blob_expired
// ============================================================================

TEST_CASE("is_blob_expired", "[sync]") {
    chromatindb::wire::BlobData blob;

    SECTION("permanent blob is never expired") {
        blob.ttl = 0;
        blob.timestamp = 1000;
        REQUIRE_FALSE(SyncProtocol::is_blob_expired(blob, 999999));
    }

    SECTION("non-expired blob") {
        blob.ttl = 604800;  // 7 days
        blob.timestamp = 10000;
        // now = 10000 + 100 = 10100, which is before 10000 + 604800
        REQUIRE_FALSE(SyncProtocol::is_blob_expired(blob, 10100));
    }

    SECTION("expired blob") {
        blob.ttl = 100;
        blob.timestamp = 1000;
        // now = 1200 > 1000 + 100 = 1100
        REQUIRE(SyncProtocol::is_blob_expired(blob, 1200));
    }

    SECTION("exactly at expiry boundary") {
        blob.ttl = 100;
        blob.timestamp = 1000;
        // now = 1100 == 1000 + 100 -- expired (<=)
        REQUIRE(SyncProtocol::is_blob_expired(blob, 1100));
    }
}

// ============================================================================
// collect_namespace_hashes
// ============================================================================

TEST_CASE("collect_namespace_hashes returns non-expired hashes", "[sync]") {
    TempDir tmp;
    Storage store(tmp.path.string(), test_clock);
    BlobEngine engine(store);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Set clock to 10000
    test_clock_value = 10000;

    // Ingest a non-expired blob (timestamp=9000, ttl=604800 => expires at 613800)
    auto blob1 = make_signed_blob(id, "non-expired", 604800, 9000);
    REQUIRE(engine.ingest(blob1).accepted);

    // Ingest an expired blob (timestamp=1, ttl=100 => expires at 101, way before now=10000)
    auto blob2 = make_signed_blob(id, "expired", 100, 1);
    REQUIRE(engine.ingest(blob2).accepted);

    // Ingest a permanent blob (ttl=0)
    auto blob3 = make_signed_blob(id, "permanent", 0, 5000);
    REQUIRE(engine.ingest(blob3).accepted);

    SyncProtocol sync(engine, test_clock);
    auto hashes = sync.collect_namespace_hashes(id.namespace_id());

    // Should have 2 hashes: non-expired + permanent. Expired should be excluded.
    REQUIRE(hashes.size() == 2);
}

// ============================================================================
// diff_hashes
// ============================================================================

TEST_CASE("diff_hashes", "[sync]") {
    std::array<uint8_t, 32> h1{}, h2{}, h3{};
    h1.fill(0x01);
    h2.fill(0x02);
    h3.fill(0x03);

    SECTION("identifies missing hashes") {
        std::vector<std::array<uint8_t, 32>> ours = {h1, h2};
        std::vector<std::array<uint8_t, 32>> theirs = {h1, h2, h3};

        auto missing = SyncProtocol::diff_hashes(ours, theirs);
        REQUIRE(missing.size() == 1);
        REQUIRE(missing[0] == h3);
    }

    SECTION("empty diff when identical") {
        std::vector<std::array<uint8_t, 32>> ours = {h1, h2};
        std::vector<std::array<uint8_t, 32>> theirs = {h1, h2};

        auto missing = SyncProtocol::diff_hashes(ours, theirs);
        REQUIRE(missing.empty());
    }

    SECTION("all missing when ours is empty") {
        std::vector<std::array<uint8_t, 32>> ours;
        std::vector<std::array<uint8_t, 32>> theirs = {h1, h2, h3};

        auto missing = SyncProtocol::diff_hashes(ours, theirs);
        REQUIRE(missing.size() == 3);
    }

    SECTION("no missing when theirs is empty") {
        std::vector<std::array<uint8_t, 32>> ours = {h1, h2};
        std::vector<std::array<uint8_t, 32>> theirs;

        auto missing = SyncProtocol::diff_hashes(ours, theirs);
        REQUIRE(missing.empty());
    }
}

// ============================================================================
// Bidirectional sync
// ============================================================================

TEST_CASE("bidirectional sync produces union", "[sync]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    BlobEngine engine1(store1);
    BlobEngine engine2(store2);

    auto id1 = chromatindb::identity::NodeIdentity::generate();
    auto id2 = chromatindb::identity::NodeIdentity::generate();

    // Store blobs in engine1 (from id1)
    auto blob_a = make_signed_blob(id1, "blob-A", 604800, 9000);
    auto blob_b = make_signed_blob(id1, "blob-B", 604800, 9001);
    REQUIRE(engine1.ingest(blob_a).accepted);
    REQUIRE(engine1.ingest(blob_b).accepted);

    // Store a different blob in engine2 (from id2)
    auto blob_c = make_signed_blob(id2, "blob-C", 604800, 9002);
    REQUIRE(engine2.ingest(blob_c).accepted);

    SyncProtocol sync1(engine1, test_clock);
    SyncProtocol sync2(engine2, test_clock);

    // Simulate sync:
    // 1. Exchange namespace lists
    auto ns_list_1 = engine1.list_namespaces();
    auto ns_list_2 = engine2.list_namespaces();

    // 2. For id1's namespace: sync1 has hashes, sync2 has none
    auto hashes_1_ns1 = sync1.collect_namespace_hashes(id1.namespace_id());
    auto hashes_2_ns1 = sync2.collect_namespace_hashes(id1.namespace_id());

    // Engine2 needs what engine1 has in id1's namespace
    auto missing_on_2 = SyncProtocol::diff_hashes(hashes_2_ns1, hashes_1_ns1);
    REQUIRE(missing_on_2.size() == 2);  // blob_a and blob_b

    // Transfer missing blobs
    auto transfer_blobs = sync1.get_blobs_by_hashes(id1.namespace_id(), missing_on_2);
    REQUIRE(transfer_blobs.size() == 2);

    auto stats_2 = sync2.ingest_blobs(transfer_blobs);
    REQUIRE(stats_2.blobs_received == 2);

    // 3. For id2's namespace: sync2 has hashes, sync1 has none
    auto hashes_1_ns2 = sync1.collect_namespace_hashes(id2.namespace_id());
    auto hashes_2_ns2 = sync2.collect_namespace_hashes(id2.namespace_id());

    auto missing_on_1 = SyncProtocol::diff_hashes(hashes_1_ns2, hashes_2_ns2);
    REQUIRE(missing_on_1.size() == 1);  // blob_c

    auto transfer_blobs_2 = sync2.get_blobs_by_hashes(id2.namespace_id(), missing_on_1);
    REQUIRE(transfer_blobs_2.size() == 1);

    auto stats_1 = sync1.ingest_blobs(transfer_blobs_2);
    REQUIRE(stats_1.blobs_received == 1);

    // 4. Verify both engines now have the union (3 blobs total)
    auto final_ns_1 = engine1.list_namespaces();
    auto final_ns_2 = engine2.list_namespaces();
    REQUIRE(final_ns_1.size() == 2);  // id1 and id2
    REQUIRE(final_ns_2.size() == 2);
}

TEST_CASE("sync skips expired blobs", "[sync]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    BlobEngine engine1(store1);
    BlobEngine engine2(store2);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Store a non-expired blob
    auto blob_ok = make_signed_blob(id, "not-expired", 604800, 9000);
    REQUIRE(engine1.ingest(blob_ok).accepted);

    // Store an expired blob (timestamp=1, ttl=100 => expired at 101)
    auto blob_expired = make_signed_blob(id, "already-expired", 100, 1);
    REQUIRE(engine1.ingest(blob_expired).accepted);

    SyncProtocol sync1(engine1, test_clock);
    SyncProtocol sync2(engine2, test_clock);

    // Collect hashes -- expired blob should be excluded
    auto hashes = sync1.collect_namespace_hashes(id.namespace_id());
    REQUIRE(hashes.size() == 1);  // Only the non-expired blob

    // Even if we try to ingest the expired blob directly, it should be skipped
    auto stats = sync2.ingest_blobs({blob_expired});
    REQUIRE(stats.blobs_received == 0);  // Expired, not ingested
}

TEST_CASE("sync handles duplicate data", "[sync]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    BlobEngine engine1(store1);
    BlobEngine engine2(store2);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Both engines have the same blob
    auto blob = make_signed_blob(id, "shared-blob", 604800, 9000);
    REQUIRE(engine1.ingest(blob).accepted);
    REQUIRE(engine2.ingest(blob).accepted);

    SyncProtocol sync1(engine1, test_clock);
    SyncProtocol sync2(engine2, test_clock);

    auto hashes1 = sync1.collect_namespace_hashes(id.namespace_id());
    auto hashes2 = sync2.collect_namespace_hashes(id.namespace_id());

    // Diff should be empty -- both sides have the same data
    auto missing = SyncProtocol::diff_hashes(hashes1, hashes2);
    REQUIRE(missing.empty());
}

TEST_CASE("sync handles empty namespace", "[sync]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    BlobEngine engine1(store1);
    BlobEngine engine2(store2);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Engine1 has data, engine2 has nothing
    auto blob = make_signed_blob(id, "only-on-one-side", 604800, 9000);
    REQUIRE(engine1.ingest(blob).accepted);

    SyncProtocol sync1(engine1, test_clock);
    SyncProtocol sync2(engine2, test_clock);

    // Engine2 has no hashes for this namespace
    auto hashes2 = sync2.collect_namespace_hashes(id.namespace_id());
    REQUIRE(hashes2.empty());

    // Full diff: everything from engine1 is missing on engine2
    auto hashes1 = sync1.collect_namespace_hashes(id.namespace_id());
    auto missing = SyncProtocol::diff_hashes(hashes2, hashes1);
    REQUIRE(missing.size() == 1);
}

// ============================================================================
// Message encoding/decoding round-trip
// ============================================================================

TEST_CASE("namespace list encode/decode round-trip", "[sync][codec]") {
    std::vector<NamespaceInfo> original;

    NamespaceInfo ns1;
    ns1.namespace_id.fill(0xAA);
    ns1.latest_seq_num = 42;
    original.push_back(ns1);

    NamespaceInfo ns2;
    ns2.namespace_id.fill(0xBB);
    ns2.latest_seq_num = 100;
    original.push_back(ns2);

    auto encoded = SyncProtocol::encode_namespace_list(original);
    auto decoded = SyncProtocol::decode_namespace_list(encoded);

    REQUIRE(decoded.size() == 2);
    REQUIRE(decoded[0].namespace_id == ns1.namespace_id);
    REQUIRE(decoded[0].latest_seq_num == 42);
    REQUIRE(decoded[1].namespace_id == ns2.namespace_id);
    REQUIRE(decoded[1].latest_seq_num == 100);
}

TEST_CASE("namespace list empty round-trip", "[sync][codec]") {
    std::vector<NamespaceInfo> empty;
    auto encoded = SyncProtocol::encode_namespace_list(empty);
    auto decoded = SyncProtocol::decode_namespace_list(encoded);
    REQUIRE(decoded.empty());
}

TEST_CASE("hash list encode/decode round-trip", "[sync][codec]") {
    std::array<uint8_t, 32> ns{};
    ns.fill(0xCC);

    std::vector<std::array<uint8_t, 32>> hashes;
    std::array<uint8_t, 32> h1{}, h2{};
    h1.fill(0x11);
    h2.fill(0x22);
    hashes.push_back(h1);
    hashes.push_back(h2);

    auto encoded = SyncProtocol::encode_hash_list(ns, hashes);
    auto [decoded_ns, decoded_hashes] = SyncProtocol::decode_hash_list(encoded);

    REQUIRE(decoded_ns == ns);
    REQUIRE(decoded_hashes.size() == 2);
    REQUIRE(decoded_hashes[0] == h1);
    REQUIRE(decoded_hashes[1] == h2);
}

TEST_CASE("blob transfer encode/decode round-trip", "[sync][codec]") {
    auto id = chromatindb::identity::NodeIdentity::generate();

    auto blob1 = make_signed_blob(id, "transfer-1", 604800, 9000);
    auto blob2 = make_signed_blob(id, "transfer-2", 604800, 9001);

    std::vector<chromatindb::wire::BlobData> blobs = {blob1, blob2};

    auto encoded = SyncProtocol::encode_blob_transfer(blobs);
    auto decoded = SyncProtocol::decode_blob_transfer(encoded);

    REQUIRE(decoded.size() == 2);
    REQUIRE(decoded[0].data == blob1.data);
    REQUIRE(decoded[0].ttl == blob1.ttl);
    REQUIRE(decoded[0].timestamp == blob1.timestamp);
    REQUIRE(decoded[1].data == blob2.data);
}
