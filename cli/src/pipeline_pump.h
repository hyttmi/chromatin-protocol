#pragma once

#include "cli/src/wire.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

namespace chromatindb::cli::pipeline {

// =============================================================================
// Pipelining pump helpers (header-only, single-threaded)
// =============================================================================
//
// These free functions contain the pure routing logic behind
// Connection::send_async and Connection::recv_for. Extracting them into a
// header-only namespace makes the logic testable without instantiating a real
// Connection (which requires a socket and a completed PQ handshake).
//
// Contracts:
//  - Source is any callable invocable as `std::optional<DecodedTransport>()`.
//    It represents one call to Connection::recv(): returns the next
//    already-AEAD-decrypted reply, or std::nullopt on transport error.
//  - `pending` is the per-Connection correlation map. Its size is bounded by
//    PIPELINE_DEPTH because send_async only lets PIPELINE_DEPTH requests be
//    in flight concurrently, so at most PIPELINE_DEPTH replies can ever land
//    in it (T-120-02 mitigation).
//  - `in_flight` is the per-Connection in-flight counter. pump_recv_for
//    decrements it by one on successful return; pump_one_for_backpressure
//    does NOT touch it — the caller (send_async) manages it.
//
// Single-sender invariant (D-09 / PIPE-02): these helpers never invoke the
// send path. They only drive the supplied Source (which wraps recv()), so
// nothing here can write send_counter_ and AEAD nonce monotonicity is
// preserved by construction.

// =============================================================================
// Drain one reply into `pending` (backpressure step)
// =============================================================================
//
// Used by Connection::send_async when in_flight_ >= PIPELINE_DEPTH: one call
// drains exactly one reply from the wire into the correlation map so a
// slot frees up. Returns false if the source is dead (transport error).
template <typename Source>
bool pump_one_for_backpressure(
    Source&& source,
    std::unordered_map<uint32_t, DecodedTransport>& pending) {
    auto msg = source();
    if (!msg) return false;
    // D-04: insert_or_assign handles the pathological "two replies for the
    // same rid" case without growth; the map stays bounded by PIPELINE_DEPTH.
    pending.insert_or_assign(msg->request_id, std::move(*msg));
    return true;
}

// =============================================================================
// Pump until the reply for `target_rid` is found
// =============================================================================
//
// Fast path: if `pending` already contains `target_rid`, return it without
// calling `source`. Otherwise loop over `source`, stashing off-target replies
// into `pending` until the matching reply arrives. On success, decrement
// `in_flight` by one. On source error (nullopt), return nullopt and leave
// `in_flight` untouched.
template <typename Source>
std::optional<DecodedTransport> pump_recv_for(
    uint32_t target_rid,
    Source&& source,
    std::unordered_map<uint32_t, DecodedTransport>& pending,
    std::size_t& in_flight) {
    // Fast path: already stashed by a prior pump cycle.
    if (auto it = pending.find(target_rid); it != pending.end()) {
        DecodedTransport msg = std::move(it->second);
        pending.erase(it);
        if (in_flight > 0) --in_flight;
        return msg;
    }

    // Pump until target arrives.
    while (true) {
        auto msg = source();
        if (!msg) return std::nullopt;

        if (msg->request_id == target_rid) {
            if (in_flight > 0) --in_flight;
            return msg;
        }

        // Off-target — stash for a future recv_for(). D-04: bounded.
        // If a reply arrives for a rid no caller will ever ask for
        // (server bug), it lingers until close() clears the map.
        pending.insert_or_assign(msg->request_id, std::move(*msg));
    }
}

// =============================================================================
// Pump one reply in arrival order (not correlated to a specific rid)
// =============================================================================
//
// Counterpart of pump_recv_for for callers that drain replies in arrival order
// (D-08 — chunked uploads/downloads, cmd::put batches, cmd::get batches).
//
// Fast path: if anything was stashed by a prior send_async backpressure cycle
// into `pending`, deliver one of those entries first (no wire I/O). Else call
// `source()` for the next wire reply.
//
// On ANY non-nullopt return: decrement `in_flight` by one (both fast-path and
// source-path represent one pipeline slot freeing). Guarded so `in_flight = 0`
// never underflows — defense-in-depth against the CR-01 class of bug where an
// errant caller could otherwise decrement past zero.
//
// On source error (nullopt) with empty `pending`: return nullopt and leave
// `in_flight` UNTOUCHED (matches pump_recv_for's error semantic).
//
// Single-sender invariant (PIPE-02): this template only invokes `source()`
// (which wraps recv()) — it never touches the send path, so send_counter_
// and AEAD nonce monotonicity are preserved by construction.
//
// Note on the WR-01 window (carried forward from 119-REVIEW.md, out of scope
// for this template): stale entries in `pending` left behind by a failed
// send_async + pump_one_for_backpressure exchange are not cleaned up here —
// in that narrow case the connection is already dead and the caller exits.
template <typename Source>
std::optional<DecodedTransport> pump_recv_any(
    Source&& source,
    std::unordered_map<uint32_t, DecodedTransport>& pending,
    std::size_t& in_flight) {
    // Fast path: deliver a stashed reply before touching the wire.
    if (!pending.empty()) {
        auto it = pending.begin();
        DecodedTransport msg = std::move(it->second);
        pending.erase(it);
        if (in_flight > 0) --in_flight;
        return msg;
    }

    auto msg = source();
    if (!msg) return std::nullopt;
    if (in_flight > 0) --in_flight;
    return msg;
}

} // namespace chromatindb::cli::pipeline
