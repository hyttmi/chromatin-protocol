#include "db/peer/blob_push_manager.h"
#include "db/peer/peer_manager.h"  // for PeerManager::encode_notification (static)
#include "db/engine/engine.h"
#include "db/storage/storage.h"
#include "db/util/blob_helpers.h"
#include "db/util/hex.h"
#include "db/wire/codec.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace chromatindb::peer {

namespace {
using chromatindb::util::to_hex;
} // anonymous namespace

BlobPushManager::BlobPushManager(
    asio::io_context& ioc,
    engine::BlobEngine& engine,
    storage::Storage& storage,
    NodeMetrics& metrics,
    const bool& stopping,
    const std::set<std::array<uint8_t, 32>>& sync_namespaces,
    std::deque<std::unique_ptr<PeerInfo>>& peers,
    std::function<void(uint64_t)> rearm_expiry)
    : ioc_(ioc)
    , engine_(engine)
    , storage_(storage)
    , metrics_(metrics)
    , stopping_(stopping)
    , sync_namespaces_(sync_namespaces)
    , peers_(peers)
    , rearm_expiry_(std::move(rearm_expiry)) {}

// =============================================================================
// Blob ingestion fan-out (Phase 79 PUSH-01/PUSH-07/PUSH-08)
// =============================================================================

void BlobPushManager::on_blob_ingested(
    const std::array<uint8_t, 32>& namespace_id,
    const std::array<uint8_t, 32>& blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone,
    uint64_t expiry_time,
    net::Connection::Ptr source) {
    // Test hook -- fire notification callback if set
    if (on_notification_) {
        on_notification_(namespace_id, blob_hash, seq_num, blob_size, is_tombstone);
    }

    // Build notification payload once (77 bytes) -- reuse existing encode_notification
    auto payload = PeerManager::encode_notification(namespace_id, blob_hash, seq_num, blob_size, is_tombstone);

    // BlobNotify (type 59) to all TCP peers except source (PUSH-01, PUSH-07, PUSH-08)
    for (auto& peer : peers_) {
        if (peer->connection == source) continue;  // Source exclusion (D-06, D-09)
        if (peer->connection->is_uds()) continue;  // UDS = client, not peer

        // Phase 86: Namespace filtering (D-05, D-07)
        // Empty announced set = replicate all (no filter applied)
        if (!peer->announced_namespaces.empty() &&
            peer->announced_namespaces.count(namespace_id) == 0) continue;

        auto conn = peer->connection;
        auto payload_copy = payload;
        asio::co_spawn(ioc_, [conn, p = std::move(payload_copy)]() -> asio::awaitable<void> {
            co_await conn->send_message(wire::TransportMsgType_BlobNotify,
                                         std::span<const uint8_t>(p));
        }, asio::detached);
    }

    // Notification (type 21) to subscribed clients -- existing pub/sub behavior
    for (auto& peer : peers_) {
        if (peer->subscribed_namespaces.count(namespace_id)) {
            auto conn = peer->connection;
            auto payload_copy = payload;
            asio::co_spawn(ioc_, [conn, p = std::move(payload_copy)]() -> asio::awaitable<void> {
                co_await conn->send_message(wire::TransportMsgType_Notification,
                                             std::span<const uint8_t>(p));
            }, asio::detached);
        }
    }

    // Event-driven expiry: rearm via callback
    if (expiry_time > 0) {
        rearm_expiry_(expiry_time);
    }
}

// =============================================================================
// Phase 80: Targeted blob fetch (PUSH-05, PUSH-06)
// =============================================================================

void BlobPushManager::on_blob_notify(net::Connection::Ptr conn, std::vector<uint8_t> payload) {
    // BlobNotify payload: [namespace_id:32][blob_hash:32][seq_num_be:8][blob_size_be:4][is_tombstone:1]
    if (payload.size() != 77) return;

    auto* peer = find_peer(conn);
    if (!peer) return;

    auto [ns, hash] = chromatindb::util::extract_namespace_hash(std::span{payload});

    // D-07: Suppress during active sync with this peer
    if (peer->syncing) {
        spdlog::debug("BlobNotify from {} suppressed: sync active", peer_display_name(conn));
        return;
    }

    // D-04: Already have this blob?
    if (storage_.has_blob(ns, hash)) return;

    // D-05: Already fetching this blob?
    if (pending_fetches_.count(hash)) return;
    pending_fetches_.emplace(hash, conn);

    // Send BlobFetch: [namespace_id:32][blob_hash:32] = 64 bytes
    asio::co_spawn(ioc_, [this, conn, ns, hash]() -> asio::awaitable<void> {
        auto fetch_payload = chromatindb::util::encode_namespace_hash(
            std::span<const uint8_t, 32>{ns}, std::span<const uint8_t, 32>{hash});
        co_await conn->send_message(wire::TransportMsgType_BlobFetch,
                                     std::span<const uint8_t>(fetch_payload));
    }, asio::detached);
}

