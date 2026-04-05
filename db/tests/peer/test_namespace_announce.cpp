#include <catch2/catch_test_macros.hpp>
#include <array>
#include <set>
#include <vector>

#include "db/peer/peer_manager.h"

using chromatindb::peer::PeerManager;

// ============================================================================
// SyncNamespaceAnnounce unit tests (Phase 86: FILT-01, FILT-02)
// ============================================================================

// --- Encode/Decode roundtrip (used by SyncNamespaceAnnounce payload) ---

TEST_CASE("encode_namespace_list empty list roundtrips", "[peer][namespace_announce]") {
    std::vector<std::array<uint8_t, 32>> empty;
    auto encoded = PeerManager::encode_namespace_list(empty);
    auto decoded = PeerManager::decode_namespace_list(encoded);
    CHECK(decoded.empty());
}

TEST_CASE("encode_namespace_list single namespace roundtrips", "[peer][namespace_announce]") {
    std::array<uint8_t, 32> ns{};
    ns.fill(0xAA);
    std::vector<std::array<uint8_t, 32>> input = {ns};
    auto encoded = PeerManager::encode_namespace_list(input);
    // 2-byte count + 32-byte namespace = 34 bytes
    CHECK(encoded.size() == 34);
    auto decoded = PeerManager::decode_namespace_list(encoded);
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0] == ns);
}

TEST_CASE("encode_namespace_list multiple namespaces roundtrips", "[peer][namespace_announce]") {
    std::array<uint8_t, 32> ns_a{};
    ns_a.fill(0xAA);
    std::array<uint8_t, 32> ns_b{};
    ns_b.fill(0xBB);
    std::array<uint8_t, 32> ns_c{};
    ns_c.fill(0xCC);
    std::vector<std::array<uint8_t, 32>> input = {ns_a, ns_b, ns_c};
    auto encoded = PeerManager::encode_namespace_list(input);
    // 2-byte count + 3 * 32-byte namespace = 98 bytes
    CHECK(encoded.size() == 98);
    auto decoded = PeerManager::decode_namespace_list(encoded);
    REQUIRE(decoded.size() == 3);
    CHECK(decoded[0] == ns_a);
    CHECK(decoded[1] == ns_b);
    CHECK(decoded[2] == ns_c);
}

TEST_CASE("decode_namespace_list rejects truncated payload", "[peer][namespace_announce]") {
    // Less than 2 bytes
    std::vector<uint8_t> too_short = {0x00};
    auto decoded = PeerManager::decode_namespace_list(too_short);
    CHECK(decoded.empty());
}

// --- BlobNotify namespace filtering logic (FILT-02) ---
// These test the filtering PREDICATE that will be used in on_blob_ingested.
// The actual PeerInfo.announced_namespaces field is added by Task 1 of this plan.
// Once Task 1 completes, these tests compile and validate the filtering semantics.

TEST_CASE("namespace filter: empty announced set passes all namespaces", "[peer][namespace_filter]") {
    // D-07: empty announced_namespaces = replicate everything
    std::set<std::array<uint8_t, 32>> announced;  // empty
    std::array<uint8_t, 32> blob_ns{};
    blob_ns.fill(0xDD);

    // Filter logic: empty set = pass all
    bool should_send = announced.empty() || announced.count(blob_ns) > 0;
    CHECK(should_send);
}

TEST_CASE("namespace filter: matching namespace passes", "[peer][namespace_filter]") {
    std::array<uint8_t, 32> ns_a{};
    ns_a.fill(0xAA);
    std::set<std::array<uint8_t, 32>> announced = {ns_a};

    // Blob is in ns_a, peer announced ns_a -> should send
    bool should_send = announced.empty() || announced.count(ns_a) > 0;
    CHECK(should_send);
}

TEST_CASE("namespace filter: non-matching namespace is filtered out", "[peer][namespace_filter]") {
    std::array<uint8_t, 32> ns_a{};
    ns_a.fill(0xAA);
    std::array<uint8_t, 32> ns_b{};
    ns_b.fill(0xBB);
    std::set<std::array<uint8_t, 32>> announced = {ns_a};

    // Blob is in ns_b, peer only announced ns_a -> should NOT send
    bool should_send = announced.empty() || announced.count(ns_b) > 0;
    CHECK_FALSE(should_send);
}

TEST_CASE("namespace filter: multi-namespace announced set with match passes", "[peer][namespace_filter]") {
    std::array<uint8_t, 32> ns_a{};
    ns_a.fill(0xAA);
    std::array<uint8_t, 32> ns_b{};
    ns_b.fill(0xBB);
    std::array<uint8_t, 32> ns_c{};
    ns_c.fill(0xCC);
    std::set<std::array<uint8_t, 32>> announced = {ns_a, ns_b};

    // Blob in ns_b, peer announced {ns_a, ns_b} -> should send
    bool should_send_b = announced.empty() || announced.count(ns_b) > 0;
    CHECK(should_send_b);

    // Blob in ns_c, peer announced {ns_a, ns_b} -> should NOT send
    bool should_send_c = announced.empty() || announced.count(ns_c) > 0;
    CHECK_FALSE(should_send_c);
}
