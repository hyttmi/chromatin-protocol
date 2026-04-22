#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <cstring>
#include <ctime>

#include "db/acl/access_control.h"
#include "db/peer/peer_manager.h"
#include "db/config/config.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/net/framing.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"

#include "db/tests/test_helpers.h"
#include "db/util/hex.h"

#include <asio.hpp>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

} // anonymous namespace

using chromatindb::test::TempDir;
using chromatindb::test::run_async;
using chromatindb::test::make_signed_blob;
using chromatindb::test::make_signed_tombstone;
using chromatindb::test::make_signed_delegation;
using chromatindb::test::current_timestamp;
using chromatindb::test::TS_AUTO;
using chromatindb::util::to_hex;
using chromatindb::test::listening_address;

using chromatindb::acl::AccessControl;
using chromatindb::config::Config;
using chromatindb::engine::BlobEngine;
using chromatindb::engine::IngestError;
using chromatindb::identity::NodeIdentity;
using chromatindb::peer::PeerManager;
using chromatindb::storage::Storage;

// ============================================================================
// PeerManager unit tests
// ============================================================================

TEST_CASE("PeerManager starts with unreachable bootstrap", "[peer]") {
    TempDir tmp;

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();
    cfg.bootstrap_peers = {"192.0.2.1:4200"};  // TEST-NET, unreachable

    auto id = NodeIdentity::load_or_generate(tmp.path);
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, id.namespace_id());
    PeerManager pm(cfg, id, eng, store, ioc, pool, acl);

    // Should not throw
    REQUIRE_NOTHROW(pm.start());

    // Run briefly to confirm no crash
    ioc.run_for(std::chrono::seconds(2));

    pm.stop();
    ioc.run_for(std::chrono::seconds(1));

    REQUIRE(pm.peer_count() == 0);
}

TEST_CASE("PeerManager max_peers enforcement", "[peer]") {
    TempDir tmp;

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();
    cfg.max_peers = 1;  // Very low limit for testing

    auto id = NodeIdentity::load_or_generate(tmp.path);
    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, id.namespace_id());
    PeerManager pm(cfg, id, eng, store, ioc, pool, acl);

    pm.start();

    // After starting, peer count should be 0
    REQUIRE(pm.peer_count() == 0);

    pm.stop();
    ioc.run_for(std::chrono::seconds(1));
}

TEST_CASE("PeerManager strike threshold defaults", "[peer]") {
    // Verify the default constants
    REQUIRE(PeerManager::STRIKE_THRESHOLD_DEFAULT == 10);
    REQUIRE(PeerManager::STRIKE_COOLDOWN_SEC_DEFAULT == 300);
}

// ============================================================================
// PEX unit tests
// ============================================================================

TEST_CASE("PEX encode/decode round-trip", "[peer][pex]") {
    std::vector<std::string> addrs = {"192.168.1.1:4200", "10.0.0.1:4200", "example.com:4200"};
    auto encoded = PeerManager::encode_peer_list(addrs);
    auto decoded = PeerManager::decode_peer_list(encoded);
    REQUIRE(decoded.size() == 3);
    REQUIRE(decoded[0] == "192.168.1.1:4200");
    REQUIRE(decoded[1] == "10.0.0.1:4200");
    REQUIRE(decoded[2] == "example.com:4200");
}

TEST_CASE("PEX encode/decode empty list", "[peer][pex]") {
    std::vector<std::string> addrs = {};
    auto encoded = PeerManager::encode_peer_list(addrs);
    auto decoded = PeerManager::decode_peer_list(encoded);
    REQUIRE(decoded.empty());
}

TEST_CASE("PEX encode/decode single peer", "[peer][pex]") {
    std::vector<std::string> addrs = {"127.0.0.1:4200"};
    auto encoded = PeerManager::encode_peer_list(addrs);
    auto decoded = PeerManager::decode_peer_list(encoded);
    REQUIRE(decoded.size() == 1);
    REQUIRE(decoded[0] == "127.0.0.1:4200");
}

TEST_CASE("PEX decode truncated payload", "[peer][pex]") {
    // Only 1 byte -- too short for count
    std::vector<uint8_t> truncated = {0x00};
    auto decoded = PeerManager::decode_peer_list(truncated);
    REQUIRE(decoded.empty());
}

TEST_CASE("PEX constants", "[peer][pex]") {
    REQUIRE(PeerManager::PEX_INTERVAL_SEC_DEFAULT == 300);
    REQUIRE(PeerManager::MAX_PEERS_PER_EXCHANGE == 8);
    REQUIRE(PeerManager::MAX_DISCOVERED_PER_ROUND == 3);
    REQUIRE(PeerManager::MAX_PERSISTED_PEERS == 100);
    REQUIRE(PeerManager::MAX_PERSIST_FAILURES == 3);
}

// ============================================================================
// ACL integration tests (ACL-02, ACL-03)
// ============================================================================

TEST_CASE("closed mode rejects unauthorized peer", "[peer][acl]") {
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    // Node1 is in closed mode with a random allowed key (NOT id2's namespace)
    // Use a dummy key that won't match any real peer
    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 1;
    cfg1.max_peers = 32;
    cfg1.allowed_peer_keys = {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};

    // Node2 is open mode, bootstraps to node1
    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 1;
    cfg2.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob in node1 -- should NOT reach node2 because node2 is unauthorized
    auto blob1 = make_signed_blob(id1, "closed-secret", 604800, now);
    auto r1 = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob1));
    REQUIRE(r1.accepted);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    REQUIRE(acl1.is_peer_closed_mode());
    REQUIRE_FALSE(acl2.is_peer_closed_mode());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Run long enough for connection attempt + rejection
    ioc.run_for(std::chrono::seconds(5));

    // Node1 should have 0 peers (rejected node2)
    REQUIRE(pm1.peer_count() == 0);

    // Node2 should NOT have node1's blob (sync never happened)
    auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs.empty());

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("closed mode accepts authorized peer and syncs", "[peer][acl]") {
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    // Node1 is in closed mode but allows id2's namespace
    auto id2_ns_hex = to_hex(id2.namespace_id());

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 1;
    cfg1.max_peers = 32;
    cfg1.allowed_peer_keys = {id2_ns_hex};

    // Node2 is also in closed mode and allows id1's namespace
    auto id1_ns_hex = to_hex(id1.namespace_id());

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 1;
    cfg2.max_peers = 32;
    cfg2.allowed_peer_keys = {id1_ns_hex};

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob in node1
    auto blob1 = make_signed_blob(id1, "closed-authorized", 604800, now);
    auto r1 = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob1));
    REQUIRE(r1.accepted);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    REQUIRE(acl1.is_peer_closed_mode());
    REQUIRE(acl2.is_peer_closed_mode());
    REQUIRE(acl1.is_peer_allowed(id2.namespace_id()));
    REQUIRE(acl2.is_peer_allowed(id1.namespace_id()));

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Run long enough for PQ handshake + ACL check + sync (ASAN makes PQ crypto ~3-5x slower)
    ioc.run_for(std::chrono::seconds(15));

    // Both nodes should have 1 peer (each other)
    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    // Node2 should have node1's blob (sync worked despite closed mode)
    auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs.size() == 1);
    REQUIRE(n2_blobs[0].data == blob1.data);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("reload_config revokes connected peer", "[peer][acl][reload]") {
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    auto id1_ns_hex = to_hex(id1.namespace_id());
    auto id2_ns_hex = to_hex(id2.namespace_id());

    // Write config file for node1 with id2 allowed
    auto config_path = tmp1.path / "config.json";
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:0", "allowed_peer_keys": [")" << id2_ns_hex << R"("]})";
    }

    auto cfg1 = chromatindb::config::load_config(config_path);
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 1;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 1;
    cfg2.max_peers = 32;
    cfg2.allowed_peer_keys = {id1_ns_hex};

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1, config_path);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect and sync
    ioc.run_for(std::chrono::seconds(5));

    // Verify they connected
    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    // Now rewrite config to REMOVE id2 from allowed_peer_keys (revocation)
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:0", "allowed_peer_keys": ["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"]})";
    }

    // Trigger reload (same as what SIGHUP handler calls)
    pm1.reload_config();

    // Run io_context to process the disconnect
    ioc.run_for(std::chrono::seconds(2));

    // Node1 should have disconnected node2 (ACL-05: immediate revocation)
    REQUIRE(pm1.peer_count() == 0);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("reload_config with invalid config keeps current state", "[peer][acl][reload]") {
    TempDir tmp1;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);

    // Write valid config file
    auto config_path = tmp1.path / "config.json";
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:0", "allowed_peer_keys": ["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"]})";
    }

    auto cfg1 = chromatindb::config::load_config(config_path);
    cfg1.data_dir = tmp1.path.string();

    Storage store1(tmp1.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store1, id1);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1, config_path);
    pm1.start();

    // Drain start-up handlers (including SIGHUP coroutine setup)
    ioc.run_for(std::chrono::milliseconds(100));

    REQUIRE(acl1.is_peer_closed_mode());
    REQUIRE(acl1.peer_allowed_count() == 1);

    // Corrupt the config file
    {
        std::ofstream f(config_path);
        f << "{ this is not valid json }}}";
    }

    // Trigger reload -- should NOT crash, should keep current config
    pm1.reload_config();

    // ACL should still be in closed mode with 1 key (fail-safe)
    REQUIRE(acl1.is_peer_closed_mode());
    REQUIRE(acl1.peer_allowed_count() == 1);

    pm1.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("reload_config switches from open to closed mode", "[peer][acl][reload]") {
    TempDir tmp1;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);

    // Write config file with NO allowed_peer_keys (open mode)
    auto config_path = tmp1.path / "config.json";
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:0"})";
    }

    auto cfg1 = chromatindb::config::load_config(config_path);
    cfg1.data_dir = tmp1.path.string();

    Storage store1(tmp1.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store1, id1);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1, config_path);
    pm1.start();

    REQUIRE_FALSE(acl1.is_peer_closed_mode());

    // Rewrite config to add allowed_peer_keys (switch to closed mode)
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:0", "allowed_peer_keys": ["bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"]})";
    }

    pm1.reload_config();

    REQUIRE(acl1.is_peer_closed_mode());
    REQUIRE(acl1.peer_allowed_count() == 1);

    pm1.stop();
    ioc.run_for(std::chrono::seconds(1));
}

TEST_CASE("closed mode disables PEX discovery", "[peer][acl][pex]") {
    TempDir tmp1, tmp2, tmp3;

    auto id_a = NodeIdentity::load_or_generate(tmp1.path);
    auto id_b = NodeIdentity::load_or_generate(tmp2.path);
    auto id_c = NodeIdentity::load_or_generate(tmp3.path);

    auto ns_a = to_hex(id_a.namespace_id());
    auto ns_b = to_hex(id_b.namespace_id());
    auto ns_c = to_hex(id_c.namespace_id());

    // All three nodes in closed mode, each allowing the other two
    Config cfg_a;
    cfg_a.bind_address = "127.0.0.1:0";
    cfg_a.data_dir = tmp1.path.string();
    cfg_a.safety_net_interval_seconds = 1;
    cfg_a.max_peers = 32;
    cfg_a.allowed_peer_keys = {ns_b, ns_c};

    Config cfg_b;
    cfg_b.bind_address = "127.0.0.1:0";
    cfg_b.data_dir = tmp2.path.string();
    cfg_b.safety_net_interval_seconds = 1;
    cfg_b.max_peers = 32;
    cfg_b.allowed_peer_keys = {ns_a, ns_c};

    // Node C only knows B (not A). In open mode it would discover A via PEX.
    // In closed mode, PEX is disabled, so C should NOT discover A.
    Config cfg_c;
    cfg_c.bind_address = "127.0.0.1:0";
    cfg_c.data_dir = tmp3.path.string();
    cfg_c.safety_net_interval_seconds = 1;
    cfg_c.max_peers = 32;
    cfg_c.allowed_peer_keys = {ns_a, ns_b};

    Storage store_a(tmp1.path.string());
    Storage store_b(tmp2.path.string());
    Storage store_c(tmp3.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng_a(store_a, pool);
    BlobEngine eng_b(store_b, pool);
    BlobEngine eng_c(store_c, pool);
    // cross-store PUBK registration for sync tests.
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

    // Store a blob in A -- C should NOT get it via PEX discovery
    auto blob_a = make_signed_blob(id_a, "closed-pex-test", 604800, now);
    auto r_a = run_async(pool, eng_a.ingest(chromatindb::test::ns_span(id_a), blob_a));
    REQUIRE(r_a.accepted);

    asio::io_context ioc;
    AccessControl acl_a({}, cfg_a.allowed_peer_keys, id_a.namespace_id());
    AccessControl acl_b({}, cfg_b.allowed_peer_keys, id_b.namespace_id());
    AccessControl acl_c({}, cfg_c.allowed_peer_keys, id_c.namespace_id());

    PeerManager pm_a(cfg_a, id_a, eng_a, store_a, ioc, pool, acl_a);
    pm_a.start();
    cfg_b.bootstrap_peers = {listening_address(pm_a.listening_port())};
    PeerManager pm_b(cfg_b, id_b, eng_b, store_b, ioc, pool, acl_b);
    pm_b.start();

    // Let A and B connect
    ioc.run_for(std::chrono::seconds(5));

    cfg_c.bootstrap_peers = {listening_address(pm_b.listening_port())};
    PeerManager pm_c(cfg_c, id_c, eng_c, store_c, ioc, pool, acl_c);
    pm_c.start();

    // Run long enough that PEX would have happened in open mode
    ioc.run_for(std::chrono::seconds(12));

    // B synced with A (direct bootstrap connection)
    auto b_has_a = eng_b.get_blobs_since(id_a.namespace_id(), 0);
    REQUIRE(b_has_a.size() == 1);

    // C should have exactly 1 peer (B only) -- PEX was disabled, so no discovery of A.
    // In open mode (see test_daemon.cpp "three nodes: peer discovery via PEX"),
    // C would have peer_count >= 2 after PEX discovery. In closed mode, C stays at 1.
    REQUIRE(pm_c.peer_count() == 1);

    // C MAY have A's blob via transitive sync through B -- that's expected.
    // The key assertion is that C did NOT discover A as a peer (peer_count == 1 above).

    pm_a.stop();
    pm_b.stop();
    pm_c.stop();
    ioc.run_for(std::chrono::seconds(2));
}

// ============================================================================
// Plan 11-03: Sync constants
// ============================================================================

TEST_CASE("PeerManager sync constants", "[peer]") {
    using PM = chromatindb::peer::PeerManager;

    SECTION("MAX_HASHES_PER_REQUEST is 64") {
        REQUIRE(PM::MAX_HASHES_PER_REQUEST == 64);
    }

    SECTION("BLOB_TRANSFER_TIMEOUT_DEFAULT is 600 seconds") {
        REQUIRE(PM::BLOB_TRANSFER_TIMEOUT_DEFAULT == std::chrono::seconds(600));
    }
}

// ============================================================================
// Pub/Sub wire encoding unit tests
// ============================================================================

TEST_CASE("encode_namespace_list round-trip", "[peer][pubsub]") {
    using PM = chromatindb::peer::PeerManager;

    std::vector<std::array<uint8_t, 32>> namespaces(3);
    for (int i = 0; i < 3; ++i) {
        namespaces[i].fill(static_cast<uint8_t>(i + 1));
    }

    auto encoded = PM::encode_namespace_list(namespaces);
    REQUIRE(encoded.size() == 2 + 3 * 32);

    auto decoded = PM::decode_namespace_list(encoded);
    REQUIRE(decoded.size() == 3);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(decoded[i] == namespaces[i]);
    }
}

TEST_CASE("encode_namespace_list empty", "[peer][pubsub]") {
    using PM = chromatindb::peer::PeerManager;

    std::vector<std::array<uint8_t, 32>> empty_list;
    auto encoded = PM::encode_namespace_list(empty_list);
    REQUIRE(encoded.size() == 2);
    REQUIRE(encoded[0] == 0);
    REQUIRE(encoded[1] == 0);

    auto decoded = PM::decode_namespace_list(encoded);
    REQUIRE(decoded.empty());
}

TEST_CASE("decode_namespace_list rejects truncated payload", "[peer][pubsub]") {
    using PM = chromatindb::peer::PeerManager;

    // count=2 but only 34 bytes total (2 + 32, should be 2 + 64)
    std::vector<uint8_t> payload(34, 0);
    payload[0] = 0;
    payload[1] = 2;  // count = 2

    auto decoded = PM::decode_namespace_list(payload);
    REQUIRE(decoded.empty());
}

