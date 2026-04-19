#pragma once

#include "cli/src/wire.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace chromatindb::cli::testing {

// Build a synthetic DecodedTransport for test script queues.
// Matches the signature of the original free function in
// test_connection_pipelining.cpp (pre-extraction).
inline DecodedTransport make_reply(
    uint32_t rid,
    uint8_t type = 0x42,
    std::vector<uint8_t> payload = {}) {
    DecodedTransport m;
    m.type = type;
    m.request_id = rid;
    m.payload = std::move(payload);
    return m;
}

// Convenience factory for WriteAck-shaped replies (41-byte payload padded
// with 0xAB sentinel). Matches the min payload validated by put_chunked.
// Used by chunked regression tests (Task 12) that simulate the CR-01 shape.
inline DecodedTransport make_ack_reply(uint32_t rid) {
    DecodedTransport m;
    m.type = 0x02;  // arbitrary WriteAck-like byte
    m.request_id = rid;
    m.payload.assign(41, 0xAB);
    return m;
}

// FIFO queue of scripted replies. Each call to operator() dequeues one.
// If over-drained, goes "dead" and returns nullopt (simulates transport
// error; also catches test bugs that over-drain).
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

}  // namespace chromatindb::cli::testing
