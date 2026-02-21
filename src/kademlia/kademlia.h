#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config/config.h"
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

// Data types stored in the DHT (matches PROTOCOL-SPEC.md wire format).
enum class DataType : uint8_t {
    PROFILE         = 0x00,
    NAME            = 0x01,
    INBOX           = 0x02,
    CONTACT_REQUEST = 0x03,
    ALLOWLIST       = 0x04,
};

class Kademlia {
public:
    Kademlia(const config::Config& cfg, NodeInfo self, TcpTransport& transport,
             RoutingTable& table, storage::Storage& storage,
             replication::ReplLog& repl_log, const crypto::KeyPair& keypair);

    // Join network via bootstrap nodes
    void bootstrap(const std::vector<std::pair<std::string, uint16_t>>& addrs);

    // Handle incoming message (called from TCP accept loop)
    void handle_message(const Message& msg, const std::string& from_addr, uint16_t from_port);

    // High-level operations
    bool store(const crypto::Hash& key, uint8_t data_type, std::span<const uint8_t> value);
    void delete_value(const crypto::Hash& key, uint8_t data_type, std::span<const uint8_t> delete_info);
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
    size_t routing_table_size() const { return table_.size(); }

    // Callback invoked after every successful local store (both handle_store
    // and the local-storage path inside store()).  Used by the WebSocket
    // server to push real-time notifications to connected clients.
    using StoreCallback = std::function<void(const crypto::Hash& key,
                                             uint8_t data_type,
                                             std::span<const uint8_t> value)>;
    void set_on_store(StoreCallback cb);

    // Configurable PoW difficulty for name registration (default 28 per spec).
    // Lower values are useful for testing.
    void set_name_pow_difficulty(int bits) { name_pow_difficulty_ = bits; }
    int name_pow_difficulty() const { return name_pow_difficulty_; }

    // Write quorum: W = min(2, R). A store is "durably replicated" when W
    // nodes (including self if responsible) have confirmed.
    size_t write_quorum() const;

    // Query replication status for a key. Returns nullopt if no pending store.
    std::optional<PendingStore> pending_store_status(const crypto::Hash& key) const;

    // Query remote nodes for their repl_log seq for a given key.
    // Sends SEQ_REQ to each node, waits up to timeout for SEQ_RESP.
    // Returns pairs of (node, seq) sorted by seq descending.
    struct NodeSeq { NodeInfo node; uint64_t seq; };
    std::vector<NodeSeq> query_remote_seqs(
        const crypto::Hash& key,
        const std::vector<NodeInfo>& nodes,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

private:
    const config::Config& cfg_;
    NodeInfo self_;
    TcpTransport& transport_;
    RoutingTable& table_;
    storage::Storage& storage_;
    replication::ReplLog& repl_log_;
    const crypto::KeyPair& keypair_;
    int name_pow_difficulty_ = 28;
    int contact_pow_difficulty_ = 16;
    StoreCallback on_store_;

    // Timing intervals (from config)
    std::chrono::seconds refresh_interval_{30};
    std::chrono::seconds refresh_interval_sparse_{5};
    std::chrono::seconds ping_sweep_interval_{10};
    std::chrono::seconds stale_threshold_{60};
    std::chrono::seconds evict_threshold_{120};
    std::chrono::hours ttl_duration_{7 * 24};
    std::chrono::minutes ttl_sweep_interval_{5};
    std::chrono::seconds pending_store_timeout_{30};
    std::chrono::seconds transfer_check_interval_{60};
    std::chrono::minutes compact_interval_{60};
    size_t compact_keep_entries_ = 10000;
    size_t replication_factor_ = 3;
    uint32_t max_profile_size_ = 1024 * 1024;
    uint32_t max_request_blob_size_ = 64 * 1024;

    // Self-healing: saved bootstrap addresses and timer state
    std::vector<std::pair<std::string, uint16_t>> bootstrap_addrs_;
    std::chrono::steady_clock::time_point last_refresh_{};
    std::chrono::steady_clock::time_point last_ping_sweep_{};
    std::chrono::steady_clock::time_point last_ttl_sweep_{};
    std::chrono::steady_clock::time_point last_compact_{};
    std::chrono::steady_clock::time_point last_sync_{};
    std::chrono::seconds sync_interval_{120};
    size_t sync_batch_size_ = 10;
    size_t sync_key_offset_ = 0;  // round-robin offset for batching

    // TTL expiry, pending store cleanup, responsibility transfer, compaction, sync
    void expire_ttl();
    void cleanup_pending_stores();
    void transfer_responsibility();
    void compact_repl_log();
    void sync_with_peers();
    size_t last_table_size_ = 0;
    std::chrono::steady_clock::time_point last_transfer_check_{};
    std::unordered_set<NodeId, NodeIdHash> prev_routing_nodes_;  // for transfer dedup

    // Pending STORE quorum tracking (key -> status)
    mutable std::mutex pending_mutex_;
    std::unordered_map<crypto::Hash, PendingStore, crypto::HashHash> pending_stores_;

    // Pending SEQ_RESP tracking for query_remote_seqs()
    struct PendingSeqQuery {
        crypto::Hash key;
        std::unordered_map<NodeId, uint64_t, NodeIdHash> responses;
        size_t expected = 0;
    };
    mutable std::mutex seq_query_mutex_;
    std::condition_variable seq_query_cv_;
    std::unordered_map<crypto::Hash, PendingSeqQuery, crypto::HashHash> pending_seq_queries_;

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
    void handle_seq_req(const Message& msg, const std::string& from, uint16_t port);
    void handle_seq_resp(const Message& msg, const std::string& from, uint16_t port);

    Message make_message(MessageType type, const std::vector<uint8_t>& payload);
    std::vector<uint8_t> make_find_node_payload() const;
    void send_to_node(const NodeInfo& node, const Message& msg);

    // Unified local storage: validates, dispatches to the correct table(s),
    // optionally appends to repl_log and fires on_store_ callback.
    // When log_and_notify is false (used by SYNC), repl_log append and
    // on_store_ callback are skipped (SYNC handles repl_log via apply()).
    bool store_locally(const crypto::Hash& key, uint8_t data_type,
                       std::span<const uint8_t> value,
                       bool validate = true, bool log_and_notify = true);

    // Data type validators (PROTOCOL-SPEC.md)
    bool validate_name_record(std::span<const uint8_t> value, const crypto::Hash& key);
    bool validate_profile(std::span<const uint8_t> value, const crypto::Hash& key);
    bool validate_inbox_message(std::span<const uint8_t> value);
    bool validate_contact_request(std::span<const uint8_t> value);
    bool validate_allowlist_entry(std::span<const uint8_t> value);
};

} // namespace chromatin::kademlia
