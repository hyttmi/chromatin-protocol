#include "kademlia/routing_table.h"

#include <algorithm>

namespace helix::kademlia {

void RoutingTable::add_or_update(NodeInfo info) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& existing : nodes_) {
        if (existing.id == info.id) {
            existing.address = std::move(info.address);
            existing.udp_port = info.udp_port;
            existing.ws_port = info.ws_port;
            existing.pubkey = std::move(info.pubkey);
            existing.last_seen = info.last_seen;
            return;
        }
    }
    nodes_.push_back(std::move(info));
}

void RoutingTable::remove(const NodeId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(nodes_, [&](const NodeInfo& n) { return n.id == id; });
}

std::optional<NodeInfo> RoutingTable::find(const NodeId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& node : nodes_) {
        if (node.id == id) {
            return node;
        }
    }
    return std::nullopt;
}

std::vector<NodeInfo> RoutingTable::all_nodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_;
}

std::vector<NodeInfo> RoutingTable::closest_to(const crypto::Hash& key, size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NodeInfo> sorted = nodes_;

    NodeId target;
    target.id = key;

    std::sort(sorted.begin(), sorted.end(), [&target](const NodeInfo& a, const NodeInfo& b) {
        return a.id.distance_to(target) < b.id.distance_to(target);
    });

    if (sorted.size() > count) {
        sorted.resize(count);
    }

    return sorted;
}

size_t RoutingTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_.size();
}

void RoutingTable::evict_older_than(std::chrono::steady_clock::time_point cutoff) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(nodes_, [&](const NodeInfo& n) { return n.last_seen < cutoff; });
}

} // namespace helix::kademlia
