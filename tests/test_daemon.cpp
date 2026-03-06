#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>
#include <cstring>
#include <ctime>

#include "db/config/config.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/peer/peer_manager.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

#include <asio.hpp>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        path = fs::temp_directory_path() /
               ("chromatindb_test_daemon_" + std::to_string(dist(gen)));
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

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

    auto signing_input = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}

} // anonymous namespace

using chromatindb::config::Config;
using chromatindb::engine::BlobEngine;
using chromatindb::identity::NodeIdentity;
using chromatindb::peer::PeerManager;
using chromatindb::storage::Storage;

// ============================================================================
// Keygen tests
// ============================================================================

TEST_CASE("keygen creates identity files", "[daemon]") {
    TempDir tmp;
    fs::create_directories(tmp.path);

    // Ensure no identity exists
    REQUIRE_FALSE(fs::exists(tmp.path / "node.key"));
    REQUIRE_FALSE(fs::exists(tmp.path / "node.pub"));

    auto identity = NodeIdentity::generate();
    identity.save_to(tmp.path);

    REQUIRE(fs::exists(tmp.path / "node.key"));
    REQUIRE(fs::exists(tmp.path / "node.pub"));

    // Load and verify same namespace
    auto loaded = NodeIdentity::load_from(tmp.path);
    auto orig_ns = identity.namespace_id();
    auto loaded_ns = loaded.namespace_id();

    REQUIRE(std::equal(orig_ns.begin(), orig_ns.end(), loaded_ns.begin()));
}

TEST_CASE("keygen load_or_generate preserves existing", "[daemon]") {
    TempDir tmp;
    fs::create_directories(tmp.path);

    // Generate first identity
    auto id1 = NodeIdentity::load_or_generate(tmp.path);
    auto ns1 = id1.namespace_id();

    // Load again -- should be same identity
    auto id2 = NodeIdentity::load_or_generate(tmp.path);
    auto ns2 = id2.namespace_id();

    REQUIRE(std::equal(ns1.begin(), ns1.end(), ns2.begin()));
}

// ============================================================================
// Daemon startup resilience
// ============================================================================

TEST_CASE("daemon starts with unreachable bootstrap peers", "[daemon]") {
    TempDir tmp;

    Config cfg;
    cfg.bind_address = "127.0.0.1:14220";
    cfg.data_dir = tmp.path.string();
    cfg.bootstrap_peers = {"192.0.2.1:4200"};  // RFC 5737 TEST-NET

    auto id = NodeIdentity::load_or_generate(tmp.path);
    Storage store(tmp.path.string());
    BlobEngine eng(store);

    asio::io_context ioc;
    PeerManager pm(cfg, id, eng, store, ioc);

    // Should not throw
    REQUIRE_NOTHROW(pm.start());

    // Run briefly
    ioc.run_for(std::chrono::seconds(2));

    pm.stop();
    ioc.run_for(std::chrono::seconds(1));
}

// ============================================================================
// Two-node E2E sync
// ============================================================================

