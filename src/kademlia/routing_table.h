#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "crypto/crypto.h"
#include "kademlia/node_id.h"

namespace chromatin::kademlia {

struct NodeInfo {
    NodeId id;
    std::string address;
    uint16_t tcp_port = 0;
    uint16_t ws_port = 0;
    std::vector<uint8_t> pubkey;
    std::chrono::steady_clock::time_point last_seen;
};

class RoutingTable {
public:
    explicit RoutingTable(size_t max_nodes = 256) : max_nodes_(max_nodes) {}

    void add_or_update(NodeInfo info);
    void remove(const NodeId& id);
    std::optional<NodeInfo> find(const NodeId& id) const;
    std::vector<NodeInfo> all_nodes() const;
    std::vector<NodeInfo> closest_to(const crypto::Hash& key, size_t count) const;
    size_t size() const;
    void evict_older_than(std::chrono::steady_clock::time_point cutoff);

    size_t max_nodes() const { return max_nodes_; }

private:
    size_t max_nodes_;
    mutable std::mutex mutex_;
    std::vector<NodeInfo> nodes_;
};

} // namespace chromatin::kademlia
