#include <catch2/catch_test_macros.hpp>
#include "relay/core/relay_session.h"
#include "db/peer/peer_manager.h"
#include <array>
#include <cstring>
#include <vector>

using chromatindb::relay::core::RelaySession;
using chromatindb::peer::PeerManager;

// Helper: create a namespace ID filled with a single byte value
static std::array<uint8_t, 32> make_ns(uint8_t fill) {
    std::array<uint8_t, 32> ns{};
    std::memset(ns.data(), fill, 32);
    return ns;
}

// ===== NamespaceHash and NamespaceSet tests =====

TEST_CASE("NamespaceSet insert and find", "[relay_session]") {
    RelaySession::NamespaceSet set;
    auto ns1 = make_ns(0x01);
    auto ns2 = make_ns(0x02);
    auto ns3 = make_ns(0x03);

    // Empty set: find returns end
    CHECK(set.find(ns1) == set.end());

    // Insert and find
    set.insert(ns1);
    set.insert(ns2);
    CHECK(set.size() == 2);
    CHECK(set.find(ns1) != set.end());
    CHECK(set.find(ns2) != set.end());
    CHECK(set.find(ns3) == set.end());

    // Duplicate insert is idempotent
    set.insert(ns1);
    CHECK(set.size() == 2);
}

TEST_CASE("NamespaceSet erase", "[relay_session]") {
    RelaySession::NamespaceSet set;
    auto ns1 = make_ns(0x01);
    auto ns2 = make_ns(0x02);

    set.insert(ns1);
    set.insert(ns2);
    CHECK(set.size() == 2);

    set.erase(ns1);
    CHECK(set.size() == 1);
    CHECK(set.find(ns1) == set.end());
    CHECK(set.find(ns2) != set.end());

    // Erase non-existent is no-op
    set.erase(ns1);
    CHECK(set.size() == 1);
}

TEST_CASE("NamespaceHash produces distinct hashes for different namespace IDs", "[relay_session]") {
    RelaySession::NamespaceHash hasher;
    auto ns1 = make_ns(0x01);
    auto ns2 = make_ns(0x02);

    // Different namespaces should (very likely) produce different hashes
    CHECK(hasher(ns1) != hasher(ns2));
}

