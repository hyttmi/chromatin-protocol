#include "kademlia/routing_table.h"

#include <algorithm>

namespace chromatin::kademlia {

std::string RoutingTable::extract_subnet(const std::string& address) {
    // IPv6: contains ':' - extract first 3 groups (/48 prefix)
    if (address.find(':') != std::string::npos) {
        size_t count = 0;
        for (size_t i = 0; i < address.size(); ++i) {
            if (address[i] == ':') {
                ++count;
                if (count == 3) return address.substr(0, i);
            }
        }
        return address;  // fewer than 3 colons — return full address
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
            if (!info.tcp_source_ip.empty()) {
                existing.tcp_source_ip = std::move(info.tcp_source_ip);
            }
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
            // Only update version/capabilities if non-zero (from PONG payload)
            if (info.proto_version_min != 0 || info.proto_version_max != 0) {
                existing.proto_version_min = info.proto_version_min;
                existing.proto_version_max = info.proto_version_max;
                existing.capabilities = info.capabilities;
                existing.app_version_major = info.app_version_major;
                existing.app_version_minor = info.app_version_minor;
                existing.app_version_patch = info.app_version_patch;
            }
            return;
        }
    }

    // Subnet diversity check: reject if too many nodes from same /24 (IPv4)
    // or /48 (IPv6) subnet to prevent Sybil/eclipse attacks.
    // Use tcp_source_ip (actual TCP connection source) when available,
    // falling back to address for test-constructed or NODES-response entries.
    if (max_per_subnet_ > 0) {
        std::string check_ip = info.tcp_source_ip.empty() ? info.address : info.tcp_source_ip;
        std::string new_subnet = extract_subnet(check_ip);
        size_t same_subnet = 0;
        for (const auto& node : nodes_) {
            std::string node_ip = node.tcp_source_ip.empty() ? node.address : node.tcp_source_ip;
            if (extract_subnet(node_ip) == new_subnet) {
                ++same_subnet;
            }
        }
        if (same_subnet >= max_per_subnet_) {
            return;  // silently reject
        }
    }

    // If full, only evict a stale node (not seen recently).
    // This prevents attackers from gradually replacing healthy nodes.
    if (nodes_.size() >= max_nodes_) {
        auto stale_cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(60);
        auto oldest = std::min_element(nodes_.begin(), nodes_.end(),
            [](const auto& a, const auto& b) { return a.last_seen < b.last_seen; });
        if (oldest != nodes_.end() && oldest->last_seen < stale_cutoff) {
            *oldest = std::move(info);
        }
        // else: all nodes are fresh, reject new node (prefer established nodes)
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