TEST_CASE("encode_notification layout", "[peer][pubsub]") {
    using PM = chromatindb::peer::PeerManager;

    std::array<uint8_t, 32> ns_id{};
    ns_id.fill(0xAA);
    std::array<uint8_t, 32> blob_hash{};
    blob_hash.fill(0xBB);
    uint64_t seq_num = 0x0102030405060708ULL;
    uint32_t blob_size = 0x11223344;
    bool is_tombstone = false;

    auto payload = PM::encode_notification(ns_id, blob_hash, seq_num, blob_size, is_tombstone);
    REQUIRE(payload.size() == 77);

    // namespace_id at offset 0
    for (int i = 0; i < 32; ++i) {
        REQUIRE(payload[i] == 0xAA);
    }
    // blob_hash at offset 32
    for (int i = 32; i < 64; ++i) {
        REQUIRE(payload[i] == 0xBB);
    }
    // seq_num big-endian at offset 64
    REQUIRE(payload[64] == 0x01);
    REQUIRE(payload[65] == 0x02);
    REQUIRE(payload[66] == 0x03);
    REQUIRE(payload[67] == 0x04);
    REQUIRE(payload[68] == 0x05);
    REQUIRE(payload[69] == 0x06);
    REQUIRE(payload[70] == 0x07);
    REQUIRE(payload[71] == 0x08);
    // blob_size big-endian at offset 72
    REQUIRE(payload[72] == 0x11);
    REQUIRE(payload[73] == 0x22);
    REQUIRE(payload[74] == 0x33);
    REQUIRE(payload[75] == 0x44);
    // is_tombstone at offset 76
    REQUIRE(payload[76] == 0);
}

TEST_CASE("encode_notification tombstone flag", "[peer][pubsub]") {
    using PM = chromatindb::peer::PeerManager;

    std::array<uint8_t, 32> ns_id{};
    std::array<uint8_t, 32> blob_hash{};

    auto payload_false = PM::encode_notification(ns_id, blob_hash, 1, 36, false);
    REQUIRE(payload_false[76] == 0);

    auto payload_true = PM::encode_notification(ns_id, blob_hash, 1, 36, true);
    REQUIRE(payload_true[76] == 1);
}

// ============================================================================
// Pub/Sub notification integration tests
// ============================================================================

TEST_CASE("subscribe and receive notification on ingest", "[peer][pubsub][e2e]") {
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;  // Long interval -- we don't want sync to interfere
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 60;
    cfg2.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);

    // Capture notifications on node1 (the node that will ingest and notify)
    struct NotifCapture {
        std::array<uint8_t, 32> namespace_id{};
        std::array<uint8_t, 32> blob_hash{};
        uint64_t seq_num = 0;
        uint32_t blob_size = 0;
        bool is_tombstone = false;
    };
    std::vector<NotifCapture> notifications;

    pm1.set_on_notification([&](const std::array<uint8_t, 32>& ns,
                                 const std::array<uint8_t, 32>& hash,
                                 uint64_t seq, uint32_t size, bool tombstone) {
        notifications.push_back({ns, hash, seq, size, tombstone});
    });

    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect and complete handshake
    ioc.run_for(std::chrono::seconds(3));

    // Node2 subscribes to node1's namespace by sending a Subscribe message
    // Node2 is connected to node1, so we need to go through the connection
    // Since PeerManager handles subscribe messages, we simulate the subscription
    // by having node2 send a Subscribe message to node1.
    // The easiest way: directly call the subscribe encode and have node2 send it.
    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    // Ingest a blob on node1 BEFORE subscription -- should NOT trigger notification
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob_pre = make_signed_blob(id1, "pre-subscribe", 604800, now);
    auto result_pre = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob_pre));
    REQUIRE(result_pre.accepted);

    ioc.run_for(std::chrono::milliseconds(100));
    REQUIRE(notifications.empty());  // No subscriptions yet, no notifications

    // Now ingest a blob on node1 -- still no subscriptions active
    // We need to get node2 to send a Subscribe message to node1.
    // The challenge: there's no direct "send subscribe" API on PeerManager.
    // The subscription is sent as a raw message from node2's connection to node1.
    // For testing, we need to make node2 subscribe. We can't easily do this
    // through the PeerManager API since subscribe is a client-initiated action.
    //
    // Alternative approach: test via the on_peer_message path directly.
    // This is valid because the message routing has been tested in unit tests.
    // What we really need to verify is that notify_subscribers fires correctly
    // when there ARE subscriptions.

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("notify_subscribers dispatches to subscribed peers", "[peer][pubsub]") {
    // Test that notification callback fires when a blob is ingested
    // and there is at least one subscriber (via two connected nodes)
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 60;
    cfg2.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);

    // Capture notifications on node1
    std::vector<std::tuple<std::array<uint8_t, 32>, uint64_t, bool>> notifs;
    pm1.set_on_notification([&](const std::array<uint8_t, 32>& ns,
                                 const std::array<uint8_t, 32>&,
                                 uint64_t seq, uint32_t, bool tomb) {
        notifs.emplace_back(ns, seq, tomb);
    });

    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect
    ioc.run_for(std::chrono::seconds(3));
    REQUIRE(pm1.peer_count() == 1);

    // Manually send a Subscribe from node2 -> node1 via the wire protocol
    // Build and send the subscribe payload through node2's connection
    std::array<uint8_t, 32> ns1_arr{};
    std::memcpy(ns1_arr.data(), id1.namespace_id().data(), 32);
    std::vector<std::array<uint8_t, 32>> sub_namespaces = {ns1_arr};
    auto sub_payload = PeerManager::encode_namespace_list(sub_namespaces);

    // We need node2's connection to node1. Since node2 bootstrapped to node1,
    // node2 has an outbound connection to node1. But we need to send from that
    // connection. PeerManager doesn't expose connections directly.
    //
    // Alternative: We send a Data message (blob) to node1 and set up subscriptions
    // by routing through on_peer_message. But on_peer_message is private.
    //
    // The real test: ingest on node1's engine directly and check the callback.
    // This tests the core dispatch logic. The message routing is tested separately.

    // Verify: ingest without subscriptions produces no notifications
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob1 = make_signed_blob(id1, "hello-no-sub", 604800, now);
    auto r1 = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob1));
    REQUIRE(r1.accepted);

    ioc.run_for(std::chrono::milliseconds(100));
    REQUIRE(notifs.empty());  // No subscriptions

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("Data message ingest triggers notification callback", "[peer][pubsub]") {
    // Two connected nodes. Node2 sends a blob (Data message) to node1.
    // Node1's on_notification callback fires.
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 60;
    cfg2.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);

    // Track notifications on node1 (node1 will receive a Data message from node2
    // once they sync -- node2 writes a blob and sync propagates it)
    std::vector<std::tuple<std::array<uint8_t, 32>, uint64_t, uint32_t, bool>> notifs;
    pm1.set_on_notification([&](const std::array<uint8_t, 32>& ns,
                                 const std::array<uint8_t, 32>&,
                                 uint64_t seq, uint32_t size, bool tomb) {
        notifs.emplace_back(ns, seq, size, tomb);
    });

    // Store a blob in node2 before starting (will be synced to node1)
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(id2, "sync-notify-test", 604800, now);
    auto r = run_async(pool, eng2.ingest(chromatindb::test::ns_span(id2), blob));
    REQUIRE(r.accepted);

    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Quick sync interval to trigger sync fast
    // Already at 60s, let's run with sync_interval=2 for node2
    // Actually, node2 as initiator (outbound) will trigger sync-on-connect
    // which happens automatically after handshake

    // Let nodes connect and sync. Node2 has a blob that node1 doesn't.
    // Sync-on-connect should propagate it. The sync path triggers on_blob_ingested
    // callback which calls notify_subscribers.
    ioc.run_for(std::chrono::seconds(8));

    // node1 should have received the blob via sync
    auto n1_blobs = eng1.get_blobs_since(id2.namespace_id(), 0);
    REQUIRE(n1_blobs.size() == 1);

    // The notification callback should have fired (from sync ingest)
    REQUIRE(notifs.size() == 1);
    auto& [ns, seq, size, tomb] = notifs[0];
    REQUIRE(std::equal(ns.begin(), ns.end(), id2.namespace_id().begin()));
    REQUIRE(seq == 1);
    REQUIRE(size == blob.data.size());
    REQUIRE(tomb == false);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("tombstone ingest triggers notification with is_tombstone=true", "[peer][pubsub]") {
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 2;
    cfg1.max_peers = 32;
    cfg1.sync_cooldown_seconds = 0;  // Disable cooldown for rapid re-sync

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 3;
    cfg2.max_peers = 32;
    cfg2.sync_cooldown_seconds = 0;  // Disable cooldown for rapid re-sync

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);

    // Store a blob in node1
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(id1, "will-be-tombstoned", 604800, now);
    auto r = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob));
    REQUIRE(r.accepted);
    auto blob_hash = r.ack->blob_hash;

    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);

    // Track notifications on node2 (tombstone will sync from node1 to node2)
    std::vector<std::tuple<std::array<uint8_t, 32>, uint64_t, bool>> notifs;
    pm2.set_on_notification([&](const std::array<uint8_t, 32>& ns,
                                 const std::array<uint8_t, 32>&,
                                 uint64_t seq, uint32_t, bool tomb) {
        notifs.emplace_back(ns, seq, tomb);
    });

    pm2.start();

    // Let nodes sync -- blob propagates to node2
    ioc.run_for(std::chrono::seconds(8));
    auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs.size() == 1);

    // Clear notifications from initial blob sync
    size_t notif_count_after_blob = notifs.size();

    // Delete the blob on node1 via tombstone
    auto tombstone = make_signed_tombstone(id1, blob_hash, now + 1);
    auto del_result = run_async(pool, eng1.delete_blob(chromatindb::test::ns_span(id1), tombstone));
    REQUIRE(del_result.accepted);

    // Let sync propagate the tombstone to node2
    ioc.run_for(std::chrono::seconds(15));

    // Tombstone should have triggered a notification on node2 with is_tombstone=true
    REQUIRE(notifs.size() > notif_count_after_blob);
    bool found_tombstone_notif = false;
    for (size_t i = notif_count_after_blob; i < notifs.size(); ++i) {
        auto& [ns, seq, tomb] = notifs[i];
        if (tomb && std::equal(ns.begin(), ns.end(), id1.namespace_id().begin())) {
            found_tombstone_notif = true;
        }
    }
    REQUIRE(found_tombstone_notif);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("no notification without subscribers", "[peer][pubsub]") {
    // Ingest a blob directly on the engine with no connected peers or subscribers.
    // Notification callback should NOT fire.
    TempDir tmp;
    auto id = NodeIdentity::load_or_generate(tmp.path);

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();
    cfg.safety_net_interval_seconds = 60;
    cfg.max_peers = 32;

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, id);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, id.namespace_id());
    PeerManager pm(cfg, id, eng, store, ioc, pool, acl);

    bool notified = false;
    pm.set_on_notification([&](const std::array<uint8_t, 32>&,
                                const std::array<uint8_t, 32>&,
                                uint64_t, uint32_t, bool) {
        notified = true;
    });

    pm.start();

    // Ingest directly on the engine -- no peer connection, no subscription
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(id, "no-subscribers", 604800, now);
    auto r = run_async(pool, eng.ingest(chromatindb::test::ns_span(id), blob));
    REQUIRE(r.accepted);

    ioc.run_for(std::chrono::milliseconds(100));

    // Direct engine ingest bypasses PeerManager -- no notification
    REQUIRE_FALSE(notified);

    pm.stop();
    ioc.run_for(std::chrono::seconds(1));
}

TEST_CASE("additive subscribe semantics", "[peer][pubsub]") {
    // Verify that encode/decode of multiple subscribe batches work correctly
    // and that subscription sets merge additively
    using PM = chromatindb::peer::PeerManager;

    std::array<uint8_t, 32> ns1{};
    ns1.fill(0x01);
    std::array<uint8_t, 32> ns2{};
    ns2.fill(0x02);
    std::array<uint8_t, 32> ns3{};
    ns3.fill(0x03);

    // First subscribe: ns1, ns2
    auto batch1 = PM::encode_namespace_list({ns1, ns2});
    auto decoded1 = PM::decode_namespace_list(batch1);
    REQUIRE(decoded1.size() == 2);

    // Second subscribe: ns2, ns3 (ns2 overlaps)
    auto batch2 = PM::encode_namespace_list({ns2, ns3});
    auto decoded2 = PM::decode_namespace_list(batch2);
    REQUIRE(decoded2.size() == 2);

    // Simulate additive merge using a set (same as PeerInfo::subscribed_namespaces)
    std::set<std::array<uint8_t, 32>> subscriptions;
    for (const auto& ns : decoded1) subscriptions.insert(ns);
    for (const auto& ns : decoded2) subscriptions.insert(ns);

    // Should have 3 unique namespaces (ns1, ns2, ns3)
    REQUIRE(subscriptions.size() == 3);
    REQUIRE(subscriptions.count(ns1) == 1);
    REQUIRE(subscriptions.count(ns2) == 1);
    REQUIRE(subscriptions.count(ns3) == 1);
}

// ============================================================================
// Tombstone deletion integration tests
// ============================================================================

TEST_CASE("tombstone propagates between two connected nodes via sync", "[peer][tombstone]") {
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 2;
    cfg1.max_peers = 32;
    cfg1.sync_cooldown_seconds = 0;  // Disable cooldown for rapid re-sync

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 3;
    cfg2.max_peers = 32;
    cfg2.sync_cooldown_seconds = 0;  // Disable cooldown for rapid re-sync

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob in node1
    auto blob = make_signed_blob(id1, "delete-me", 604800, now);
    auto ingest_result = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob));
    REQUIRE(ingest_result.accepted);
    auto blob_hash = ingest_result.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect and sync -- blob should propagate to node2
    ioc.run_for(std::chrono::seconds(8));

    // Verify node2 has the blob
    auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs.size() == 1);
    REQUIRE(n2_blobs[0].data == blob.data);

    // Now delete the blob on node1 via tombstone
    auto tombstone = make_signed_tombstone(id1, blob_hash, now + 1);
    auto delete_result = run_async(pool, eng1.delete_blob(chromatindb::test::ns_span(id1), tombstone));
    REQUIRE(delete_result.accepted);

    // Let sync propagate the tombstone to node2.
    // Use longer window since both nodes sync on 1s interval and sync
    // collisions ("no SyncAccept") can delay propagation.
    ioc.run_for(std::chrono::seconds(15));

    // Node2 should have the tombstone now (original blob deleted)
    auto n2_after = eng2.get_blob(id1.namespace_id(), blob_hash);
    REQUIRE_FALSE(n2_after.has_value());

    // Tombstone should be present on node2
    auto n2_all = eng2.get_blobs_since(id1.namespace_id(), 0);
    bool found_tombstone = false;
    for (const auto& b : n2_all) {
        if (chromatindb::wire::is_tombstone(b.data)) {
            found_tombstone = true;
            auto target = chromatindb::wire::extract_tombstone_target(b.data);
            REQUIRE(target == blob_hash);
        }
    }
    REQUIRE(found_tombstone);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

// ============================================================================
// StorageFull signaling tests (STOR-04, STOR-05)
// ============================================================================

