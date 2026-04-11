#pragma once

#include "db/net/connection.h"

#include <asio/steady_timer.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <set>
#include <string>
#include <vector>

namespace chromatindb::peer {

/// A persisted peer address with connection tracking.
struct PersistedPeer {
    std::string address;
    uint64_t last_seen = 0;     // Unix timestamp
    uint32_t fail_count = 0;
    std::string pubkey_hash;    // SHA3-256(pubkey) hex string, empty = unknown
};

/// A sync message received from a peer, queued for processing.
struct SyncMessage {
    wire::TransportMsgType type;
    std::vector<uint8_t> payload;
};

/// Info about a connected peer.
struct PeerInfo {
    net::Connection::Ptr connection;
    std::string address;
    bool is_bootstrap = false;
    uint32_t strike_count = 0;
    bool syncing = false;
    // Sync message queue (timer-cancel pattern)
    std::deque<SyncMessage> sync_inbox;
    asio::steady_timer* sync_notify = nullptr;
    // Pub/Sub subscriptions (connection-scoped)
    std::set<std::array<uint8_t, 32>> subscribed_namespaces;
    // Phase 16: Storage capacity signaling (resets on reconnect via PeerInfo recreation)
    bool peer_is_full = false;
    // Phase 18: Token bucket rate limiting (resets on reconnect via PeerInfo recreation)
    uint64_t bucket_tokens = 0;        // Available throughput tokens (bytes)
    uint64_t bucket_last_refill = 0;   // steady_clock milliseconds since epoch
    uint64_t last_sync_initiated = 0;  // steady_clock ms since epoch (0 = never synced as responder)
    uint64_t last_message_time = 0;   // steady_clock ms since epoch (0 = not yet set)
    // Phase 86: Peer's declared replication scope (empty = replicate all, per D-07)
    std::set<std::array<uint8_t, 32>> announced_namespaces;
    // Phase 86: Announce handshake coordination (timer-cancel pattern)
    bool announce_received = false;
    asio::steady_timer* announce_notify = nullptr;
};

/// Runtime metrics counters. Plain uint64_t -- strand-confined to io_context thread.
/// All increment sites verified Phase 99 (CORO-01):
///   message_dispatcher.cpp: inline or after co_await post(ioc_)
///   sync_orchestrator.cpp: coroutines on ioc_
///   blob_push_manager.cpp: after co_await post(ioc_)
///   connection_manager.cpp: inline callbacks on ioc_
/// Do NOT use std::atomic -- that masks design bugs (D-14).
/// Monotonically increasing since startup (never reset).
struct NodeMetrics {
    uint64_t ingests = 0;                  // Successful blob ingestions
    uint64_t rejections = 0;               // Failed ingestions (validation errors)
    uint64_t syncs = 0;                    // Completed sync rounds
    uint64_t rate_limited = 0;             // Rate limit disconnections
    uint64_t peers_connected_total = 0;    // Total peer connections since startup
    uint64_t peers_disconnected_total = 0; // Total peer disconnections since startup
    uint64_t cursor_hits = 0;             // Namespaces skipped via cursor match
    uint64_t cursor_misses = 0;           // Namespaces requiring full hash diff
    uint64_t full_resyncs = 0;            // Full resync rounds triggered
    uint64_t quota_rejections = 0;        // Namespace quota exceeded rejections
    uint64_t sync_rejections = 0;          // Sync rate limit rejections (cooldown + session + byte rate)
    uint64_t error_responses = 0;          // ErrorResponse messages sent to clients
};

/// Hash functor for 32-byte arrays (first 8 bytes as uint64_t -- sufficient entropy for blob hashes).
struct ArrayHash32 {
    size_t operator()(const std::array<uint8_t, 32>& arr) const noexcept {
        uint64_t h;
        std::memcpy(&h, arr.data(), sizeof(h));
        return static_cast<size_t>(h);
    }
};

/// Hash functor for 64-byte arrays (first 8 bytes as uint64_t -- sufficient entropy for namespace||hash keys).
struct ArrayHash64 {
    size_t operator()(const std::array<uint8_t, 64>& arr) const noexcept {
        uint64_t h;
        std::memcpy(&h, arr.data(), sizeof(h));
        return static_cast<size_t>(h);
    }
};

/// Tracks when a peer disconnected for cursor grace period (Phase 82 MAINT-04).
/// Cursors persist in MDBX -- we only need the disconnect timestamp to decide
/// whether to reuse them (within 5 min) or discard them (after 5 min).
struct DisconnectedPeerState {
    uint64_t disconnect_time;  // steady_clock milliseconds since epoch
};

} // namespace chromatindb::peer
