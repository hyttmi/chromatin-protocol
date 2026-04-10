#include "relay/ws/session_manager.h"
#include "relay/core/subscription_tracker.h"

namespace chromatindb::relay::ws {

uint64_t SessionManager::add_session(std::shared_ptr<WsSession> session) {
    uint64_t id = next_id_++;
    sessions_.emplace(id, std::move(session));
    return id;
}

void SessionManager::remove_session(uint64_t id) {
    // Clean up subscriptions BEFORE erasing session (per D-05).
    if (tracker_) {
        auto empty_namespaces = tracker_->remove_client(id);
        if (!empty_namespaces.empty() && on_namespaces_empty_) {
            on_namespaces_empty_(empty_namespaces);
        }
    }
    sessions_.erase(id);
}

std::shared_ptr<WsSession> SessionManager::get_session(uint64_t id) const {
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return nullptr;
    return it->second;
}

size_t SessionManager::count() const {
    return sessions_.size();
}

void SessionManager::set_tracker(core::SubscriptionTracker* t) {
    tracker_ = t;
}

void SessionManager::set_on_namespaces_empty(
    std::function<void(const std::vector<std::array<uint8_t, 32>>&)> cb) {
    on_namespaces_empty_ = std::move(cb);
}

} // namespace chromatindb::relay::ws
