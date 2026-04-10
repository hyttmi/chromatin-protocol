#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace chromatindb::relay::core {
class SubscriptionTracker;
} // namespace chromatindb::relay::core

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

    /// Set subscription tracker for disconnect cleanup (Phase 104).
    void set_tracker(core::SubscriptionTracker* t);

    /// Set callback for when namespaces become empty after client disconnect.
    /// Used by main() to send Unsubscribe to node via UdsMultiplexer.
    void set_on_namespaces_empty(
        std::function<void(const std::vector<std::array<uint8_t, 32>>&)> cb);

private:
    std::unordered_map<uint64_t, std::shared_ptr<WsSession>> sessions_;
    uint64_t next_id_ = 1;
    core::SubscriptionTracker* tracker_ = nullptr;
    std::function<void(const std::vector<std::array<uint8_t, 32>>&)> on_namespaces_empty_;
};

} // namespace chromatindb::relay::ws
