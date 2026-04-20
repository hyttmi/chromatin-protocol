#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>
#include <cstring>
#include <ctime>
#include <thread>

#include "db/acl/access_control.h"
#include "db/config/config.h"
#include "db/crypto/hash.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/peer/peer_manager.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

#include "db/tests/test_helpers.h"

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

namespace fs = std::filesystem;

namespace {

/// Run an awaitable synchronously using a temporary io_context.
template <typename T>
T run_async(asio::thread_pool& pool, asio::awaitable<T> aw) {
    asio::io_context ioc;
    T result{};
    asio::co_spawn(ioc, [&result, a = std::move(aw)]() mutable -> asio::awaitable<T> {
        result = co_await std::move(a);
        co_return result;
    }, asio::detached);
    ioc.run();
    return result;
}

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

std::string listening_address(uint16_t port) {
    return "127.0.0.1:" + std::to_string(port);
}

chromatindb::wire::BlobData make_signed_blob(
    const chromatindb::identity::NodeIdentity& id,
    const std::string& payload,
    uint32_t ttl = 604800,
    uint64_t timestamp = 0)
{
    chromatindb::wire::BlobData blob;
    // Post-122: signer_hint = SHA3(id.public_key()) = id.namespace_id() for owner writes.
    auto hint = chromatindb::crypto::sha3_256(id.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data.assign(payload.begin(), payload.end());
    blob.ttl = ttl;
    blob.timestamp = (timestamp == 0)
        ? static_cast<uint64_t>(std::time(nullptr))
        : timestamp;

    // target_namespace (the caller-carried ns) absorbs into sponge per D-01.
    // For owner writes, target_namespace = id.namespace_id() = signer_hint.
    auto signing_input = chromatindb::wire::build_signing_input(
        id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}

} // anonymous namespace

using chromatindb::acl::AccessControl;
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
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();
    cfg.bootstrap_peers = {"192.0.2.1:4200"};  // RFC 5737 TEST-NET

    auto id = NodeIdentity::load_or_generate(tmp.path);
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // Phase 122 auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, id.namespace_id());
    PeerManager pm(cfg, id, eng, store, ioc, pool, acl);

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
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 1;
    cfg1.max_peers = 32;

    // Node 2 config -- bootstrap to node 1 (port set after pm1.start())
    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 1;
    cfg2.max_peers = 32;

    // Create identities
    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    // Create storages + engines
    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // Phase 122-07: cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    // Use current time for blob timestamps so they are not considered expired during sync
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob in node1 (signed by id1)
    auto blob1 = make_signed_blob(id1, "e2e-from-node1", 604800, now);
    auto r1 = run_async(pool, eng1.ingest(std::span<const uint8_t, 32>(id1.namespace_id()), blob1));
    REQUIRE(r1.accepted);

    // Store a different blob in node2 (signed by id2) -- tests bidirectional sync
    auto blob2 = make_signed_blob(id2, "e2e-from-node2", 604800, now + 1);
    auto r2 = run_async(pool, eng2.ingest(std::span<const uint8_t, 32>(id2.namespace_id()), blob2));
    REQUIRE(r2.accepted);

    // Create PeerManagers on shared io_context
    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());
    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Run for enough time to connect + complete full sync protocol exchange
    // (SyncRequest -> SyncAccept -> NamespaceList -> Reconciliation -> SyncComplete -> BlobRequest -> BlobTransfer)
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
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 1;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 1;
    cfg2.max_peers = 32;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // Phase 122-07: cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob that is valid now but will expire before sync (TTL=1 second).
    // Engine accepts it at ingest, but sync 2+ seconds later sees it as expired.
    auto expired_blob = make_signed_blob(id1, "should-not-sync", 1, now);
    auto r1 = run_async(pool, eng1.ingest(std::span<const uint8_t, 32>(id1.namespace_id()), expired_blob));
    REQUIRE(r1.accepted);
    // Wait 2s so the blob expires before sync begins
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Store a valid blob in node1 -- proves sync works for non-expired blobs
    auto valid_blob = make_signed_blob(id1, "should-sync", 604800, now);
    auto r2 = run_async(pool, eng1.ingest(std::span<const uint8_t, 32>(id1.namespace_id()), valid_blob));
    REQUIRE(r2.accepted);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());
    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
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
    cfg_a.bind_address = "127.0.0.1:0";
    cfg_a.data_dir = tmp1.path.string();
    cfg_a.safety_net_interval_seconds = 1;
    cfg_a.max_peers = 32;

    // Node B config -- bootstraps to A (port set after pm_a.start())
    Config cfg_b;
    cfg_b.bind_address = "127.0.0.1:0";
    cfg_b.data_dir = tmp2.path.string();
    cfg_b.safety_net_interval_seconds = 1;
    cfg_b.max_peers = 32;

    // Node C config -- bootstraps to B only (port set after pm_b.start())
    Config cfg_c;
    cfg_c.bind_address = "127.0.0.1:0";
    cfg_c.data_dir = tmp3.path.string();
    cfg_c.safety_net_interval_seconds = 1;
    cfg_c.max_peers = 32;

    // Create identities
    auto id_a = NodeIdentity::load_or_generate(tmp1.path);
    auto id_b = NodeIdentity::load_or_generate(tmp2.path);
    auto id_c = NodeIdentity::load_or_generate(tmp3.path);

    // Create storages + engines
    Storage store_a(tmp1.path.string());
    Storage store_b(tmp2.path.string());
    Storage store_c(tmp3.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng_a(store_a, pool);
    BlobEngine eng_b(store_b, pool);
    BlobEngine eng_c(store_c, pool);
    // Phase 122-07: cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store_a, id_a);
    chromatindb::test::register_pubk(store_a, id_b);
    chromatindb::test::register_pubk(store_a, id_c);
    chromatindb::test::register_pubk(store_b, id_a);
    chromatindb::test::register_pubk(store_b, id_b);
    chromatindb::test::register_pubk(store_b, id_c);
    chromatindb::test::register_pubk(store_c, id_a);
    chromatindb::test::register_pubk(store_c, id_b);
    chromatindb::test::register_pubk(store_c, id_c);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob in Node A -- Node C should eventually get it through discovery
    auto blob_a = make_signed_blob(id_a, "from-node-a", 604800, now);
    auto r_a = run_async(pool, eng_a.ingest(std::span<const uint8_t, 32>(id_a.namespace_id()), blob_a));
    REQUIRE(r_a.accepted);

    // Create PeerManagers on shared io_context
    asio::io_context ioc;
    AccessControl acl_a({}, cfg_a.allowed_peer_keys, id_a.namespace_id());
    AccessControl acl_b({}, cfg_b.allowed_peer_keys, id_b.namespace_id());
    AccessControl acl_c({}, cfg_c.allowed_peer_keys, id_c.namespace_id());
    PeerManager pm_a(cfg_a, id_a, eng_a, store_a, ioc, pool, acl_a);
    pm_a.start();
    cfg_b.bootstrap_peers = {listening_address(pm_a.listening_port())};
    PeerManager pm_b(cfg_b, id_b, eng_b, store_b, ioc, pool, acl_b);
    pm_b.start();

    // Let B connect to A and exchange peer lists
    ioc.run_for(std::chrono::seconds(5));

    // Now start C (after B has connected to A and learned about A)
    cfg_c.bootstrap_peers = {listening_address(pm_b.listening_port())};
    PeerManager pm_c(cfg_c, id_c, eng_c, store_c, ioc, pool, acl_c);
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
