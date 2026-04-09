#include "relay/ws/session_manager.h"

namespace chromatindb::relay::ws {

uint64_t SessionManager::add_session(std::shared_ptr<WsSession> session) {
    uint64_t id = next_id_++;
    sessions_.emplace(id, std::move(session));
    return id;
}

void SessionManager::remove_session(uint64_t id) {
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

} // namespace chromatindb::relay::ws
