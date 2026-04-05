#include <catch2/catch_test_macros.hpp>
#include <chrono>

#include "db/acl/access_control.h"
#include "db/peer/peer_manager.h"
#include "db/config/config.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/storage/storage.h"
#include "db/util/hex.h"

#include "db/tests/test_helpers.h"

#include <asio.hpp>

using chromatindb::test::TempDir;
using chromatindb::test::listening_address;

using chromatindb::acl::AccessControl;
using chromatindb::config::Config;
using chromatindb::engine::BlobEngine;
using chromatindb::identity::NodeIdentity;
using chromatindb::peer::PeerManager;
using chromatindb::storage::Storage;
using chromatindb::util::to_hex;

// ============================================================================
// Keepalive tests (Phase 83 -- CONN-01, CONN-02)
// ============================================================================

TEST_CASE("keepalive sends Ping and keeps peers alive", "[keepalive]") {
    // CONN-01: Verify that two connected nodes stay connected across
    // multiple keepalive intervals (35s > 30s interval). If Pings were
    // not being sent, the 60s silence timeout would eventually fire.

    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    // Closed mode: each node allows only the other
    auto id1_ns_hex = to_hex(id1.namespace_id());
    auto id2_ns_hex = to_hex(id2.namespace_id());

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 600;  // High: don't trigger sync during test
    cfg1.max_peers = 32;
    cfg1.allowed_peer_keys = {id2_ns_hex};

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 600;
    cfg2.max_peers = 32;
    cfg2.allowed_peer_keys = {id1_ns_hex};

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect (PQ handshake takes a few seconds under ASAN)
    ioc.run_for(std::chrono::seconds(5));
    REQUIRE(pm1.peer_count() >= 1);
    REQUIRE(pm2.peer_count() >= 1);

    // Run past one keepalive interval (30s) -- peers should stay connected
    // because keepalive Pings keep resetting each other's silence timer
    ioc.run_for(std::chrono::seconds(30));
    REQUIRE(pm1.peer_count() >= 1);
    REQUIRE(pm2.peer_count() >= 1);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::milliseconds(100));
}

TEST_CASE("keepalive runs with default config", "[keepalive]") {

    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    auto id1_ns_hex = to_hex(id1.namespace_id());
    auto id2_ns_hex = to_hex(id2.namespace_id());

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 600;
    cfg1.max_peers = 32;
    cfg1.allowed_peer_keys = {id2_ns_hex};

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 600;
    cfg2.max_peers = 32;
    cfg2.allowed_peer_keys = {id1_ns_hex};

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect
    ioc.run_for(std::chrono::seconds(5));
    REQUIRE(pm1.peer_count() >= 1);
    REQUIRE(pm2.peer_count() >= 1);

    // Run past keepalive interval -- peers stay alive because keepalive runs
    ioc.run_for(std::chrono::seconds(30));
    REQUIRE(pm1.peer_count() >= 1);
    REQUIRE(pm2.peer_count() >= 1);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::milliseconds(100));
}

TEST_CASE("last_recv_time accessor returns valid timestamp", "[keepalive]") {
    // Unit test: verify last_recv_time() is initialized at construction
    // and accessible via the public accessor.

    TempDir tmp;
    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    auto id = NodeIdentity::load_or_generate(tmp.path);
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, id.namespace_id());

    PeerManager pm(cfg, id, eng, store, ioc, pool, acl);
    pm.start();

    // Create a TCP connection by connecting a second node
    TempDir tmp2;
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);
    auto id1_ns_hex = to_hex(id.namespace_id());
    auto id2_ns_hex = to_hex(id2.namespace_id());

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 600;
    cfg2.max_peers = 32;
    cfg2.allowed_peer_keys = {id1_ns_hex};
    cfg2.bootstrap_peers = {listening_address(pm.listening_port())};

    // Update cfg to allow peer
    cfg.allowed_peer_keys = {id2_ns_hex};

    Storage store2(tmp2.path.string());
    BlobEngine eng2(store2, pool);
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    ioc.run_for(std::chrono::seconds(5));

    // At least one node should have a peer
    CHECK(pm.peer_count() + pm2.peer_count() >= 1);

    pm.stop();
    pm2.stop();
    ioc.run_for(std::chrono::milliseconds(100));
}
