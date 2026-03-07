#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <cstring>
#include <ctime>

#include "db/acl/access_control.h"
#include "db/peer/peer_manager.h"
#include "db/config/config.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
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
               ("chromatindb_test_peer_" + std::to_string(dist(gen)));
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// Convert a 32-byte namespace hash to 64-char hex string (for allowed_keys config).
std::string ns_to_hex(std::span<const uint8_t, 32> ns) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto b : ns) {
        result += hex_chars[(b >> 4) & 0xF];
        result += hex_chars[b & 0xF];
    }
    return result;
}

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

    auto signing_input = chromatindb::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
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
// PeerManager unit tests
// ============================================================================

TEST_CASE("PeerManager starts with unreachable bootstrap", "[peer]") {
    TempDir tmp;

    Config cfg;
    cfg.bind_address = "127.0.0.1:14210";
    cfg.data_dir = tmp.path.string();
    cfg.bootstrap_peers = {"192.0.2.1:4200"};  // TEST-NET, unreachable

    auto id = NodeIdentity::load_or_generate(tmp.path);
    Storage store(tmp.path.string());
    BlobEngine eng(store);

    asio::io_context ioc;
    AccessControl acl(cfg.allowed_keys, id.namespace_id());
    PeerManager pm(cfg, id, eng, store, ioc, acl);

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
    cfg.bind_address = "127.0.0.1:14211";
    cfg.data_dir = tmp.path.string();
    cfg.max_peers = 1;  // Very low limit for testing

    auto id = NodeIdentity::load_or_generate(tmp.path);
    Storage store(tmp.path.string());
    BlobEngine eng(store);

    asio::io_context ioc;
    AccessControl acl(cfg.allowed_keys, id.namespace_id());
    PeerManager pm(cfg, id, eng, store, ioc, acl);

    pm.start();

    // After starting, peer count should be 0
    REQUIRE(pm.peer_count() == 0);

    pm.stop();
    ioc.run_for(std::chrono::seconds(1));
}

