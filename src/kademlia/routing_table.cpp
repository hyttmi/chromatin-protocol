#include "kademlia/routing_table.h"

#include <algorithm>

namespace chromatin::kademlia {

void RoutingTable::add_or_update(NodeInfo info) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& existing : nodes_) {
        if (existing.id == info.id) {
            existing.address = std::move(info.address);
            existing.tcp_port = info.tcp_port;
            existing.ws_port = info.ws_port;
            // Don't overwrite a verified pubkey with an empty one.
            // PING/FIND_NODE handlers add nodes without pubkeys; NODES
            // responses populate them later.  A subsequent PING/FIND_NODE
            // must not erase the already-verified pubkey.
            if (!info.pubkey.empty()) {
                existing.pubkey = std::move(info.pubkey);
            }
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

} // namespace chromatin::kademlia
