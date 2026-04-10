#include <catch2/catch_test_macros.hpp>

#include "relay/core/request_router.h"

#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

using chromatindb::relay::core::RequestRouter;

TEST_CASE("register_request returns sequential IDs starting at 1", "[request_router]") {
    RequestRouter router;

    auto id1 = router.register_request(100, 1);
    auto id2 = router.register_request(100, 2);
    auto id3 = router.register_request(100, 3);

    REQUIRE(id1 == 1);
    REQUIRE(id2 == 2);
    REQUIRE(id3 == 3);
}

TEST_CASE("resolve_response returns correct client info", "[request_router]") {
    RequestRouter router;

    auto relay_rid = router.register_request(42, 7);
    auto result = router.resolve_response(relay_rid);

    REQUIRE(result.has_value());
    REQUIRE(result->client_session_id == 42);
    REQUIRE(result->client_request_id == 7);
}

TEST_CASE("resolve_response removes entry", "[request_router]") {
    RequestRouter router;

    auto relay_rid = router.register_request(1, 1);
    REQUIRE(router.pending_count() == 1);

    auto result1 = router.resolve_response(relay_rid);
    REQUIRE(result1.has_value());
    REQUIRE(router.pending_count() == 0);

    // Second resolve of same ID returns nullopt
    auto result2 = router.resolve_response(relay_rid);
    REQUIRE(!result2.has_value());
}

TEST_CASE("resolve_response returns nullopt for unknown ID", "[request_router]") {
    RequestRouter router;

    auto result = router.resolve_response(999);
    REQUIRE(!result.has_value());
}

TEST_CASE("remove_client purges all entries for that client", "[request_router]") {
    RequestRouter router;

    uint64_t client_a = 100;
    uint64_t client_b = 200;

    // Register 3 from A, 2 from B
    auto a1 = router.register_request(client_a, 1);
    auto a2 = router.register_request(client_a, 2);
    auto a3 = router.register_request(client_a, 3);
    auto b1 = router.register_request(client_b, 10);
    auto b2 = router.register_request(client_b, 20);

    REQUIRE(router.pending_count() == 5);

    router.remove_client(client_a);
    REQUIRE(router.pending_count() == 2);

    // A's entries are gone
    REQUIRE(!router.resolve_response(a1).has_value());
    REQUIRE(!router.resolve_response(a2).has_value());
    REQUIRE(!router.resolve_response(a3).has_value());

    // B's entries still resolvable
    auto rb1 = router.resolve_response(b1);
    REQUIRE(rb1.has_value());
    REQUIRE(rb1->client_session_id == client_b);
    REQUIRE(rb1->client_request_id == 10);

    auto rb2 = router.resolve_response(b2);
    REQUIRE(rb2.has_value());
    REQUIRE(rb2->client_session_id == client_b);
    REQUIRE(rb2->client_request_id == 20);
}

TEST_CASE("purge_stale removes old entries", "[request_router]") {
    RequestRouter router;

    // Register some requests
    router.register_request(1, 1);
    router.register_request(2, 2);
    router.register_request(3, 3);

    REQUIRE(router.pending_count() == 3);

    // Use 0s timeout to purge all (everything is "stale" relative to now)
    auto purged = router.purge_stale(std::chrono::seconds(0));
    REQUIRE(purged == 3);
    REQUIRE(router.pending_count() == 0);
}

TEST_CASE("counter wraps from UINT32_MAX to 1 skipping 0", "[request_router]") {
    RequestRouter router;

    // Set internal counter near UINT32_MAX
    router.set_next_relay_rid(UINT32_MAX - 1);

    auto id1 = router.register_request(1, 1);
    REQUIRE(id1 == UINT32_MAX - 1);

    auto id2 = router.register_request(1, 2);
    REQUIRE(id2 == UINT32_MAX);

    // Next should wrap to 1, skipping 0
    auto id3 = router.register_request(1, 3);
    REQUIRE(id3 == 1);
}

TEST_CASE("register_request with client_request_id 0 (fire-and-forget per D-12)", "[request_router]") {
    RequestRouter router;

    auto relay_rid = router.register_request(50, 0);
    REQUIRE(relay_rid == 1);

    auto result = router.resolve_response(relay_rid);
    REQUIRE(result.has_value());
    REQUIRE(result->client_session_id == 50);
    REQUIRE(result->client_request_id == 0);
}

TEST_CASE("pending_count reflects current state", "[request_router]") {
    RequestRouter router;

    REQUIRE(router.pending_count() == 0);

    router.register_request(1, 1);
    REQUIRE(router.pending_count() == 1);

    router.register_request(1, 2);
    REQUIRE(router.pending_count() == 2);

    router.resolve_response(1);
    REQUIRE(router.pending_count() == 1);

    router.resolve_response(2);
    REQUIRE(router.pending_count() == 0);
}

TEST_CASE("bulk_fail_all invokes callback for each pending entry", "[request_router][bulk_fail]") {
    RequestRouter router;

    router.register_request(1, 10);
    router.register_request(2, 20);
    router.register_request(3, 30);

    std::vector<std::pair<uint64_t, uint32_t>> failed;
    router.bulk_fail_all([&](uint64_t session_id, uint32_t client_rid) {
        failed.emplace_back(session_id, client_rid);
    });

    REQUIRE(failed.size() == 3);
    REQUIRE(router.pending_count() == 0);

    // All three pairs must be present (order may vary)
    std::unordered_set<uint64_t> sessions;
    std::unordered_set<uint32_t> rids;
    for (const auto& [sid, rid] : failed) {
        sessions.insert(sid);
        rids.insert(rid);
    }
    REQUIRE(sessions.count(1) == 1);
    REQUIRE(sessions.count(2) == 1);
    REQUIRE(sessions.count(3) == 1);
    REQUIRE(rids.count(10) == 1);
    REQUIRE(rids.count(20) == 1);
    REQUIRE(rids.count(30) == 1);
}

TEST_CASE("bulk_fail_all on empty map is no-op", "[request_router][bulk_fail]") {
    RequestRouter router;

    int counter = 0;
    router.bulk_fail_all([&](uint64_t, uint32_t) {
        ++counter;
    });

    REQUIRE(counter == 0);
    REQUIRE(router.pending_count() == 0);
}