void BlobPushManager::handle_blob_fetch(net::Connection::Ptr conn, std::vector<uint8_t> payload) {
    // BlobFetch payload: [namespace_id:32][blob_hash:32] = 64 bytes
    if (payload.size() != 64) return;

    // co_spawn on ioc_ for storage lookup + send (same pattern as ReadRequest)
    asio::co_spawn(ioc_, [this, conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        auto [ns, hash] = chromatindb::util::extract_namespace_hash(std::span{payload});

        auto blob = storage_.get_blob(ns, hash);
        if (blob) {
            auto encoded = wire::encode_blob(*blob);
            std::vector<uint8_t> resp(1 + encoded.size());
            resp[0] = 0x00;  // D-03: found
            std::memcpy(resp.data() + 1, encoded.data(), encoded.size());
            co_await conn->send_message(wire::TransportMsgType_BlobFetchResponse,
                                         std::span<const uint8_t>(resp));
        } else {
            std::vector<uint8_t> resp = {0x01};  // D-03: not-found
            co_await conn->send_message(wire::TransportMsgType_BlobFetchResponse,
                                         std::span<const uint8_t>(resp));
        }
    }, asio::detached);
}

void BlobPushManager::handle_blob_fetch_response(net::Connection::Ptr conn, std::vector<uint8_t> payload) {
    if (payload.empty()) return;

    uint8_t status = payload[0];
    if (status == 0x01) {
        // D-08: Not-found -- debug log, pending set entry stays until disconnect (harmless)
        spdlog::debug("BlobFetchResponse not-found from {}", peer_display_name(conn));
        return;
    }
    if (status != 0x00 || payload.size() < 2) return;

    // co_spawn with engine offload (same pattern as Data handler, CONC-03)
    asio::co_spawn(ioc_, [this, conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        try {
            auto blob = wire::decode_blob(
                std::span<const uint8_t>(payload.data() + 1, payload.size() - 1));

            auto result = co_await engine_.ingest(blob, conn);

            // CONC-03: Transfer back to IO thread before accessing state
            co_await asio::post(ioc_, asio::use_awaitable);

            // Clean pending set using ack blob_hash (Pitfall 5 from RESEARCH.md)
            if (result.accepted && result.ack.has_value()) {
                pending_fetches_.erase(result.ack->blob_hash);

                if (result.ack->status == engine::IngestStatus::stored) {
                    uint64_t expiry_time = wire::saturating_expiry(blob.timestamp, blob.ttl);
                    // Fan-out notification to other peers (source=conn excludes sender)
                    on_blob_ingested(
                        blob.namespace_id,
                        result.ack->blob_hash,
                        result.ack->seq_num,
                        static_cast<uint32_t>(blob.data.size()),
                        wire::is_tombstone(blob.data),
                        expiry_time,
                        conn);  // Source exclusion: don't notify the peer we fetched from
                    ++metrics_.ingests;
                } else {
                    // Duplicate -- already had it (race with concurrent sync)
                    ++metrics_.ingests;
                }
            } else if (!result.accepted && result.error.has_value()) {
                // D-10: Failed ingestion -- log warning, no disconnect, no strike
                spdlog::warn("BlobFetchResponse ingest rejected from {}: {}",
                             peer_display_name(conn), result.error_detail);
            }
        } catch (const std::exception& e) {
            // D-10: Malformed response -- log warning, no disconnect, no strike
            spdlog::warn("malformed BlobFetchResponse from {}: {}",
                         conn->remote_address(), e.what());
        }
    }, asio::detached);
}

// =============================================================================
// Pending fetches cleanup
// =============================================================================

void BlobPushManager::clean_pending_fetches(const net::Connection::Ptr& conn) {
    for (auto it = pending_fetches_.begin(); it != pending_fetches_.end(); ) {
        if (it->second == conn) {
            it = pending_fetches_.erase(it);
        } else {
            ++it;
        }
    }
}

// =============================================================================
// Helpers
// =============================================================================

PeerInfo* BlobPushManager::find_peer(const net::Connection::Ptr& conn) {
    for (auto& p : peers_) {
        if (p->connection == conn) return p.get();
    }
    return nullptr;
}

std::string BlobPushManager::peer_display_name(const net::Connection::Ptr& conn) {
    auto ns_hex = to_hex(conn->peer_pubkey(), 8);
    return ns_hex + "@" + conn->remote_address();
}

} // namespace chromatindb::peer
