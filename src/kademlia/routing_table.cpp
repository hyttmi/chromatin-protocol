#include "kademlia/routing_table.h"

#include <algorithm>

namespace chromatin::kademlia {

std::string RoutingTable::extract_subnet(const std::string& address) {
    // IPv6: contains ':' - extract first 3 groups (48-bit prefix)
    if (address.find(':') != std::string::npos) {
        size_t count = 0;
        size_t pos = 0;
        for (size_t i = 0; i < address.size() && count < 3; ++i) {
            if (address[i] == ':') ++count;
            if (count < 3) pos = i + 1;
        }
        return address.substr(0, pos);
    }
    // IPv4: extract first 3 octets (e.g., "192.168.1")
    size_t count = 0;
    for (size_t i = 0; i < address.size(); ++i) {
        if (address[i] == '.') {
            ++count;
            if (count == 3) {
                return address.substr(0, i);
            }
        }
    }
    return address;  // fallback: treat whole address as subnet
}

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

    // Subnet diversity check: reject if too many nodes from same /24 (IPv4)
    // or /48 (IPv6) subnet to prevent Sybil/eclipse attacks.
    if (max_per_subnet_ > 0) {
        std::string new_subnet = extract_subnet(info.address);
        size_t same_subnet = 0;
        for (const auto& node : nodes_) {
            if (extract_subnet(node.address) == new_subnet) {
                ++same_subnet;
            }
        }
        if (same_subnet >= max_per_subnet_) {
            return;  // silently reject
        }
    }

    // If full, evict the node with the oldest last_seen timestamp.
    if (nodes_.size() >= max_nodes_) {
        auto oldest = std::min_element(nodes_.begin(), nodes_.end(),
            [](const auto& a, const auto& b) { return a.last_seen < b.last_seen; });
        *oldest = std::move(info);
    } else {
        nodes_.push_back(std::move(info));
    }
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
    std::vector<NodeInfo> result = nodes_;
    count = std::min(count, result.size());

    NodeId target;
    target.id = key;

    std::partial_sort(result.begin(), result.begin() + count, result.end(),
        [&target](const auto& a, const auto& b) {
            return a.id.distance_to(target) < b.id.distance_to(target);
        });
    result.resize(count);
    return result;
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
