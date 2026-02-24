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

// Capability bits advertised in PONG payload.
enum class Capability : uint32_t {
    GROUPS       = 1 << 0,   // supports group messaging (data_type 0x05, 0x06)
    ENCRYPTED_TCP = 1 << 1,  // supports ML-KEM + ChaCha20 TCP encryption
};

struct NodeInfo {
    NodeId id;
    std::string address;           // routable address (may be self-reported)
    std::string tcp_source_ip;     // actual TCP connection source IP (for subnet checks)
    uint16_t tcp_port = 0;
    uint16_t ws_port = 0;
    std::vector<uint8_t> pubkey;
    std::chrono::steady_clock::time_point last_seen;
    // Version/capability negotiation (populated from PONG payload)
    uint8_t proto_version_min = 0;
    uint8_t proto_version_max = 0;
    uint32_t capabilities = 0;
};

class RoutingTable {
public:
    explicit RoutingTable(size_t max_nodes = 256, size_t max_per_subnet = 3)
        : max_nodes_(max_nodes), max_per_subnet_(max_per_subnet) {}

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
    size_t max_per_subnet_;
    mutable std::mutex mutex_;
    std::vector<NodeInfo> nodes_;

    // Extract /24 subnet for IPv4 ("192.168.1") or /48 prefix for IPv6
    static std::string extract_subnet(const std::string& address);
};

} // namespace chromatin::kademlia