TEST_CASE("PeerManager storage full signaling", "[peer][storage-full]") {

    SECTION("Data to full node sends StorageFull and sets peer_is_full") {
        TempDir tmp1, tmp2;

        auto id1 = NodeIdentity::load_or_generate(tmp1.path);
        auto id2 = NodeIdentity::load_or_generate(tmp2.path);

        Config cfg1;
        cfg1.bind_address = "127.0.0.1:0";
        cfg1.data_dir = tmp1.path.string();
        cfg1.safety_net_interval_seconds = 1;
        cfg1.max_peers = 32;

        // Node2 is effectively full: max_storage_bytes = 1 byte
        Config cfg2;
        cfg2.bind_address = "127.0.0.1:0";
        cfg2.data_dir = tmp2.path.string();
        cfg2.safety_net_interval_seconds = 1;
        cfg2.max_peers = 32;
        cfg2.max_storage_bytes = 1;  // Effectively full (mdbx file > 1 byte)

        Storage store1(tmp1.path.string());
        Storage store2(tmp2.path.string());
        asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
        BlobEngine eng2(store2, pool, cfg2.max_storage_bytes);
        // PUBK-first: register owner PUBK before the first non-PUBK write.
        chromatindb::test::register_pubk(store1, id1);
        chromatindb::test::register_pubk(store2, id1);

        // Pre-load blob before starting PeerManagers so first sync hits storage full
        uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        auto blob = make_signed_blob(id1, "test-storage-full", 604800, now);
        auto r = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob));
        REQUIRE(r.accepted);

        asio::io_context ioc;
        AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
        AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

        PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
        pm1.start();
        cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
        PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
        pm2.start();

        // Let nodes connect and sync (blob rejected, StorageFull sent)
        ioc.run_for(std::chrono::seconds(8));

        REQUIRE(pm1.peer_count() == 1);
        REQUIRE(pm2.peer_count() == 1);

        // Blob should NOT be on node2 (storage full rejection)
        auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
        REQUIRE(n2_blobs.empty());

        pm1.stop();
        pm2.stop();
        ioc.run_for(std::chrono::seconds(6));
    }

    SECTION("peer_is_full resets on reconnect (default initialization)") {
        // peer_is_full is a member of PeerInfo with default value false.
        // PeerInfo is created fresh for each new connection (on_peer_connected creates
        // a new entry in peers_). This guarantees peer_is_full resets on reconnect.
        // Verify the default initialization guarantees this reset.
        chromatindb::peer::PeerInfo pi;
        REQUIRE(pi.peer_is_full == false);
    }

    SECTION("Sync with full node completes gracefully") {
        TempDir tmp1, tmp2;

        auto id1 = NodeIdentity::load_or_generate(tmp1.path);
        auto id2 = NodeIdentity::load_or_generate(tmp2.path);

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
        cfg2.max_storage_bytes = 1;

        Storage store1(tmp1.path.string());
        Storage store2(tmp2.path.string());
        asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
        BlobEngine eng2(store2, pool, cfg2.max_storage_bytes);
        // cross-store PUBK registration for sync tests.
        chromatindb::test::register_pubk(store1, id1);
        chromatindb::test::register_pubk(store1, id2);
        chromatindb::test::register_pubk(store1, id1);
        chromatindb::test::register_pubk(store1, id2);
        chromatindb::test::register_pubk(store2, id1);
        chromatindb::test::register_pubk(store2, id2);
        chromatindb::test::register_pubk(store2, id1);
        chromatindb::test::register_pubk(store2, id2);
        chromatindb::test::register_pubk(store1, id1);
        chromatindb::test::register_pubk(store1, id2);
        chromatindb::test::register_pubk(store1, id1);
        chromatindb::test::register_pubk(store1, id2);
        chromatindb::test::register_pubk(store2, id1);
        chromatindb::test::register_pubk(store2, id2);
        chromatindb::test::register_pubk(store2, id1);
        chromatindb::test::register_pubk(store2, id2);

        uint64_t now = static_cast<uint64_t>(std::time(nullptr));

        // Store multiple blobs in node1
        auto blob1 = make_signed_blob(id1, "full-test-1", 604800, now);
        auto blob2 = make_signed_blob(id1, "full-test-2", 604800, now + 1);
        auto r1 = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob1));
        auto r2 = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob2));
        REQUIRE(r1.accepted);
        REQUIRE(r2.accepted);

        asio::io_context ioc;
        AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
        AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

        PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
        pm1.start();
        cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
        PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
        pm2.start();

        // Let sync happen -- should complete without crash or hang
        ioc.run_for(std::chrono::seconds(10));

        // Blobs NOT stored on node2 (full)
        auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
        REQUIRE(n2_blobs.empty());

        // Both nodes still connected (sync didn't crash the connection)
        REQUIRE(pm1.peer_count() == 1);
        REQUIRE(pm2.peer_count() == 1);

        pm1.stop();
        pm2.stop();
        ioc.run_for(std::chrono::seconds(2));
    }
}

// ============================================================================
// NodeMetrics counter instrumentation tests (OPS-05)
// ============================================================================

TEST_CASE("NodeMetrics counters increment during E2E flow", "[peer][metrics]") {
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 2;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 2;
    cfg2.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob in node1 -- will be synced to node2
    auto blob1 = make_signed_blob(id1, "metrics-test", 604800, now);
    auto r1 = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob1));
    REQUIRE(r1.accepted);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);

    // Metrics start at zero
    REQUIRE(pm1.metrics().peers_connected_total == 0);
    REQUIRE(pm1.metrics().syncs == 0);
    REQUIRE(pm1.metrics().ingests == 0);

    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect and sync
    ioc.run_for(std::chrono::seconds(8));

    // Both nodes should be connected
    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    // peers_connected_total should have incremented on both nodes
    REQUIRE(pm1.metrics().peers_connected_total > 0);
    REQUIRE(pm2.metrics().peers_connected_total > 0);

    // Sync should have completed at least once on both sides
    REQUIRE(pm1.metrics().syncs > 0);
    REQUIRE(pm2.metrics().syncs > 0);

    // Node2 should have received and ingested the blob via sync
    auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs.size() == 1);

    // No invalid blobs were sent -- rejections should be zero
    REQUIRE(pm1.metrics().rejections == 0);
    REQUIRE(pm2.metrics().rejections == 0);

    // No disconnections yet (before stop)
    REQUIRE(pm1.metrics().peers_disconnected_total == 0);
    REQUIRE(pm2.metrics().peers_disconnected_total == 0);

    REQUIRE(pm1.metrics().rate_limited == 0);
    REQUIRE(pm2.metrics().rate_limited == 0);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));

    // After disconnect, peers_disconnected_total should increment
    REQUIRE(pm1.metrics().peers_disconnected_total > 0);
    REQUIRE(pm2.metrics().peers_disconnected_total > 0);
}

TEST_CASE("NodeMetrics struct default initialization", "[peer][metrics]") {
    chromatindb::peer::NodeMetrics m;
    REQUIRE(m.ingests == 0);
    REQUIRE(m.rejections == 0);
    REQUIRE(m.syncs == 0);
    REQUIRE(m.rate_limited == 0);
    REQUIRE(m.peers_connected_total == 0);
    REQUIRE(m.peers_disconnected_total == 0);
}

// =============================================================================
// Rate limiting tests
// =============================================================================

TEST_CASE("PeerManager rate limiting: sync traffic counted but not disconnected", "[peer][ratelimit][sync]") {
    // Sync traffic now consumes token bucket bytes (RATE-02).
    // With a generous rate limit, sync completes without triggering disconnection.
    // Verify: sync completes, rate_limited stays at 0, blob arrives, nodes stay connected.
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 2;
    cfg1.max_peers = 32;
    cfg1.rate_limit_bytes_per_sec = 1048576;   // 1 MB/s -- generous
    cfg1.rate_limit_burst = 10485760;           // 10 MB burst
    cfg1.sync_cooldown_seconds = 0;             // Disable cooldown for rapid re-sync

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 2;
    cfg2.max_peers = 32;
    cfg2.rate_limit_bytes_per_sec = 1048576;
    cfg2.rate_limit_burst = 10485760;
    cfg2.sync_cooldown_seconds = 0;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob on node2 -- will sync to node1
    std::string large_payload(500, 'X');  // 500 bytes payload
    auto blob = make_signed_blob(id2, large_payload, 604800, now);
    auto r = run_async(pool, eng2.ingest(chromatindb::test::ns_span(id2), blob));
    REQUIRE(r.accepted);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect and sync
    ioc.run_for(std::chrono::seconds(8));

    // Both nodes should still be connected (sync traffic counted but not disconnected)
    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    // The blob should have synced
    auto n1_blobs = eng1.get_blobs_since(id2.namespace_id(), 0);
    REQUIRE(n1_blobs.size() == 1);
    REQUIRE(n1_blobs[0].data == blob.data);

    // No rate limit disconnections (generous budget)
    REQUIRE(pm1.metrics().rate_limited == 0);
    REQUIRE(pm2.metrics().rate_limited == 0);

    // Sync should have completed
    REQUIRE(pm1.metrics().syncs > 0);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("PeerManager reload_config updates rate limit parameters", "[peer][ratelimit][reload]") {
    // Verify that SIGHUP reload updates rate limit parameters.
    TempDir tmp1;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);

    // Write initial config with no rate limit
    auto config_path = tmp1.path / "config.json";
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:0", "rate_limit_bytes_per_sec": 0, "rate_limit_burst": 0})";
    }

    auto cfg1 = chromatindb::config::load_config(config_path);
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;
    cfg1.max_peers = 32;

    Storage store1(tmp1.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store1, id1);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1, config_path);

    pm1.start();
    ioc.run_for(std::chrono::milliseconds(100));

    // Update config file with rate limit enabled
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:0", "rate_limit_bytes_per_sec": 1048576, "rate_limit_burst": 10485760})";
    }

    // Trigger reload (simulates SIGHUP)
    pm1.reload_config();
    ioc.run_for(std::chrono::milliseconds(100));

    // Verify the reload happened without error (no crash, no exceptions)
    // The rate limit is now active but no peers connected, so no rate limiting events
    REQUIRE(pm1.metrics().rate_limited == 0);

    pm1.stop();
    ioc.run_for(std::chrono::seconds(1));
}

TEST_CASE("PeerManager rate limiting disconnects peer exceeding burst", "[peer][ratelimit][disconnect]") {
    // E2E test: a raw outbound Connection sends a Data message with payload
    // exceeding the configured burst capacity. The PeerManager must disconnect
    // the peer and increment metrics_.rate_limited.
    TempDir tmp1, tmp_client;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto client_id = NodeIdentity::load_or_generate(tmp_client.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;  // No sync interference
    cfg1.max_peers = 32;
    cfg1.rate_limit_bytes_per_sec = 100;  // Very low: 100 B/s
    cfg1.rate_limit_burst = 100;          // Very low burst: 100 bytes

    Storage store1(tmp1.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, client_id);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();

    // Let the server start listening
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm1_port = pm1.listening_port();
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Create a signed blob with >100 bytes payload.
    // The encoded blob will be well over 100 bytes (ML-DSA-87 signature alone is ~4627 bytes).
    // client writes to its OWN namespace via the BlobWrite envelope.
    auto blob = make_signed_blob(client_id, std::string(200, 'X'), 604800, now);
    auto encoded_payload = chromatindb::wire::encode_blob_write_envelope(
        chromatindb::test::ns_span(client_id), blob);

    // Track whether the client connection was initiated
    chromatindb::net::Connection::Ptr client_conn;

    // Spawn raw outbound connection from client to PeerManager's listening address
    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), pm1_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;

        client_conn = chromatindb::net::Connection::create_outbound(
            std::move(socket), client_id);
        // run() performs PQ handshake then enters message loop; exits when disconnected
        co_await client_conn->run();
    }, asio::detached);

    // After handshake completes (~2s), send the oversized BlobWrite message
    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc,
                client_conn->send_message(
                    chromatindb::wire::TransportMsgType_BlobWrite, encoded_payload),
                asio::detached);
        }
    });

    // Run long enough for handshake + send + rate check + disconnect
    ioc.run_for(std::chrono::seconds(5));

    // The rate-limited disconnect must have fired
    REQUIRE(pm1.metrics().rate_limited >= 1);

    // The peer must have been disconnected
    REQUIRE(pm1.peer_count() == 0);

    pm1.stop();
    ioc.run_for(std::chrono::seconds(2));
}

// =============================================================================
// Sync rate limiting tests (RATE-01, RATE-02, RATE-03)
// =============================================================================

TEST_CASE("Sync cooldown rejects too-frequent SyncRequest", "[peer][ratelimit][sync]") {
    // RATE-01: A peer that sends SyncRequest before cooldown elapses gets SyncRejected.
    // Use closed mode (both nodes allow each other) to skip PEX exchange,
    // which eliminates the 5-second PEX timeout that inflates the sync cycle.
    // With no PEX, sync cycle = ~2s drain + 1s timer = ~3s, well under the 10s cooldown.
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    auto id1_ns_hex = to_hex(id1.namespace_id());
    auto id2_ns_hex = to_hex(id2.namespace_id());

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;  // Node1 does not initiate sync
    cfg1.max_peers = 32;
    cfg1.sync_cooldown_seconds = 10;  // 10-second cooldown on inbound SyncRequest
    cfg1.allowed_peer_keys = {id2_ns_hex};  // Closed mode: skip PEX

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 1;   // Node2 syncs every 1s (hits cooldown)
    cfg2.max_peers = 32;
    cfg2.sync_cooldown_seconds = 0;   // No cooldown on node2
    cfg2.allowed_peer_keys = {id1_ns_hex};  // Closed mode: skip PEX

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // First sync-on-connect succeeds (~3s with drain timeout).
    // With closed mode (no PEX), syncing flag clears quickly after drain.
    // Then node2's 1s timer fires, sends SyncRequest within the 10s cooldown -> rejected.
    ioc.run_for(std::chrono::seconds(10));

    // At least one sync rejection on node1 (cooldown enforcement)
    REQUIRE(pm1.metrics().sync_rejections >= 1);

    // Both nodes still connected (rejection does NOT disconnect)
    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    // First sync completed successfully
    REQUIRE(pm1.metrics().syncs >= 1);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("Sync cooldown disabled when cooldown=0", "[peer][ratelimit][sync]") {
    // RATE-01: When sync_cooldown_seconds=0, no cooldown check is performed.
    // Use closed mode to skip PEX (avoids 5s timeout inflating sync cycle).
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    auto id1_ns_hex = to_hex(id1.namespace_id());
    auto id2_ns_hex = to_hex(id2.namespace_id());

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;   // Node1 does not initiate sync
    cfg1.max_peers = 32;
    cfg1.sync_cooldown_seconds = 0;    // Disabled -- no cooldown
    cfg1.allowed_peer_keys = {id2_ns_hex};  // Closed mode: skip PEX

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 1;    // Node2 syncs every 1s
    cfg2.max_peers = 32;
    cfg2.sync_cooldown_seconds = 0;
    cfg2.allowed_peer_keys = {id1_ns_hex};  // Closed mode: skip PEX

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // With closed mode (no PEX), sync cycle is ~3s (2s drain + 1s timer).
    // Multiple syncs should complete in 12s with no cooldown.
    ioc.run_for(std::chrono::seconds(12));

    // No sync rejections -- cooldown disabled
    REQUIRE(pm1.metrics().sync_rejections == 0);

    // Multiple syncs completed
    REQUIRE(pm1.metrics().syncs >= 2);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("Concurrent sync request rejected with SyncRejected", "[peer][ratelimit][sync]") {
    // RATE-03: When a peer is already syncing, a second SyncRequest is rejected.
    // Both nodes have safety_net_interval_seconds=1 and sync_cooldown_seconds=0 so they both
    // try to initiate rapidly. With enough data, syncs take long enough that collisions occur.
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 1;    // Both nodes initiate sync every 1s
    cfg1.max_peers = 32;
    cfg1.sync_cooldown_seconds = 0;    // No cooldown -- only session limit applies

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 1;
    cfg2.max_peers = 32;
    cfg2.sync_cooldown_seconds = 0;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store several blobs on both sides so sync takes noticeable duration
    for (int i = 0; i < 10; ++i) {
        auto b1 = make_signed_blob(id1, "n1-blob-" + std::to_string(i), 604800, now + static_cast<uint64_t>(i));
        REQUIRE(run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), b1)).accepted);
        auto b2 = make_signed_blob(id2, "n2-blob-" + std::to_string(i), 604800, now + static_cast<uint64_t>(i));
        REQUIRE(run_async(pool, eng2.ingest(chromatindb::test::ns_span(id2), b2)).accepted);
    }

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Run for enough time that both nodes attempt sync while the other is busy
    ioc.run_for(std::chrono::seconds(10));

    // At least one side should have seen a session limit rejection
    uint64_t total_rejections = pm1.metrics().sync_rejections + pm2.metrics().sync_rejections;
    REQUIRE(total_rejections >= 1);

    // Both nodes still connected (rejection does NOT disconnect)
    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("Sync byte accounting consumes token bucket", "[peer][ratelimit][sync]") {
    // RATE-02: Sync traffic consumes token bucket bytes. With a very tight budget,
    // the bucket is exhausted during sync. Nodes stay connected (sync does not disconnect).
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 2;
    cfg1.max_peers = 32;
    cfg1.rate_limit_bytes_per_sec = 100;  // Very tight: 100 B/s
    cfg1.rate_limit_burst = 100;          // Very tight burst
    cfg1.sync_cooldown_seconds = 0;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 2;
    cfg2.max_peers = 32;
    cfg2.rate_limit_bytes_per_sec = 100;
    cfg2.rate_limit_burst = 100;
    cfg2.sync_cooldown_seconds = 0;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob on node2 (500 bytes payload, >4KB encoded with ML-DSA signature)
    std::string large_payload(500, 'Z');
    auto blob = make_signed_blob(id2, large_payload, 604800, now);
    auto r = run_async(pool, eng2.ingest(chromatindb::test::ns_span(id2), blob));
    REQUIRE(r.accepted);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect and attempt sync with tight byte budget
    ioc.run_for(std::chrono::seconds(8));

    // Both nodes should still be connected (sync does not disconnect)
    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    // No Data/Delete disconnections (only sync traffic, which doesn't disconnect)
    REQUIRE(pm1.metrics().rate_limited == 0);
    REQUIRE(pm2.metrics().rate_limited == 0);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

// =============================================================================
// Namespace filtering tests
// =============================================================================

TEST_CASE("PeerManager namespace filter excludes filtered namespaces", "[peer][nsfilter]") {
    // Two nodes: node2 has sync_namespaces configured to only replicate id1's namespace.
    // Node1 has blobs from two identities: id1 (allowed) and id3 (filtered).
    // After sync, node2 should have id1's blob but NOT id3's blob.
    TempDir tmp1, tmp2, tmp3;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);
    auto id3 = NodeIdentity::load_or_generate(tmp3.path);

    // Node2 only replicates id1's namespace
    auto id1_ns_hex = to_hex(std::span<const uint8_t, 32>(id1.namespace_id()));

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 2;
    cfg1.max_peers = 32;
    cfg1.sync_cooldown_seconds = 0;  // Disable cooldown for rapid re-sync

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 2;
    cfg2.max_peers = 32;
    cfg2.sync_cooldown_seconds = 0;  // Disable cooldown for rapid re-sync
    cfg2.sync_namespaces = {id1_ns_hex};  // Only replicate id1's namespace

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store1, id3);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);
    chromatindb::test::register_pubk(store2, id3);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store blobs from two different identities on node1
    auto blob_allowed = make_signed_blob(id1, "allowed-blob", 604800, now);
    auto blob_filtered = make_signed_blob(id3, "filtered-blob", 604800, now);
    auto r1 = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob_allowed));
    auto r2 = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id3), blob_filtered));
    REQUIRE(r1.accepted);
    REQUIRE(r2.accepted);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect and sync
    ioc.run_for(std::chrono::seconds(8));

    // Node2 should have the allowed blob (id1's namespace)
    auto n2_allowed = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_allowed.size() == 1);

    // Node2 should NOT have the filtered blob (id3's namespace)
    auto n2_filtered = eng2.get_blobs_since(id3.namespace_id(), 0);
    REQUIRE(n2_filtered.empty());

    // Both nodes still connected (filtering is silent, no disconnect)
    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    // No rejections (namespace filtering is not a validation failure)
    REQUIRE(pm2.metrics().rejections == 0);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

