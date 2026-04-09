#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace chromatindb::relay::ws {

class WsSession;  // Forward declare

/// Tracks active WebSocket sessions by unique ID.
/// Used by WsAcceptor for registration and future fan-out (Phase 104).
class SessionManager {
public:
    /// Register a session. Returns assigned unique ID.
    uint64_t add_session(std::shared_ptr<WsSession> session);

    /// Remove a session by ID.
    void remove_session(uint64_t id);

    /// Look up a session by ID. Returns nullptr if not found.
    std::shared_ptr<WsSession> get_session(uint64_t id) const;

    /// Number of active sessions.
    size_t count() const;

    /// Iterate all sessions (for SIGTERM close, future fan-out).
    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [id, session] : sessions_) {
            fn(id, session);
        }
    }

private:
    std::unordered_map<uint64_t, std::shared_ptr<WsSession>> sessions_;
    uint64_t next_id_ = 1;
};

} // namespace chromatindb::relay::ws
