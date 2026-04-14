#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>

namespace chromatindb::relay::core {

/// Information about a pending client request.
struct PendingRequest {
    uint64_t client_session_id;
    uint32_t client_request_id;
    std::chrono::steady_clock::time_point created;
    uint8_t original_type = 0;  // D-08: message type byte for ErrorResponse construction
};

/// Maps relay-scoped request_ids to client sessions and their original request_ids.
/// Used for multiplexing multiple client requests through a single UDS connection.
///
/// Access serialized via strand -- all callers must be on the strand.
class RequestRouter {
public:
    /// Register a client request. Returns relay-scoped request_id.
    /// Per D-07: counter starts at 1 (0 reserved for server-initiated), wraps at UINT32_MAX.
    /// @param original_type the wire message type byte (for timeout ErrorResponse).
    uint32_t register_request(uint64_t client_id, uint32_t client_rid, uint8_t original_type);

    /// Resolve a node response by relay_rid. Removes the entry from pending map.
    /// Returns the original client info, or nullopt if not found.
    std::optional<PendingRequest> resolve_response(uint32_t relay_rid);

    /// Per D-10: Remove all pending entries for a disconnected client.
    void remove_client(uint64_t client_id);

    /// Per D-11: Purge entries older than timeout (default 60s).
    /// When on_timeout is non-null, invokes callback for each stale entry before erasure.
    /// Returns the number of purged entries.
    size_t purge_stale(std::chrono::seconds timeout,
                       std::function<void(const PendingRequest&)> on_timeout);
    size_t purge_stale(std::chrono::seconds timeout = std::chrono::seconds(60));

    /// Bulk-fail all pending requests. Calls on_fail for each entry before clearing.
    /// Per D-13: used on UDS disconnect to fail all pending client requests.
    void bulk_fail_all(std::function<void(uint64_t session_id, uint32_t client_rid)> on_fail);

    /// Number of pending requests (for metrics/debugging).
    size_t pending_count() const;

    /// Set internal counter (for testing wrap behavior).
    void set_next_relay_rid(uint32_t val) { next_relay_rid_ = val; }

private:
    uint32_t next_relay_rid_ = 1;  // Per D-07
    std::unordered_map<uint32_t, PendingRequest> pending_;  // Per D-08
};

} // namespace chromatindb::relay::core