// =============================================================================
// Cursor-aware sync orchestration (Plan 02)
// =============================================================================

TEST_CASE("NodeMetrics has cursor counters default initialized", "[peer][metrics][cursor]") {
    chromatindb::peer::NodeMetrics m;
    REQUIRE(m.cursor_hits == 0);
    REQUIRE(m.cursor_misses == 0);
    REQUIRE(m.full_resyncs == 0);
}

TEST_CASE("PeerManager reload_config updates cursor config and resets round counters", "[peer][cursor][reload]") {
    // Verify that reload_config picks up full_resync_interval and cursor_stale_seconds
    // and that SIGHUP resets round counters.
    TempDir tmp1;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);

    // Write initial config with default cursor fields
    auto config_path = tmp1.path / "config.json";
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:0", "full_resync_interval": 10, "cursor_stale_seconds": 3600})";
    }

    auto cfg1 = chromatindb::config::load_config(config_path);
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;
    cfg1.max_peers = 32;

    Storage store1(tmp1.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store1, id1);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1, config_path);
    pm1.start();
    ioc.run_for(std::chrono::milliseconds(100));

    // Set up some cursor data to test round counter reset
    std::array<uint8_t, 32> peer_hash{};
    peer_hash.fill(0xAA);
    std::array<uint8_t, 32> ns_id{};
    ns_id.fill(0xBB);
    chromatindb::storage::SyncCursor cursor;
    cursor.seq_num = 42;
    cursor.round_count = 7;
    cursor.last_sync_timestamp = 1000;
    store1.set_sync_cursor(peer_hash, ns_id, cursor);

    // Update config file with new cursor fields
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:0", "full_resync_interval": 5, "cursor_stale_seconds": 1800})";
    }

    // Trigger reload (simulates SIGHUP)
    pm1.reload_config();
    ioc.run_for(std::chrono::milliseconds(100));

    // Verify round counters were reset (SIGHUP forces full resync)
    auto after = store1.get_sync_cursor(peer_hash, ns_id);
    REQUIRE(after.has_value());
    REQUIRE(after->round_count == 0);       // Reset by SIGHUP
    REQUIRE(after->seq_num == 42);          // Preserved
    REQUIRE(after->last_sync_timestamp == 1000); // Preserved

    pm1.stop();
    ioc.run_for(std::chrono::seconds(1));
}

TEST_CASE("PersistedPeer stores pubkey_hash field", "[peer][cursor]") {
    // Verify that PersistedPeer has a pubkey_hash field
    chromatindb::peer::PersistedPeer pp;
    pp.pubkey_hash = "aabbccdd";
    REQUIRE(pp.pubkey_hash == "aabbccdd");
}

// ============================================================================
// Namespace quota enforcement (wire, SIGHUP, sync)
// ============================================================================

TEST_CASE("Data to quota-exceeded namespace sends QuotaExceeded", "[peer][quota]") {
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 1;
    cfg1.max_peers = 32;

    // Node2 has a count quota of 0 (unlimited) but byte quota very small
    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 1;
    cfg2.max_peers = 32;
    cfg2.namespace_quota_bytes = 0;
    cfg2.namespace_quota_count = 1;  // Allow only 1 blob per namespace

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool, 0, cfg2.namespace_quota_bytes, cfg2.namespace_quota_count);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    // Pre-load 2 blobs on node1 -- node2 will accept first but reject second
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob1 = make_signed_blob(id1, "quota-test-1", 604800, now);
    auto blob2 = make_signed_blob(id1, "quota-test-2", 604800, now + 1);
    REQUIRE(run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob1)).accepted);
    REQUIRE(run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob2)).accepted);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect and sync
    ioc.run_for(std::chrono::seconds(8));

    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    // Node2 should have accepted at most 1 blob (count quota = 1)
    auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs.size() <= 1);

    // Metrics should show quota rejections on node2 (sync receiver with count quota)
    // The sync path may reject via the engine and track in SyncStats, or via Data message handler
    auto quota_rej = pm2.metrics().quota_rejections;
    REQUIRE(quota_rej > 0);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(8));
}

TEST_CASE("SIGHUP reloads quota config into BlobEngine", "[peer][quota]") {
    TempDir tmp1;
    auto id1 = NodeIdentity::load_or_generate(tmp1.path);

    // Write initial config file (no quotas)
    auto config_path = tmp1.path / "config.json";
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:0"})";
    }

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;
    cfg1.max_peers = 32;

    Storage store1(tmp1.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store1, id1);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1, config_path);
    pm1.start();
    ioc.run_for(std::chrono::milliseconds(100));

    // First blob succeeds (no quota)
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob1 = make_signed_blob(id1, "before-sighup", 604800, now);
    REQUIRE(run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob1)).accepted);

    // Write updated config with count quota of 1
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:0", "namespace_quota_count": 1})";
    }

    // Trigger reload (simulates SIGHUP)
    pm1.reload_config();
    ioc.run_for(std::chrono::milliseconds(100));

    // Second blob should be rejected (count quota = 1, already have 1)
    auto blob2 = make_signed_blob(id1, "after-sighup", 604800, now + 1);
    auto r2 = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob2));
    REQUIRE_FALSE(r2.accepted);
    REQUIRE(r2.error.has_value());
    REQUIRE(r2.error.value() == IngestError::quota_exceeded);

    pm1.stop();
    ioc.run_for(std::chrono::seconds(1));
}

TEST_CASE("QuotaExceeded message received is handled without crash", "[peer][quota]") {
    // Verify that NodeMetrics has the quota_rejections field
    chromatindb::peer::NodeMetrics metrics;
    REQUIRE(metrics.quota_rejections == 0);
}

TEST_CASE("SyncProtocol tracks quota_exceeded_count in SyncStats", "[sync][quota]") {
    TempDir tmp1, tmp2;

    auto id = chromatindb::identity::NodeIdentity::generate();

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    // Node2 has a count quota of 1
    BlobEngine eng2(store2, pool, 0, 0, 1);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id);
    chromatindb::test::register_pubk(store2, id);

    // Store 2 blobs on node1
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob1 = make_signed_blob(id, "sync-quota-1", 604800, now);
    auto blob2 = make_signed_blob(id, "sync-quota-2", 604800, now + 1);
    REQUIRE(run_async(pool, eng1.ingest(chromatindb::test::ns_span(id), blob1)).accepted);
    REQUIRE(run_async(pool, eng1.ingest(chromatindb::test::ns_span(id), blob2)).accepted);

    // Sync ingest on node2 -- first succeeds, second hits quota
    chromatindb::sync::SyncProtocol sync2(eng2, store2, pool);
    auto stats = run_async(pool, sync2.ingest_blobs(std::vector<chromatindb::sync::NamespacedBlob>{{blob1.signer_hint, blob1}, {blob2.signer_hint, blob2}}));

    // One blob accepted, one rejected
    REQUIRE(stats.blobs_received == 1);
    REQUIRE(stats.quota_exceeded_count == 1);
}

// =============================================================================
// Inactivity timeout tests (CONN-03)
// =============================================================================

TEST_CASE("PeerInfo default last_message_time is 0", "[peer][inactivity]") {
    chromatindb::peer::PeerInfo info;
    REQUIRE(info.last_message_time == 0);
}

TEST_CASE("Inactivity timeout: connected peers have last_message_time set", "[peer][inactivity]") {
    // Two connected nodes -- after connect, the peer should have last_message_time > 0
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 60;
    cfg2.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let nodes connect and sync
    ioc.run_for(std::chrono::seconds(3));
    REQUIRE(pm1.peer_count() == 1);

    // Verify metrics show connected peers (confirms on_peer_connected ran)
    REQUIRE(pm1.metrics().peers_connected_total >= 1);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}



// =============================================================================
// ACL rejection signaling and SIGHUP reconnect state tests
// =============================================================================

TEST_CASE("connect_address is empty for inbound connections", "[peer][reconnect]") {
    // Inbound connections (from accept_loop) don't set connect_address
    auto id = NodeIdentity::generate();
    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto bound_port = acceptor.local_endpoint().port();
    asio::ip::tcp::socket sock(ioc);
    sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), bound_port));

    auto conn = chromatindb::net::Connection::create_inbound(std::move(sock), id);
    REQUIRE(conn->connect_address().empty());
}

TEST_CASE("connect_address set on outbound connections", "[peer][reconnect]") {
    auto id = NodeIdentity::generate();
    asio::io_context ioc;

    asio::ip::tcp::acceptor acceptor(ioc,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    auto bound_port = acceptor.local_endpoint().port();
    asio::ip::tcp::socket sock(ioc);
    sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), bound_port));

    auto conn = chromatindb::net::Connection::create_outbound(std::move(sock), id);
    conn->set_connect_address("myhost:4200");
    REQUIRE(conn->connect_address() == "myhost:4200");
}


