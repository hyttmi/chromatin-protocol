#include <catch2/catch_test_macros.hpp>
#include "cli/src/pipeline_pump.h"
#include "cli/src/wire.h"

#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace chromatindb::cli;

namespace {

DecodedTransport make_reply(uint32_t rid, uint8_t type = 0x42,
                            std::vector<uint8_t> payload = {}) {
    DecodedTransport m;
    m.type = type;
    m.request_id = rid;
    m.payload = std::move(payload);
    return m;
}

// Source fixture: a FIFO queue of replies. Each call dequeues one.
// If the test asks for more than scripted, the source goes "dead" and
// returns nullopt (simulates transport error). This also catches test
// bugs that over-drain.
struct ScriptedSource {
    std::deque<DecodedTransport> queue;
    bool dead = false;
    int call_count = 0;

    std::optional<DecodedTransport> operator()() {
        ++call_count;
        if (dead) return std::nullopt;
        if (queue.empty()) {
            dead = true;
            return std::nullopt;
        }
        auto m = std::move(queue.front());
        queue.pop_front();
        return m;
    }
};

} // namespace

TEST_CASE("pipeline: recv_for returns matching reply on direct hit", "[pipeline]") {
    ScriptedSource src;
    src.queue.push_back(make_reply(7));
    std::unordered_map<uint32_t, DecodedTransport> pending;
    size_t in_flight = 1;

    auto got = pipeline::pump_recv_for(7, std::ref(src), pending, in_flight);
    REQUIRE(got.has_value());
    REQUIRE(got->request_id == 7);
    REQUIRE(pending.empty());
    REQUIRE(in_flight == 0);
}

TEST_CASE("pipeline: send-8-receive-out-of-order correlation", "[pipeline]") {
    // Simulate firing rids 1..8, server returns them in shuffled order.
    ScriptedSource src;
    for (uint32_t r : {5u, 1u, 8u, 3u, 7u, 2u, 6u, 4u}) {
        src.queue.push_back(make_reply(r));
    }
    std::unordered_map<uint32_t, DecodedTransport> pending;
    size_t in_flight = 8;

    // Caller asks for rid=1 first. Pump drains 5 (stashed), then returns 1.
    auto got1 = pipeline::pump_recv_for(1, std::ref(src), pending, in_flight);
    REQUIRE(got1.has_value());
    REQUIRE(got1->request_id == 1);
    REQUIRE(in_flight == 7);
    REQUIRE(pending.count(5) == 1);

    // Caller asks for rid=8 next. Pump drains 8 immediately (queue head).
    auto got8 = pipeline::pump_recv_for(8, std::ref(src), pending, in_flight);
    REQUIRE(got8.has_value());
    REQUIRE(got8->request_id == 8);
    REQUIRE(in_flight == 6);

    // Drain the rest in arbitrary caller order.
    for (uint32_t r : {2u, 3u, 4u, 5u, 6u, 7u}) {
        auto m = pipeline::pump_recv_for(r, std::ref(src), pending, in_flight);
        REQUIRE(m.has_value());
        REQUIRE(m->request_id == r);
    }
    REQUIRE(in_flight == 0);
    REQUIRE(pending.empty());
}

TEST_CASE("pipeline: pre-stashed reply returns without calling source", "[pipeline]") {
    ScriptedSource src;  // empty — would go dead if called
    std::unordered_map<uint32_t, DecodedTransport> pending;
    pending.emplace(42, make_reply(42));
    size_t in_flight = 1;

    auto got = pipeline::pump_recv_for(42, std::ref(src), pending, in_flight);
    REQUIRE(got.has_value());
    REQUIRE(got->request_id == 42);
    REQUIRE(pending.empty());
    REQUIRE(in_flight == 0);
    REQUIRE(src.call_count == 0);  // source never invoked
    REQUIRE_FALSE(src.dead);
}

TEST_CASE("pipeline: source-dead during pump returns nullopt", "[pipeline]") {
    ScriptedSource src;
    src.dead = true;
    std::unordered_map<uint32_t, DecodedTransport> pending;
    size_t in_flight = 1;

    auto got = pipeline::pump_recv_for(99, std::ref(src), pending, in_flight);
    REQUIRE_FALSE(got.has_value());
    // in_flight unchanged on error.
    REQUIRE(in_flight == 1);
}

TEST_CASE("pipeline: backpressure drains one reply per call", "[pipeline]") {
    ScriptedSource src;
    src.queue.push_back(make_reply(11));
    src.queue.push_back(make_reply(12));
    std::unordered_map<uint32_t, DecodedTransport> pending;

    REQUIRE(pipeline::pump_one_for_backpressure(std::ref(src), pending));
    REQUIRE(pending.count(11) == 1);
    REQUIRE(pipeline::pump_one_for_backpressure(std::ref(src), pending));
    REQUIRE(pending.count(12) == 1);
    REQUIRE(pending.size() == 2);
}

TEST_CASE("pipeline: backpressure source-dead returns false", "[pipeline]") {
    ScriptedSource src;
    src.dead = true;
    std::unordered_map<uint32_t, DecodedTransport> pending;

    REQUIRE_FALSE(pipeline::pump_one_for_backpressure(std::ref(src), pending));
    REQUIRE(pending.empty());
}

TEST_CASE("pipeline: unknown-rid reply is stashed not crashed (D-04)", "[pipeline]") {
    // A reply arrives for rid=99999 that the caller never sent. The pump
    // stashes it harmlessly. A later recv_for(7) drains its actual reply.
    ScriptedSource src;
    src.queue.push_back(make_reply(99999));  // server bug / stray
    src.queue.push_back(make_reply(7));
    std::unordered_map<uint32_t, DecodedTransport> pending;
    size_t in_flight = 1;

    auto got = pipeline::pump_recv_for(7, std::ref(src), pending, in_flight);
    REQUIRE(got.has_value());
    REQUIRE(got->request_id == 7);
    REQUIRE(in_flight == 0);
    // The 99999 reply is stashed; bounded by depth in real usage, no
    // eviction needed in the test. It lingers until close() clears the map.
    REQUIRE(pending.count(99999) == 1);
}

TEST_CASE("pipeline: duplicate rid reply overwrites without growth", "[pipeline]") {
    // Pathological: server sends two replies for the same rid.
    // insert_or_assign keeps the map bounded (D-04 bound is PIPELINE_DEPTH).
    ScriptedSource src;
    src.queue.push_back(make_reply(5, 0x10));
    src.queue.push_back(make_reply(5, 0x20));
    src.queue.push_back(make_reply(7));
    std::unordered_map<uint32_t, DecodedTransport> pending;
    size_t in_flight = 2;

    auto got = pipeline::pump_recv_for(7, std::ref(src), pending, in_flight);
    REQUIRE(got.has_value());
    REQUIRE(got->request_id == 7);
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[5].type == 0x20);  // second write won
}
