#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "crypto/crypto.h"
#include "kademlia/node_id.h"

namespace helix::kademlia {

struct NodeInfo {
    NodeId id;
    std::string address;
    uint16_t udp_port = 0;
    uint16_t ws_port = 0;
    std::vector<uint8_t> pubkey;
    std::chrono::steady_clock::time_point last_seen;
};

class RoutingTable {
public:
    void add_or_update(NodeInfo info);
    void remove(const NodeId& id);
    std::optional<NodeInfo> find(const NodeId& id) const;
    std::vector<NodeInfo> all_nodes() const;
    std::vector<NodeInfo> closest_to(const crypto::Hash& key, size_t count) const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<NodeInfo> nodes_;
};

} // namespace helix::kademlia
