#include "relay/core/request_router.h"

namespace chromatindb::relay::core {

uint32_t RequestRouter::register_request(uint64_t client_id, uint32_t client_rid) {
    uint32_t rid = next_relay_rid_;

    pending_[rid] = PendingRequest{
        .client_session_id = client_id,
        .client_request_id = client_rid,
        .created = std::chrono::steady_clock::now(),
    };

    // Advance counter, wrap from UINT32_MAX to 1 (skip 0, per D-07)
    if (next_relay_rid_ == UINT32_MAX) {
        next_relay_rid_ = 1;
    } else {
        ++next_relay_rid_;
    }

    return rid;
}

std::optional<PendingRequest> RequestRouter::resolve_response(uint32_t relay_rid) {
    auto it = pending_.find(relay_rid);
    if (it == pending_.end()) {
        return std::nullopt;
    }

    auto result = it->second;
    pending_.erase(it);
    return result;
}

void RequestRouter::remove_client(uint64_t client_id) {
    std::erase_if(pending_, [client_id](const auto& entry) {
        return entry.second.client_session_id == client_id;
    });
}

size_t RequestRouter::purge_stale(std::chrono::seconds timeout) {
    auto now = std::chrono::steady_clock::now();
    size_t count = 0;

    std::erase_if(pending_, [&](const auto& entry) {
        if (now - entry.second.created > timeout) {
            ++count;
            return true;
        }
        return false;
    });

    return count;
}

void RequestRouter::bulk_fail_all(
    std::function<void(uint64_t session_id, uint32_t client_rid)> on_fail) {
    for (const auto& [relay_rid, pending] : pending_) {
        on_fail(pending.client_session_id, pending.client_request_id);
    }
    pending_.clear();
}

size_t RequestRouter::pending_count() const {
    return pending_.size();
}

} // namespace chromatindb::relay::core
