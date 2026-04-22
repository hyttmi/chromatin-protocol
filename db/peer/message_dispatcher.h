#pragma once

#include "db/peer/peer_types.h"

#include <asio.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace chromatindb::engine { class BlobEngine; }
namespace chromatindb::storage { class Storage; }

namespace chromatindb::peer {

class SyncOrchestrator;
class PexManager;
class BlobPushManager;
class ConnectionManager;

/// Routes incoming messages to the appropriate component.
/// Owns the dispatch switch and all inline query handlers.
/// Extracted from PeerManager (ARCH-01, component D-02).
class MessageDispatcher {
public:
    using StrikeCallback = std::function<void(net::Connection::Ptr, const std::string&)>;
    using OnBlobIngestedCallback = std::function<void(
        const std::array<uint8_t, 32>&, const std::array<uint8_t, 32>&,
        uint64_t, uint32_t, bool, uint64_t, net::Connection::Ptr)>;
    using UptimeCallback = std::function<uint64_t()>;
    using MaxStorageCallback = std::function<uint64_t()>;

    MessageDispatcher(asio::io_context& ioc,
                      asio::thread_pool& pool,
                      engine::BlobEngine& engine,
                      storage::Storage& storage,
                      NodeMetrics& metrics,
                      const bool& stopping,
                      const std::set<std::array<uint8_t, 32>>& sync_namespaces,
                      std::deque<std::unique_ptr<PeerInfo>>& peers,
                      SyncOrchestrator& sync,
                      PexManager& pex,
                      BlobPushManager& blob_push,
                      ConnectionManager& conn_mgr,
                      StrikeCallback record_strike,
                      OnBlobIngestedCallback on_blob_ingested,
                      UptimeCallback metrics_collector_uptime,
                      MaxStorageCallback config_max_storage_bytes);

    // Main dispatch entry point
    void on_peer_message(net::Connection::Ptr conn,
                         wire::TransportMsgType type,
                         std::vector<uint8_t> payload,
                         uint32_t request_id);

    // Config reload
    void set_rate_limits(uint64_t bytes_per_sec, uint64_t burst);
    void set_max_subscriptions(uint32_t limit) { max_subscriptions_ = limit; }

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
    SyncOrchestrator& sync_;
    PexManager& pex_;
    BlobPushManager& blob_push_;
    ConnectionManager& conn_mgr_;
    StrikeCallback record_strike_;
    OnBlobIngestedCallback on_blob_ingested_;
    UptimeCallback metrics_collector_uptime_;
    MaxStorageCallback config_max_storage_bytes_;

    uint64_t rate_limit_bytes_per_sec_ = 0;
    uint64_t rate_limit_burst_ = 0;
    uint32_t max_subscriptions_ = 256;   // RES-01 (D-07): per-connection subscription limit
};

} // namespace chromatindb::peer
