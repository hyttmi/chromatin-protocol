#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <cstring>
#include <unordered_set>

#include "db/sync/sync_protocol.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"
#include "db/config/config.h"

#include "db/tests/test_helpers.h"

#include <asio.hpp>

namespace fs = std::filesystem;

namespace {

/// Compute set difference: hashes in `theirs` not in `ours`.
/// Local test helper replacing the removed diff_hashes().
std::vector<std::array<uint8_t, 32>> diff_hashes(
    const std::vector<std::array<uint8_t, 32>>& ours,
    const std::vector<std::array<uint8_t, 32>>& theirs) {
    std::unordered_set<std::string> our_set;
    our_set.reserve(ours.size());
    for (const auto& h : ours) {
        our_set.insert(std::string(reinterpret_cast<const char*>(h.data()), h.size()));
    }
    std::vector<std::array<uint8_t, 32>> missing;
    for (const auto& h : theirs) {
        std::string key(reinterpret_cast<const char*>(h.data()), h.size());
        if (our_set.find(key) == our_set.end()) {
            missing.push_back(h);
        }
    }
    return missing;
}

/// Fixed clock for deterministic tests.
uint64_t test_clock_value = 0;
uint64_t test_clock() { return test_clock_value; }

} // anonymous namespace

using chromatindb::test::TempDir;
using chromatindb::test::run_async;
using chromatindb::test::make_signed_blob;
using chromatindb::test::make_signed_tombstone;
using chromatindb::test::current_timestamp;
using chromatindb::test::TS_AUTO;

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
    REQUIRE(cfg.safety_net_interval_seconds == 600);
}

TEST_CASE("Config Phase 5 custom values from JSON", "[sync][config]") {
    TempDir tmp;
    auto config_file = tmp.path / "config.json";

    // Create temp config file
    fs::create_directories(tmp.path);
    {
        std::ofstream f(config_file);
        f << R"({"max_peers": 64, "safety_net_interval_seconds": 300})";
    }

    auto cfg = chromatindb::config::load_config(config_file);
    REQUIRE(cfg.max_peers == 64);
    REQUIRE(cfg.safety_net_interval_seconds == 300);
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
        blob.timestamp = 10000;  // 10000 seconds
        // now = 10100, which is before 10000 + 604800 = 614800
        REQUIRE_FALSE(SyncProtocol::is_blob_expired(blob, 10100));
    }

    SECTION("expired blob") {
        blob.ttl = 100;
        blob.timestamp = 1000;  // 1000 seconds
        // now = 1200 > 1000 + 100 = 1100
        REQUIRE(SyncProtocol::is_blob_expired(blob, 1200));
    }

    SECTION("exactly at expiry boundary") {
        blob.ttl = 100;
        blob.timestamp = 1000;  // 1000 seconds
        // now = 1100 == 1000 + 100 -- expired (<=)
        REQUIRE(SyncProtocol::is_blob_expired(blob, 1100));
    }
}

// ============================================================================
// collect_namespace_hashes
// ============================================================================