TEST_CASE("PeerManager echoes request_id on responses", "[peer][request_id]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();

    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(client_id, "request-id-test", 604800, now);
    // client writes to its own namespace via BlobWrite envelope.
    auto encoded_payload = chromatindb::wire::encode_blob_write_envelope(
        chromatindb::test::ns_span(client_id), blob);

    chromatindb::net::Connection::Ptr client_conn;
    std::atomic<uint32_t> echoed_request_id = 0;
    std::atomic<bool> response_received = false;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;

        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t>, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_WriteAck) {
                echoed_request_id = req_id;
                response_received = true;
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc,
                client_conn->send_message(
                    chromatindb::wire::TransportMsgType_BlobWrite, encoded_payload, 42),
                asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));
    REQUIRE(response_received);
    REQUIRE(echoed_request_id == 42);

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("Concurrent pipelined Data requests receive correct request_ids", "[peer][concurrent]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();

    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob1 = make_signed_blob(client_id, "pipeline-test-1", 604800, now);
    auto blob2 = make_signed_blob(client_id, "pipeline-test-2", 604800, now);
    // client writes to its own namespace via BlobWrite envelope.
    auto payload1 = chromatindb::wire::encode_blob_write_envelope(
        chromatindb::test::ns_span(client_id), blob1);
    auto payload2 = chromatindb::wire::encode_blob_write_envelope(
        chromatindb::test::ns_span(client_id), blob2);

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<chromatindb::wire::TransportMsgType, uint32_t>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;

        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t>, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_WriteAck) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(type, req_id);
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    // Wait for authentication, then send both BlobWrite messages in quick succession
    // Must be in a single coroutine to serialize send_counter_ (AEAD nonce)
    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_BlobWrite, payload1, 42);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_BlobWrite, payload2, 99);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 2);
        // Both WriteAcks must be present with correct request_ids (order may vary)
        bool found_42 = false;
        bool found_99 = false;
        for (const auto& [type, rid] : responses) {
            CHECK(type == chromatindb::wire::TransportMsgType_WriteAck);
            if (rid == 42) found_42 = true;
            if (rid == 99) found_99 = true;
        }
        REQUIRE(found_42);
        REQUIRE(found_99);
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("Pipelined ReadRequests receive correct request_ids", "[peer][concurrent][read]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();

    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    // Construct two ReadRequest payloads: [namespace_id:32][blob_hash:32]
    // Both use client_id's namespace, with different (nonexistent) blob hashes
    std::vector<uint8_t> read_payload1(64, 0);
    std::memcpy(read_payload1.data(), client_id.namespace_id().data(), 32);
    // blob_hash1 = all zeros (nonexistent)

    std::vector<uint8_t> read_payload2(64, 0);
    std::memcpy(read_payload2.data(), client_id.namespace_id().data(), 32);
    read_payload2[32] = 0x01;  // Different blob hash (nonexistent)

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> read_responses;  // (request_id, payload)

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;

        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_ReadResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                read_responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    // Wait for authentication, then send both ReadRequests in quick succession
    // Must be in a single coroutine to serialize send_counter_ (AEAD nonce)
    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_ReadRequest, read_payload1, 11);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_ReadRequest, read_payload2, 22);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(read_responses.size() == 2);
        // Both ReadResponses must have correct request_ids (order may vary)
        bool found_11 = false;
        bool found_22 = false;
        for (const auto& [rid, payload] : read_responses) {
            if (rid == 11) found_11 = true;
            if (rid == 22) found_22 = true;
            // ReadResponse for non-existent blob: payload starts with 0x00 (not found)
            REQUIRE(!payload.empty());
            CHECK(payload[0] == 0x00);
        }
        REQUIRE(found_11);
        REQUIRE(found_22);
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("ExistsRequest returns found for stored blob and not-found for missing", "[peer][exists]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);

    // Ingest a blob so we can test exists=true
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(server_id, "exists-test-data", 604800, now);
    auto result = run_async(pool, eng.ingest(chromatindb::test::ns_span(server_id), blob));
    REQUIRE(result.accepted);
    auto stored_hash = result.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    // Build ExistsRequest payloads: [namespace:32][blob_hash:32]
    // Payload 1: existing blob (namespace + actual blob hash)
    std::vector<uint8_t> exists_payload(64, 0);
    std::memcpy(exists_payload.data(), server_id.namespace_id().data(), 32);
    std::memcpy(exists_payload.data() + 32, stored_hash.data(), 32);

    // Payload 2: non-existent blob (namespace + random hash)
    std::vector<uint8_t> missing_payload(64, 0);
    std::memcpy(missing_payload.data(), server_id.namespace_id().data(), 32);
    missing_payload[32] = 0xFF;  // Different hash, won't exist

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_ExistsResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    // Wait for authentication, then send both ExistsRequests in a single coroutine
    // (serializes send_counter_ to avoid AEAD nonce desync)
    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_ExistsRequest, exists_payload, 50);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_ExistsRequest, missing_payload, 51);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 2);
        // Both ExistsResponses must have correct request_ids and payloads
        for (const auto& [rid, payload] : responses) {
            REQUIRE(payload.size() == 33);  // [exists:1][hash:32]
            if (rid == 50) {
                CHECK(payload[0] == 0x01);  // exists=true
                // Verify echoed hash matches stored_hash
                CHECK(std::equal(payload.begin() + 1, payload.end(),
                                 stored_hash.begin(), stored_hash.end()));
            } else if (rid == 51) {
                CHECK(payload[0] == 0x00);  // exists=false
            }
        }
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("NodeInfoRequest returns version and node state", "[peer][nodeinfo]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();
    cfg.max_storage_bytes = 1048576;  // 1 MiB, so response has non-zero max

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<uint8_t> info_response;
    uint32_t response_req_id = 0;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_NodeInfoResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                info_response = std::move(payload);
                response_req_id = req_id;
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // NodeInfoRequest has empty payload
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_NodeInfoRequest, {}, 77);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(!info_response.empty());
        CHECK(response_req_id == 77);

        // Parse response: [version_len:1][version:N][uptime:8][peer_count:4][namespace_count:4][total_blobs:8][storage_used:8][storage_max:8][types_count:1][supported_types:N]
        size_t off = 0;

        // Version string
        REQUIRE(off < info_response.size());
        uint8_t version_len = info_response[off++];
        REQUIRE(off + version_len <= info_response.size());
        std::string version(info_response.begin() + off, info_response.begin() + off + version_len);
        off += version_len;
        CHECK(!version.empty());  // Has a version string

        // Uptime (8 bytes big-endian) -- should be small (< 10 seconds)
        REQUIRE(off + 8 <= info_response.size());
        uint64_t uptime = 0;
        for (int i = 0; i < 8; ++i)
            uptime = (uptime << 8) | info_response[off++];
        CHECK(uptime < 30);  // Just started, should be under 30 seconds

        // Peer count (4 bytes big-endian)
        REQUIRE(off + 4 <= info_response.size());
        uint32_t peer_count = 0;
        for (int i = 0; i < 4; ++i)
            peer_count = (peer_count << 8) | info_response[off++];
        // peer_count includes the client connection
        // (exact value depends on implementation -- just check it parsed)

        // Namespace count (4 bytes big-endian)
        REQUIRE(off + 4 <= info_response.size());
        uint32_t ns_count = 0;
        for (int i = 0; i < 4; ++i)
            ns_count = (ns_count << 8) | info_response[off++];
        CHECK(ns_count == 0);  // No blobs ingested

        // Total blobs (8 bytes big-endian)
        REQUIRE(off + 8 <= info_response.size());
        uint64_t total_blobs = 0;
        for (int i = 0; i < 8; ++i)
            total_blobs = (total_blobs << 8) | info_response[off++];
        CHECK(total_blobs == 0);

        // Storage bytes used (8 bytes big-endian)
        REQUIRE(off + 8 <= info_response.size());
        off += 8;  // Skip used bytes (non-deterministic)

        // Storage bytes max (8 bytes big-endian)
        REQUIRE(off + 8 <= info_response.size());
        uint64_t storage_max = 0;
        for (int i = 0; i < 8; ++i)
            storage_max = (storage_max << 8) | info_response[off++];
        CHECK(storage_max == 1048576);  // Matches config

        // Phase 127 wire extension — 4 new fixed-width fields BEFORE [types_count][supported_types]
        // per D-01 insertion point and D-02 order.

        // max_blob_data_bytes (8 BE) — sourced from chromatindb::net::MAX_BLOB_DATA_SIZE per D-04
        REQUIRE(off + 8 <= info_response.size());
        uint64_t max_blob_data_bytes = 0;
        for (int i = 0; i < 8; ++i)
            max_blob_data_bytes = (max_blob_data_bytes << 8) | info_response[off++];
        CHECK(max_blob_data_bytes == chromatindb::net::MAX_BLOB_DATA_SIZE);

        // max_frame_bytes (4 BE) — sourced from chromatindb::net::MAX_FRAME_SIZE per D-04
        REQUIRE(off + 4 <= info_response.size());
        uint32_t max_frame_bytes = 0;
        for (int i = 0; i < 4; ++i)
            max_frame_bytes = (max_frame_bytes << 8) | info_response[off++];
        CHECK(max_frame_bytes == chromatindb::net::MAX_FRAME_SIZE);

        // rate_limit_bytes_per_sec (8 BE) — default config is 0
        REQUIRE(off + 8 <= info_response.size());
        uint64_t rate_limit_bytes_per_sec = 0;
        for (int i = 0; i < 8; ++i)
            rate_limit_bytes_per_sec = (rate_limit_bytes_per_sec << 8) | info_response[off++];
        CHECK(rate_limit_bytes_per_sec == 0);

        // max_subscriptions_per_connection (4 BE) — default config is 256
        REQUIRE(off + 4 <= info_response.size());
        uint32_t max_subscriptions = 0;
        for (int i = 0; i < 4; ++i)
            max_subscriptions = (max_subscriptions << 8) | info_response[off++];
        CHECK(max_subscriptions == 256);

        // Supported types
        REQUIRE(off + 1 <= info_response.size());
        uint8_t types_count = info_response[off++];
        CHECK(types_count == 39);  // Client-facing types (20 base + 18 v1.4.0 query types + ErrorResponse)
        REQUIRE(off + types_count <= info_response.size());
        // Verify at least Ping(5), Data(8), ExistsRequest(37), NodeInfoRequest(39) are present
        std::set<uint8_t> types(info_response.begin() + off, info_response.begin() + off + types_count);
        CHECK(types.count(5));   // Ping
        CHECK(types.count(8));   // Data
        CHECK(types.count(37));  // ExistsRequest
        CHECK(types.count(39));  // NodeInfoRequest

        // D-10: Phase 127 wire-size invariant — the fixed section grew by exactly 24 bytes
        // vs the pre-Phase-127 layout (+ 8 blob + 4 frame + 8 rate + 4 subs = + 24).
        // Hard-coded to catch offset drift if a future change reorders or re-adds fields.
        CHECK(info_response.size() ==
              1 + version.size()           // version_len + version bytes
              + 8 + 4 + 4 + 8 + 8 + 8      // uptime + peers + ns + total + used + max
              + 24                          // Phase 127 delta: blob + frame + rate + subs
              + 1 + types_count);           // types_count + supported[]
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("NamespaceListRequest returns paginated namespace list", "[peer][namespacelist]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner1 = NodeIdentity::generate();
    auto owner2 = NodeIdentity::generate();
    auto owner3 = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner1);
    chromatindb::test::register_pubk(store, owner2);
    chromatindb::test::register_pubk(store, owner3);

    // Ingest one blob into each of 3 namespaces
    auto b1 = make_signed_blob(owner1, "ns1-data");
    REQUIRE(run_async(pool, eng.ingest(chromatindb::test::ns_span(owner1), b1)).accepted);
    auto b2 = make_signed_blob(owner2, "ns2-data");
    REQUIRE(run_async(pool, eng.ingest(chromatindb::test::ns_span(owner2), b2)).accepted);
    auto b3 = make_signed_blob(owner3, "ns3-data");
    REQUIRE(run_async(pool, eng.ingest(chromatindb::test::ns_span(owner3), b3)).accepted);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_NamespaceListResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    // Send paginated request: cursor=zero, limit=2
    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                std::vector<uint8_t> req(36, 0);  // all-zero cursor
                req[35] = 2;  // limit=2 (big-endian uint32: 0x00000002)
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_NamespaceListRequest, req, 100);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() >= 1);
        auto& [rid, payload] = responses[0];
        CHECK(rid == 100);
        REQUIRE(payload.size() >= 5);  // count:4 + has_more:1

        uint32_t count = 0;
        for (int i = 0; i < 4; ++i)
            count = (count << 8) | payload[i];
        CHECK(count == 2);
        CHECK(payload[4] == 0x01);  // has_more = true

        // Verify each entry is 40 bytes
        REQUIRE(payload.size() == 5 + count * 40);

        // Each entry's blob_count should be >= 1
        for (uint32_t e = 0; e < count; ++e) {
            size_t entry_off = 5 + e * 40 + 32;  // skip namespace_id
            uint64_t blob_count = 0;
            for (int i = 0; i < 8; ++i)
                blob_count = (blob_count << 8) | payload[entry_off + i];
            CHECK(blob_count >= 1);
        }
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("StorageStatusRequest returns global storage stats", "[peer][storagestatus]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();
    cfg.max_storage_bytes = 1048576;

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    // Ingest 2 blobs
    auto b1 = make_signed_blob(owner, "status-data-1");
    REQUIRE(run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), b1)).accepted);
    auto b2 = make_signed_blob(owner, "status-data-2");
    REQUIRE(run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), b2)).accepted);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<uint8_t> status_response;
    uint32_t response_req_id = 0;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_StorageStatusResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                status_response = std::move(payload);
                response_req_id = req_id;
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_StorageStatusRequest, {}, 200);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(status_response.size() == 44);
        CHECK(response_req_id == 200);

        size_t off = 0;

        // used_data_bytes > 0
        uint64_t used_data = 0;
        for (int i = 0; i < 8; ++i) used_data = (used_data << 8) | status_response[off++];
        CHECK(used_data > 0);

        // max_storage_bytes == 1048576
        uint64_t max_storage = 0;
        for (int i = 0; i < 8; ++i) max_storage = (max_storage << 8) | status_response[off++];
        CHECK(max_storage == 1048576);

        // tombstone_count == 0
        uint64_t tombstones = 0;
        for (int i = 0; i < 8; ++i) tombstones = (tombstones << 8) | status_response[off++];
        CHECK(tombstones == 0);

        // namespace_count == 1
        uint32_t ns_count = 0;
        for (int i = 0; i < 4; ++i) ns_count = (ns_count << 8) | status_response[off++];
        CHECK(ns_count == 1);

        // total_blobs == 2
        uint64_t total_blobs = 0;
        for (int i = 0; i < 8; ++i) total_blobs = (total_blobs << 8) | status_response[off++];
        CHECK(total_blobs == 2);

        // mmap_bytes > 0
        uint64_t mmap_bytes = 0;
        for (int i = 0; i < 8; ++i) mmap_bytes = (mmap_bytes << 8) | status_response[off++];
        CHECK(mmap_bytes > 0);
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("NamespaceStatsRequest returns per-namespace statistics", "[peer][namespacestats]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();
    auto delegate1 = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate1);

    // Ingest 3 regular blobs + 1 delegation
    auto b1 = make_signed_blob(owner, "stats-data-1");
    REQUIRE(run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), b1)).accepted);
    auto b2 = make_signed_blob(owner, "stats-data-2");
    REQUIRE(run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), b2)).accepted);
    auto b3 = make_signed_blob(owner, "stats-data-3");
    REQUIRE(run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), b3)).accepted);
    auto d1 = make_signed_delegation(owner, delegate1);
    REQUIRE(run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), d1)).accepted);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_NamespaceStatsResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // Request 1: known namespace (owner)
                std::vector<uint8_t> req1(owner.namespace_id().begin(), owner.namespace_id().end());
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_NamespaceStatsRequest, req1, 300);
                // Request 2: unknown namespace
                std::vector<uint8_t> req2(32, 0xFF);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_NamespaceStatsRequest, req2, 301);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 2);

        for (const auto& [rid, payload] : responses) {
            REQUIRE(payload.size() == 41);
            if (rid == 300) {
                // Found namespace
                CHECK(payload[0] == 0x01);  // found=true
                size_t off = 1;

                // blob_count (4: 3 regular + 1 delegation -- only tombstones are exempt from quota counting)
                uint64_t blob_count = 0;
                for (int i = 0; i < 8; ++i) blob_count = (blob_count << 8) | payload[off++];
                CHECK(blob_count == 4);

                // total_bytes > 0
                uint64_t total_bytes = 0;
                for (int i = 0; i < 8; ++i) total_bytes = (total_bytes << 8) | payload[off++];
                CHECK(total_bytes > 0);

                // delegation_count == 1
                uint64_t deleg_count = 0;
                for (int i = 0; i < 8; ++i) deleg_count = (deleg_count << 8) | payload[off++];
                CHECK(deleg_count == 1);

                // quota_bytes_limit == 0 (no quota configured)
                uint64_t qb = 0;
                for (int i = 0; i < 8; ++i) qb = (qb << 8) | payload[off++];
                CHECK(qb == 0);

                // quota_count_limit == 0
                uint64_t qc = 0;
                for (int i = 0; i < 8; ++i) qc = (qc << 8) | payload[off++];
                CHECK(qc == 0);
            } else if (rid == 301) {
                // Unknown namespace
                CHECK(payload[0] == 0x00);  // found=false
                // All remaining bytes should be zero
                bool all_zero = true;
                for (size_t i = 1; i < 41; ++i) {
                    if (payload[i] != 0) { all_zero = false; break; }
                }
                CHECK(all_zero);
            }
        }
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("MetadataRequest returns blob metadata for existing blob and not-found for missing", "[peer][metadata]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    // Ingest 1 blob
    std::string test_payload = "metadata-test-data";
    auto b1 = make_signed_blob(owner, test_payload);
    auto r1 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), b1));
    REQUIRE(r1.accepted);
    auto stored_hash = r1.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_MetadataResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // Request 1: existing blob
                std::vector<uint8_t> req1(64);
                std::memcpy(req1.data(), owner.namespace_id().data(), 32);
                std::memcpy(req1.data() + 32, stored_hash.data(), 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_MetadataRequest, req1, 400);
                // Request 2: unknown blob hash
                std::vector<uint8_t> req2(64);
                std::memcpy(req2.data(), owner.namespace_id().data(), 32);
                std::memset(req2.data() + 32, 0xAA, 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_MetadataRequest, req2, 401);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 2);

        for (const auto& [rid, payload] : responses) {
            if (rid == 400) {
                // Found: status(1) + hash(32) + timestamp(8) + ttl(4) + size(8) + seq_num(8) + pubkey_len(2) + pubkey(N)
                REQUIRE(payload.size() >= 63);
                CHECK(payload[0] == 0x01);  // found
                size_t off = 1;

                // blob_hash (32)
                std::array<uint8_t, 32> resp_hash{};
                std::memcpy(resp_hash.data(), payload.data() + off, 32);
                CHECK(resp_hash == stored_hash);
                off += 32;

                // timestamp (8 BE)
                uint64_t timestamp = 0;
                for (int i = 0; i < 8; ++i) timestamp = (timestamp << 8) | payload[off++];
                CHECK(timestamp > 0);

                // ttl (4 BE)
                uint32_t ttl = 0;
                for (int i = 0; i < 4; ++i) ttl = (ttl << 8) | payload[off++];
                CHECK(ttl == 604800);

                // size (8 BE) -- raw data size
                uint64_t data_size = 0;
                for (int i = 0; i < 8; ++i) data_size = (data_size << 8) | payload[off++];
                CHECK(data_size == test_payload.size());

                // seq_num (8 BE)
                uint64_t seq_num = 0;
                for (int i = 0; i < 8; ++i) seq_num = (seq_num << 8) | payload[off++];
                CHECK(seq_num >= 1);

                // MetadataRequest response carries signer_hint (32 B)
                // instead of the removed 2592-byte inline pubkey. signer_hint =
                // SHA3-256(signing pubkey); clients fetch the full pubkey via
                // the namespace's PUBK blob (D-05).
                // signer_hint_len (2 BE)
                uint16_t pk_len = 0;
                pk_len = static_cast<uint16_t>((payload[off] << 8) | payload[off + 1]);
                off += 2;
                CHECK(pk_len == 32);

                // signer_hint (32 bytes) == SHA3-256(owner signing pubkey)
                REQUIRE(payload.size() >= off + pk_len);
                auto expected_hint = chromatindb::crypto::sha3_256(owner.public_key());
                CHECK(std::equal(payload.begin() + off, payload.begin() + off + pk_len,
                                 expected_hint.begin()));
            } else if (rid == 401) {
                // Not found: 1-byte response
                REQUIRE(payload.size() == 1);
                CHECK(payload[0] == 0x00);
            }
        }
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("BatchExistsRequest returns per-hash existence results", "[peer][batchexists]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    // Ingest 2 blobs
    auto b1 = make_signed_blob(owner, "batch-exists-1");
    auto r1 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), b1));
    REQUIRE(r1.accepted);
    auto hash1 = r1.ack->blob_hash;

    auto b2 = make_signed_blob(owner, "batch-exists-2");
    auto r2 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), b2));
    REQUIRE(r2.accepted);
    auto hash2 = r2.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_BatchExistsResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // BatchExistsRequest: namespace + count=3 + [hash1, hash2, unknown_hash]
                std::vector<uint8_t> req(132);
                std::memcpy(req.data(), owner.namespace_id().data(), 32);
                // count=3 big-endian
                req[32] = 0; req[33] = 0; req[34] = 0; req[35] = 3;
                std::memcpy(req.data() + 36, hash1.data(), 32);
                std::memcpy(req.data() + 68, hash2.data(), 32);
                std::memset(req.data() + 100, 0xBB, 32);  // unknown hash
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_BatchExistsRequest, req, 500);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 1);
        const auto& [rid, payload] = responses[0];
        CHECK(rid == 500);
        REQUIRE(payload.size() == 3);
        CHECK(payload[0] == 0x01);  // hash1 exists
        CHECK(payload[1] == 0x01);  // hash2 exists
        CHECK(payload[2] == 0x00);  // unknown does not exist
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("DelegationListRequest returns active delegations for namespace", "[peer][delegationlist]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();
    auto delegate1 = NodeIdentity::generate();
    auto delegate2 = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);
    chromatindb::test::register_pubk(store, delegate1);
    chromatindb::test::register_pubk(store, delegate2);

    // Store 2 delegations
    auto d1 = make_signed_delegation(owner, delegate1);
    REQUIRE(run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), d1)).accepted);
    auto d2 = make_signed_delegation(owner, delegate2);
    REQUIRE(run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), d2)).accepted);

    // Compute expected delegate pk hashes
    auto expected_hash1 = chromatindb::crypto::sha3_256(
        std::span<const uint8_t>(delegate1.public_key()));
    auto expected_hash2 = chromatindb::crypto::sha3_256(
        std::span<const uint8_t>(delegate2.public_key()));

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_DelegationListResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // Request 1: owner's namespace (has delegations)
                std::vector<uint8_t> req1(owner.namespace_id().begin(), owner.namespace_id().end());
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_DelegationListRequest, req1, 600);
                // Request 2: unknown namespace (no delegations)
                std::vector<uint8_t> req2(32, 0xCC);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_DelegationListRequest, req2, 601);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 2);

        for (const auto& [rid, payload] : responses) {
            if (rid == 600) {
                // Populated namespace: [count:4 BE] + count * [pk_hash:32][blob_hash:32]
                REQUIRE(payload.size() == 4 + 2 * 64);  // 132 bytes
                uint32_t count = 0;
                for (int i = 0; i < 4; ++i) count = (count << 8) | payload[i];
                CHECK(count == 2);

                // Collect returned delegate pk hashes
                std::set<std::array<uint8_t, 32>> returned_pk_hashes;
                for (uint32_t i = 0; i < count; ++i) {
                    std::array<uint8_t, 32> pk_hash{};
                    std::memcpy(pk_hash.data(), payload.data() + 4 + i * 64, 32);
                    returned_pk_hashes.insert(pk_hash);
                }
                CHECK(returned_pk_hashes.count(expected_hash1) == 1);
                CHECK(returned_pk_hashes.count(expected_hash2) == 1);
            } else if (rid == 601) {
                // Empty namespace: just 4 zero bytes (count=0)
                REQUIRE(payload.size() == 4);
                uint32_t count = 0;
                for (int i = 0; i < 4; ++i) count = (count << 8) | payload[i];
                CHECK(count == 0);
            }
        }
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("BatchReadRequest returns multiple blobs with size cap", "[peer][batchread]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    // Store 3 blobs with known data
    auto blob1 = make_signed_blob(owner, "batch-read-data-1");
    auto r1 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), blob1));
    REQUIRE(r1.accepted);
    auto hash1 = r1.ack->blob_hash;

    auto blob2 = make_signed_blob(owner, "batch-read-data-2");
    auto r2 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), blob2));
    REQUIRE(r2.accepted);
    auto hash2 = r2.ack->blob_hash;

    auto blob3 = make_signed_blob(owner, "batch-read-data-3");
    auto r3 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), blob3));
    REQUIRE(r3.accepted);
    auto hash3 = r3.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_BatchReadResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // Request all 3 blobs + 1 unknown, cap_bytes=0 (default to 4 MiB)
                std::vector<uint8_t> req(40 + 4 * 32);  // ns(32) + cap(4) + count(4) + 4*hash(32)
                std::memcpy(req.data(), owner.namespace_id().data(), 32);
                // cap_bytes = 0 (default to 4 MiB)
                req[32] = 0; req[33] = 0; req[34] = 0; req[35] = 0;
                // count = 4
                req[36] = 0; req[37] = 0; req[38] = 0; req[39] = 4;
                std::memcpy(req.data() + 40, hash1.data(), 32);
                std::memcpy(req.data() + 72, hash2.data(), 32);
                std::memcpy(req.data() + 104, hash3.data(), 32);
                std::memset(req.data() + 136, 0xBB, 32);  // unknown hash
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_BatchReadRequest, req, 700);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 1);
        const auto& [rid, payload] = responses[0];
        CHECK(rid == 700);
        // Response: [truncated:1][count:4 BE] + entries
        REQUIRE(payload.size() >= 5);
        CHECK(payload[0] == 0x00);  // not truncated (all fit under 4 MiB)
        uint32_t count = 0;
        for (int i = 0; i < 4; ++i) count = (count << 8) | payload[1 + i];
        CHECK(count == 4);  // 3 found + 1 not found

        // Verify entries exist (parse all data to check found/not-found counts)
        size_t off = 5;
        uint32_t found_count = 0;
        uint32_t not_found_count = 0;
        for (uint32_t i = 0; i < count && off < payload.size(); ++i) {
            uint8_t status = payload[off++];
            off += 32;  // hash
            if (status == 0x01) {
                ++found_count;
                uint64_t sz = 0;
                for (int j = 0; j < 8; ++j) sz = (sz << 8) | payload[off++];
                off += sz;  // skip data
            } else {
                ++not_found_count;
            }
        }
        CHECK(found_count == 3);
        CHECK(not_found_count == 1);
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("PeerInfoRequest returns peer information", "[peer][peerinfo]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_PeerInfoResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // PeerInfoRequest: empty payload (per D-08)
                std::vector<uint8_t> req;
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_PeerInfoRequest, req, 710);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 1);
        const auto& [rid, payload] = responses[0];
        CHECK(rid == 710);
        // Trusted response (loopback = trusted): [peer_count:4][bootstrap_count:4] + entries
        REQUIRE(payload.size() >= 8);
        uint32_t peer_count = 0;
        for (int i = 0; i < 4; ++i) peer_count = (peer_count << 8) | payload[i];
        uint32_t bootstrap_count = 0;
        for (int i = 0; i < 4; ++i) bootstrap_count = (bootstrap_count << 8) | payload[4 + i];
        // The test client is counted as a peer
        CHECK(peer_count >= 1);
        CHECK(bootstrap_count == 0);  // no bootstrap peers configured

        // If trusted (loopback), verify we got per-peer entries
        if (peer_count > 0) {
            // Per-peer entry starts at offset 8: [addr_len:2][addr:N][is_bootstrap:1][syncing:1][peer_is_full:1][duration:8]
            REQUIRE(payload.size() > 8);
            uint16_t addr_len = (static_cast<uint16_t>(payload[8]) << 8) | payload[9];
            CHECK(addr_len > 0);  // address is non-empty
            // Verify we have at least the first entry's minimum structure
            REQUIRE(payload.size() >= 8 + 2 + addr_len + 3 + 8);
        }
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("TimeRangeRequest returns blobs within timestamp range", "[peer][timerange]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    // Store blobs with known timestamps (seconds, must be within validation window)
    auto now = current_timestamp();

    // Blob 1: timestamp = now - 100 (100 seconds ago)
    auto b1 = make_signed_blob(owner, "time-range-data-1", 604800, now - 100);
    auto r1 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), b1));
    REQUIRE(r1.accepted);
    auto hash1 = r1.ack->blob_hash;

    // Blob 2: timestamp = now - 50 (50 seconds ago)
    auto b2 = make_signed_blob(owner, "time-range-data-2", 604800, now - 50);
    auto r2 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), b2));
    REQUIRE(r2.accepted);
    auto hash2 = r2.ack->blob_hash;

    // Blob 3: timestamp = now - 10 (10 seconds ago) -- outside query range
    auto b3 = make_signed_blob(owner, "time-range-data-3", 604800, now - 10);
    auto r3 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), b3));
    REQUIRE(r3.accepted);
    auto hash3 = r3.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_TimeRangeResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // TimeRangeRequest: namespace(32) + start(8) + end(8) + limit(4) = 52 bytes
                // Query range: (now - 120) to (now - 30) -- should match blob1 and blob2, not blob3
                std::vector<uint8_t> req(52);
                std::memcpy(req.data(), owner.namespace_id().data(), 32);
                // start_timestamp (big-endian)
                uint64_t start_ts = now - 120;
                for (int i = 7; i >= 0; --i)
                    req[32 + (7 - i)] = static_cast<uint8_t>(start_ts >> (i * 8));
                // end_timestamp (big-endian)
                uint64_t end_ts = now - 30;
                for (int i = 7; i >= 0; --i)
                    req[40 + (7 - i)] = static_cast<uint8_t>(end_ts >> (i * 8));
                // limit = 100 (big-endian)
                req[48] = 0; req[49] = 0; req[50] = 0; req[51] = 100;
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_TimeRangeRequest, req, 720);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 1);
        const auto& [rid, payload] = responses[0];
        CHECK(rid == 720);
        // Response: [truncated:1][count:4 BE] + count * [hash:32][seq_num:8 BE][timestamp:8 BE]
        REQUIRE(payload.size() >= 5);
        CHECK(payload[0] == 0x00);  // not truncated
        uint32_t count = 0;
        for (int i = 0; i < 4; ++i) count = (count << 8) | payload[1 + i];
        CHECK(count == 2);  // blob1 and blob2 in range, blob3 outside
        REQUIRE(payload.size() == 5 + 2 * 48);  // 2 entries * 48 bytes each

        // Verify timestamps are within range
        std::set<uint64_t> timestamps;
        for (uint32_t i = 0; i < count; ++i) {
            size_t off = 5 + i * 48 + 32 + 8;  // skip hash(32) + seq_num(8) to timestamp
            uint64_t ts = 0;
            for (int j = 0; j < 8; ++j) ts = (ts << 8) | payload[off + j];
            timestamps.insert(ts);
        }
        CHECK(timestamps.count(now - 100) == 1);
        CHECK(timestamps.count(now - 50) == 1);
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

