#pragma once

#include "db/acl/access_control.h"
#include "db/config/config.h"
#include "db/crypto/hash.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/net/connection.h"
#include "db/net/server.h"
#include "db/net/uds_acceptor.h"
#include "db/storage/storage.h"
#include "db/sync/sync_protocol.h"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
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
    uint64_t last_message_time = 0;   // steady_clock ms since epoch (0 = not yet set)
    // Phase 86: Peer's declared replication scope (empty = replicate all, per D-07)
    std::set<std::array<uint8_t, 32>> announced_namespaces;
    // Phase 86: Announce handshake coordination (timer-cancel pattern)
    bool announce_received = false;
    asio::steady_timer* announce_notify = nullptr;
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

/// Hash functor for 32-byte arrays (first 8 bytes as uint64_t -- sufficient entropy for blob hashes).
struct ArrayHash32 {
    size_t operator()(const std::array<uint8_t, 32>& arr) const noexcept {
        uint64_t h;
        std::memcpy(&h, arr.data(), sizeof(h));
        return static_cast<size_t>(h);
    }
};

/// Tracks when a peer disconnected for cursor grace period (Phase 82 MAINT-04).
/// Cursors persist in MDBX -- we only need the disconnect timestamp to decide
/// whether to reuse them (within 5 min) or discard them (after 5 min).
struct DisconnectedPeerState {
    uint64_t disconnect_time;  // steady_clock milliseconds since epoch
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

    /// Actual listening port (after bind). Useful when bind_address uses port 0.
    uint16_t listening_port() const { return server_.listening_port(); }

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

    /// Reload allowed_client_keys and allowed_peer_keys from config file and disconnect revoked peers.
    /// Public for testing; called internally by SIGHUP handler.
    void reload_config();

    /// Format current metrics as Prometheus text exposition format (public for testing).
    std::string prometheus_metrics_text();

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

    // Unified notification fan-out (Phase 79 PUSH-01/PUSH-07/PUSH-08)
    /// BlobNotify (type 59) to all TCP peers + Notification (type 21) to subscribed clients.
    /// Called after every successful ingest (Data, Delete, or sync).
    /// Also handles event-driven expiry timer rearm (MAINT-03).
    /// @param source Connection that originated the blob (nullptr for client writes via relay/UDS).
    void on_blob_ingested(
        const std::array<uint8_t, 32>& namespace_id,
        const std::array<uint8_t, 32>& blob_hash,
        uint64_t seq_num,
        uint32_t blob_size,
        bool is_tombstone,
        uint64_t expiry_time,
        net::Connection::Ptr source);

private:
    // Server callbacks
    void on_peer_connected(net::Connection::Ptr conn);
    void on_peer_disconnected(net::Connection::Ptr conn);

    // Phase 86: Announce exchange + optional sync-on-connect
    asio::awaitable<void> announce_and_sync(net::Connection::Ptr conn);
    bool should_accept_connection();

    // Message routing
    void on_peer_message(net::Connection::Ptr conn,
                         wire::TransportMsgType type,
                         std::vector<uint8_t> payload,
                         uint32_t request_id);

    // Sync orchestration
    asio::awaitable<void> run_sync_with_peer(net::Connection::Ptr conn);
    asio::awaitable<void> sync_all_peers();
    asio::awaitable<void> sync_timer_loop();

    // Sync message handling (responder side)
    asio::awaitable<void> handle_sync_as_responder(net::Connection::Ptr conn);

    // Sync message queue
    void route_sync_message(PeerInfo* peer, wire::TransportMsgType type, std::vector<uint8_t> payload);
    asio::awaitable<std::optional<SyncMessage>> recv_sync_msg(const net::Connection::Ptr& conn, std::chrono::seconds timeout);

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

    // Cursor compaction (prune cursors for disconnected peers)
    asio::awaitable<void> cursor_compaction_loop();

    // Keepalive: send Ping to TCP peers, disconnect silent ones (Phase 83)
    asio::awaitable<void> keepalive_loop();

    // Prometheus /metrics HTTP endpoint (Phase 90)
    asio::awaitable<void> metrics_accept_loop();
    asio::awaitable<void> metrics_handle_connection(asio::ip::tcp::socket socket);
    std::string format_prometheus_metrics();
    void start_metrics_listener();
    void stop_metrics_listener();

    // Helpers
    uint64_t compute_uptime_seconds() const;

    // Expiry scanning (cancellable member coroutine)
    asio::awaitable<void> expiry_scan_loop();

    // Storage compaction (periodic timer)
    asio::awaitable<void> compaction_loop();