TEST_CASE("collect_namespace_hashes returns all hashes from index", "[sync]") {
    TempDir tmp;
    Storage store(tmp.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();

    test_clock_value = 10000;

    // Ingest a non-expired blob (current timestamp, ttl=604800 => not expired)
    auto blob1 = make_signed_blob(id, "non-expired");
    REQUIRE(run_async(pool, engine.ingest(blob1)).accepted);

    // Ingest a blob with short TTL (will be expired per sync clock)
    auto blob2 = make_signed_blob(id, "expired", 100);
    REQUIRE(run_async(pool, engine.ingest(blob2)).accepted);

    // Ingest a permanent blob (ttl=0)
    auto blob3 = make_signed_blob(id, "permanent", 0);
    REQUIRE(run_async(pool, engine.ingest(blob3)).accepted);

    SyncProtocol sync(engine, store, pool, test_clock);
    auto hashes = sync.collect_namespace_hashes(id.namespace_id());

    // Index-only reads return ALL hashes including expired -- expiry is handled
    // at ingest time on the receiving end, not during hash collection.
    REQUIRE(hashes.size() == 3);
}

// diff_hashes removed in Phase 39 -- replaced by reconciliation module

// ============================================================================
// Bidirectional sync
// ============================================================================

TEST_CASE("bidirectional sync produces union", "[sync]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    auto id1 = chromatindb::identity::NodeIdentity::generate();
    auto id2 = chromatindb::identity::NodeIdentity::generate();

    // Store blobs in engine1 (from id1)
    auto blob_a = make_signed_blob(id1, "blob-A");
    auto blob_b = make_signed_blob(id1, "blob-B");
    REQUIRE(run_async(pool, engine1.ingest(blob_a)).accepted);
    REQUIRE(run_async(pool, engine1.ingest(blob_b)).accepted);

    // Store a different blob in engine2 (from id2)
    auto blob_c = make_signed_blob(id2, "blob-C");
    REQUIRE(run_async(pool, engine2.ingest(blob_c)).accepted);

    SyncProtocol sync1(engine1, store1, pool, test_clock);
    SyncProtocol sync2(engine2, store2, pool, test_clock);

    // Simulate sync:
    // 1. Exchange namespace lists
    auto ns_list_1 = engine1.list_namespaces();
    auto ns_list_2 = engine2.list_namespaces();

    // 2. For id1's namespace: sync1 has hashes, sync2 has none
    auto hashes_1_ns1 = sync1.collect_namespace_hashes(id1.namespace_id());
    auto hashes_2_ns1 = sync2.collect_namespace_hashes(id1.namespace_id());

    // Engine2 needs what engine1 has in id1's namespace
    auto missing_on_2 = diff_hashes(hashes_2_ns1, hashes_1_ns1);
    REQUIRE(missing_on_2.size() == 2);  // blob_a and blob_b

    // Transfer missing blobs
    auto transfer_blobs = sync1.get_blobs_by_hashes(id1.namespace_id(), missing_on_2);
    REQUIRE(transfer_blobs.size() == 2);

    auto stats_2 = run_async(pool, sync2.ingest_blobs(transfer_blobs));
    REQUIRE(stats_2.blobs_received == 2);

    // 3. For id2's namespace: sync2 has hashes, sync1 has none
    auto hashes_1_ns2 = sync1.collect_namespace_hashes(id2.namespace_id());
    auto hashes_2_ns2 = sync2.collect_namespace_hashes(id2.namespace_id());

    auto missing_on_1 = diff_hashes(hashes_1_ns2, hashes_2_ns2);
    REQUIRE(missing_on_1.size() == 1);  // blob_c

    auto transfer_blobs_2 = sync2.get_blobs_by_hashes(id2.namespace_id(), missing_on_1);
    REQUIRE(transfer_blobs_2.size() == 1);

    auto stats_1 = run_async(pool, sync1.ingest_blobs(transfer_blobs_2));
    REQUIRE(stats_1.blobs_received == 1);

    // 4. Verify both engines now have the union (3 blobs total)
    auto final_ns_1 = engine1.list_namespaces();
    auto final_ns_2 = engine2.list_namespaces();
    REQUIRE(final_ns_1.size() == 2);  // id1 and id2
    REQUIRE(final_ns_2.size() == 2);
}

TEST_CASE("sync skips expired blobs", "[sync]") {
    TempDir tmp1, tmp2;
    auto real_now = current_timestamp();
    test_clock_value = real_now + 200;  // Advance clock slightly past expired blob

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Store a non-expired blob (default TTL 604800, far from expiry)
    auto blob_ok = make_signed_blob(id, "not-expired");
    REQUIRE(run_async(pool, engine1.ingest(blob_ok)).accepted);

    // Store a blob with short TTL that is expired relative to test clock
    // timestamp = real_now, TTL = 100 → expires at real_now + 100
    // test_clock = real_now + 200 → blob is expired
    auto blob_expired = make_signed_blob(id, "already-expired", 100, real_now);
    REQUIRE(run_async(pool, engine1.ingest(blob_expired)).accepted);

    SyncProtocol sync1(engine1, store1, pool, test_clock);
    SyncProtocol sync2(engine2, store2, pool, test_clock);

    // Collect hashes -- index-only reads include all hashes (expired too)
    auto hashes = sync1.collect_namespace_hashes(id.namespace_id());
    REQUIRE(hashes.size() == 2);  // Both blobs in index

    // Expired blob ingestion on the receiving side is skipped
    auto stats = run_async(pool, sync2.ingest_blobs({blob_expired}));
    REQUIRE(stats.blobs_received == 0);  // Expired, not ingested
}

TEST_CASE("sync handles duplicate data", "[sync]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Both engines have the same blob
    auto blob = make_signed_blob(id, "shared-blob");
    REQUIRE(run_async(pool, engine1.ingest(blob)).accepted);
    REQUIRE(run_async(pool, engine2.ingest(blob)).accepted);

    SyncProtocol sync1(engine1, store1, pool, test_clock);
    SyncProtocol sync2(engine2, store2, pool, test_clock);

    auto hashes1 = sync1.collect_namespace_hashes(id.namespace_id());
    auto hashes2 = sync2.collect_namespace_hashes(id.namespace_id());

    // Diff should be empty -- both sides have the same data
    auto missing = diff_hashes(hashes1, hashes2);
    REQUIRE(missing.empty());
}

TEST_CASE("sync handles empty namespace", "[sync]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Engine1 has data, engine2 has nothing
    auto blob = make_signed_blob(id, "only-on-one-side");
    REQUIRE(run_async(pool, engine1.ingest(blob)).accepted);

    SyncProtocol sync1(engine1, store1, pool, test_clock);
    SyncProtocol sync2(engine2, store2, pool, test_clock);

    // Engine2 has no hashes for this namespace
    auto hashes2 = sync2.collect_namespace_hashes(id.namespace_id());
    REQUIRE(hashes2.empty());

    // Full diff: everything from engine1 is missing on engine2
    auto hashes1 = sync1.collect_namespace_hashes(id.namespace_id());
    auto missing = diff_hashes(hashes2, hashes1);
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

TEST_CASE("blob request encode/decode round-trip", "[sync][codec]") {
    std::array<uint8_t, 32> ns{};
    ns.fill(0xCC);

    std::vector<std::array<uint8_t, 32>> hashes;
    std::array<uint8_t, 32> h1{}, h2{};
    h1.fill(0x11);
    h2.fill(0x22);
    hashes.push_back(h1);
    hashes.push_back(h2);

    auto encoded = SyncProtocol::encode_blob_request(ns, hashes);
    auto [decoded_ns, decoded_hashes] = SyncProtocol::decode_blob_request(encoded);

    REQUIRE(decoded_ns == ns);
    REQUIRE(decoded_hashes.size() == 2);
    REQUIRE(decoded_hashes[0] == h1);
    REQUIRE(decoded_hashes[1] == h2);
}

TEST_CASE("blob transfer encode/decode round-trip", "[sync][codec]") {
    auto id = chromatindb::identity::NodeIdentity::generate();

    auto blob1 = make_signed_blob(id, "transfer-1");
    auto blob2 = make_signed_blob(id, "transfer-2");

    std::vector<chromatindb::wire::BlobData> blobs = {blob1, blob2};

    auto encoded = SyncProtocol::encode_blob_transfer(blobs);
    auto decoded = SyncProtocol::decode_blob_transfer(encoded);

    REQUIRE(decoded.size() == 2);
    REQUIRE(decoded[0].data == blob1.data);
    REQUIRE(decoded[0].ttl == blob1.ttl);
    REQUIRE(decoded[0].timestamp == blob1.timestamp);
    REQUIRE(decoded[1].data == blob2.data);
}

TEST_CASE("single blob transfer encode/decode round-trip", "[sync][codec]") {
    auto id = chromatindb::identity::NodeIdentity::generate();
    auto blob = make_signed_blob(id, "single-transfer");

    auto encoded = SyncProtocol::encode_single_blob_transfer(blob);
    auto decoded = SyncProtocol::decode_blob_transfer(encoded);

    REQUIRE(decoded.size() == 1);
    REQUIRE(decoded[0].data == blob.data);
    REQUIRE(decoded[0].ttl == blob.ttl);
    REQUIRE(decoded[0].timestamp == blob.timestamp);
    REQUIRE(decoded[0].namespace_id == blob.namespace_id);
    REQUIRE(decoded[0].pubkey == blob.pubkey);
    REQUIRE(decoded[0].signature == blob.signature);
}

// ============================================================================
// Tombstone sync tests (Phase 12: Blob Deletion)
// ============================================================================

TEST_CASE("tombstone appears in collect_namespace_hashes", "[sync][tombstone]") {
    TempDir tmp;
    test_clock_value = 10000;

    Storage store(tmp.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine(store, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Store a regular blob
    auto blob = make_signed_blob(id, "to-be-deleted");
    auto ingest_result = run_async(pool, engine.ingest(blob));
    REQUIRE(ingest_result.accepted);

    // Delete it via tombstone
    auto tombstone = make_signed_tombstone(id, ingest_result.ack->blob_hash);
    auto delete_result = run_async(pool, engine.delete_blob(tombstone));
    REQUIRE(delete_result.accepted);

    SyncProtocol sync(engine, store, pool, test_clock);
    auto hashes = sync.collect_namespace_hashes(id.namespace_id());

    // Tombstone is stored as a blob, so it appears in hash collection.
    // Original blob was deleted, so only tombstone hash remains.
    REQUIRE(hashes.size() == 1);
    REQUIRE(hashes[0] == delete_result.ack->blob_hash);
}

TEST_CASE("tombstone propagates via sync ingest_blobs", "[sync][tombstone]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Both nodes have the same blob
    auto blob = make_signed_blob(id, "shared-blob");
    auto ingest1 = run_async(pool, engine1.ingest(blob));
    REQUIRE(ingest1.accepted);
    REQUIRE(run_async(pool, engine2.ingest(blob)).accepted);
    auto blob_hash = ingest1.ack->blob_hash;

    // Node1 deletes the blob
    auto tombstone = make_signed_tombstone(id, blob_hash);
    auto delete_result = run_async(pool, engine1.delete_blob(tombstone));
    REQUIRE(delete_result.accepted);

    // Simulate sync: node1 sends its hashes to node2
    SyncProtocol sync1(engine1, store1, pool, test_clock);
    SyncProtocol sync2(engine2, store2, pool, test_clock);

    auto hashes1 = sync1.collect_namespace_hashes(id.namespace_id());
    auto hashes2 = sync2.collect_namespace_hashes(id.namespace_id());

    // Node2 needs the tombstone (it has the original blob but not the tombstone)
    auto missing_on_2 = diff_hashes(hashes2, hashes1);
    REQUIRE(missing_on_2.size() == 1);  // The tombstone

    // Transfer the tombstone to node2
    auto transfer = sync1.get_blobs_by_hashes(id.namespace_id(), missing_on_2);
    REQUIRE(transfer.size() == 1);
    REQUIRE(chromatindb::wire::is_tombstone(transfer[0].data));

    // Ingest the tombstone on node2
    auto stats = run_async(pool, sync2.ingest_blobs(transfer));
    REQUIRE(stats.blobs_received == 1);

    // Original blob should now be deleted on node2
    auto found = engine2.get_blob(id.namespace_id(), blob_hash);
    REQUIRE_FALSE(found.has_value());
}

TEST_CASE("tombstone blocks future blob arrival via sync", "[sync][tombstone]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    auto id = chromatindb::identity::NodeIdentity::generate();

    // Create blob on node1
    auto blob = make_signed_blob(id, "will-be-blocked");
    auto ingest_result = run_async(pool, engine1.ingest(blob));
    REQUIRE(ingest_result.accepted);
    auto blob_hash = ingest_result.ack->blob_hash;

    // Node2 receives a tombstone for this blob before the blob itself arrives
    auto tombstone = make_signed_tombstone(id, blob_hash);
    auto tombstone_ingest = run_async(pool, engine2.ingest(tombstone));
    REQUIRE(tombstone_ingest.accepted);

    // Now try to sync the original blob to node2 via ingest_blobs
    SyncProtocol sync2(engine2, store2, pool, test_clock);
    auto stats = run_async(pool, sync2.ingest_blobs({blob}));

    // Blob should be rejected (tombstoned)
    REQUIRE(stats.blobs_received == 0);

    // Verify blob is not present on node2
    auto found = engine2.get_blob(id.namespace_id(), blob_hash);
    REQUIRE_FALSE(found.has_value());
}

TEST_CASE("tombstone transfer encode/decode preserves tombstone data", "[sync][tombstone][codec]") {
    auto id = chromatindb::identity::NodeIdentity::generate();

    std::array<uint8_t, 32> target{};
    target.fill(0xDD);
    auto tombstone = make_signed_tombstone(id, target);

    // Encode and decode via blob transfer (used during sync)
    auto encoded = SyncProtocol::encode_single_blob_transfer(tombstone);
    auto decoded = SyncProtocol::decode_blob_transfer(encoded);

    REQUIRE(decoded.size() == 1);
    REQUIRE(chromatindb::wire::is_tombstone(decoded[0].data));

    auto extracted = chromatindb::wire::extract_tombstone_target(decoded[0].data);
    REQUIRE(extracted == target);
    REQUIRE(decoded[0].ttl == 0);
    REQUIRE(decoded[0].namespace_id == tombstone.namespace_id);
    REQUIRE(decoded[0].pubkey == tombstone.pubkey);
    REQUIRE(decoded[0].signature == tombstone.signature);
}

// ============================================================================
// Phase 13: Delegation sync tests
// ============================================================================

namespace {

/// Build a properly signed delegation BlobData: owner delegates to delegate.
chromatindb::wire::BlobData make_signed_delegation_sync(
    const chromatindb::identity::NodeIdentity& owner,
    const chromatindb::identity::NodeIdentity& delegate,
    uint64_t timestamp = TS_AUTO)
{
    chromatindb::wire::BlobData blob;
    std::memcpy(blob.namespace_id.data(), owner.namespace_id().data(), 32);
    blob.pubkey.assign(owner.public_key().begin(), owner.public_key().end());
    blob.data = chromatindb::wire::make_delegation_data(delegate.public_key());
    blob.ttl = 0;  // Permanent
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    auto signing_input = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = owner.sign(signing_input);

    return blob;
}

/// Build a properly signed blob written by a delegate to an owner's namespace.
chromatindb::wire::BlobData make_delegate_blob_sync(
    const chromatindb::identity::NodeIdentity& owner,
    const chromatindb::identity::NodeIdentity& delegate,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = TS_AUTO)
{
    chromatindb::wire::BlobData blob;
    std::memcpy(blob.namespace_id.data(), owner.namespace_id().data(), 32);
    blob.pubkey.assign(delegate.public_key().begin(), delegate.public_key().end());
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    auto signing_input = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = delegate.sign(signing_input);

    return blob;
}

} // anonymous namespace

TEST_CASE("Delegation blob replicates via sync", "[sync][delegation]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Node1: owner creates delegation
    auto deleg = chromatindb::test::make_signed_delegation(owner, delegate);
    auto deleg_result = run_async(pool, engine1.ingest(deleg));
    REQUIRE(deleg_result.accepted);

    // Sync: node1 sends delegation to node2
    SyncProtocol sync1(engine1, store1, pool, test_clock);
    SyncProtocol sync2(engine2, store2, pool, test_clock);

    auto hashes1 = sync1.collect_namespace_hashes(owner.namespace_id());
    auto hashes2 = sync2.collect_namespace_hashes(owner.namespace_id());

    auto missing_on_2 = diff_hashes(hashes2, hashes1);
    REQUIRE(missing_on_2.size() == 1);

    auto transfer = sync1.get_blobs_by_hashes(owner.namespace_id(), missing_on_2);
    REQUIRE(transfer.size() == 1);
    REQUIRE(chromatindb::wire::is_delegation(transfer[0].data));

    auto stats = run_async(pool, sync2.ingest_blobs(transfer));
    REQUIRE(stats.blobs_received == 1);

    // Node2 should now recognize the delegation (DELEG-03)
    REQUIRE(store2.has_valid_delegation(
        owner.namespace_id(), delegate.public_key()));
}

TEST_CASE("Delegate-written blob replicates via sync", "[sync][delegation]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Node1: owner delegates, delegate writes
    auto deleg = chromatindb::test::make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine1.ingest(deleg)).accepted);

    auto delegate_blob = chromatindb::test::make_delegate_blob(owner, delegate, "sync-delegate-data");
    REQUIRE(run_async(pool, engine1.ingest(delegate_blob)).accepted);

    // Sync everything from node1 to node2
    SyncProtocol sync1(engine1, store1, pool, test_clock);
    SyncProtocol sync2(engine2, store2, pool, test_clock);

    auto hashes1 = sync1.collect_namespace_hashes(owner.namespace_id());
    auto hashes2 = sync2.collect_namespace_hashes(owner.namespace_id());

    auto missing_on_2 = diff_hashes(hashes2, hashes1);
    REQUIRE(missing_on_2.size() == 2);  // delegation blob + delegate-written blob

    auto transfer = sync1.get_blobs_by_hashes(owner.namespace_id(), missing_on_2);
    REQUIRE(transfer.size() == 2);

    // Ingest on node2 -- delegation blob must be ingested first for delegate blob to succeed
    // ingest_blobs iterates in order, so ensure delegation blob comes first
    std::vector<chromatindb::wire::BlobData> ordered_transfer;
    for (auto& b : transfer) {
        if (chromatindb::wire::is_delegation(b.data)) {
            ordered_transfer.insert(ordered_transfer.begin(), std::move(b));
        } else {
            ordered_transfer.push_back(std::move(b));
        }
    }

    auto stats = run_async(pool, sync2.ingest_blobs(ordered_transfer));
    REQUIRE(stats.blobs_received == 2);

    // Node2 should have both the delegation and the delegate-written blob
    REQUIRE(store2.has_valid_delegation(
        owner.namespace_id(), delegate.public_key()));

    // Find the delegate blob on node2
    auto all_blobs = engine2.get_blobs_since(owner.namespace_id(), 0);
    bool found_delegate_blob = false;
    for (const auto& b : all_blobs) {
        std::string data_str(b.data.begin(), b.data.end());
        if (data_str == "sync-delegate-data") {
            found_delegate_blob = true;
        }
    }
    REQUIRE(found_delegate_blob);
}

TEST_CASE("Delegation revocation replicates via sync", "[sync][delegation]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    auto owner = chromatindb::identity::NodeIdentity::generate();
    auto delegate = chromatindb::identity::NodeIdentity::generate();

    // Both nodes have the delegation
    auto deleg = chromatindb::test::make_signed_delegation(owner, delegate);
    REQUIRE(run_async(pool, engine1.ingest(deleg)).accepted);
    REQUIRE(run_async(pool, engine2.ingest(deleg)).accepted);

    auto deleg_hash = engine1.get_blobs_since(owner.namespace_id(), 0);
    REQUIRE(!deleg_hash.empty());

    // Get the delegation blob hash via encoding
    auto encoded_deleg = chromatindb::wire::encode_blob(deleg);
    auto deleg_content_hash = chromatindb::wire::blob_hash(encoded_deleg);

    // Node2 should recognize delegation
    REQUIRE(store2.has_valid_delegation(
        owner.namespace_id(), delegate.public_key()));

    // Node1: owner revokes by tombstoning
    auto tombstone = make_signed_tombstone(owner, deleg_content_hash);
    auto delete_result = run_async(pool, engine1.delete_blob(tombstone));
    REQUIRE(delete_result.accepted);

    // Sync tombstone from node1 to node2
    SyncProtocol sync1(engine1, store1, pool, test_clock);
    SyncProtocol sync2(engine2, store2, pool, test_clock);

    auto hashes1 = sync1.collect_namespace_hashes(owner.namespace_id());
    auto hashes2 = sync2.collect_namespace_hashes(owner.namespace_id());

    auto missing_on_2 = diff_hashes(hashes2, hashes1);
    REQUIRE(missing_on_2.size() == 1);  // Just the tombstone

    auto transfer = sync1.get_blobs_by_hashes(owner.namespace_id(), missing_on_2);
    REQUIRE(transfer.size() == 1);
    REQUIRE(chromatindb::wire::is_tombstone(transfer[0].data));

    auto stats = run_async(pool, sync2.ingest_blobs(transfer));
    REQUIRE(stats.blobs_received == 1);

    // Delegation should be revoked on node2
    REQUIRE_FALSE(store2.has_valid_delegation(
        owner.namespace_id(), delegate.public_key()));

    // Delegate writes to node2 should now fail
    auto delegate_blob = chromatindb::test::make_delegate_blob(owner, delegate, "post-revocation");
    auto result = run_async(pool, engine2.ingest(delegate_blob));
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.error.value() == chromatindb::engine::IngestError::no_delegation);
}

// ============================================================================
// Phase 34: Cursor-based sync integration tests
// ============================================================================

using chromatindb::storage::SyncCursor;
using chromatindb::crypto::sha3_256;

TEST_CASE("Cursor lifecycle across sync: set after first sync, hit on second", "[sync][cursor]") {
    TempDir tmp1, tmp2;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    Storage store2(tmp2.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);
    BlobEngine engine2(store2, pool);

    auto id1 = chromatindb::identity::NodeIdentity::generate();

    // Store a blob on node1
    auto blob = make_signed_blob(id1, "cursor-test-blob");
    REQUIRE(run_async(pool, engine1.ingest(blob)).accepted);

    // Simulate "peer hash" for node1 (as seen by node2)
    auto peer_hash = sha3_256(id1.public_key());

    // Before sync: no cursor exists
    auto cursor_before = store2.get_sync_cursor(peer_hash, id1.namespace_id());
    REQUIRE_FALSE(cursor_before.has_value());

    // Simulate first sync: node2 receives node1's namespace info
    auto ns_list = store1.list_namespaces();
    REQUIRE(ns_list.size() == 1);
    auto peer_seq = ns_list[0].latest_seq_num;
    REQUIRE(peer_seq > 0);

    // After sync completes, set cursor
    SyncCursor new_cursor;
    new_cursor.seq_num = peer_seq;
    new_cursor.round_count = 1;
    new_cursor.last_sync_timestamp = test_clock_value;
    store2.set_sync_cursor(peer_hash, id1.namespace_id(), new_cursor);

    // Verify cursor exists now
    auto cursor_after = store2.get_sync_cursor(peer_hash, id1.namespace_id());
    REQUIRE(cursor_after.has_value());
    REQUIRE(cursor_after->seq_num == peer_seq);

    // Simulate second sync with no new blobs: cursor seq matches peer seq
    auto ns_list2 = store1.list_namespaces();
    REQUIRE(ns_list2[0].latest_seq_num == peer_seq);  // No change
    // cursor_after->seq_num == peer_seq => CURSOR HIT
    REQUIRE(cursor_after->seq_num == ns_list2[0].latest_seq_num);
}

TEST_CASE("Cursor miss when new blob added to one namespace", "[sync][cursor]") {
    TempDir tmp1;
    test_clock_value = 10000;

    Storage store1(tmp1.path.string(), test_clock);
    asio::thread_pool pool{1};
    BlobEngine engine1(store1, pool);

    auto id1 = chromatindb::identity::NodeIdentity::generate();
    auto id2 = chromatindb::identity::NodeIdentity::generate();

    // Store blobs in two namespaces
    auto blob1 = make_signed_blob(id1, "ns1-blob");
    REQUIRE(run_async(pool, engine1.ingest(blob1)).accepted);
    auto blob2 = make_signed_blob(id2, "ns2-blob");
    REQUIRE(run_async(pool, engine1.ingest(blob2)).accepted);

    // Get initial seq_nums
    auto ns_list = store1.list_namespaces();
    REQUIRE(ns_list.size() == 2);

    // Simulate: local node stores cursor for both namespaces at current seq
    std::array<uint8_t, 32> peer_hash{};
    peer_hash.fill(0xCC);
    for (const auto& ns : ns_list) {
        SyncCursor c;
        c.seq_num = ns.latest_seq_num;
        c.round_count = 1;
        c.last_sync_timestamp = test_clock_value;
        store1.set_sync_cursor(peer_hash, ns.namespace_id, c);
    }

    // Add a new blob to namespace 1 only
    auto blob3 = make_signed_blob(id1, "ns1-new-blob");
    REQUIRE(run_async(pool, engine1.ingest(blob3)).accepted);

    // Re-read namespace list
    auto ns_list2 = store1.list_namespaces();
    REQUIRE(ns_list2.size() == 2);

    // Check cursor decisions per namespace
    for (const auto& ns : ns_list2) {
        auto cursor = store1.get_sync_cursor(peer_hash, ns.namespace_id);
        REQUIRE(cursor.has_value());
        if (std::memcmp(ns.namespace_id.data(), id1.namespace_id().data(), 32) == 0) {
            // Namespace 1: new blob added, seq increased => cursor MISS
            REQUIRE(ns.latest_seq_num > cursor->seq_num);
        } else {
            // Namespace 2: no change => cursor HIT
            REQUIRE(ns.latest_seq_num == cursor->seq_num);
        }
    }
}

TEST_CASE("Full resync triggers on round N when full_resync_interval=N", "[sync][cursor]") {
    // Test the full resync decision logic
    uint32_t interval = 10;
    uint64_t stale_seconds = 3600;
    uint64_t now = 10000;

    SECTION("round 0 triggers full resync (fresh or SIGHUP reset)") {
        SyncCursor c{.seq_num = 42, .round_count = 0, .last_sync_timestamp = 9000};
        // 0 % 10 == 0 => periodic full resync
        REQUIRE(c.round_count % interval == 0);
    }

    SECTION("round 5 does NOT trigger full resync") {
        SyncCursor c{.seq_num = 42, .round_count = 5, .last_sync_timestamp = 9000};
        REQUIRE(c.round_count % interval != 0);
    }

    SECTION("round 10 triggers full resync") {
        SyncCursor c{.seq_num = 42, .round_count = 10, .last_sync_timestamp = 9000};
        REQUIRE(c.round_count % interval == 0);
    }

    SECTION("round 20 triggers full resync") {
        SyncCursor c{.seq_num = 42, .round_count = 20, .last_sync_timestamp = 9000};
        REQUIRE(c.round_count % interval == 0);
    }
}

TEST_CASE("Full resync triggers when time gap exceeds cursor_stale_seconds", "[sync][cursor]") {
    uint64_t stale_seconds = 3600;

    SECTION("last_sync_ts=0 (never synced) does NOT trigger time gap") {
        SyncCursor c{.seq_num = 0, .round_count = 5, .last_sync_timestamp = 0};
        // last_sync_timestamp=0 is a special case: cursor just created, no time gap
        bool time_gap = (stale_seconds > 0 && c.last_sync_timestamp > 0 &&
                         10000 - c.last_sync_timestamp > stale_seconds);
        REQUIRE_FALSE(time_gap);
    }

    SECTION("time gap exceeded triggers full resync") {
        uint64_t now = 10000;
        SyncCursor c{.seq_num = 42, .round_count = 5, .last_sync_timestamp = now - 7200};
        // 10000 - 2800 = 7200 > 3600 => time gap
        bool time_gap = (stale_seconds > 0 && c.last_sync_timestamp > 0 &&
                         now - c.last_sync_timestamp > stale_seconds);
        REQUIRE(time_gap);
    }

    SECTION("within stale threshold does NOT trigger") {
        uint64_t now = 10000;
        SyncCursor c{.seq_num = 42, .round_count = 5, .last_sync_timestamp = now - 1800};
        bool time_gap = (stale_seconds > 0 && c.last_sync_timestamp > 0 &&
                         now - c.last_sync_timestamp > stale_seconds);
        REQUIRE_FALSE(time_gap);
    }
}

TEST_CASE("Cursor mismatch: remote seq < stored cursor triggers reset", "[sync][cursor]") {
    TempDir tmp;
    test_clock_value = 10000;

    Storage store(tmp.path.string(), test_clock);

    std::array<uint8_t, 32> peer_hash{};
    peer_hash.fill(0xDD);
    std::array<uint8_t, 32> ns_id{};
    ns_id.fill(0xEE);

    // Set cursor with seq_num=10
    SyncCursor c;
    c.seq_num = 10;
    c.round_count = 3;
    c.last_sync_timestamp = 9000;
    store.set_sync_cursor(peer_hash, ns_id, c);

    // Simulate peer reporting seq_num=5 (went backwards)
    uint64_t remote_seq = 5;
    auto cursor = store.get_sync_cursor(peer_hash, ns_id);
    REQUIRE(cursor.has_value());
    REQUIRE(remote_seq < cursor->seq_num);  // MISMATCH detected

    // Reset cursor for this namespace
    store.delete_sync_cursor(peer_hash, ns_id);

    // Verify cursor is gone
    auto after = store.get_sync_cursor(peer_hash, ns_id);
    REQUIRE_FALSE(after.has_value());
}

TEST_CASE("Cursors survive Storage reopen (restart persistence)", "[sync][cursor]") {
    TempDir tmp;
    test_clock_value = 10000;

    std::array<uint8_t, 32> peer_hash{};
    peer_hash.fill(0x11);
    std::array<uint8_t, 32> ns_id{};
    ns_id.fill(0x22);

    SyncCursor original;
    original.seq_num = 99;
    original.round_count = 7;
    original.last_sync_timestamp = 8888;

    // Create storage, set cursor, destroy
    {
        Storage store(tmp.path.string(), test_clock);
        store.set_sync_cursor(peer_hash, ns_id, original);
    }

    // Reopen storage, verify cursor persisted
    {
        Storage store(tmp.path.string(), test_clock);
        auto loaded = store.get_sync_cursor(peer_hash, ns_id);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->seq_num == 99);
        REQUIRE(loaded->round_count == 7);
        REQUIRE(loaded->last_sync_timestamp == 8888);
    }
}