TEST_CASE("PeerManager strike threshold", "[peer]") {
    // Verify the constant
    REQUIRE(PeerManager::STRIKE_THRESHOLD == 10);
    REQUIRE(PeerManager::STRIKE_COOLDOWN_SEC == 300);
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
    REQUIRE(PeerManager::PEX_INTERVAL_SEC == 300);
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
    cfg1.bind_address = "127.0.0.1:14250";
    cfg1.data_dir = tmp1.path.string();
    cfg1.sync_interval_seconds = 1;
    cfg1.max_peers = 32;
    cfg1.allowed_keys = {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};

    // Node2 is open mode, bootstraps to node1
    Config cfg2;
    cfg2.bind_address = "127.0.0.1:14251";
    cfg2.data_dir = tmp2.path.string();
    cfg2.bootstrap_peers = {"127.0.0.1:14250"};
    cfg2.sync_interval_seconds = 1;
    cfg2.max_peers = 32;

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    BlobEngine eng1(store1);
    BlobEngine eng2(store2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob in node1 -- should NOT reach node2 because node2 is unauthorized
    auto blob1 = make_signed_blob(id1, "closed-secret", 604800, now);
    auto r1 = eng1.ingest(blob1);
    REQUIRE(r1.accepted);

    asio::io_context ioc;
    AccessControl acl1(cfg1.allowed_keys, id1.namespace_id());
    AccessControl acl2(cfg2.allowed_keys, id2.namespace_id());

    REQUIRE(acl1.is_closed_mode());
    REQUIRE_FALSE(acl2.is_closed_mode());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, acl1);
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, acl2);

    pm1.start();
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
    auto id2_ns_hex = ns_to_hex(id2.namespace_id());

    Config cfg1;
    cfg1.bind_address = "127.0.0.1:14252";
    cfg1.data_dir = tmp1.path.string();
    cfg1.sync_interval_seconds = 1;
    cfg1.max_peers = 32;
    cfg1.allowed_keys = {id2_ns_hex};

    // Node2 is also in closed mode and allows id1's namespace
    auto id1_ns_hex = ns_to_hex(id1.namespace_id());

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:14253";
    cfg2.data_dir = tmp2.path.string();
    cfg2.bootstrap_peers = {"127.0.0.1:14252"};
    cfg2.sync_interval_seconds = 1;
    cfg2.max_peers = 32;
    cfg2.allowed_keys = {id1_ns_hex};

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    BlobEngine eng1(store1);
    BlobEngine eng2(store2);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob in node1
    auto blob1 = make_signed_blob(id1, "closed-authorized", 604800, now);
    auto r1 = eng1.ingest(blob1);
    REQUIRE(r1.accepted);

    asio::io_context ioc;
    AccessControl acl1(cfg1.allowed_keys, id1.namespace_id());
    AccessControl acl2(cfg2.allowed_keys, id2.namespace_id());

    REQUIRE(acl1.is_closed_mode());
    REQUIRE(acl2.is_closed_mode());
    REQUIRE(acl1.is_allowed(id2.namespace_id()));
    REQUIRE(acl2.is_allowed(id1.namespace_id()));

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, acl1);
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, acl2);

    pm1.start();
    pm2.start();

    // Run long enough for handshake + ACL check + sync
    ioc.run_for(std::chrono::seconds(8));

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

    auto id1_ns_hex = ns_to_hex(id1.namespace_id());
    auto id2_ns_hex = ns_to_hex(id2.namespace_id());

    // Write config file for node1 with id2 allowed
    auto config_path = tmp1.path / "config.json";
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:14260", "allowed_keys": [")" << id2_ns_hex << R"("]})";
    }

    auto cfg1 = chromatindb::config::load_config(config_path);
    cfg1.data_dir = tmp1.path.string();
    cfg1.sync_interval_seconds = 1;
    cfg1.max_peers = 32;

    Config cfg2;
    cfg2.bind_address = "127.0.0.1:14261";
    cfg2.data_dir = tmp2.path.string();
    cfg2.bootstrap_peers = {"127.0.0.1:14260"};
    cfg2.sync_interval_seconds = 1;
    cfg2.max_peers = 32;
    cfg2.allowed_keys = {id1_ns_hex};

    Storage store1(tmp1.path.string());
    Storage store2(tmp2.path.string());
    BlobEngine eng1(store1);
    BlobEngine eng2(store2);

    asio::io_context ioc;
    AccessControl acl1(cfg1.allowed_keys, id1.namespace_id());
    AccessControl acl2(cfg2.allowed_keys, id2.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, acl1, config_path);
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, acl2);

    pm1.start();
    pm2.start();

    // Let nodes connect and sync
    ioc.run_for(std::chrono::seconds(5));

    // Verify they connected
    REQUIRE(pm1.peer_count() == 1);
    REQUIRE(pm2.peer_count() == 1);

    // Now rewrite config to REMOVE id2 from allowed_keys (revocation)
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:14260", "allowed_keys": ["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"]})";
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
        f << R"({"bind_address": "127.0.0.1:14262", "allowed_keys": ["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"]})";
    }

    auto cfg1 = chromatindb::config::load_config(config_path);
    cfg1.data_dir = tmp1.path.string();

    Storage store1(tmp1.path.string());
    BlobEngine eng1(store1);

    asio::io_context ioc;
    AccessControl acl1(cfg1.allowed_keys, id1.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, acl1, config_path);
    pm1.start();

    // Drain start-up handlers (including SIGHUP coroutine setup)
    ioc.run_for(std::chrono::milliseconds(100));

    REQUIRE(acl1.is_closed_mode());
    REQUIRE(acl1.allowed_count() == 1);

    // Corrupt the config file
    {
        std::ofstream f(config_path);
        f << "{ this is not valid json }}}";
    }

    // Trigger reload -- should NOT crash, should keep current config
    pm1.reload_config();

    // ACL should still be in closed mode with 1 key (fail-safe)
    REQUIRE(acl1.is_closed_mode());
    REQUIRE(acl1.allowed_count() == 1);

    pm1.stop();
    ioc.run_for(std::chrono::seconds(2));
}

