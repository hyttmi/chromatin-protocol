#pragma once

#include "db/peer/peer_types.h"

#include <asio.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace chromatindb::engine { class BlobEngine; }
namespace chromatindb::storage { class Storage; }

namespace chromatindb::peer {

/// Owns pending_fetches_ and handles BlobNotify/BlobFetch/on_blob_ingested
/// fan-out.  Extracted from PeerManager (Phase 96 ARCH-01, component D-05).
class BlobPushManager {
public:
    using NotificationCallback = std::function<void(
        const std::array<uint8_t, 32>&, const std::array<uint8_t, 32>&,
        uint64_t, uint32_t, bool)>;
    using StrikeCallback = std::function<void(net::Connection::Ptr, const std::string&)>;

    BlobPushManager(asio::io_context& ioc,
                    engine::BlobEngine& engine,
                    storage::Storage& storage,
                    NodeMetrics& metrics,
                    const bool& stopping,
                    const std::set<std::array<uint8_t, 32>>& sync_namespaces,
                    std::deque<std::unique_ptr<PeerInfo>>& peers,
                    std::function<void(uint64_t)> rearm_expiry);

    // Blob push protocol (D-05)
    void on_blob_ingested(
        const std::array<uint8_t, 32>& namespace_id,
        const std::array<uint8_t, 32>& blob_hash,
        uint64_t seq_num, uint32_t blob_size,
        bool is_tombstone, uint64_t expiry_time,
        net::Connection::Ptr source);
    void on_blob_notify(net::Connection::Ptr conn, std::vector<uint8_t> payload);
    void handle_blob_fetch(net::Connection::Ptr conn, std::vector<uint8_t> payload);
    void handle_blob_fetch_response(net::Connection::Ptr conn, std::vector<uint8_t> payload);

    // Pending fetches cleanup (called from on_peer_disconnected)
    void clean_pending_fetches(const net::Connection::Ptr& conn);

    // Test hook
    void set_on_notification(NotificationCallback cb) { on_notification_ = std::move(cb); }

private:
    PeerInfo* find_peer(const net::Connection::Ptr& conn);
    std::string peer_display_name(const net::Connection::Ptr& conn);

    asio::io_context& ioc_;
    engine::BlobEngine& engine_;
    storage::Storage& storage_;
    NodeMetrics& metrics_;
    const bool& stopping_;
    const std::set<std::array<uint8_t, 32>>& sync_namespaces_;
    std::deque<std::unique_ptr<PeerInfo>>& peers_;
    std::function<void(uint64_t)> rearm_expiry_;

    std::unordered_map<std::array<uint8_t, 32>, net::Connection::Ptr, ArrayHash32>
        pending_fetches_;
    NotificationCallback on_notification_;
};

} // namespace chromatindb::peer