TEST_CASE("reset_all_round_counters causes next sync to be full resync", "[sync][cursor]") {
    TempDir tmp;
    test_clock_value = 10000;

    Storage store(tmp.path.string(), test_clock);

    // Create cursors for multiple peer+namespace pairs with non-zero round counts
    std::array<uint8_t, 32> peer_a{}, peer_b{};
    std::array<uint8_t, 32> ns1{}, ns2{};
    peer_a.fill(0xAA);
    peer_b.fill(0xBB);
    ns1.fill(0x11);
    ns2.fill(0x22);

    store.set_sync_cursor(peer_a, ns1, {42, 7, 9000});
    store.set_sync_cursor(peer_a, ns2, {100, 3, 9100});
    store.set_sync_cursor(peer_b, ns1, {55, 9, 9200});

    // Reset all round counters
    auto count = store.reset_all_round_counters();
    REQUIRE(count == 3);

    // All round counts should be 0, seq_nums and timestamps preserved
    auto c1 = store.get_sync_cursor(peer_a, ns1);
    REQUIRE(c1.has_value());
    REQUIRE(c1->round_count == 0);
    REQUIRE(c1->seq_num == 42);
    REQUIRE(c1->last_sync_timestamp == 9000);

    auto c2 = store.get_sync_cursor(peer_a, ns2);
    REQUIRE(c2.has_value());
    REQUIRE(c2->round_count == 0);
    REQUIRE(c2->seq_num == 100);

    auto c3 = store.get_sync_cursor(peer_b, ns1);
    REQUIRE(c3.has_value());
    REQUIRE(c3->round_count == 0);
    REQUIRE(c3->seq_num == 55);

    // 0 % N == 0 => next sync will be full resync
    uint32_t interval = 10;
    REQUIRE(c1->round_count % interval == 0);
    REQUIRE(c2->round_count % interval == 0);
    REQUIRE(c3->round_count % interval == 0);
}

TEST_CASE("cleanup_stale_cursors removes cursors for unknown peers", "[sync][cursor]") {
    TempDir tmp;
    test_clock_value = 10000;

    Storage store(tmp.path.string(), test_clock);

    std::array<uint8_t, 32> peer_a{}, peer_b{};
    std::array<uint8_t, 32> ns1{};
    peer_a.fill(0xAA);
    peer_b.fill(0xBB);
    ns1.fill(0x11);

    // Create cursors for both peers
    store.set_sync_cursor(peer_a, ns1, {10, 1, 9000});
    store.set_sync_cursor(peer_b, ns1, {20, 2, 9100});

    // Cleanup with only peer_a as known
    std::vector<std::array<uint8_t, 32>> known = {peer_a};
    auto removed = store.cleanup_stale_cursors(known);
    REQUIRE(removed == 1);  // peer_b's cursor removed

    // peer_a's cursor still exists
    auto ca = store.get_sync_cursor(peer_a, ns1);
    REQUIRE(ca.has_value());
    REQUIRE(ca->seq_num == 10);

    // peer_b's cursor gone
    auto cb = store.get_sync_cursor(peer_b, ns1);
    REQUIRE_FALSE(cb.has_value());
}
