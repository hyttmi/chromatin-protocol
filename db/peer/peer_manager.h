#pragma once

#include "db/acl/access_control.h"
#include "db/config/config.h"
#include "db/crypto/hash.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/net/connection.h"
#include "db/net/server.h"
#include "db/storage/storage.h"
#include "db/sync/sync_protocol.h"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace chromatindb::peer {

/// A persisted peer address with connection tracking.
struct PersistedPeer {
    std::string address;
    uint64_t last_seen = 0;     // Unix timestamp
    uint32_t fail_count = 0;
    std::string pubkey_hash;    // SHA3-256(pubkey) hex string, empty = unknown
};

/// A sync message received from a peer, queued for processing.
struct SyncMessage {
    wire::TransportMsgType type;
    std::vector<uint8_t> payload;
};

/// Info about a connected peer.
struct PeerInfo {
    net::Connection::Ptr connection;
    std::string address;
    bool is_bootstrap = false;
    uint32_t strike_count = 0;
    bool syncing = false;
    // Sync message queue (timer-cancel pattern)
    std::deque<SyncMessage> sync_inbox;
    asio::steady_timer* sync_notify = nullptr;
    // Pub/Sub subscriptions (connection-scoped)
    std::set<std::array<uint8_t, 32>> subscribed_namespaces;
    // Phase 16: Storage capacity signaling (resets on reconnect via PeerInfo recreation)
    bool peer_is_full = false;
    // Phase 18: Token bucket rate limiting (resets on reconnect via PeerInfo recreation)
    uint64_t bucket_tokens = 0;        // Available throughput tokens (bytes)
    uint64_t bucket_last_refill = 0;   // steady_clock milliseconds since epoch
    uint64_t last_sync_initiated = 0;  // steady_clock ms since epoch (0 = never synced as responder)
};

/// Runtime metrics counters. Plain uint64_t (single io_context thread, no races).
/// Monotonically increasing since startup (never reset).
struct NodeMetrics {
    uint64_t ingests = 0;                  // Successful blob ingestions
    uint64_t rejections = 0;               // Failed ingestions (validation errors)
    uint64_t syncs = 0;                    // Completed sync rounds
    uint64_t rate_limited = 0;             // Rate limit disconnections
    uint64_t peers_connected_total = 0;    // Total peer connections since startup
    uint64_t peers_disconnected_total = 0; // Total peer disconnections since startup
    uint64_t cursor_hits = 0;             // Namespaces skipped via cursor match
    uint64_t cursor_misses = 0;           // Namespaces requiring full hash diff
    uint64_t full_resyncs = 0;            // Full resync rounds triggered
    uint64_t quota_rejections = 0;        // Namespace quota exceeded rejections
    uint64_t sync_rejections = 0;          // Sync rate limit rejections (cooldown + session + byte rate)
};

/// Manages peer connections, sync scheduling, and connection policies.
///
/// Wraps Server and adds:
/// - Connection limit enforcement (max_peers)
/// - Bootstrap vs non-bootstrap peer tracking
/// - Sync-on-connect and periodic sync timer
/// - Strike system for misbehaving peers
///
/// Thread safety: NOT thread-safe. Runs on single io_context thread.
class PeerManager {
public:
    PeerManager(const config::Config& config,
                identity::NodeIdentity& identity,
                engine::BlobEngine& engine,
                storage::Storage& storage,
                asio::io_context& ioc,
                asio::thread_pool& pool,
                acl::AccessControl& acl,
                const std::filesystem::path& config_path = {});

    /// Start the server and sync timer.
    void start();

    /// Stop the server and cancel timers.
    void stop();

    /// Number of currently connected peers.
    size_t peer_count() const;

    /// Exit code from Server: 0 = clean shutdown, 1 = forced/timeout.
    int exit_code() const;

    /// Number of connected bootstrap peers.
    size_t bootstrap_peer_count() const;

    /// Access metrics (for testing and metrics output).
    const NodeMetrics& metrics() const { return metrics_; }