TEST_CASE("reload_config switches from open to closed mode", "[peer][acl][reload]") {
    TempDir tmp1;

    auto id1 = NodeIdentity::load_or_generate(tmp1.path);

    // Write config file with NO allowed_keys (open mode)
    auto config_path = tmp1.path / "config.json";
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:14263"})";
    }

    auto cfg1 = chromatindb::config::load_config(config_path);
    cfg1.data_dir = tmp1.path.string();

    Storage store1(tmp1.path.string());
    BlobEngine eng1(store1);

    asio::io_context ioc;
    AccessControl acl1(cfg1.allowed_keys, id1.namespace_id());

    PeerManager pm1(cfg1, id1, eng1, store1, ioc, acl1, config_path);
    pm1.start();

    REQUIRE_FALSE(acl1.is_closed_mode());

    // Rewrite config to add allowed_keys (switch to closed mode)
    {
        std::ofstream f(config_path);
        f << R"({"bind_address": "127.0.0.1:14263", "allowed_keys": ["bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"]})";
    }

    pm1.reload_config();

    REQUIRE(acl1.is_closed_mode());
    REQUIRE(acl1.allowed_count() == 1);

    pm1.stop();
    ioc.run_for(std::chrono::seconds(1));
}

TEST_CASE("closed mode disables PEX discovery", "[peer][acl][pex]") {
    TempDir tmp1, tmp2, tmp3;

    auto id_a = NodeIdentity::load_or_generate(tmp1.path);
    auto id_b = NodeIdentity::load_or_generate(tmp2.path);
    auto id_c = NodeIdentity::load_or_generate(tmp3.path);

    auto ns_a = ns_to_hex(id_a.namespace_id());
    auto ns_b = ns_to_hex(id_b.namespace_id());
    auto ns_c = ns_to_hex(id_c.namespace_id());

    // All three nodes in closed mode, each allowing the other two
    Config cfg_a;
    cfg_a.bind_address = "127.0.0.1:14254";
    cfg_a.data_dir = tmp1.path.string();
    cfg_a.sync_interval_seconds = 1;
    cfg_a.max_peers = 32;
    cfg_a.allowed_keys = {ns_b, ns_c};

    Config cfg_b;
    cfg_b.bind_address = "127.0.0.1:14255";
    cfg_b.data_dir = tmp2.path.string();
    cfg_b.bootstrap_peers = {"127.0.0.1:14254"};
    cfg_b.sync_interval_seconds = 1;
    cfg_b.max_peers = 32;
    cfg_b.allowed_keys = {ns_a, ns_c};

    // Node C only knows B (not A). In open mode it would discover A via PEX.
    // In closed mode, PEX is disabled, so C should NOT discover A.
    Config cfg_c;
    cfg_c.bind_address = "127.0.0.1:14256";
    cfg_c.data_dir = tmp3.path.string();
    cfg_c.bootstrap_peers = {"127.0.0.1:14255"};
    cfg_c.sync_interval_seconds = 1;
    cfg_c.max_peers = 32;
    cfg_c.allowed_keys = {ns_a, ns_b};

    Storage store_a(tmp1.path.string());
    Storage store_b(tmp2.path.string());
    Storage store_c(tmp3.path.string());
    BlobEngine eng_a(store_a);
    BlobEngine eng_b(store_b);
    BlobEngine eng_c(store_c);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    // Store a blob in A -- C should NOT get it via PEX discovery
    auto blob_a = make_signed_blob(id_a, "closed-pex-test", 604800, now);
    auto r_a = eng_a.ingest(blob_a);
    REQUIRE(r_a.accepted);

    asio::io_context ioc;
    AccessControl acl_a(cfg_a.allowed_keys, id_a.namespace_id());
    AccessControl acl_b(cfg_b.allowed_keys, id_b.namespace_id());
    AccessControl acl_c(cfg_c.allowed_keys, id_c.namespace_id());

    PeerManager pm_a(cfg_a, id_a, eng_a, store_a, ioc, acl_a);
    PeerManager pm_b(cfg_b, id_b, eng_b, store_b, ioc, acl_b);
    PeerManager pm_c(cfg_c, id_c, eng_c, store_c, ioc, acl_c);

    pm_a.start();
    pm_b.start();

    // Let A and B connect
    ioc.run_for(std::chrono::seconds(5));

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

    SECTION("BLOB_TRANSFER_TIMEOUT is 120 seconds") {
        REQUIRE(PM::BLOB_TRANSFER_TIMEOUT == std::chrono::seconds(120));
    }
}