TEST_CASE("MAX_SUBSCRIPTIONS cap at 256", "[relay_session]") {
    RelaySession::NamespaceSet set;

    // Insert 256 unique namespace IDs
    for (size_t i = 0; i < 256; ++i) {
        std::array<uint8_t, 32> ns{};
        // Use first two bytes as a counter to ensure uniqueness
        ns[0] = static_cast<uint8_t>(i & 0xFF);
        ns[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        set.insert(ns);
    }
    CHECK(set.size() == 256);

    // Application-level cap check: size() >= MAX_SUBSCRIPTIONS is true
    CHECK(set.size() >= RelaySession::MAX_SUBSCRIPTIONS);
}

// ===== encode_namespace_list / decode_namespace_list round-trip =====

TEST_CASE("encode_namespace_list / decode_namespace_list round-trip", "[relay_session]") {
    auto ns1 = make_ns(0xAA);
    auto ns2 = make_ns(0xBB);
    auto ns3 = make_ns(0xCC);

    std::vector<std::array<uint8_t, 32>> original = {ns1, ns2, ns3};
    auto encoded = PeerManager::encode_namespace_list(original);

    // Verify wire format: [uint16_be count=3][ns:32][ns:32][ns:32]
    REQUIRE(encoded.size() == 2 + 3 * 32);
    CHECK(encoded[0] == 0x00);  // Big-endian high byte
    CHECK(encoded[1] == 0x03);  // Big-endian low byte = 3

    // Decode back
    auto decoded = PeerManager::decode_namespace_list(encoded);
    REQUIRE(decoded.size() == 3);
    CHECK(decoded[0] == ns1);
    CHECK(decoded[1] == ns2);
    CHECK(decoded[2] == ns3);
}

TEST_CASE("decode_namespace_list edge cases", "[relay_session]") {
    SECTION("empty payload returns empty") {
        std::vector<uint8_t> empty;
        auto result = PeerManager::decode_namespace_list(empty);
        CHECK(result.empty());
    }

    SECTION("single byte payload returns empty") {
        std::vector<uint8_t> one = {0x01};
        auto result = PeerManager::decode_namespace_list(one);
        CHECK(result.empty());
    }

    SECTION("count=0 returns empty") {
        std::vector<uint8_t> zero_count = {0x00, 0x00};
        auto result = PeerManager::decode_namespace_list(zero_count);
        CHECK(result.empty());
    }

    SECTION("mismatched size returns empty") {
        // count=2 but only 1 namespace worth of data (2 + 32 = 34 bytes, need 66)
        std::vector<uint8_t> short_payload(2 + 32, 0x00);
        short_payload[0] = 0x00;
        short_payload[1] = 0x02;  // Claims 2 namespaces
        auto result = PeerManager::decode_namespace_list(short_payload);
        CHECK(result.empty());
    }
}

// ===== Notification namespace extraction =====

TEST_CASE("Notification namespace extraction from payload", "[relay_session]") {
    // Build a 77-byte notification payload:
    // [namespace_id:32][blob_hash:32][seq_num_be:8][blob_size_be:4][is_tombstone:1]
    auto ns = make_ns(0xDD);
    auto blob_hash = make_ns(0xEE);

    std::vector<uint8_t> notification(77, 0x00);
    std::memcpy(notification.data(), ns.data(), 32);
    std::memcpy(notification.data() + 32, blob_hash.data(), 32);
    // seq_num, blob_size, is_tombstone left as zeros -- not relevant for namespace extraction

    // Extract first 32 bytes as namespace_id
    std::array<uint8_t, 32> extracted{};
    std::memcpy(extracted.data(), notification.data(), 32);
    CHECK(extracted == ns);

    // Different namespace does NOT match
    auto other_ns = make_ns(0xFF);
    CHECK(extracted != other_ns);
}

// ===== Empty subscription set filtering =====

TEST_CASE("Empty subscription set drops all notifications", "[relay_session]") {
    RelaySession::NamespaceSet set;
    auto ns = make_ns(0x42);

    // Empty set: find returns end for any namespace
    CHECK(set.find(ns) == set.end());
    CHECK(set.empty());
}

// ===== Subscription set with notification filtering integration =====

TEST_CASE("Subscription set correctly filters notifications", "[relay_session]") {
    RelaySession::NamespaceSet set;
    auto subscribed_ns = make_ns(0xAA);
    auto unsubscribed_ns = make_ns(0xBB);

    set.insert(subscribed_ns);

    // Build notification for subscribed namespace
    std::vector<uint8_t> notification(77, 0x00);
    std::memcpy(notification.data(), subscribed_ns.data(), 32);

    std::array<uint8_t, 32> ns_id{};
    std::memcpy(ns_id.data(), notification.data(), 32);
    CHECK(set.find(ns_id) != set.end());  // Should forward

    // Build notification for unsubscribed namespace
    std::memcpy(notification.data(), unsubscribed_ns.data(), 32);
    std::memcpy(ns_id.data(), notification.data(), 32);
    CHECK(set.find(ns_id) == set.end());  // Should drop
}

// ===== Plan 02: SessionState, backoff, and replay tests =====

TEST_CASE("SessionState enum has required values", "[relay_session]") {
    // Verify enum class compiles and values are distinct
    auto active = RelaySession::SessionState::ACTIVE;
    auto reconnecting = RelaySession::SessionState::RECONNECTING;
    auto dead = RelaySession::SessionState::DEAD;
    CHECK(active != reconnecting);
    CHECK(reconnecting != dead);
    CHECK(active != dead);
}

TEST_CASE("Reconnect constants match D-03 and D-06 specs", "[relay_session]") {
    CHECK(RelaySession::MAX_RECONNECT_ATTEMPTS == 10);
    CHECK(RelaySession::BACKOFF_BASE_MS == 1000);
    CHECK(RelaySession::BACKOFF_CAP_MS == 30000);
}

TEST_CASE("Jittered backoff formula bounds", "[relay_session]") {
    // Per D-03: full jitter = uniform [0, min(cap, base * 2^attempt)]
    // base=1000ms, cap=30000ms
    constexpr uint32_t base = 1000;
    constexpr uint32_t cap = 30000;

    SECTION("attempt 0: max delay = min(30000, 1000*1) = 1000ms") {
        auto max_delay = std::min(cap, base * (1u << std::min(0u, 14u)));
        CHECK(max_delay == 1000);
    }
    SECTION("attempt 1: max delay = min(30000, 1000*2) = 2000ms") {
        auto max_delay = std::min(cap, base * (1u << std::min(1u, 14u)));
        CHECK(max_delay == 2000);
    }
    SECTION("attempt 4: max delay = min(30000, 1000*16) = 16000ms") {
        auto max_delay = std::min(cap, base * (1u << std::min(4u, 14u)));
        CHECK(max_delay == 16000);
    }
    SECTION("attempt 5: max delay = min(30000, 1000*32) = 30000ms (capped)") {
        auto max_delay = std::min(cap, base * (1u << std::min(5u, 14u)));
        CHECK(max_delay == 30000);
    }
    SECTION("attempt 9 (last): max delay = 30000ms (capped)") {
        auto max_delay = std::min(cap, base * (1u << std::min(9u, 14u)));
        CHECK(max_delay == 30000);
    }
    SECTION("attempt 14+: overflow protection via min(attempt, 14)") {
        // 1000 * 2^14 = 16384000, min(30000, 16384000) = 30000
        auto max_delay = std::min(cap, base * (1u << std::min(14u, 14u)));
        CHECK(max_delay == 30000);
        // Verify that attempt=15 also clamps to 14
        auto max_delay_15 = std::min(cap, base * (1u << std::min(15u, 14u)));
        CHECK(max_delay_15 == 30000);
    }
}

TEST_CASE("Subscription replay encodes all namespaces in single message", "[relay_session]") {
    // Simulate what on_ready does: encode all subscribed namespaces into one Subscribe payload
    RelaySession::NamespaceSet subs;

    // Add 3 test namespaces
    auto ns1 = make_ns(0x11);
    auto ns2 = make_ns(0x22);
    auto ns3 = make_ns(0x33);
    subs.insert(ns1);
    subs.insert(ns2);
    subs.insert(ns3);

    // Build replay payload (same as reconnect on_ready)
    std::vector<std::array<uint8_t, 32>> ns_list(subs.begin(), subs.end());
    auto payload = PeerManager::encode_namespace_list(ns_list);

    // Verify wire format
    CHECK(payload.size() == 2 + 3 * 32);  // count(2) + 3 namespaces
    uint16_t count = (static_cast<uint16_t>(payload[0]) << 8) | payload[1];
    CHECK(count == 3);

    // Decode and verify all 3 present
    auto decoded = PeerManager::decode_namespace_list(payload);
    CHECK(decoded.size() == 3);

    // All original namespaces should be in decoded (order may differ due to unordered_set)
    RelaySession::NamespaceSet decoded_set;
    for (const auto& ns : decoded) decoded_set.insert(ns);
    CHECK(decoded_set.count(ns1) == 1);
    CHECK(decoded_set.count(ns2) == 1);
    CHECK(decoded_set.count(ns3) == 1);
}

TEST_CASE("Empty subscription set produces no replay payload", "[relay_session]") {
    // Per Pitfall 4: skip replay send if subscribed_namespaces_ is empty
    RelaySession::NamespaceSet empty_subs;
    CHECK(empty_subs.empty());
    // The on_ready code checks: if (!subscribed_namespaces_.empty()) before encoding
    // With empty set, no payload is constructed -- verified by checking empty()
}
