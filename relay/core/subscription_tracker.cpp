#include "relay/core/subscription_tracker.h"

#include <spdlog/spdlog.h>

namespace chromatindb::relay::core {

SubscribeResult SubscriptionTracker::subscribe(uint64_t /*session_id*/,
                                                const std::vector<Namespace32>& /*namespaces*/) {
    // TDD RED: stub returns wrong result
    return {false, {}};
}

UnsubscribeResult SubscriptionTracker::unsubscribe(uint64_t /*session_id*/,
                                                    const std::vector<Namespace32>& /*namespaces*/) {
    return {false, {}};
}

std::vector<Namespace32> SubscriptionTracker::remove_client(uint64_t /*session_id*/) {
    return {};
}

std::vector<Namespace32> SubscriptionTracker::get_all_namespaces() const {
    return {};
}

std::unordered_set<uint64_t> SubscriptionTracker::get_subscribers(const Namespace32& /*ns*/) const {
    return {};
}

size_t SubscriptionTracker::client_subscription_count(uint64_t /*session_id*/) const {
    return 0;
}

} // namespace chromatindb::relay::core
