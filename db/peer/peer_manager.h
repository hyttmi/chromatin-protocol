#pragma once

#include "db/peer/peer_types.h"
#include "db/peer/metrics_collector.h"
#include "db/peer/pex_manager.h"
#include "db/peer/connection_manager.h"
#include "db/peer/blob_push_manager.h"
#include "db/peer/sync_orchestrator.h"
#include "db/peer/message_dispatcher.h"

#include "db/acl/access_control.h"
#include "db/config/config.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/net/connection.h"
#include "db/net/server.h"
#include "db/net/uds_acceptor.h"
#include "db/storage/storage.h"

#include <asio.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace chromatindb::peer {

/// Manages peer connections, sync scheduling, and connection policies.
///
/// Thin facade owning 6 focused components:
/// - MetricsCollector: NodeMetrics, Prometheus /metrics, SIGUSR1 dump, periodic log
/// - ConnectionManager: peers_ deque, connection lifecycle, keepalive, strike
/// - SyncOrchestrator: sync protocol, expiry scanning, cursor/storage compaction
/// - PexManager: PEX protocol, peer persistence, known address tracking
/// - BlobPushManager: BlobNotify/BlobFetch protocol, on_blob_ingested fan-out
/// - MessageDispatcher: message routing switch, all query handlers
///
/// Thread safety: thread-confined to the io_context executor. All Storage
/// access is proxied through member functions on this thread; the Storage
/// layer enforces this at runtime via STORAGE_THREAD_CHECK() (debug builds).
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
    size_t peer_count() const { return conn_mgr_.peer_count(); }

    /// Exit code from Server: 0 = clean shutdown, 1 = forced/timeout.
    int exit_code() const;

    /// Actual listening port (after bind). Useful when bind_address uses port 0.
    uint16_t listening_port() const { return server_.listening_port(); }

    /// Number of connected bootstrap peers.
    size_t bootstrap_peer_count() const;

    /// Access metrics (for testing and metrics output).
    const NodeMetrics& metrics() const { return metrics_collector_.node_metrics(); }

    /// Sync constants (public for testing).
    static constexpr uint32_t MAX_HASHES_PER_REQUEST = 64;
    static constexpr auto BLOB_TRANSFER_TIMEOUT_DEFAULT = std::chrono::seconds(600);

    /// Strike defaults (public for testing).
    static constexpr uint32_t STRIKE_THRESHOLD_DEFAULT = 10;
    static constexpr uint32_t STRIKE_COOLDOWN_SEC_DEFAULT = 300;

    /// PEX defaults (public for testing).
    static constexpr uint32_t PEX_INTERVAL_SEC_DEFAULT = 300;
    static constexpr uint32_t MAX_PEERS_PER_EXCHANGE = 8;
    static constexpr uint32_t MAX_DISCOVERED_PER_ROUND = 3;
    static constexpr uint32_t MAX_PERSISTED_PEERS = 100;
    static constexpr uint32_t MAX_PERSIST_FAILURES = 3;

    /// PEX wire encoding (public for testing).
    static std::vector<uint8_t> encode_peer_list(const std::vector<std::string>& addresses);
    static std::vector<std::string> decode_peer_list(std::span<const uint8_t> payload);

    /// Pub/Sub wire encoding (public for testing).
    static std::vector<uint8_t> encode_namespace_list(
        const std::vector<std::array<uint8_t, 32>>& namespaces);
    static std::vector<std::array<uint8_t, 32>> decode_namespace_list(
        std::span<const uint8_t> payload);

    /// Encode a notification payload.
    static std::vector<uint8_t> encode_notification(
        std::span<const uint8_t, 32> namespace_id,
        std::span<const uint8_t, 32> blob_hash,
        uint64_t seq_num,
        uint32_t blob_size,
        bool is_tombstone);

    /// Reload allowed_client_keys and allowed_peer_keys from config file.
    void reload_config();

    /// Format current metrics as Prometheus text exposition format (public for testing).
    std::string prometheus_metrics_text();

    /// Check if an IP address is trusted.
    bool is_trusted_address(const asio::ip::address& addr) const {
        return conn_mgr_.is_trusted_address(addr);
    }

    /// Callback type for notification dispatch (public for testing).
    using NotificationCallback = BlobPushManager::NotificationCallback;

    /// Set a callback invoked whenever a notification is dispatched.
    void set_on_notification(NotificationCallback cb) { blob_push_.set_on_notification(std::move(cb)); }

    /// Unified notification fan-out.
    void on_blob_ingested(
        const std::array<uint8_t, 32>& namespace_id,
        const std::array<uint8_t, 32>& blob_hash,
        uint64_t seq_num,
        uint32_t blob_size,
        bool is_tombstone,
        uint64_t expiry_time,
        net::Connection::Ptr source);

private:
    // SIGHUP config reload
    void setup_sighup_handler();
    asio::awaitable<void> sighup_loop();
    void handle_sighup();

    // SIGUSR1 metrics dump
    void setup_sigusr1_handler();
    asio::awaitable<void> sigusr1_loop();

    /// Cancel all periodic timers.
    void cancel_all_timers();

    config::Config config_;
    identity::NodeIdentity& identity_;
    engine::BlobEngine& engine_;
    storage::Storage& storage_;
    asio::io_context& ioc_;
    asio::thread_pool& pool_;
    acl::AccessControl& acl_;

    net::Server server_;
    std::unique_ptr<net::UdsAcceptor> uds_acceptor_;
    asio::signal_set sighup_signal_;
    asio::signal_set sigusr1_signal_;
    std::filesystem::path config_path_;

    bool stopping_ = false;
    std::set<std::array<uint8_t, 32>> sync_namespaces_;

    // Component initialization order matters:
    // 1. metrics_collector_ (owns NodeMetrics)
    // 2. conn_mgr_ (owns peers_ deque)
    // 3. sync_ (references peers_ and disconnected_peers)
    // 4. pex_ (references peers_)
    // 5. blob_push_ (references peers_)
    // 6. dispatcher_ (references all components)
    MetricsCollector metrics_collector_;
    ConnectionManager conn_mgr_;
    SyncOrchestrator sync_;
    PexManager pex_;
    BlobPushManager blob_push_;
    MessageDispatcher dispatcher_;
};

} // namespace chromatindb::peer