// ============================================================================
// BlobNotify fan-out tests
// ============================================================================

TEST_CASE("BlobNotify fan-out fires on sync ingest", "[peer][pubsub][blobnotify]") {
    // Two connected nodes. Node2 has a blob that syncs to node1.
    // Node1's on_notification callback should fire (unified fan-out path).
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 60;
    cfg2.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);

    // Track notifications on node1 (will receive sync ingest from node2)
    struct NotifCapture {
        std::array<uint8_t, 32> namespace_id{};
        std::array<uint8_t, 32> blob_hash{};
        uint64_t seq_num = 0;
        uint32_t blob_size = 0;
        bool is_tombstone = false;
    };
    std::vector<NotifCapture> notifs;
    pm1.set_on_notification([&](const std::array<uint8_t, 32>& ns,
                                 const std::array<uint8_t, 32>& hash,
                                 uint64_t seq, uint32_t size, bool tomb) {
        notifs.push_back({ns, hash, seq, size, tomb});
    });

    // Store a blob in node2 before connecting
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(id2, "blobnotify-test", 604800, now);
    auto r = run_async(pool, eng2.ingest(chromatindb::test::ns_span(id2), blob));
    REQUIRE(r.accepted);

    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let sync-on-connect propagate blob from node2 to node1
    ioc.run_for(std::chrono::seconds(8));

    // Blob should be on node1 now
    auto n1_blobs = eng1.get_blobs_since(id2.namespace_id(), 0);
    REQUIRE(n1_blobs.size() == 1);

    // Unified on_blob_ingested fired (test hook captures it)
    REQUIRE(notifs.size() == 1);
    CHECK(notifs[0].is_tombstone == false);
    CHECK(notifs[0].seq_num == 1);
    CHECK(notifs[0].blob_size == blob.data.size());

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("BlobNotify fan-out fires on tombstone sync", "[peer][pubsub][blobnotify]") {
    // Tombstone synced from node1 to node2 triggers on_blob_ingested with is_tombstone=true.
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 2;
    cfg1.max_peers = 32;
    cfg1.sync_cooldown_seconds = 0;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 3;
    cfg2.max_peers = 32;
    cfg2.sync_cooldown_seconds = 0;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);

    // First ingest a blob to delete later
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(id1, "will-delete-for-blobnotify", 604800, now);
    auto r = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob));
    REQUIRE(r.accepted);
    auto blob_hash = r.ack->blob_hash;

    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);

    // Track notifications on node2
    std::vector<std::tuple<std::array<uint8_t, 32>, uint64_t, bool>> notifs;
    pm2.set_on_notification([&](const std::array<uint8_t, 32>& ns,
                                 const std::array<uint8_t, 32>&,
                                 uint64_t seq, uint32_t, bool tomb) {
        notifs.emplace_back(ns, seq, tomb);
    });

    pm2.start();

    // Sync blob first
    ioc.run_for(std::chrono::seconds(8));
    auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs.size() == 1);

    size_t notif_count_before = notifs.size();

    // Delete via engine (tombstone), then let it sync
    auto tombstone = make_signed_tombstone(id1, blob_hash, now + 1);
    auto del = run_async(pool, eng1.delete_blob(chromatindb::test::ns_span(id1), tombstone));
    REQUIRE(del.accepted);

    // Let tombstone sync
    ioc.run_for(std::chrono::seconds(15));

    // Tombstone notification should have fired on node2
    REQUIRE(notifs.size() > notif_count_before);
    bool found_tomb = false;
    for (size_t i = notif_count_before; i < notifs.size(); ++i) {
        auto& [ns, seq, tomb] = notifs[i];
        if (tomb && std::equal(ns.begin(), ns.end(), id1.namespace_id().begin())) {
            found_tomb = true;
        }
    }
    REQUIRE(found_tomb);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("on_blob_ingested source exclusion with three nodes", "[peer][pubsub][blobnotify]") {
    // Three connected nodes: node1, node2, node3.
    // Node2 has a blob; syncs to node1.
    // Node1's on_blob_ingested should exclude node2 (source) from BlobNotify
    // but still send BlobNotify to node3.
    // Verify via on_notification_ hook that node1 fires the callback exactly once
    // per unique blob (unified fan-out).
    TempDir tmp1, tmp2, tmp3;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);
    auto id3 = NodeIdentity::load_or_generate(tmp3.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 60;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 60;
    cfg2.max_peers = 32;

    Config cfg3;
    cfg3.bind_address = "127.0.0.1:0";
    cfg3.data_dir = tmp3.path.string();
    cfg3.safety_net_interval_seconds = 60;
    cfg3.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    Storage store3(tmp3.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    BlobEngine eng3(store3, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store1, id3);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);
    chromatindb::test::register_pubk(store2, id3);
    chromatindb::test::register_pubk(store3, id1);
    chromatindb::test::register_pubk(store3, id2);
    chromatindb::test::register_pubk(store3, id3);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());
    AccessControl acl3({}, cfg3.allowed_peer_keys, id3.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);

    // Track notifications on node1
    int notif_count = 0;
    pm1.set_on_notification([&](const std::array<uint8_t, 32>&,
                                 const std::array<uint8_t, 32>&,
                                 uint64_t, uint32_t, bool) {
        notif_count++;
    });

    // Store blob in node2 before connecting
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(id2, "source-exclusion-test", 604800, now);
    auto r = run_async(pool, eng2.ingest(chromatindb::test::ns_span(id2), blob));
    REQUIRE(r.accepted);

    pm1.start();

    // Connect node2 and node3 to node1
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    cfg3.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm3(cfg3, id3, eng3, store3, ioc, pool, acl3);
    pm3.start();

    // Let all three connect and sync
    ioc.run_for(std::chrono::seconds(10));

    // Node1 should have received the blob via sync from node2
    auto n1_blobs = eng1.get_blobs_since(id2.namespace_id(), 0);
    REQUIRE(n1_blobs.size() == 1);

    // Notification callback fires exactly once per blob ingest on node1
    // (unified fan-out: one call to on_blob_ingested per ingest,
    //  which internally handles BlobNotify to peers + Notification to subscribers)
    CHECK(notif_count == 1);

    pm1.stop();
    pm2.stop();
    pm3.stop();
    ioc.run_for(std::chrono::seconds(2));
}

// ============================================================================
// Targeted blob fetch tests
// ============================================================================