    // Phase 80: Targeted blob fetch (PUSH-05, PUSH-06)
    /// Handle incoming BlobNotify: dedup check, send BlobFetch if needed.
    void on_blob_notify(net::Connection::Ptr conn, std::vector<uint8_t> payload);

    /// Handle incoming BlobFetch: look up blob, send BlobFetchResponse.
    void handle_blob_fetch(net::Connection::Ptr conn, std::vector<uint8_t> payload);

    /// Handle incoming BlobFetchResponse: ingest blob, clean pending set.
    void handle_blob_fetch_response(net::Connection::Ptr conn, std::vector<uint8_t> payload);

    // Cursor-aware sync helpers
    enum class FullResyncReason { None, Periodic, TimeGap };
    FullResyncReason check_full_resync(
        const storage::SyncCursor& cursor, uint64_t now) const;

    // Sync rate limiting (Phase 40)
    void send_sync_rejected(net::Connection::Ptr conn, uint8_t reason);

    /// Cancel all periodic timers. Called from stop() and on_shutdown.
    /// Must not throw (called from signal handler context in on_shutdown).
    void cancel_all_timers();

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
    std::unique_ptr<net::UdsAcceptor> uds_acceptor_;
    sync::SyncProtocol sync_proto_;
    asio::signal_set sighup_signal_;
    asio::signal_set sigusr1_signal_;
    std::filesystem::path config_path_;

    std::deque<std::unique_ptr<PeerInfo>> peers_;
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
    asio::steady_timer* cursor_compaction_timer_ = nullptr;  // Timer-cancel pattern for cursor compaction
    asio::steady_timer* keepalive_timer_ = nullptr;           // Timer-cancel pattern for keepalive loop
    asio::steady_timer* compaction_timer_ = nullptr;          // Timer-cancel pattern for storage compaction
    std::unique_ptr<asio::ip::tcp::acceptor> metrics_acceptor_;  // Prometheus /metrics HTTP acceptor
    std::string metrics_bind_;                                    // SIGHUP-reloadable (Phase 90)
    uint64_t rate_limit_bytes_per_sec_ = 0;       // 0 = disabled (Phase 18)
    uint64_t rate_limit_burst_ = 0;               // Burst capacity in bytes (Phase 18)
    uint32_t full_resync_interval_ = 10;          // Full resync every Nth round (Phase 34)
    uint64_t cursor_stale_seconds_ = 3600;        // Force full resync after gap (Phase 34)
    std::set<std::array<uint8_t, 32>> sync_namespaces_;  // Empty = replicate all
    uint32_t sync_cooldown_seconds_ = 30;         // SIGHUP-reloadable (Phase 40)
    uint32_t safety_net_interval_seconds_ = 600;  // SIGHUP-reloadable (Phase 82)
    uint32_t max_peers_ = 32;                     // SIGHUP-reloadable (Phase 86)
    uint32_t max_sync_sessions_ = 1;              // SIGHUP-reloadable (Phase 40)
    uint64_t next_expiry_target_ = 0;   // 0 = no timer armed, wall-clock seconds otherwise
    bool expiry_loop_running_ = false;   // Prevents double co_spawn of expiry_scan_loop
    uint32_t compaction_interval_hours_ = 6;      // SIGHUP-reloadable (Phase 55)
    uint64_t last_compaction_time_ = 0;           // Epoch seconds of last successful compaction
    uint64_t compaction_count_ = 0;               // Monotonic counter of successful compactions
    NotificationCallback on_notification_;        // Test hook for notification dispatch
    // Phase 80: Track in-flight BlobFetch requests for dedup (D-05)
    // Maps blob_hash -> connection that we sent the BlobFetch to.
    // Cleaned on: successful ingest (by hash), peer disconnect (by connection).
    std::unordered_map<std::array<uint8_t, 32>, net::Connection::Ptr, ArrayHash32> pending_fetches_;
    // Phase 82: Track disconnected peers for cursor grace period (MAINT-04)
    // Map: SHA3-256(peer_pubkey) -> disconnect timestamp
    // Cursors live in MDBX; this map only tracks WHEN they disconnected.
    std::unordered_map<std::array<uint8_t, 32>, DisconnectedPeerState, ArrayHash32>
        disconnected_peers_;
    static constexpr uint64_t CURSOR_GRACE_PERIOD_MS = 5 * 60 * 1000;  // 5 minutes
    NodeMetrics metrics_;
    std::chrono::steady_clock::time_point start_time_;
};

} // namespace chromatindb::peer
