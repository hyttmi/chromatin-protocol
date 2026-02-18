#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "crypto/crypto.h"
#include "kademlia/node_id.h"
#include "kademlia/routing_table.h"
#include "kademlia/tcp_transport.h"
#include "replication/repl_log.h"
#include "storage/storage.h"

namespace chromatin::kademlia {

// Tracks replication status for a pending STORE operation.
struct PendingStore {
    size_t expected;     // number of remote nodes we sent STORE to
    size_t acked;        // number of STORE_ACKs received
    bool local_stored;   // whether we stored locally (counts toward quorum)
    std::chrono::steady_clock::time_point created;
};

class Kademlia {
public:
    Kademlia(NodeInfo self, TcpTransport& transport, RoutingTable& table,
             storage::Storage& storage, replication::ReplLog& repl_log,
             const crypto::KeyPair& keypair);

    // Join network via bootstrap nodes
    void bootstrap(const std::vector<std::pair<std::string, uint16_t>>& addrs);

    // Handle incoming message (called from TCP accept loop)
    void handle_message(const Message& msg, const std::string& from_addr, uint16_t from_port);

    // High-level operations
    bool store(const crypto::Hash& key, uint8_t data_type, std::span<const uint8_t> value);
    std::optional<std::vector<uint8_t>> find_value(const crypto::Hash& key);

    // Responsibility
    bool is_responsible(const crypto::Hash& key) const;
    std::vector<NodeInfo> responsible_nodes(const crypto::Hash& key) const;
    size_t replication_factor() const;

    // Periodic maintenance — call from main loop (~200ms interval)
    void tick();

    // Save bootstrap addresses for periodic re-bootstrap
    void set_bootstrap_addrs(std::vector<std::pair<std::string, uint16_t>> addrs);

    const NodeInfo& self() const { return self_; }

    // Configurable PoW difficulty for name registration (default 28 per spec).
    // Lower values are useful for testing.
    void set_name_pow_difficulty(int bits) { name_pow_difficulty_ = bits; }
    int name_pow_difficulty() const { return name_pow_difficulty_; }

    // Write quorum: W = min(2, R). A store is "durably replicated" when W
    // nodes (including self if responsible) have confirmed.
    size_t write_quorum() const;

    // Query replication status for a key. Returns nullopt if no pending store.
    std::optional<PendingStore> pending_store_status(const crypto::Hash& key) const;

private:
    NodeInfo self_;
    TcpTransport& transport_;
    RoutingTable& table_;
    storage::Storage& storage_;
    replication::ReplLog& repl_log_;
    crypto::KeyPair keypair_;
    int name_pow_difficulty_ = 28;

    // Self-healing: saved bootstrap addresses and timer state
    std::vector<std::pair<std::string, uint16_t>> bootstrap_addrs_;
    std::chrono::steady_clock::time_point last_refresh_{};
    std::chrono::steady_clock::time_point last_ping_sweep_{};

    // Pending STORE quorum tracking (key -> status)
    mutable std::mutex pending_mutex_;
    std::unordered_map<crypto::Hash, PendingStore, crypto::HashHash> pending_stores_;

    // Message handlers
    void handle_ping(const Message& msg, const std::string& from, uint16_t port);
    void handle_pong(const Message& msg, const std::string& from, uint16_t port);
    void handle_find_node(const Message& msg, const std::string& from, uint16_t port);
    void handle_nodes(const Message& msg, const std::string& from, uint16_t port);
    void handle_store(const Message& msg, const std::string& from, uint16_t port);
    void handle_find_value(const Message& msg, const std::string& from, uint16_t port);
    void handle_value(const Message& msg, const std::string& from, uint16_t port);
    void handle_sync_req(const Message& msg, const std::string& from, uint16_t port);
    void handle_sync_resp(const Message& msg, const std::string& from, uint16_t port);
    void handle_store_ack(const Message& msg, const std::string& from, uint16_t port);

    Message make_message(MessageType type, const std::vector<uint8_t>& payload);
    void send_to_node(const NodeInfo& node, const Message& msg);

    // Name registration validation (PROTOCOL-SPEC.md section 5)
    bool validate_name_record(std::span<const uint8_t> value, const crypto::Hash& key);
};

} // namespace chromatin::kademlia