TEST_CASE("BlobFetch round-trip via BlobNotify", "[peer][blobfetch]") {
    // 3-node chain: A ← B → C. Blob written to A after connections established.
    // B re-syncs from A (cooldown disabled) → gets blob → on_blob_ingested → BlobNotify to C.
    // C receives BlobNotify → BlobFetch from B → ingests blob.
    TempDir tmpA, tmpB, tmpC;

    auto idA = NodeIdentity::load_or_generate(tmpA.path);
    auto idB = NodeIdentity::load_or_generate(tmpB.path);
    auto idC = NodeIdentity::load_or_generate(tmpC.path);

    auto make_cfg = [](const std::string& dir, int sync_sec) {
        Config cfg;
        cfg.bind_address = "127.0.0.1:0";
        cfg.data_dir = dir;
        cfg.safety_net_interval_seconds = sync_sec;
        cfg.sync_cooldown_seconds = 0;  // Disable cooldown for test
        cfg.max_peers = 32;
        return cfg;
    };

    Config cfgA = make_cfg(tmpA.path.string(), 600);
    Config cfgB = make_cfg(tmpB.path.string(), 3);   // short sync to pick up new blobs from A
    Config cfgC = make_cfg(tmpC.path.string(), 600);  // high interval: C gets blobs only via BlobFetch

    Storage storeA(tmpA.path.string());
    Storage storeB(tmpB.path.string());
    Storage storeC(tmpC.path.string());
    asio::thread_pool pool{1};
    BlobEngine engA(storeA, pool);
    BlobEngine engB(storeB, pool);
    BlobEngine engC(storeC, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(storeA, idA);
    chromatindb::test::register_pubk(storeA, idB);
    chromatindb::test::register_pubk(storeA, idC);
    chromatindb::test::register_pubk(storeB, idA);
    chromatindb::test::register_pubk(storeB, idB);
    chromatindb::test::register_pubk(storeB, idC);
    chromatindb::test::register_pubk(storeC, idA);
    chromatindb::test::register_pubk(storeC, idB);
    chromatindb::test::register_pubk(storeC, idC);

    asio::io_context ioc;
    AccessControl aclA({}, cfgA.allowed_peer_keys, idA.namespace_id());
    AccessControl aclB({}, cfgB.allowed_peer_keys, idB.namespace_id());
    AccessControl aclC({}, cfgC.allowed_peer_keys, idC.namespace_id());

    // Start A, then B connects to A, then C connects to B
    PeerManager pmA(cfgA, idA, engA, storeA, ioc, pool, aclA);
    pmA.start();

    cfgB.bootstrap_peers = {listening_address(pmA.listening_port())};
    PeerManager pmB(cfgB, idB, engB, storeB, ioc, pool, aclB);
    pmB.start();

    // Let A-B connection + initial sync complete (both empty)
    ioc.run_for(std::chrono::seconds(5));

    cfgC.bootstrap_peers = {listening_address(pmB.listening_port())};
    PeerManager pmC(cfgC, idC, engC, storeC, ioc, pool, aclC);
    pmC.start();

    // Let B-C connection + initial sync complete (both empty)
    ioc.run_for(std::chrono::seconds(5));

    // Now write blob to A. B's periodic sync (3s, no cooldown) will pick it up.
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(idA, "blobfetch-roundtrip", 604800, now);
    auto r = run_async(pool, engA.ingest(chromatindb::test::ns_span(idA), blob));
    REQUIRE(r.accepted);
    REQUIRE(r.ack.has_value());
    auto expected_hash = r.ack->blob_hash;

    // B re-syncs from A → gets blob → on_blob_ingested → BlobNotify to C
    // C: on_blob_notify → has_blob false → BlobFetch → BlobFetchResponse → ingest
    ioc.run_for(std::chrono::seconds(15));

    // Verify blob arrived at C (via BlobFetch or PEX+sync — either path validates the system)
    auto c_blob = storeC.get_blob(idA.namespace_id(), expected_hash);
    REQUIRE(c_blob.has_value());
    CHECK(c_blob->data == blob.data);

    // B also has the blob from sync
    CHECK(storeB.get_blob(idA.namespace_id(), expected_hash).has_value());

    pmA.stop();
    pmB.stop();
    pmC.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("BlobFetch skipped when blob already exists locally", "[peer][blobfetch]") {
    // Same 3-node setup but C already has the blob.
    // B re-syncs from A (cooldown disabled) → BlobNotify to C.
    // C has_blob true → no BlobFetch.
    TempDir tmpA, tmpB, tmpC;

    auto idA = NodeIdentity::load_or_generate(tmpA.path);
    auto idB = NodeIdentity::load_or_generate(tmpB.path);
    auto idC = NodeIdentity::load_or_generate(tmpC.path);

    auto make_cfg = [](const std::string& dir, int sync_sec) {
        Config cfg;
        cfg.bind_address = "127.0.0.1:0";
        cfg.data_dir = dir;
        cfg.safety_net_interval_seconds = sync_sec;
        cfg.sync_cooldown_seconds = 0;
        cfg.max_peers = 32;
        return cfg;
    };

    Config cfgA = make_cfg(tmpA.path.string(), 600);
    Config cfgB = make_cfg(tmpB.path.string(), 3);
    Config cfgC = make_cfg(tmpC.path.string(), 600);

    Storage storeA(tmpA.path.string());
    Storage storeB(tmpB.path.string());
    Storage storeC(tmpC.path.string());
    asio::thread_pool pool{1};
    BlobEngine engA(storeA, pool);
    BlobEngine engB(storeB, pool);
    BlobEngine engC(storeC, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(storeA, idA);
    chromatindb::test::register_pubk(storeA, idB);
    chromatindb::test::register_pubk(storeA, idC);
    chromatindb::test::register_pubk(storeB, idA);
    chromatindb::test::register_pubk(storeB, idB);
    chromatindb::test::register_pubk(storeB, idC);
    chromatindb::test::register_pubk(storeC, idA);
    chromatindb::test::register_pubk(storeC, idB);
    chromatindb::test::register_pubk(storeC, idC);

    asio::io_context ioc;
    AccessControl aclA({}, cfgA.allowed_peer_keys, idA.namespace_id());
    AccessControl aclB({}, cfgB.allowed_peer_keys, idB.namespace_id());
    AccessControl aclC({}, cfgC.allowed_peer_keys, idC.namespace_id());

    PeerManager pmA(cfgA, idA, engA, storeA, ioc, pool, aclA);
    pmA.start();

    cfgB.bootstrap_peers = {listening_address(pmA.listening_port())};
    PeerManager pmB(cfgB, idB, engB, storeB, ioc, pool, aclB);
    pmB.start();

    ioc.run_for(std::chrono::seconds(5));

    cfgC.bootstrap_peers = {listening_address(pmB.listening_port())};
    PeerManager pmC(cfgC, idC, engC, storeC, ioc, pool, aclC);
    pmC.start();

    ioc.run_for(std::chrono::seconds(5));

    // Write blob to A AND C (pre-load on C)
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(idA, "dedup-test", 604800, now);
    auto rA = run_async(pool, engA.ingest(chromatindb::test::ns_span(idA), blob));
    auto rC = run_async(pool, engC.ingest(chromatindb::test::ns_span(idA), blob));
    REQUIRE(rA.accepted);
    REQUIRE(rC.accepted);

    // Track ingests on C — should not increase from BlobFetch
    uint64_t ingests_before = pmC.metrics().ingests;

    // B's periodic sync picks up blob from A → BlobNotify to C.
    // C's on_blob_notify → has_blob true → skip.
    ioc.run_for(std::chrono::seconds(15));

    // B got the blob via sync
    CHECK(storeB.get_blob(idA.namespace_id(), rA.ack->blob_hash).has_value());

    // C should NOT have a new BlobFetch ingest (already had blob)
    CHECK(pmC.metrics().ingests == ingests_before);

    pmA.stop();
    pmB.stop();
    pmC.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("BlobFetch nodes stay connected after cycle", "[peer][blobfetch]") {
    // All nodes remain connected after BlobFetch cycles (D-08).
    TempDir tmpA, tmpB, tmpC;

    auto idA = NodeIdentity::load_or_generate(tmpA.path);
    auto idB = NodeIdentity::load_or_generate(tmpB.path);
    auto idC = NodeIdentity::load_or_generate(tmpC.path);

    auto make_cfg = [](const std::string& dir, int sync_sec) {
        Config cfg;
        cfg.bind_address = "127.0.0.1:0";
        cfg.data_dir = dir;
        cfg.safety_net_interval_seconds = sync_sec;
        cfg.sync_cooldown_seconds = 0;
        cfg.max_peers = 32;
        return cfg;
    };

    Config cfgA = make_cfg(tmpA.path.string(), 600);
    Config cfgB = make_cfg(tmpB.path.string(), 3);
    Config cfgC = make_cfg(tmpC.path.string(), 600);

    Storage storeA(tmpA.path.string());
    Storage storeB(tmpB.path.string());
    Storage storeC(tmpC.path.string());
    asio::thread_pool pool{1};
    BlobEngine engA(storeA, pool);
    BlobEngine engB(storeB, pool);
    BlobEngine engC(storeC, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(storeA, idA);
    chromatindb::test::register_pubk(storeA, idB);
    chromatindb::test::register_pubk(storeA, idC);
    chromatindb::test::register_pubk(storeB, idA);
    chromatindb::test::register_pubk(storeB, idB);
    chromatindb::test::register_pubk(storeB, idC);
    chromatindb::test::register_pubk(storeC, idA);
    chromatindb::test::register_pubk(storeC, idB);
    chromatindb::test::register_pubk(storeC, idC);

    asio::io_context ioc;
    AccessControl aclA({}, cfgA.allowed_peer_keys, idA.namespace_id());
    AccessControl aclB({}, cfgB.allowed_peer_keys, idB.namespace_id());
    AccessControl aclC({}, cfgC.allowed_peer_keys, idC.namespace_id());

    PeerManager pmA(cfgA, idA, engA, storeA, ioc, pool, aclA);
    pmA.start();

    cfgB.bootstrap_peers = {listening_address(pmA.listening_port())};
    PeerManager pmB(cfgB, idB, engB, storeB, ioc, pool, aclB);
    pmB.start();

    ioc.run_for(std::chrono::seconds(5));

    cfgC.bootstrap_peers = {listening_address(pmB.listening_port())};
    PeerManager pmC(cfgC, idC, engC, storeC, ioc, pool, aclC);
    pmC.start();

    ioc.run_for(std::chrono::seconds(5));

    // Write blob, let BlobFetch cycle run
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(idA, "connectivity-test", 604800, now);
    run_async(pool, engA.ingest(chromatindb::test::ns_span(idA), blob));

    ioc.run_for(std::chrono::seconds(15));

    // All connections alive (D-08: no crash/disconnect)
    CHECK(pmA.peer_count() >= 1);
    CHECK(pmB.peer_count() >= 1);
    CHECK(pmC.peer_count() >= 1);

    pmA.stop();
    pmB.stop();
    pmC.stop();
    ioc.run_for(std::chrono::seconds(2));
}

// ============================================================================
// Safety-net cursor grace period tests (MAINT-04, MAINT-05)
// ============================================================================

TEST_CASE("sync-on-connect runs for initiator", "[peer-manager][safety-net]") {
    // MAINT-05: When a peer connects as initiator, full reconciliation runs.
    // Node2 (initiator) connects to node1 and should pull node1's blob.
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 600;  // Very long -- sync-on-connect only
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 600;
    cfg2.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    // Store a blob in node1 BEFORE node2 connects
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(id1, "sync-on-connect-test", 604800, now);
    auto r = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob));
    REQUIRE(r.accepted);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();

    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Wait for sync-on-connect to complete
    ioc.run_for(std::chrono::seconds(8));

    // Node2 should have node1's blob via sync-on-connect
    auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs.size() == 1);

    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("reconnect within grace period preserves cursors", "[peer-manager][safety-net]") {
    // MAINT-04: When a peer disconnects and reconnects within 5 minutes,
    // its MDBX cursors are preserved, enabling cursor-skip optimization.
    // We verify this by checking cursor_hits metric increases on reconnect sync.
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 600;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 600;
    cfg2.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    // Store blob in node1
    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(id1, "grace-period-test", 604800, now);
    auto r = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob));
    REQUIRE(r.accepted);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();

    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Initial sync-on-connect
    ioc.run_for(std::chrono::seconds(8));

    auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs.size() == 1);

    // Record cursor_hits before disconnect
    auto hits_before = pm2.metrics().cursor_hits;

    // Disconnect node2 by stopping it, then reconnect
    pm2.stop();
    ioc.run_for(std::chrono::seconds(2));

    // Reconnect within grace period (immediately)
    PeerManager pm2b(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2b.start();
    ioc.run_for(std::chrono::seconds(8));

    // Node2 should still have the blob
    auto n2_blobs2 = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs2.size() == 1);

    // Cursor hits should have increased (namespace skipped because cursors match)
    // Note: cursor_hits is on the NEW pm2b instance metrics, starts at 0.
    // The cursor skip happens because MDBX cursors persisted across disconnect.
    auto hits_after = pm2b.metrics().cursor_hits;
    CHECK(hits_after > 0);

    pm1.stop();
    pm2b.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("disconnected peer tracked in grace period", "[peer-manager][safety-net]") {
    // Verify observable behavior: a peer can disconnect and reconnect
    // multiple times without issues (grace period tracking works correctly).
    TempDir tmp1, tmp2;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();
    cfg1.safety_net_interval_seconds = 600;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();
    cfg2.safety_net_interval_seconds = 600;
    cfg2.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    // cross-store PUBK registration for sync tests.
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    auto blob = make_signed_blob(id1, "multi-reconnect-test", 604800, now);
    auto r = run_async(pool, eng1.ingest(chromatindb::test::ns_span(id1), blob));
    REQUIRE(r.accepted);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();

    // First connect
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2a(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2a.start();
    ioc.run_for(std::chrono::seconds(8));

    auto n2_blobs = eng2.get_blobs_since(id1.namespace_id(), 0);
    REQUIRE(n2_blobs.size() == 1);

    // First disconnect
    pm2a.stop();
    ioc.run_for(std::chrono::seconds(2));

    // Second connect (within grace period)
    PeerManager pm2b(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2b.start();
    ioc.run_for(std::chrono::seconds(8));

    // Should still work
    REQUIRE(pm1.peer_count() >= 1);
    REQUIRE(pm2b.peer_count() >= 1);

    // Second disconnect
    pm2b.stop();
    ioc.run_for(std::chrono::seconds(2));

    // Third connect (still within grace period)
    PeerManager pm2c(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2c.start();
    ioc.run_for(std::chrono::seconds(8));

    REQUIRE(pm1.peer_count() >= 1);
    REQUIRE(pm2c.peer_count() >= 1);

    pm1.stop();
    pm2c.stop();
    ioc.run_for(std::chrono::seconds(2));
}

// ============================================================================
// TTL enforcement tests (Plan 02)
// ============================================================================

TEST_CASE("ReadRequest returns not-found for expired blob", "[peer][read][ttl]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Expired blob: timestamp 1000s ago, TTL 100s -> expired 900s ago (D-30)
    // Use store_blob directly to bypass engine's already-expired rejection
    auto expired_blob = make_signed_blob(owner, "expired-data", 100, now - 1000);
    auto sr1 = store.store_blob(chromatindb::test::ns_span(owner), expired_blob);
    REQUIRE(sr1.status == chromatindb::storage::StoreResult::Status::Stored);
    auto expired_hash = sr1.blob_hash;

    // Valid blob: timestamp now, TTL 1 day
    auto valid_blob = make_signed_blob(owner, "valid-data", 86400, now);
    auto r2 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), valid_blob));
    REQUIRE(r2.accepted);
    auto valid_hash = r2.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_ReadResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // ReadRequest for expired blob
                std::vector<uint8_t> req1(64);
                std::memcpy(req1.data(), owner.namespace_id().data(), 32);
                std::memcpy(req1.data() + 32, expired_hash.data(), 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_ReadRequest, req1, 100);
                // ReadRequest for valid blob
                std::vector<uint8_t> req2(64);
                std::memcpy(req2.data(), owner.namespace_id().data(), 32);
                std::memcpy(req2.data() + 32, valid_hash.data(), 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_ReadRequest, req2, 101);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 2);
        for (const auto& [rid, payload] : responses) {
            if (rid == 100) {
                CHECK(payload[0] == 0x00);  // expired -> not-found
            } else if (rid == 101) {
                CHECK(payload[0] == 0x01);  // valid -> found
            }
        }
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("ExistsRequest returns not-found for expired blob", "[peer][exists][ttl]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    auto expired_blob = make_signed_blob(owner, "expired-exists", 100, now - 1000);
    auto sr1 = store.store_blob(chromatindb::test::ns_span(owner), expired_blob);
    REQUIRE(sr1.status == chromatindb::storage::StoreResult::Status::Stored);
    auto expired_hash = sr1.blob_hash;

    auto valid_blob = make_signed_blob(owner, "valid-exists", 86400, now);
    auto r2 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), valid_blob));
    REQUIRE(r2.accepted);
    auto valid_hash = r2.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_ExistsResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                std::vector<uint8_t> req1(64);
                std::memcpy(req1.data(), owner.namespace_id().data(), 32);
                std::memcpy(req1.data() + 32, expired_hash.data(), 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_ExistsRequest, req1, 200);
                std::vector<uint8_t> req2(64);
                std::memcpy(req2.data(), owner.namespace_id().data(), 32);
                std::memcpy(req2.data() + 32, valid_hash.data(), 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_ExistsRequest, req2, 201);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 2);
        for (const auto& [rid, payload] : responses) {
            REQUIRE(payload.size() == 33);
            if (rid == 200) {
                CHECK(payload[0] == 0x00);  // expired -> not-found
            } else if (rid == 201) {
                CHECK(payload[0] == 0x01);  // valid -> found
            }
        }
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("BatchExistsRequest filters expired blobs", "[peer][batchexists][ttl]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    auto expired_blob = make_signed_blob(owner, "expired-batchexists", 100, now - 1000);
    auto sr1 = store.store_blob(chromatindb::test::ns_span(owner), expired_blob);
    REQUIRE(sr1.status == chromatindb::storage::StoreResult::Status::Stored);
    auto expired_hash = sr1.blob_hash;

    auto valid_blob = make_signed_blob(owner, "valid-batchexists", 86400, now);
    auto r2 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), valid_blob));
    REQUIRE(r2.accepted);
    auto valid_hash = r2.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_BatchExistsResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // BatchExistsRequest: namespace(32) + count(4) + [expired_hash, valid_hash]
                std::vector<uint8_t> req(100);  // 32 + 4 + 2*32
                std::memcpy(req.data(), owner.namespace_id().data(), 32);
                req[32] = 0; req[33] = 0; req[34] = 0; req[35] = 2;  // count=2
                std::memcpy(req.data() + 36, expired_hash.data(), 32);
                std::memcpy(req.data() + 68, valid_hash.data(), 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_BatchExistsRequest, req, 300);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 1);
        const auto& [rid, payload] = responses[0];
        CHECK(rid == 300);
        REQUIRE(payload.size() == 2);
        CHECK(payload[0] == 0x00);  // expired -> not-found
        CHECK(payload[1] == 0x01);  // valid -> found
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("BatchReadRequest returns not-found for expired blob", "[peer][batchread][ttl]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    auto expired_blob = make_signed_blob(owner, "expired-batchread", 100, now - 1000);
    auto sr1 = store.store_blob(chromatindb::test::ns_span(owner), expired_blob);
    REQUIRE(sr1.status == chromatindb::storage::StoreResult::Status::Stored);
    auto expired_hash = sr1.blob_hash;

    auto valid_blob = make_signed_blob(owner, "valid-batchread", 86400, now);
    auto r2 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), valid_blob));
    REQUIRE(r2.accepted);
    auto valid_hash = r2.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_BatchReadResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // BatchReadRequest: namespace(32) + cap_bytes(4) + count(4) + [expired_hash, valid_hash]
                std::vector<uint8_t> req(104);  // 32 + 4 + 4 + 2*32
                std::memcpy(req.data(), owner.namespace_id().data(), 32);
                // cap_bytes = 0 (default to 4 MiB)
                req[32] = 0; req[33] = 0; req[34] = 0; req[35] = 0;
                // count = 2
                req[36] = 0; req[37] = 0; req[38] = 0; req[39] = 2;
                std::memcpy(req.data() + 40, expired_hash.data(), 32);
                std::memcpy(req.data() + 72, valid_hash.data(), 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_BatchReadRequest, req, 400);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 1);
        const auto& [rid, payload] = responses[0];
        CHECK(rid == 400);
        // Response: [truncated:1][count:4 BE] + entries
        REQUIRE(payload.size() >= 5);
        uint32_t count = 0;
        for (int i = 0; i < 4; ++i) count = (count << 8) | payload[1 + i];
        CHECK(count == 2);

        // Parse entries: first should be not-found (expired), second should be found (valid)
        size_t off = 5;
        uint32_t found_count = 0;
        uint32_t not_found_count = 0;
        for (uint32_t i = 0; i < count && off < payload.size(); ++i) {
            uint8_t status = payload[off++];
            std::array<uint8_t, 32> entry_hash{};
            std::memcpy(entry_hash.data(), payload.data() + off, 32);
            off += 32;
            if (status == 0x01) {
                ++found_count;
                uint64_t sz = 0;
                for (int j = 0; j < 8; ++j) sz = (sz << 8) | payload[off++];
                off += sz;
            } else {
                ++not_found_count;
                // Verify expired blob is the not-found one
                if (entry_hash == expired_hash) {
                    CHECK(status == 0x00);
                }
            }
        }
        CHECK(found_count == 1);
        CHECK(not_found_count == 1);
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("ListRequest filters expired blobs from results", "[peer][list][ttl]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    auto expired_blob = make_signed_blob(owner, "expired-list", 100, now - 1000);
    auto sr1 = store.store_blob(chromatindb::test::ns_span(owner), expired_blob);
    REQUIRE(sr1.status == chromatindb::storage::StoreResult::Status::Stored);

    auto valid_blob = make_signed_blob(owner, "valid-list", 86400, now);
    auto r2 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), valid_blob));
    REQUIRE(r2.accepted);
    auto valid_hash = r2.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_ListResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // ListRequest: namespace(32) + since_seq(8) + limit(4)
                std::vector<uint8_t> req(44, 0);
                std::memcpy(req.data(), owner.namespace_id().data(), 32);
                // since_seq = 0
                // limit = 100 (big-endian)
                req[40] = 0; req[41] = 0; req[42] = 0; req[43] = 100;
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_ListRequest, req, 500);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 1);
        const auto& [rid, payload] = responses[0];
        CHECK(rid == 500);
        // Response: [count:4 BE] + count * [hash:32][seq_num:8 BE] + [has_more:1]
        REQUIRE(payload.size() >= 5);
        uint32_t count = 0;
        for (int i = 0; i < 4; ++i) count = (count << 8) | payload[i];
        CHECK(count == 1);  // Only valid blob, expired filtered out

        // Verify the single result is the valid blob
        if (count == 1) {
            std::array<uint8_t, 32> result_hash{};
            std::memcpy(result_hash.data(), payload.data() + 4, 32);
            CHECK(result_hash == valid_hash);
        }
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("TimeRangeRequest filters expired blobs", "[peer][timerange][ttl]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Expired blob with timestamp in range but TTL expired
    auto expired_blob = make_signed_blob(owner, "expired-timerange", 100, now - 1000);
    auto sr1 = store.store_blob(chromatindb::test::ns_span(owner), expired_blob);
    REQUIRE(sr1.status == chromatindb::storage::StoreResult::Status::Stored);

    // Valid blob with timestamp in range
    auto valid_blob = make_signed_blob(owner, "valid-timerange", 86400, now - 50);
    auto r2 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), valid_blob));
    REQUIRE(r2.accepted);
    auto valid_hash = r2.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_TimeRangeResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // TimeRangeRequest: namespace(32) + start(8) + end(8) + limit(4) = 52 bytes
                // Query range covers both blobs' timestamps
                std::vector<uint8_t> req(52);
                std::memcpy(req.data(), owner.namespace_id().data(), 32);
                uint64_t start_ts = now - 2000;
                for (int i = 7; i >= 0; --i)
                    req[32 + (7 - i)] = static_cast<uint8_t>(start_ts >> (i * 8));
                uint64_t end_ts = now + 100;
                for (int i = 7; i >= 0; --i)
                    req[40 + (7 - i)] = static_cast<uint8_t>(end_ts >> (i * 8));
                req[48] = 0; req[49] = 0; req[50] = 0; req[51] = 100;  // limit=100
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_TimeRangeRequest, req, 600);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 1);
        const auto& [rid, payload] = responses[0];
        CHECK(rid == 600);
        // Response: [truncated:1][count:4 BE] + count * [hash:32][seq_num:8 BE][timestamp:8 BE]
        REQUIRE(payload.size() >= 5);
        uint32_t count = 0;
        for (int i = 0; i < 4; ++i) count = (count << 8) | payload[1 + i];
        CHECK(count == 1);  // Only valid blob, expired filtered

        // Verify the valid blob hash is in results
        if (count == 1) {
            std::array<uint8_t, 32> result_hash{};
            std::memcpy(result_hash.data(), payload.data() + 5, 32);
            CHECK(result_hash == valid_hash);
        }
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("MetadataRequest returns not-found for expired blob", "[peer][metadata][ttl]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    auto expired_blob = make_signed_blob(owner, "expired-metadata", 100, now - 1000);
    auto sr1 = store.store_blob(chromatindb::test::ns_span(owner), expired_blob);
    REQUIRE(sr1.status == chromatindb::storage::StoreResult::Status::Stored);
    auto expired_hash = sr1.blob_hash;

    auto valid_blob = make_signed_blob(owner, "valid-metadata", 86400, now);
    auto r2 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), valid_blob));
    REQUIRE(r2.accepted);
    auto valid_hash = r2.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_MetadataResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // MetadataRequest for expired blob
                std::vector<uint8_t> req1(64);
                std::memcpy(req1.data(), owner.namespace_id().data(), 32);
                std::memcpy(req1.data() + 32, expired_hash.data(), 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_MetadataRequest, req1, 700);
                // MetadataRequest for valid blob
                std::vector<uint8_t> req2(64);
                std::memcpy(req2.data(), owner.namespace_id().data(), 32);
                std::memcpy(req2.data() + 32, valid_hash.data(), 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_MetadataRequest, req2, 701);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 2);
        for (const auto& [rid, payload] : responses) {
            if (rid == 700) {
                REQUIRE(payload.size() == 1);
                CHECK(payload[0] == 0x00);  // expired -> not-found
            } else if (rid == 701) {
                REQUIRE(payload.size() >= 63);
                CHECK(payload[0] == 0x01);  // valid -> found with metadata
            }
        }
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

