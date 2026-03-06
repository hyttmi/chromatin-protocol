#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>
#include <cstring>

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

    auto signing_input = chromatin::wire::build_signing_input(
        blob.namespace_id, blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);

    return blob;
}

} // anonymous namespace

using chromatin::config::Config;
using chromatin::engine::BlobEngine;
using chromatin::identity::NodeIdentity;
using chromatin::peer::PeerManager;
using chromatin::storage::Storage;

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
    PeerManager pm(cfg, id, eng, store, ioc);

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
    PeerManager pm(cfg, id, eng, store, ioc);

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
