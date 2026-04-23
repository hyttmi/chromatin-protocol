#pragma once

#include "db/peer/peer_types.h"

#include <asio.hpp>

#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace chromatindb::engine { class BlobEngine; }
namespace chromatindb::storage { class Storage; }

namespace chromatindb::peer {

class MetricsCollector;

/// Build composite pending_fetches key: namespace_id || blob_hash (64 bytes).
/// Prevents cross-namespace hash collision (SYNC-02, D-02).
inline std::array<uint8_t, 64> make_pending_key(
    const std::array<uint8_t, 32>& namespace_id,
    const std::array<uint8_t, 32>& blob_hash) {
    std::array<uint8_t, 64> key;
    std::memcpy(key.data(), namespace_id.data(), 32);
    std::memcpy(key.data() + 32, blob_hash.data(), 32);
    return key;
}

/// Phase 129 sync-out cap-divergence filter (SYNC-02 / SYNC-03 / CONTEXT.md D-04).
///
/// Returns true iff this blob should be skipped for the given peer based on the
/// peer's advertised blob cap. Rules:
///   - advertised_blob_cap == 0 means "unknown" (pre-v4.2.0 peer or response
///     not yet landed) -- MUST NOT skip (D-01 conservative default; receiving
///     peer still enforces its own cap at ingest via Phase 128).
///   - Boundary is strict: blob_size > advertised_blob_cap. When equal, do NOT
///     skip -- the peer can accept a blob exactly at its cap.
///
/// Lives in the header so the 3 sync-out sites (blob_push_manager.cpp for
/// BlobNotify fan-out + BlobFetch response, sync_orchestrator.cpp for PULL
/// set-reconciliation announce) share one definition without an extra TU.
inline bool should_skip_for_peer_cap(uint64_t blob_size,
                                     uint64_t advertised_blob_cap) {
    return advertised_blob_cap > 0 && blob_size > advertised_blob_cap;
}

/// Owns pending_fetches_ and handles BlobNotify/BlobFetch/on_blob_ingested
/// fan-out.  Extracted from PeerManager (ARCH-01, component D-05).
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
                    MetricsCollector& metrics_collector,
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
    MetricsCollector& metrics_collector_;  // Phase 129: for increment_sync_skipped_oversized
    const bool& stopping_;
    const std::set<std::array<uint8_t, 32>>& sync_namespaces_;
    std::deque<std::unique_ptr<PeerInfo>>& peers_;
    std::function<void(uint64_t)> rearm_expiry_;

    std::unordered_map<std::array<uint8_t, 64>, net::Connection::Ptr, ArrayHash64>
        pending_fetches_;
    NotificationCallback on_notification_;
};

} // namespace chromatindb::peer