TEST_CASE("two nodes sync blobs end-to-end", "[daemon][e2e]") {
    TempDir tmp1, tmp2;

    // Node 1 config
    Config cfg1;
    cfg1.bind_address = "127.0.0.1:14230";
    cfg1.data_dir = tmp1.path.string();
    cfg1.sync_interval_seconds = 1;
    cfg1.max_peers = 32;

    // Node 2 config -- bootstrap to node 1
    Config cfg2;
    cfg2.bind_address = "127.0.0.1:14231";
    cfg2.data_dir = tmp2.path.string();
    cfg2.bootstrap_peers = {"127.0.0.1:14230"};
    cfg2.sync_interval_seconds = 1;
    cfg2.max_peers = 32;

    // Create identities
    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    // Create storages + engines
    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    BlobEngine eng1(store1);
    BlobEngine eng2(store2);

    // Use current time for blob timestamps so they are not considered expired during sync
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob in node1 (signed by id1)
    auto blob1 = make_signed_blob(id1, "e2e-from-node1", 604800, now);
    auto r1 = eng1.ingest(blob1);
    REQUIRE(r1.accepted);

    // Store a different blob in node2 (signed by id2) -- tests bidirectional sync
    auto blob2 = make_signed_blob(id2, "e2e-from-node2", 604800, now + 1);
    auto r2 = eng2.ingest(blob2);
    REQUIRE(r2.accepted);

    // Create PeerManagers on shared io_context
    asio::io_context ioc;
    PeerManager pm1(cfg1, id1, eng1, store1, ioc);
    PeerManager pm2(cfg2, id2, eng2, store2, ioc);

    pm1.start();
    pm2.start();

    // Run for enough time to connect + complete full sync protocol exchange
    // (SyncRequest -> SyncAccept -> NamespaceList -> HashLists -> SyncComplete -> BlobRequest -> BlobTransfer)
    ioc.run_for(std::chrono::seconds(8));

    // Verify node2 received node1's blob (SYNC-01 + SYNC-02: hash-list diff + bidirectional)
    auto n2_has_n1 = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_has_n1.size() == 1);
    REQUIRE(n2_has_n1[0].data == blob1.data);

    // Verify node1 received node2's blob (SYNC-02: bidirectional sync)
    auto n1_has_n2 = eng1.get_blobs_since(id2.namespace_id(), 0);
    REQUIRE(n1_has_n2.size() == 1);
    REQUIRE(n1_has_n2[0].data == blob2.data);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("expired blobs not synced between nodes", "[daemon][e2e]") {
    TempDir tmp1, tmp2;

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:14232";
    cfg1.data_dir = tmp1.path.string();
    cfg1.sync_interval_seconds = 1;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:14233";
    cfg2.data_dir = tmp2.path.string();
    cfg2.bootstrap_peers = {"127.0.0.1:14232"};
    cfg2.sync_interval_seconds = 1;
    cfg2.max_peers = 32;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    BlobEngine eng1(store1);
    BlobEngine eng2(store2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store an expired blob in node1 (ttl=1, timestamp=1 => expired a long time ago)
    auto expired_blob = make_signed_blob(id1, "should-not-sync", 1, 1);
    auto r1 = eng1.ingest(expired_blob);
    REQUIRE(r1.accepted);

    // Store a valid blob in node1 -- proves sync works for non-expired blobs
    auto valid_blob = make_signed_blob(id1, "should-sync", 604800, now);
    auto r2 = eng1.ingest(valid_blob);
    REQUIRE(r2.accepted);

    asio::io_context ioc;
    PeerManager pm1(cfg1, id1, eng1, store1, ioc);
    PeerManager pm2(cfg2, id2, eng2, store2, ioc);

    pm1.start();
    pm2.start();

    // Run for enough time to complete sync exchange
    ioc.run_for(std::chrono::seconds(8));

    // Node2 should have the valid blob but NOT the expired one (SYNC-03)
    // SyncProtocol filters expired blobs from hash lists
    auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs.size() == 1);
    REQUIRE(n2_blobs[0].data == valid_blob.data);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

// ============================================================================
// Three-node peer discovery via PEX (DISC-02)
// ============================================================================

TEST_CASE("three nodes: peer discovery via PEX", "[daemon][e2e][pex]") {
    TempDir tmp1, tmp2, tmp3;

    // Node A config -- standalone, no bootstrap
    Config cfg_a;
    cfg_a.bind_address = "127.0.0.1:14240";
    cfg_a.data_dir = tmp1.path.string();
    cfg_a.sync_interval_seconds = 1;
    cfg_a.max_peers = 32;

    // Node B config -- bootstraps to A
    Config cfg_b;
    cfg_b.bind_address = "127.0.0.1:14241";
    cfg_b.data_dir = tmp2.path.string();
    cfg_b.bootstrap_peers = {"127.0.0.1:14240"};
    cfg_b.sync_interval_seconds = 1;
    cfg_b.max_peers = 32;

    // Node C config -- bootstraps to B only (does NOT know A)
    Config cfg_c;
    cfg_c.bind_address = "127.0.0.1:14242";
    cfg_c.data_dir = tmp3.path.string();
    cfg_c.bootstrap_peers = {"127.0.0.1:14241"};
    cfg_c.sync_interval_seconds = 1;
    cfg_c.max_peers = 32;

    // Create identities
    auto id_a = NodeIdentity::load_or_generate(tmp1.path);
    auto id_b = NodeIdentity::load_or_generate(tmp2.path);
    auto id_c = NodeIdentity::load_or_generate(tmp3.path);

    // Create storages + engines
    Storage store_a(tmp1.path.string());
    Storage store_b(tmp2.path.string());
    Storage store_c(tmp3.path.string());
    BlobEngine eng_a(store_a);
    BlobEngine eng_b(store_b);
    BlobEngine eng_c(store_c);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob in Node A -- Node C should eventually get it through discovery
    auto blob_a = make_signed_blob(id_a, "from-node-a", 604800, now);
    auto r_a = eng_a.ingest(blob_a);
    REQUIRE(r_a.accepted);

    // Create PeerManagers on shared io_context
    asio::io_context ioc;
    PeerManager pm_a(cfg_a, id_a, eng_a, store_a, ioc);
    PeerManager pm_b(cfg_b, id_b, eng_b, store_b, ioc);
    PeerManager pm_c(cfg_c, id_c, eng_c, store_c, ioc);

    // Start nodes: A first, then B, then C (natural bootstrap order)
    pm_a.start();
    pm_b.start();

    // Let B connect to A and exchange peer lists
    ioc.run_for(std::chrono::seconds(5));

    // Now start C (after B has connected to A and learned about A)
    pm_c.start();

    // Give enough time for:
    // 1. C connects to B (bootstrap)
    // 2. C sends PeerListRequest to B
    // 3. B responds with A's address
    // 4. C connects to A via connect_once
    // 5. Sync runs between C and A
    ioc.run_for(std::chrono::seconds(12));

    // DISC-02: Node C should have discovered Node A through B's peer list
    // Verify: Node C has Node A's blob (proving discovery + sync worked)
    auto c_has_a = eng_c.get_blobs_since(id_a.namespace_id(), 0);
    REQUIRE(c_has_a.size() == 1);
    REQUIRE(c_has_a[0].data == blob_a.data);

    // Also verify B has A's blob (basic sync still works)
    auto b_has_a = eng_b.get_blobs_since(id_a.namespace_id(), 0);
    REQUIRE(b_has_a.size() == 1);

    // Verify C has more than just B as a peer (it should have discovered A)
    // C bootstrapped to B only, so if peer_count > 1, discovery worked
    REQUIRE(pm_c.peer_count() >= 2);

    pm_a.stop();
    pm_b.stop();
    pm_c.stop();
    ioc.run_for(std::chrono::seconds(2));
}
