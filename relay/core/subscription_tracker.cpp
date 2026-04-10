#include "relay/core/subscription_tracker.h"

#include <spdlog/spdlog.h>

namespace chromatindb::relay::core {

SubscribeResult SubscriptionTracker::subscribe(uint64_t session_id,
                                                const std::vector<Namespace32>& namespaces) {
    SubscribeResult result{false, {}};

    for (const auto& ns : namespaces) {
        auto& session_set = subs_[ns];
        bool was_empty = session_set.empty();
        session_set.insert(session_id);
        client_subs_[session_id].insert(ns);

        if (was_empty) {
            result.new_namespaces.push_back(ns);
        }
    }

    result.forward_to_node = !result.new_namespaces.empty();

    spdlog::debug("subscription_tracker: session {} subscribed to {} namespace(s), {} new",
                  session_id, namespaces.size(), result.new_namespaces.size());

    return result;
}

UnsubscribeResult SubscriptionTracker::unsubscribe(uint64_t session_id,
                                                    const std::vector<Namespace32>& namespaces) {
    UnsubscribeResult result{false, {}};

    for (const auto& ns : namespaces) {
        auto it = subs_.find(ns);
        if (it == subs_.end()) {
            continue;
        }

        it->second.erase(session_id);

        if (it->second.empty()) {
            subs_.erase(it);
            result.removed_namespaces.push_back(ns);
        }
    }

    // Also clean up client_subs_
    auto client_it = client_subs_.find(session_id);
    if (client_it != client_subs_.end()) {
        for (const auto& ns : namespaces) {
            client_it->second.erase(ns);
        }
        if (client_it->second.empty()) {
            client_subs_.erase(client_it);
        }
    }

    result.forward_to_node = !result.removed_namespaces.empty();

    spdlog::debug("subscription_tracker: session {} unsubscribed from {} namespace(s), {} removed from node",
                  session_id, namespaces.size(), result.removed_namespaces.size());

    return result;
}

std::vector<Namespace32> SubscriptionTracker::remove_client(uint64_t session_id) {
    auto client_it = client_subs_.find(session_id);
    if (client_it == client_subs_.end()) {
        return {};
    }

    std::vector<Namespace32> empty_namespaces;
    size_t total_subs = client_it->second.size();

    for (const auto& ns : client_it->second) {
        auto subs_it = subs_.find(ns);
        if (subs_it == subs_.end()) {
            continue;
        }

        subs_it->second.erase(session_id);

        if (subs_it->second.empty()) {
            subs_.erase(subs_it);
            empty_namespaces.push_back(ns);
        }
    }

    client_subs_.erase(client_it);

    spdlog::info("subscription_tracker: removed client {} ({} subscriptions cleaned, {} node unsubscribes)",
                 session_id, total_subs, empty_namespaces.size());

    return empty_namespaces;
}

std::vector<Namespace32> SubscriptionTracker::get_all_namespaces() const {
    std::vector<Namespace32> result;
    result.reserve(subs_.size());
    for (const auto& [ns, _] : subs_) {
        result.push_back(ns);
    }
    return result;
}

std::unordered_set<uint64_t> SubscriptionTracker::get_subscribers(const Namespace32& ns) const {
    auto it = subs_.find(ns);
    if (it == subs_.end()) {
        return {};
    }
    return it->second;
}

size_t SubscriptionTracker::client_subscription_count(uint64_t session_id) const {
    auto it = client_subs_.find(session_id);
    if (it == client_subs_.end()) {
        return 0;
    }
    return it->second.size();
}

} // namespace chromatindb::relay::core
