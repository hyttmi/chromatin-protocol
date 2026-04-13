#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>

namespace chromatindb::relay::core {

/// Transport-agnostic session dispatch interface.
/// UdsMultiplexer calls these to route responses and notifications
/// without knowing whether the transport is WebSocket or HTTP.
struct SessionDispatch {
    /// Send JSON to a specific session. No-op if session not found.
    std::function<void(uint64_t session_id, const nlohmann::json& msg)> send_json;

    /// Broadcast JSON to ALL active sessions (for StorageFull, QuotaExceeded).
    std::function<void(const nlohmann::json& msg)> broadcast;

    /// Send JSON to a specific session by ID, with error context.
    /// Same as send_json but named distinctly for error paths.
    /// Falls back to send_json if not set.
    std::function<void(uint64_t session_id, const nlohmann::json& err)> send_error;
};

} // namespace chromatindb::relay::core