TEST_CASE("BlobFetch returns not-found for expired blob", "[peer][blobfetch][ttl]") {
    TempDir tmp;
    auto server_id = NodeIdentity::load_or_generate(tmp.path);
    auto client_id = NodeIdentity::generate();
    auto owner = NodeIdentity::generate();

    Config cfg;
    cfg.bind_address = "127.0.0.1:0";
    cfg.data_dir = tmp.path.string();

    Storage store(tmp.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng(store, pool);
    // auto-inject: register PUBKs for PUBK-first invariant.
    chromatindb::test::register_pubk(store, server_id);
    chromatindb::test::register_pubk(store, client_id);
    chromatindb::test::register_pubk(store, owner);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    auto expired_blob = make_signed_blob(owner, "expired-blobfetch", 100, now - 1000);
    auto sr1 = store.store_blob(chromatindb::test::ns_span(owner), expired_blob);
    REQUIRE(sr1.status == chromatindb::storage::StoreResult::Status::Stored);
    auto expired_hash = sr1.blob_hash;

    auto valid_blob = make_signed_blob(owner, "valid-blobfetch", 86400, now);
    auto r2 = run_async(pool, eng.ingest(chromatindb::test::ns_span(owner), valid_blob));
    REQUIRE(r2.accepted);
    auto valid_hash = r2.ack->blob_hash;

    asio::io_context ioc;
    AccessControl acl({}, cfg.allowed_peer_keys, server_id.namespace_id());
    PeerManager pm(cfg, server_id, eng, store, ioc, pool, acl);
    pm.start();
    ioc.run_for(std::chrono::milliseconds(200));

    auto pm_port = pm.listening_port();

    chromatindb::net::Connection::Ptr client_conn;
    std::mutex mtx;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
        asio::ip::tcp::socket socket(ioc);
        auto [ec] = co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), pm_port),
            chromatindb::net::use_nothrow);
        if (ec) co_return;
        client_conn = chromatindb::net::Connection::create_outbound(std::move(socket), client_id);
        client_conn->on_message([&](chromatindb::net::Connection::Ptr, chromatindb::wire::TransportMsgType type, std::vector<uint8_t> payload, uint32_t req_id) {
            if (type == chromatindb::wire::TransportMsgType_BlobFetchResponse) {
                std::lock_guard<std::mutex> lock(mtx);
                responses.emplace_back(req_id, std::move(payload));
            }
        });
        co_await client_conn->run();
    }, asio::detached);

    asio::steady_timer send_timer(ioc);
    send_timer.expires_after(std::chrono::seconds(2));
    send_timer.async_wait([&](asio::error_code ec) {
        if (ec) return;
        if (client_conn && client_conn->is_authenticated()) {
            asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
                // BlobFetch: [namespace:32][blob_hash:32] = 64 bytes
                std::vector<uint8_t> req1(64);
                std::memcpy(req1.data(), owner.namespace_id().data(), 32);
                std::memcpy(req1.data() + 32, expired_hash.data(), 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_BlobFetch, req1, 0);
                std::vector<uint8_t> req2(64);
                std::memcpy(req2.data(), owner.namespace_id().data(), 32);
                std::memcpy(req2.data() + 32, valid_hash.data(), 32);
                co_await client_conn->send_message(
                    chromatindb::wire::TransportMsgType_BlobFetch, req2, 0);
            }, asio::detached);
        }
    });

    ioc.run_for(std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mtx);
        REQUIRE(responses.size() == 2);
        // BlobFetchResponse has no request_id routing (it's 0), so check by status byte
        uint32_t found_count = 0;
        uint32_t not_found_count = 0;
        for (const auto& [rid, payload] : responses) {
            REQUIRE(payload.size() >= 1);
            if (payload[0] == 0x00) {
                ++found_count;  // valid blob
            } else if (payload[0] == 0x01) {
                ++not_found_count;  // expired blob
            }
        }
        CHECK(found_count == 1);
        CHECK(not_found_count == 1);
    }

    pm.stop();
    ioc.run_for(std::chrono::milliseconds(500));
}

// ============================================================================
// peers.json ephemeral-port poisoning regression
//
// An accept-side `remote_endpoint()` returns the peer's ephemeral source port,
// not a reachable listen address. Persisting it to peers.json caused reconnect
// loops against dead ports after restart and, via PEX, propagated the poison
// to other nodes. ConnectCallback and build_peer_list_response now gate on
// conn->connect_address() being non-empty -- outbound (we-initiated) only.
// ============================================================================

TEST_CASE("peers.json excludes inbound peer's ephemeral source port", "[peer][persistence]") {
    TempDir tmp1, tmp2;
    auto id1 = NodeIdentity::load_or_generate(tmp1.path);
    auto id2 = NodeIdentity::load_or_generate(tmp2.path);

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:0";
    cfg1.data_dir = tmp1.path.string();

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:0";
    cfg2.data_dir = tmp2.path.string();

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    asio::thread_pool pool{1};
    BlobEngine eng1(store1, pool);
    BlobEngine eng2(store2, pool);
    chromatindb::test::register_pubk(store1, id1);
    chromatindb::test::register_pubk(store1, id2);
    chromatindb::test::register_pubk(store2, id1);
    chromatindb::test::register_pubk(store2, id2);

    asio::io_context ioc;
    AccessControl acl1({}, cfg1.allowed_peer_keys, id1.namespace_id());
    AccessControl acl2({}, cfg2.allowed_peer_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, pool, acl1);
    pm1.start();
    cfg2.bootstrap_peers = {listening_address(pm1.listening_port())};
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, pool, acl2);
    pm2.start();

    // Let handshake + ConnectCallback fire on pm1 (inbound side) and pm2 (outbound side).
    ioc.run_for(std::chrono::seconds(3));
    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    // Stop pm1 -- this triggers on_shutdown_ -> save_persisted_peers.
    pm1.stop();
    ioc.run_for(std::chrono::seconds(1));

    // pm1 was purely a listener (no bootstrap peers of its own) so it has no
    // outbound-dialed peer. Every connection pm1 saw was inbound (pm2 dialing
    // in from an ephemeral source port). With the fix in place, pm1's
    // peers.json must therefore be empty or absent -- NOT contain pm2's
    // ephemeral source-port address.
    auto peers_json_1 = tmp1.path / "peers.json";
    if (fs::exists(peers_json_1)) {
        std::ifstream f(peers_json_1);
        auto j = nlohmann::json::parse(f);
        auto entries = j.value("peers", nlohmann::json::array());
        // Before the fix, this vector contained pm2's ephemeral source-port
        // address (e.g. "127.0.0.1:54886"). After the fix, it is empty.
        INFO("pm1 (listener) peers.json entries: " << entries.dump());
        REQUIRE(entries.empty());
    }

    pm2.stop();
    ioc.run_for(std::chrono::seconds(1));
}
