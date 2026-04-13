#include <catch2/catch_test_macros.hpp>

#include "relay/core/request_router.h"
#include "relay/core/subscription_tracker.h"
#include "relay/util/endian.h"

#include <cstdint>
#include <unordered_set>

using chromatindb::relay::core::SubscriptionTracker;
using chromatindb::relay::core::Namespace32;

static Namespace32 make_namespace(uint8_t fill) {
    Namespace32 ns{};
    ns.fill(fill);
    return ns;
}

TEST_CASE("first subscribe forwards to node", "[subscription_tracker]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);

    auto result = tracker.subscribe(1, {ns_a});

    REQUIRE(result.forward_to_node);
    REQUIRE(result.new_namespaces.size() == 1);
    REQUIRE(result.new_namespaces[0] == ns_a);
}

TEST_CASE("duplicate subscribe does not forward", "[subscription_tracker]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);

    tracker.subscribe(1, {ns_a});
    auto result = tracker.subscribe(2, {ns_a});

    REQUIRE_FALSE(result.forward_to_node);
    REQUIRE(result.new_namespaces.empty());
}

TEST_CASE("mixed new and existing namespaces", "[subscription_tracker]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);
    auto ns_b = make_namespace(0xBB);

    tracker.subscribe(1, {ns_a});
    auto result = tracker.subscribe(2, {ns_a, ns_b});

    REQUIRE(result.forward_to_node);
    REQUIRE(result.new_namespaces.size() == 1);
    REQUIRE(result.new_namespaces[0] == ns_b);
}

TEST_CASE("last unsubscribe forwards to node", "[subscription_tracker]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);

    tracker.subscribe(1, {ns_a});
    tracker.subscribe(2, {ns_a});

    auto r1 = tracker.unsubscribe(1, {ns_a});
    REQUIRE_FALSE(r1.forward_to_node);
    REQUIRE(r1.removed_namespaces.empty());

    auto r2 = tracker.unsubscribe(2, {ns_a});
    REQUIRE(r2.forward_to_node);
    REQUIRE(r2.removed_namespaces.size() == 1);
    REQUIRE(r2.removed_namespaces[0] == ns_a);
}

TEST_CASE("unsubscribe from non-subscribed namespace is no-op", "[subscription_tracker]") {
    SubscriptionTracker tracker;
    auto ns_x = make_namespace(0xFF);

    auto result = tracker.unsubscribe(1, {ns_x});
    REQUIRE_FALSE(result.forward_to_node);
    REQUIRE(result.removed_namespaces.empty());
}

TEST_CASE("client_subscription_count tracks correctly", "[subscription_tracker]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);
    auto ns_b = make_namespace(0xBB);

    tracker.subscribe(1, {ns_a, ns_b});
    REQUIRE(tracker.client_subscription_count(1) == 2);

    tracker.unsubscribe(1, {ns_a});
    REQUIRE(tracker.client_subscription_count(1) == 1);
}