    /// Sync constants (public for testing).
    static constexpr uint32_t MAX_HASHES_PER_REQUEST = 64;  ///< Max hashes per BlobRequest
    static constexpr auto BLOB_TRANSFER_TIMEOUT = std::chrono::seconds(120);  ///< Per-blob timeout

    /// Strike threshold (public for testing).
    static constexpr uint32_t STRIKE_THRESHOLD = 10;
    static constexpr uint32_t STRIKE_COOLDOWN_SEC = 300;  // 5 minutes

    /// PEX constants (public for testing).
    static constexpr uint32_t PEX_INTERVAL_SEC = 300;        // 5 minutes
    static constexpr uint32_t MAX_PEERS_PER_EXCHANGE = 8;    // Max peers to share per response
    static constexpr uint32_t MAX_DISCOVERED_PER_ROUND = 3;  // Max new peers to connect per round
    static constexpr uint32_t MAX_PERSISTED_PEERS = 100;     // Max entries in peers.json
    static constexpr uint32_t MAX_PERSIST_FAILURES = 3;      // Prune after N consecutive startup failures

    /// PEX wire encoding (public for testing).
    static std::vector<uint8_t> encode_peer_list(const std::vector<std::string>& addresses);
    static std::vector<std::string> decode_peer_list(std::span<const uint8_t> payload);

    /// Pub/Sub wire encoding (public for testing).
    /// Encode a list of 32-byte namespace IDs for Subscribe/Unsubscribe payload.
    /// Format: [uint16_be count][ns_id:32][ns_id:32]...
    static std::vector<uint8_t> encode_namespace_list(
        const std::vector<std::array<uint8_t, 32>>& namespaces);

    /// Decode Subscribe/Unsubscribe payload back to namespace ID list.
    static std::vector<std::array<uint8_t, 32>> decode_namespace_list(
        std::span<const uint8_t> payload);

    /// Encode a notification payload.
    /// Format: [namespace_id:32][blob_hash:32][seq_num_be:8][blob_size_be:4][is_tombstone:1]
    static std::vector<uint8_t> encode_notification(
        std::span<const uint8_t, 32> namespace_id,
        std::span<const uint8_t, 32> blob_hash,
        uint64_t seq_num,
        uint32_t blob_size,
        bool is_tombstone);

    /// Reload allowed_keys from config file and disconnect revoked peers.
    /// Public for testing; called internally by SIGHUP handler.
    void reload_config();

    /// Check if an IP address is trusted (localhost or in trusted_peers).
    /// Passed to Connection as trust-check function.
    bool is_trusted_address(const asio::ip::address& addr) const;

    /// Callback type for notification dispatch (public for testing).
    using NotificationCallback = std::function<void(
        const std::array<uint8_t, 32>& namespace_id,
        const std::array<uint8_t, 32>& blob_hash,
        uint64_t seq_num,
        uint32_t blob_size,
        bool is_tombstone)>;

    /// Set a callback invoked whenever a notification is dispatched.
    /// For testing only -- allows capturing notification events.
    void set_on_notification(NotificationCallback cb) { on_notification_ = std::move(cb); }

private:
    // Server callbacks
    void on_peer_connected(net::Connection::Ptr conn);
    void on_peer_disconnected(net::Connection::Ptr conn);
    bool should_accept_connection();

    // Message routing
    void on_peer_message(net::Connection::Ptr conn,
                         wire::TransportMsgType type,
                         std::vector<uint8_t> payload);

    // Sync orchestration
    asio::awaitable<void> run_sync_with_peer(net::Connection::Ptr conn);
    asio::awaitable<void> sync_all_peers();
    asio::awaitable<void> sync_timer_loop();

    // Sync message handling (responder side)
    asio::awaitable<void> handle_sync_as_responder(net::Connection::Ptr conn);

    // Sync message queue
    void route_sync_message(PeerInfo* peer, wire::TransportMsgType type, std::vector<uint8_t> payload);
    asio::awaitable<std::optional<SyncMessage>> recv_sync_msg(PeerInfo* peer, std::chrono::seconds timeout);

    // Periodic peer list flush
    asio::awaitable<void> peer_flush_timer_loop();

