#pragma once

#include "db/peer/peer_types.h"
#include "db/sync/sync_protocol.h"

#include <asio.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace chromatindb::engine { class BlobEngine; }
namespace chromatindb::storage { class Storage; }

namespace chromatindb::peer {

class ConnectionManager;

/// Owns sync protocol, expiry scanning, cursor compaction, and storage
/// compaction.  Extracted from PeerManager (Phase 96 ARCH-01, component D-03).
class SyncOrchestrator {
public:
    using StrikeCallback = std::function<void(net::Connection::Ptr, const std::string&)>;
    using OnBlobIngestedCallback = std::function<void(
        const std::array<uint8_t, 32>&, const std::array<uint8_t, 32>&,
        uint64_t, uint32_t, bool, uint64_t, net::Connection::Ptr)>;
    /// PEX callbacks for inline PEX after sync (both are awaitable, run before syncing=false).
    using PexRequestCallback = std::function<asio::awaitable<void>(net::Connection::Ptr)>;
    using PexRespondCallback = std::function<asio::awaitable<void>(net::Connection::Ptr)>;

    SyncOrchestrator(asio::io_context& ioc,
                     asio::thread_pool& pool,
                     engine::BlobEngine& engine,
                     storage::Storage& storage,
                     NodeMetrics& metrics,
                     const bool& stopping,
                     const std::set<std::array<uint8_t, 32>>& sync_namespaces,
                     std::deque<std::unique_ptr<PeerInfo>>& peers,
                     std::unordered_map<std::array<uint8_t, 32>,
                         DisconnectedPeerState, ArrayHash32>& disconnected_peers,
                     OnBlobIngestedCallback on_blob_ingested,
                     StrikeCallback record_strike,
                     PexRequestCallback pex_request,
                     PexRespondCallback pex_respond,
                     uint32_t blob_transfer_timeout,
                     uint32_t sync_timeout);

    // Sync protocol (D-03)
    asio::awaitable<void> run_sync_with_peer(net::Connection::Ptr conn);
    asio::awaitable<void> handle_sync_as_responder(net::Connection::Ptr conn);
    asio::awaitable<void> sync_all_peers();
    asio::awaitable<void> sync_timer_loop();

    // Sync message queue
    void route_sync_message(PeerInfo* peer, wire::TransportMsgType type,
                            std::vector<uint8_t> payload);
    asio::awaitable<std::optional<SyncMessage>> recv_sync_msg(
        const net::Connection::Ptr& conn, std::chrono::seconds timeout);

    // Sync rejection
    void send_sync_rejected(net::Connection::Ptr conn, uint8_t reason);

    // Maintenance loops
    asio::awaitable<void> expiry_scan_loop();
    asio::awaitable<void> cursor_compaction_loop();
    asio::awaitable<void> compaction_loop();

    // Expiry timer rearm (called from BlobPushManager via callback)
    void rearm_expiry_timer(uint64_t expiry_time);

    // Cancel all timers
    void cancel_timers();

    // Config reload
    void set_sync_config(uint32_t cooldown, uint32_t max_sessions,
                         uint32_t safety_net_interval);
    void set_rate_limit(uint64_t bytes_per_sec);
    void set_compaction_interval(uint32_t hours);
    void set_cursor_config(uint32_t full_resync_interval, uint64_t stale_seconds);
    void set_blob_transfer_timeout(uint32_t seconds) {
        blob_transfer_timeout_ = std::chrono::seconds(seconds);
    }
    void set_sync_timeout(uint32_t seconds) {
        sync_timeout_ = std::chrono::seconds(seconds);
    }

    // Public for testing
    enum class FullResyncReason { None, Periodic, TimeGap };
    FullResyncReason check_full_resync(const storage::SyncCursor& cursor,
                                       uint64_t now) const;

    // Access for facade
    sync::SyncProtocol& sync_proto() { return sync_proto_; }
    uint32_t sync_cooldown_seconds() const { return sync_cooldown_seconds_; }

    // Sync constants (public for testing)
    static constexpr uint32_t MAX_HASHES_PER_REQUEST = 64;

    // State access for facade (dump_extra callback)
    uint64_t last_compaction_time() const { return last_compaction_time_; }
    uint64_t compaction_count() const { return compaction_count_; }

    // Expiry state
    uint64_t next_expiry_target() const { return next_expiry_target_; }
    bool expiry_loop_running() const { return expiry_loop_running_; }

    // Compaction interval for reload detection
    uint32_t compaction_interval_hours() const { return compaction_interval_hours_; }

private:
    PeerInfo* find_peer(const net::Connection::Ptr& conn);
    std::string peer_display_name(const net::Connection::Ptr& conn);

    asio::io_context& ioc_;
    asio::thread_pool& pool_;
    engine::BlobEngine& engine_;
    storage::Storage& storage_;
    NodeMetrics& metrics_;
    const bool& stopping_;
    const std::set<std::array<uint8_t, 32>>& sync_namespaces_;
    std::deque<std::unique_ptr<PeerInfo>>& peers_;
    std::unordered_map<std::array<uint8_t, 32>, DisconnectedPeerState, ArrayHash32>&
        disconnected_peers_;
    OnBlobIngestedCallback on_blob_ingested_;
    StrikeCallback record_strike_;
    PexRequestCallback pex_request_;
    PexRespondCallback pex_respond_;

    sync::SyncProtocol sync_proto_;

    asio::steady_timer* sync_timer_ = nullptr;
    asio::steady_timer* expiry_timer_ = nullptr;
    asio::steady_timer* cursor_compaction_timer_ = nullptr;
    asio::steady_timer* compaction_timer_ = nullptr;
    uint64_t next_expiry_target_ = 0;
    bool expiry_loop_running_ = false;
    uint32_t sync_cooldown_seconds_;
    uint32_t safety_net_interval_seconds_;
    uint32_t max_sync_sessions_;
    uint32_t full_resync_interval_;
    uint64_t cursor_stale_seconds_;
    uint32_t compaction_interval_hours_;
    uint64_t rate_limit_bytes_per_sec_ = 0;
    uint64_t last_compaction_time_ = 0;
    uint64_t compaction_count_ = 0;
    std::chrono::seconds blob_transfer_timeout_{std::chrono::seconds(600)};
    std::chrono::seconds sync_timeout_{std::chrono::seconds(30)};
};

} // namespace chromatindb::peer
