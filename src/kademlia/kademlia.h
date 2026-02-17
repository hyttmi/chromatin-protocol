#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "crypto/crypto.h"
#include "kademlia/node_id.h"
#include "kademlia/routing_table.h"
#include "kademlia/udp_transport.h"
#include "storage/storage.h"

namespace helix::kademlia {

class Kademlia {
public:
    Kademlia(NodeInfo self, UdpTransport& transport, RoutingTable& table,
             storage::Storage& storage, const crypto::KeyPair& keypair);

    // Join network via bootstrap nodes
    void bootstrap(const std::vector<std::pair<std::string, uint16_t>>& addrs);

    // Handle incoming UDP message (called from recv loop)
    void handle_message(const Message& msg, const std::string& from_addr, uint16_t from_port);

    // High-level operations
    bool store(const crypto::Hash& key, uint8_t data_type, std::span<const uint8_t> value);
    std::optional<std::vector<uint8_t>> find_value(const crypto::Hash& key);

    // Responsibility
    bool is_responsible(const crypto::Hash& key) const;
    std::vector<NodeInfo> responsible_nodes(const crypto::Hash& key) const;
    size_t replication_factor() const;

    const NodeInfo& self() const { return self_; }

    // Configurable PoW difficulty for name registration (default 28 per spec).
    // Lower values are useful for testing.
    void set_name_pow_difficulty(int bits) { name_pow_difficulty_ = bits; }
    int name_pow_difficulty() const { return name_pow_difficulty_; }

private:
    NodeInfo self_;
    UdpTransport& transport_;
    RoutingTable& table_;
    storage::Storage& storage_;
    crypto::KeyPair keypair_;
    int name_pow_difficulty_ = 28;

    // Message handlers
    void handle_ping(const Message& msg, const std::string& from, uint16_t port);
    void handle_pong(const Message& msg, const std::string& from, uint16_t port);
    void handle_find_node(const Message& msg, const std::string& from, uint16_t port);
    void handle_nodes(const Message& msg, const std::string& from, uint16_t port);
    void handle_store(const Message& msg, const std::string& from, uint16_t port);
    void handle_find_value(const Message& msg, const std::string& from, uint16_t port);
    void handle_value(const Message& msg, const std::string& from, uint16_t port);

    Message make_message(MessageType type, const std::vector<uint8_t>& payload);
    void send_to_node(const NodeInfo& node, const Message& msg);

    // Name registration validation (PROTOCOL-SPEC.md section 5)
    bool validate_name_record(std::span<const uint8_t> value, const crypto::Hash& key);
};

} // namespace helix::kademlia
