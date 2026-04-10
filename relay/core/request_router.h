#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace chromatindb::relay::core {

/// Information about a pending client request.
struct PendingRequest {
    uint64_t client_session_id;
    uint32_t client_request_id;
    std::chrono::steady_clock::time_point created;
};

/// Maps relay-scoped request_ids to client sessions and their original request_ids.
/// Used for multiplexing multiple client requests through a single UDS connection.
///
/// NOT thread-safe -- all access must be on the same strand/thread.
class RequestRouter {
public:
    /// Register a client request. Returns relay-scoped request_id.
    /// Per D-07: counter starts at 1 (0 reserved for server-initiated), wraps at UINT32_MAX.
    uint32_t register_request(uint64_t client_id, uint32_t client_rid);

    /// Resolve a node response by relay_rid. Removes the entry from pending map.
    /// Returns the original client info, or nullopt if not found.
    std::optional<PendingRequest> resolve_response(uint32_t relay_rid);

    /// Per D-10: Remove all pending entries for a disconnected client.
    void remove_client(uint64_t client_id);

    /// Per D-11: Purge entries older than timeout (default 60s).
    /// Returns the number of purged entries.
    size_t purge_stale(std::chrono::seconds timeout = std::chrono::seconds(60));

    /// Number of pending requests (for metrics/debugging).
    size_t pending_count() const;

    /// Set internal counter (for testing wrap behavior).
    void set_next_relay_rid(uint32_t val) { next_relay_rid_ = val; }

private:
    uint32_t next_relay_rid_ = 1;  // Per D-07
    std::unordered_map<uint32_t, PendingRequest> pending_;  // Per D-08
};

} // namespace chromatindb::relay::core