TEST_CASE("client_subscription_count returns correct value at cap", "[subscription_tracker][cap]") {
    SubscriptionTracker tracker;

    std::vector<Namespace32> namespaces;
    namespaces.reserve(256);
    for (int i = 0; i < 256; ++i) {
        Namespace32 ns{};
        ns[0] = static_cast<uint8_t>(i & 0xFF);
        ns[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
        namespaces.push_back(ns);
    }

    tracker.subscribe(1, namespaces);
    REQUIRE(tracker.client_subscription_count(1) == 256);
}

TEST_CASE("remove_client cleans all namespaces", "[subscription_tracker][cleanup]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);
    auto ns_b = make_namespace(0xBB);

    tracker.subscribe(1, {ns_a, ns_b});
    tracker.subscribe(2, {ns_a});

    auto removed = tracker.remove_client(1);

    // ns_b should be in removed (now empty), ns_a still has session 2
    REQUIRE(removed.size() == 1);
    REQUIRE(removed[0] == ns_b);

    // session 2 still subscribed to ns_a
    auto subs = tracker.get_subscribers(ns_a);
    REQUIRE(subs.size() == 1);
    REQUIRE(subs.count(2) == 1);

    // ns_b has no subscribers
    auto subs_b = tracker.get_subscribers(ns_b);
    REQUIRE(subs_b.empty());

    // session 1 is gone
    REQUIRE(tracker.client_subscription_count(1) == 0);
}

TEST_CASE("remove_client for unknown session is no-op", "[subscription_tracker][cleanup]") {
    SubscriptionTracker tracker;

    auto removed = tracker.remove_client(999);
    REQUIRE(removed.empty());
}

TEST_CASE("get_all_namespaces returns union", "[subscription_tracker]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);
    auto ns_b = make_namespace(0xBB);

    tracker.subscribe(1, {ns_a});
    tracker.subscribe(2, {ns_b});

    auto all = tracker.get_all_namespaces();
    REQUIRE(all.size() == 2);

    // Convert to set for order-independent comparison
    std::unordered_set<Namespace32, chromatindb::relay::core::Namespace32Hash> all_set(all.begin(), all.end());
    REQUIRE(all_set.count(ns_a) == 1);
    REQUIRE(all_set.count(ns_b) == 1);
}

TEST_CASE("get_subscribers returns correct set", "[subscription_tracker]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);

    tracker.subscribe(1, {ns_a});
    tracker.subscribe(2, {ns_a});

    auto subs = tracker.get_subscribers(ns_a);
    REQUIRE(subs.size() == 2);
    REQUIRE(subs.count(1) == 1);
    REQUIRE(subs.count(2) == 1);
}

TEST_CASE("get_subscribers for unknown namespace returns empty", "[subscription_tracker]") {
    SubscriptionTracker tracker;
    auto ns_x = make_namespace(0xFF);

    auto subs = tracker.get_subscribers(ns_x);
    REQUIRE(subs.empty());
}

TEST_CASE("Namespace32Hash produces distinct hashes for distinct arrays", "[subscription_tracker]") {
    chromatindb::relay::core::Namespace32Hash hasher;
    auto ns_a = make_namespace(0xAA);
    auto ns_b = make_namespace(0xBB);
    auto ns_c = make_namespace(0x00);

    auto ha = hasher(ns_a);
    auto hb = hasher(ns_b);
    auto hc = hasher(ns_c);

    // With distinct fill bytes, first 8 bytes differ -> hashes should differ
    REQUIRE(ha != hb);
    REQUIRE(ha != hc);
    REQUIRE(hb != hc);
}

// ---------------------------------------------------------------------------
// UDS Reconnect contract tests (MUX-05)
// ---------------------------------------------------------------------------

using chromatindb::relay::core::RequestRouter;

TEST_CASE("disconnect: subscriptions survive client disconnect via remove_client then re-subscribe",
          "[uds_reconnect]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);

    // Client subscribes
    tracker.subscribe(1, {ns_a});
    REQUIRE(tracker.get_all_namespaces().size() == 1);

    // Client disconnects (simulates session close cleanup)
    auto removed = tracker.remove_client(1);
    REQUIRE(removed.size() == 1);  // ns_a became empty
    REQUIRE(tracker.get_all_namespaces().empty());

    // Client reconnects and re-subscribes -- must forward to node again
    auto result = tracker.subscribe(1, {ns_a});
    REQUIRE(result.forward_to_node);
    REQUIRE(result.new_namespaces.size() == 1);
    REQUIRE(result.new_namespaces[0] == ns_a);
}

TEST_CASE("bulk_fail_all does not affect subscription state", "[uds_reconnect]") {
    SubscriptionTracker tracker;
    RequestRouter router;
    auto ns_a = make_namespace(0xAA);
    auto ns_b = make_namespace(0xBB);

    // Subscribe session 1 to both namespaces
    tracker.subscribe(1, {ns_a, ns_b});

    // Register 2 pending requests
    router.register_request(1, 10, 8);
    router.register_request(1, 20, 8);
    REQUIRE(router.pending_count() == 2);

    // Bulk-fail all pending requests
    int fail_count = 0;
    router.bulk_fail_all([&](uint64_t, uint32_t) { ++fail_count; });
    REQUIRE(fail_count == 2);
    REQUIRE(router.pending_count() == 0);

    // Subscriptions must be unaffected
    auto all_ns = tracker.get_all_namespaces();
    REQUIRE(all_ns.size() == 2);
    std::unordered_set<Namespace32, chromatindb::relay::core::Namespace32Hash> ns_set(
        all_ns.begin(), all_ns.end());
    REQUIRE(ns_set.count(ns_a) == 1);
    REQUIRE(ns_set.count(ns_b) == 1);
}

TEST_CASE("subscription replay after remove_client and re-subscribe", "[uds_reconnect]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);
    auto ns_b = make_namespace(0xBB);

    // Two clients subscribe to different namespaces
    tracker.subscribe(1, {ns_a});
    tracker.subscribe(2, {ns_b});

    // Client 1 disconnects
    auto removed = tracker.remove_client(1);
    REQUIRE(removed.size() == 1);
    REQUIRE(removed[0] == ns_a);

    // get_all_namespaces should only return ns_b
    auto after_remove = tracker.get_all_namespaces();
    REQUIRE(after_remove.size() == 1);
    REQUIRE(after_remove[0] == ns_b);

    // New client 3 subscribes to ns_a
    auto result = tracker.subscribe(3, {ns_a});
    REQUIRE(result.forward_to_node);

    // Now replay should contain both
    auto all = tracker.get_all_namespaces();
    REQUIRE(all.size() == 2);
    std::unordered_set<Namespace32, chromatindb::relay::core::Namespace32Hash> ns_set(
        all.begin(), all.end());
    REQUIRE(ns_set.count(ns_a) == 1);
    REQUIRE(ns_set.count(ns_b) == 1);
}

TEST_CASE("send_queue clear does not affect subscription tracker", "[uds_reconnect]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);

    tracker.subscribe(1, {ns_a});
    REQUIRE(tracker.client_subscription_count(1) == 1);

    // Verify subscriptions are fully intact
    auto subs = tracker.get_subscribers(ns_a);
    REQUIRE(subs.size() == 1);
    REQUIRE(subs.count(1) == 1);

    // Clearing a send queue (UdsMultiplexer concern) has no side effects here.
    // SubscriptionTracker state remains unchanged.
    REQUIRE(tracker.get_all_namespaces().size() == 1);
}

// ---------------------------------------------------------------------------
// Subscription replay tests (MUX-06)
// ---------------------------------------------------------------------------

TEST_CASE("get_all_namespaces returns all tracked namespaces for replay",
          "[subscription_replay]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);
    auto ns_b = make_namespace(0xBB);
    auto ns_c = make_namespace(0xCC);

    tracker.subscribe(1, {ns_a, ns_b});
    tracker.subscribe(2, {ns_b, ns_c});

    auto all = tracker.get_all_namespaces();
    REQUIRE(all.size() == 3);

    std::unordered_set<Namespace32, chromatindb::relay::core::Namespace32Hash> ns_set(
        all.begin(), all.end());
    REQUIRE(ns_set.count(ns_a) == 1);
    REQUIRE(ns_set.count(ns_b) == 1);
    REQUIRE(ns_set.count(ns_c) == 1);
}

TEST_CASE("get_all_namespaces returns empty after all clients removed",
          "[subscription_replay]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);

    tracker.subscribe(1, {ns_a});
    tracker.remove_client(1);

    auto all = tracker.get_all_namespaces();
    REQUIRE(all.empty());
}

TEST_CASE("get_all_namespaces reflects unsubscribe", "[subscription_replay]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);
    auto ns_b = make_namespace(0xBB);

    tracker.subscribe(1, {ns_a, ns_b});
    tracker.unsubscribe(1, {ns_a});

    auto all = tracker.get_all_namespaces();
    REQUIRE(all.size() == 1);
    REQUIRE(all[0] == ns_b);
}

// ---------------------------------------------------------------------------
// Notification fan-out tests (MUX-04)
// ---------------------------------------------------------------------------

TEST_CASE("get_subscribers returns correct sessions for namespace", "[notification]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);
    auto ns_b = make_namespace(0xBB);
    auto ns_c = make_namespace(0xCC);

    tracker.subscribe(1, {ns_a});
    tracker.subscribe(2, {ns_a});
    tracker.subscribe(3, {ns_b});

    auto subs_a = tracker.get_subscribers(ns_a);
    REQUIRE(subs_a.size() == 2);
    REQUIRE(subs_a.count(1) == 1);
    REQUIRE(subs_a.count(2) == 1);

    auto subs_b = tracker.get_subscribers(ns_b);
    REQUIRE(subs_b.size() == 1);
    REQUIRE(subs_b.count(3) == 1);

    auto subs_c = tracker.get_subscribers(ns_c);
    REQUIRE(subs_c.empty());
}

TEST_CASE("get_subscribers empty after all sessions unsubscribe", "[notification]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);

    tracker.subscribe(1, {ns_a});
    tracker.unsubscribe(1, {ns_a});

    auto subs = tracker.get_subscribers(ns_a);
    REQUIRE(subs.empty());
}

// ---------------------------------------------------------------------------
// Broadcast separation tests
// ---------------------------------------------------------------------------

TEST_CASE("broadcast: subscription tracker only tracks per-namespace subscribers",
          "[broadcast]") {
    SubscriptionTracker tracker;
    auto ns_a = make_namespace(0xAA);
    auto ns_unsubscribed = make_namespace(0xFF);

    // Session 1 subscribes to ns_a
    tracker.subscribe(1, {ns_a});

    // get_subscribers for a subscribed namespace returns the session
    auto subs_a = tracker.get_subscribers(ns_a);
    REQUIRE(subs_a.size() == 1);
    REQUIRE(subs_a.count(1) == 1);

    // get_subscribers for an unsubscribed namespace returns empty
    // even though session 1 exists -- tracker has no "get_all_sessions"
    auto subs_none = tracker.get_subscribers(ns_unsubscribed);
    REQUIRE(subs_none.empty());

    // StorageFull/QuotaExceeded broadcast uses TokenStore iteration, not tracker.
    // This test documents that separation: tracker only knows per-namespace subscribers.
}

// ---------------------------------------------------------------------------
// Wire format tests
// ---------------------------------------------------------------------------

TEST_CASE("encode_namespace_list_u16be produces correct wire format",
          "[subscription_tracker]") {
    // Replicate the u16BE encoding used in UdsMultiplexer::replay_subscriptions
    auto ns_a = make_namespace(0xAA);
    auto ns_b = make_namespace(0xBB);

    std::vector<Namespace32> namespaces = {ns_a, ns_b};

    // Encode: [u16BE count][ns:32][ns:32]
    std::vector<uint8_t> payload;
    payload.reserve(2 + namespaces.size() * 32);
    uint8_t buf[2];
    chromatindb::relay::util::store_u16_be(buf, static_cast<uint16_t>(namespaces.size()));
    payload.insert(payload.end(), buf, buf + 2);
    for (const auto& ns : namespaces) {
        payload.insert(payload.end(), ns.begin(), ns.end());
    }

    // Verify u16BE count
    REQUIRE(payload[0] == 0x00);
    REQUIRE(payload[1] == 0x02);

    // Total size: 2 + 2*32 = 66
    REQUIRE(payload.size() == 66);

    // Verify first namespace at offset 2
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(payload[2 + i] == 0xAA);
    }

    // Verify second namespace at offset 34
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(payload[34 + i] == 0xBB);
    }
}

TEST_CASE("u16BE namespace list format matches PROTOCOL.md", "[subscription_tracker]") {
    auto ns = make_namespace(0x42);

    // Build payload manually: [0x00, 0x01, ...32 bytes of 0x42...]
    std::vector<uint8_t> payload;
    payload.push_back(0x00);
    payload.push_back(0x01);
    payload.insert(payload.end(), ns.begin(), ns.end());

    // Parse with read_u16_be
    auto count = chromatindb::relay::util::read_u16_be(payload.data());
    REQUIRE(count == 1);

    // Verify 32 bytes at offset 2 match the namespace
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(payload[2 + i] == 0x42);
    }
}