    // PEX protocol
    asio::awaitable<void> pex_timer_loop();
    asio::awaitable<void> request_peers_from_all();
    asio::awaitable<void> run_pex_with_peer(net::Connection::Ptr conn);
    asio::awaitable<void> handle_pex_as_responder(net::Connection::Ptr conn);
    void handle_peer_list_response(net::Connection::Ptr conn, std::vector<uint8_t> payload);
    std::vector<std::string> build_peer_list_response(const std::string& exclude_address);

    // Peer persistence
    void load_persisted_peers();
    void save_persisted_peers();
    void update_persisted_peer(const std::string& address, bool success);
    std::filesystem::path peers_file_path() const;

    // Strike system
    void record_strike(net::Connection::Ptr conn, const std::string& reason);

    // SIGHUP config reload
    void setup_sighup_handler();
    asio::awaitable<void> sighup_loop();
    void handle_sighup();
    void disconnect_unauthorized_peers();

    // SIGUSR1 metrics dump
    void setup_sigusr1_handler();
    asio::awaitable<void> sigusr1_loop();
    void dump_metrics();

    // Periodic metrics
    asio::awaitable<void> metrics_timer_loop();
    void log_metrics_line();

    // Helpers
    uint64_t compute_uptime_seconds() const;

    // Expiry scanning (cancellable member coroutine)
    asio::awaitable<void> expiry_scan_loop();

    // Pub/Sub notification dispatch
    /// Notify all peers subscribed to a namespace about a new blob.
    /// Fires co_spawn per subscriber -- async, does not block caller.
    void notify_subscribers(
        const std::array<uint8_t, 32>& namespace_id,
        const std::array<uint8_t, 32>& blob_hash,
        uint64_t seq_num,
        uint32_t blob_size,
        bool is_tombstone);

    // Cursor-aware sync helpers
    enum class FullResyncReason { None, Periodic, TimeGap };
    FullResyncReason check_full_resync(
        const storage::SyncCursor& cursor, uint64_t now) const;

    // Helpers
    PeerInfo* find_peer(const net::Connection::Ptr& conn);
    std::string peer_display_name(const net::Connection::Ptr& conn);

    const config::Config& config_;
    identity::NodeIdentity& identity_;
    engine::BlobEngine& engine_;
    storage::Storage& storage_;
    asio::io_context& ioc_;
    asio::thread_pool& pool_;
    acl::AccessControl& acl_;

    net::Server server_;
    sync::SyncProtocol sync_proto_;
    asio::signal_set sighup_signal_;
    asio::signal_set sigusr1_signal_;
    std::filesystem::path config_path_;

    std::deque<PeerInfo> peers_;
    std::set<std::string> bootstrap_addresses_;
    std::set<std::string> known_addresses_;      // All addresses we know about
    std::set<std::string> trusted_peers_;         // IP strings for transport trust
    std::vector<PersistedPeer> persisted_peers_;  // Peers persisted to disk
    bool stopping_ = false;
    asio::steady_timer* expiry_timer_ = nullptr;  // Timer-cancel pattern for expiry scan
    asio::steady_timer* sync_timer_ = nullptr;    // Timer-cancel pattern for sync loop
    asio::steady_timer* pex_timer_ = nullptr;     // Timer-cancel pattern for PEX loop
    asio::steady_timer* flush_timer_ = nullptr;   // Timer-cancel pattern for peer flush loop
    asio::steady_timer* metrics_timer_ = nullptr;  // Timer-cancel pattern for metrics loop
    uint64_t rate_limit_bytes_per_sec_ = 0;       // 0 = disabled (Phase 18)
    uint64_t rate_limit_burst_ = 0;               // Burst capacity in bytes (Phase 18)
    uint32_t full_resync_interval_ = 10;          // Full resync every Nth round (Phase 34)
    uint64_t cursor_stale_seconds_ = 3600;        // Force full resync after gap (Phase 34)
    std::set<std::array<uint8_t, 32>> sync_namespaces_;  // Empty = replicate all
    NotificationCallback on_notification_;        // Test hook for notification dispatch
    NodeMetrics metrics_;
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace chromatindb::peer
