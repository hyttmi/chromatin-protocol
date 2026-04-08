#include "db/peer/sync_orchestrator.h"
#include "db/peer/connection_manager.h"
#include "db/peer/peer_manager.h"  // for static encode_peer_list
#include "db/peer/pex_manager.h"
#include "db/peer/sync_reject.h"
#include "db/crypto/hash.h"
#include "db/engine/engine.h"
#include "db/logging/logging.h"
#include "db/storage/storage.h"
#include "db/sync/reconciliation.h"
#include "db/util/blob_helpers.h"
#include "db/util/endian.h"
#include "db/util/hex.h"

#include <spdlog/spdlog.h>

#include <ctime>
#include <unordered_set>

namespace chromatindb::peer {

using chromatindb::util::to_hex;

SyncOrchestrator::SyncOrchestrator(
    asio::io_context& ioc,
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
    PexRespondCallback pex_respond)
    : ioc_(ioc)
    , pool_(pool)
    , engine_(engine)
    , storage_(storage)
    , metrics_(metrics)
    , stopping_(stopping)
    , sync_namespaces_(sync_namespaces)
    , peers_(peers)
    , disconnected_peers_(disconnected_peers)
    , on_blob_ingested_(std::move(on_blob_ingested))
    , record_strike_(std::move(record_strike))
    , pex_request_(std::move(pex_request))
    , pex_respond_(std::move(pex_respond))
    , sync_proto_(engine, storage, pool)
    , sync_cooldown_seconds_(0)
    , safety_net_interval_seconds_(600)
    , max_sync_sessions_(1)
    , full_resync_interval_(10)
    , cursor_stale_seconds_(3600)
    , compaction_interval_hours_(6) {
}

PeerInfo* SyncOrchestrator::find_peer(const net::Connection::Ptr& conn) {
    for (auto& p : peers_) {
        if (p->connection == conn) return p.get();
    }
    return nullptr;
}

std::string SyncOrchestrator::peer_display_name(const net::Connection::Ptr& conn) {
    auto* peer = find_peer(conn);
    if (peer) return peer->address.empty() ? conn->remote_address() : peer->address;
    return conn->remote_address();
}

// =============================================================================
// Sync message queue
// =============================================================================

void SyncOrchestrator::route_sync_message(PeerInfo* peer, wire::TransportMsgType type,
                                           std::vector<uint8_t> payload) {
    peer->sync_inbox.push_back({type, std::move(payload)});
    if (peer->sync_notify) {
        peer->sync_notify->cancel();  // Wake up waiting coroutine
    }
}

asio::awaitable<std::optional<SyncMessage>>
SyncOrchestrator::recv_sync_msg(const net::Connection::Ptr& conn, std::chrono::seconds timeout) {
    // Ensure we're on the io_context thread before accessing sync_inbox.
    co_await asio::post(ioc_, asio::use_awaitable);

    auto* peer = find_peer(conn);
    if (!peer) co_return std::nullopt;

    if (!peer->sync_inbox.empty()) {
        auto msg = std::move(peer->sync_inbox.front());
        peer->sync_inbox.pop_front();
        co_return msg;
    }
    asio::steady_timer timer(ioc_);
    peer->sync_notify = &timer;
    timer.expires_after(timeout);
    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));

    // Re-lookup after co_await -- peer may have disconnected during wait
    peer = find_peer(conn);
    if (!peer) co_return std::nullopt;

    peer->sync_notify = nullptr;
    if (peer->sync_inbox.empty()) {
        co_return std::nullopt;  // Timeout
    }
    auto msg = std::move(peer->sync_inbox.front());
    peer->sync_inbox.pop_front();
    co_return msg;
}

// =============================================================================
// Sync orchestration
// =============================================================================

SyncOrchestrator::FullResyncReason SyncOrchestrator::check_full_resync(
    const storage::SyncCursor& cursor, uint64_t now) const {
    // Periodic: every Nth round (round_count 0 always triggers -- covers SIGHUP reset)
    if (full_resync_interval_ > 0 &&
        cursor.round_count % full_resync_interval_ == 0)
        return FullResyncReason::Periodic;
    // Time gap: too long since last sync
    if (cursor_stale_seconds_ > 0 &&
        cursor.last_sync_timestamp > 0 &&
        now - cursor.last_sync_timestamp > cursor_stale_seconds_)
        return FullResyncReason::TimeGap;
    return FullResyncReason::None;
}

asio::awaitable<void> SyncOrchestrator::run_sync_with_peer(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer || peer->syncing) co_return;
    peer->syncing = true;
    peer->sync_inbox.clear();  // Fresh sync session -- set up BEFORE sending

    sync::SyncStats total_stats;
    constexpr auto SYNC_TIMEOUT = std::chrono::seconds(30);

    // Send SyncRequest
    std::span<const uint8_t> empty{};
    if (!co_await conn->send_message(wire::TransportMsgType_SyncRequest, empty)) {
        peer = find_peer(conn);
        if (peer) peer->syncing = false;
        co_return;
    }

    // Wait for SyncAccept (or SyncRejected)
    auto accept_msg = co_await recv_sync_msg(conn, std::chrono::seconds(5));
    peer = find_peer(conn);
    if (!peer) co_return;
    if (!accept_msg || accept_msg->type != wire::TransportMsgType_SyncAccept) {
        if (accept_msg && accept_msg->type == wire::TransportMsgType_SyncRejected) {
            uint8_t reason = accept_msg->payload.empty() ? 0 : accept_msg->payload[0];
            auto reason_str = sync_reject_reason_string(reason);
            spdlog::info("sync with {}: rejected ({})", conn->remote_address(), reason_str);
        } else {
            spdlog::warn("sync with {}: no SyncAccept received", conn->remote_address());
        }
        peer->syncing = false;
        co_return;
    }

    // Phase A: Send our data (filtered by sync_namespaces)
    auto our_namespaces = engine_.list_namespaces();
    if (!sync_namespaces_.empty()) {
        std::erase_if(our_namespaces, [this](const storage::NamespaceInfo& ns) {
            return sync_namespaces_.find(ns.namespace_id) == sync_namespaces_.end();
        });
    }
    // Phase 86: Also filter by remote peer's announced namespaces (D-04, D-05)
    {
        auto* p = find_peer(conn);
        if (p && !p->announced_namespaces.empty()) {
            std::erase_if(our_namespaces, [&p](const storage::NamespaceInfo& ns) {
                return p->announced_namespaces.count(ns.namespace_id) == 0;
            });
        }
    }
    auto ns_payload = sync::SyncProtocol::encode_namespace_list(our_namespaces);
    if (!co_await conn->send_message(wire::TransportMsgType_NamespaceList, ns_payload)) {
        peer = find_peer(conn);
        if (peer) peer->syncing = false;
        co_return;
    }

    // Phase A (continued): Receive peer's NamespaceList
    auto ns_msg = co_await recv_sync_msg(conn, SYNC_TIMEOUT);
    peer = find_peer(conn);
    if (!peer) co_return;
    if (!ns_msg || ns_msg->type != wire::TransportMsgType_NamespaceList) {
        spdlog::warn("sync with {}: expected NamespaceList", conn->remote_address());
        peer->syncing = false;
        co_return;
    }
    auto peer_namespaces = sync::SyncProtocol::decode_namespace_list(ns_msg->payload);

    // Cursor decision: determine which namespaces to skip
    auto peer_hash = crypto::sha3_256(conn->peer_pubkey());
    auto now_ts = static_cast<uint64_t>(std::time(nullptr));
    std::set<std::array<uint8_t, 32>> cursor_skip_namespaces;
    bool sync_is_full_resync = false;
    uint64_t cursor_hits_this_round = 0;
    uint64_t cursor_misses_this_round = 0;

    for (const auto& pns : peer_namespaces) {
        auto cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
        if (cursor.has_value()) {
            auto reason = check_full_resync(*cursor, now_ts);
            if (reason != FullResyncReason::None) {
                sync_is_full_resync = true;
                if (reason == FullResyncReason::Periodic) {
                    spdlog::info("sync with {}: full resync (periodic, round {})",
                                 conn->remote_address(), cursor->round_count);
                } else {
                    spdlog::warn("sync with {}: full resync (time gap, last={}s ago)",
                                 conn->remote_address(),
                                 now_ts - cursor->last_sync_timestamp);
                }
                break;
            }
        }
    }

    if (!sync_is_full_resync) {
        for (const auto& pns : peer_namespaces) {
            auto cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
            if (cursor.has_value()) {
                if (cursor->seq_num == pns.latest_seq_num) {
                    cursor_skip_namespaces.insert(pns.namespace_id);
                    cursor_hits_this_round++;
                    spdlog::debug("sync cursor hit: ns={} seq={}", to_hex(pns.namespace_id, 8), pns.latest_seq_num);
                } else if (pns.latest_seq_num < cursor->seq_num) {
                    spdlog::warn("sync cursor mismatch: ns={} remote_seq={} stored_seq={}, resetting",
                                 to_hex(pns.namespace_id, 8), pns.latest_seq_num, cursor->seq_num);
                    storage_.delete_sync_cursor(peer_hash, pns.namespace_id);
                    cursor_misses_this_round++;
                } else {
                    cursor_misses_this_round++;
                    spdlog::debug("sync cursor miss: ns={} remote_seq={} stored_seq={}",
                                  to_hex(pns.namespace_id, 8), pns.latest_seq_num, cursor->seq_num);
                }
            } else {
                cursor_misses_this_round++;
            }
        }
    } else {
        cursor_misses_this_round = peer_namespaces.size();
        ++metrics_.full_resyncs;
    }

    // Phase B: Set Reconciliation (initiator drives)
    std::set<std::array<uint8_t, 32>> all_namespaces;
    for (const auto& ns_info : our_namespaces) all_namespaces.insert(ns_info.namespace_id);
    for (const auto& pns : peer_namespaces) all_namespaces.insert(pns.namespace_id);

    // Collect missing hashes per namespace (what WE need from peer)
    std::map<std::array<uint8_t, 32>, std::vector<std::array<uint8_t, 32>>> missing_per_ns;

    sync::Hash32 max_hash;
    max_hash.fill(0xFF);

    for (const auto& ns : all_namespaces) {
        // Byte budget check: if token bucket is exhausted, stop starting new namespaces
        if (rate_limit_bytes_per_sec_ > 0 && peer->bucket_tokens == 0) {
            spdlog::info("sync with {}: byte budget exhausted after {} namespaces, sending SyncComplete early",
                         conn->remote_address(), missing_per_ns.size());
            break;  // Fall through to SyncComplete
        }

        // Namespace filter
        if (!sync_namespaces_.empty() &&
            sync_namespaces_.find(ns) == sync_namespaces_.end()) {
            continue;
        }
        // Phase 86: Remote peer namespace filter (D-04, D-05)
        {
            auto* p = find_peer(conn);
            if (p && !p->announced_namespaces.empty() &&
                p->announced_namespaces.count(ns) == 0) {
                continue;
            }
        }

        bool cursor_hit = cursor_skip_namespaces.count(ns) > 0;
        if (cursor_hit) {
            ++metrics_.cursor_hits;
        } else {
            ++metrics_.cursor_misses;
        }

        // Snapshot our hashes ONCE (Pitfall 6: stale hash vector)
        auto our_hashes = sync_proto_.collect_namespace_hashes(ns);
        std::sort(our_hashes.begin(), our_hashes.end());

        auto our_fp = sync::xor_fingerprint(our_hashes, 0, our_hashes.size());

        // Send ReconcileInit for this namespace
        sync::ReconcileInit init;
        init.version = sync::RECONCILE_VERSION;
        init.namespace_id = ns;
        init.count = static_cast<uint32_t>(our_hashes.size());
        init.fingerprint = our_fp;

        auto init_payload = sync::encode_reconcile_init(init);
        if (!co_await conn->send_message(wire::TransportMsgType_ReconcileInit, init_payload)) {
            peer = find_peer(conn);
            if (peer) peer->syncing = false;
            co_return;
        }
        peer = find_peer(conn);
        if (!peer) co_return;

        // Multi-round reconciliation loop
        std::vector<sync::Hash32> peer_items;

        for (uint32_t round = 0; round < sync::MAX_RECONCILE_ROUNDS; ++round) {
            auto msg = co_await recv_sync_msg(conn, SYNC_TIMEOUT);
            peer = find_peer(conn);
            if (!peer) co_return;
            if (!msg) {
                spdlog::warn("sync with {}: timeout during reconciliation for ns={}",
                             conn->remote_address(), to_hex(ns, 8));
                peer->syncing = false;
                co_return;
            }

            if (msg->type == wire::TransportMsgType_ReconcileRanges) {
                auto decoded = sync::decode_reconcile_ranges(msg->payload);
                if (!decoded) {
                    spdlog::warn("sync with {}: invalid ReconcileRanges", conn->remote_address());
                    peer->syncing = false;
                    co_return;
                }

                // Empty ranges = reconciliation done for this namespace
                if (decoded->ranges.empty()) break;

                bool has_fingerprint = false;
                for (const auto& r : decoded->ranges) {
                    if (r.mode == sync::RangeMode::Fingerprint) {
                        has_fingerprint = true;
                        break;
                    }
                }

                if (!has_fingerprint) {
                    // Final exchange: collect peer items from ItemList ranges
                    for (const auto& r : decoded->ranges) {
                        if (r.mode == sync::RangeMode::ItemList) {
                            peer_items.insert(peer_items.end(),
                                              r.items.begin(), r.items.end());
                        }
                    }
                    sync::Hash32 lower = sync::Hash32{};
                    std::vector<sync::Hash32> our_range_items;
                    for (const auto& r : decoded->ranges) {
                        if (r.mode == sync::RangeMode::ItemList) {
                            auto [b, e] = sync::range_indices(our_hashes, lower, r.upper_bound);
                            for (size_t idx = b; idx < e; ++idx) {
                                our_range_items.push_back(our_hashes[idx]);
                            }
                        }
                        lower = r.upper_bound;
                    }
                    auto items_payload = sync::encode_reconcile_items(ns, our_range_items);
                    if (!co_await conn->send_message(wire::TransportMsgType_ReconcileItems,
                                                     items_payload)) {
                        peer = find_peer(conn);
                        if (peer) peer->syncing = false;
                        co_return;
                    }
                    break;
                }

                auto result = sync::process_ranges(our_hashes, decoded->ranges);
                peer_items.insert(peer_items.end(),
                                  result.have_items.begin(), result.have_items.end());

                auto resp_payload = sync::encode_reconcile_ranges(ns, result.response_ranges);
                if (!co_await conn->send_message(wire::TransportMsgType_ReconcileRanges,
                                                 resp_payload)) {
                    peer = find_peer(conn);
                    if (peer) peer->syncing = false;
                    co_return;
                }
            } else if (msg->type == wire::TransportMsgType_ReconcileItems) {
                auto decoded = sync::decode_reconcile_items(msg->payload);
                if (decoded) {
                    peer_items.insert(peer_items.end(),
                                      decoded->items.begin(), decoded->items.end());
                }
                break;
            } else {
                spdlog::warn("sync with {}: unexpected message type {} during reconciliation",
                             conn->remote_address(), static_cast<int>(msg->type));
                break;
            }
        }

        // Compute diff: peer items we don't have (only request if not cursor-hit)
        if (!cursor_hit) {
            std::vector<std::array<uint8_t, 32>> missing;
            {
                std::unordered_set<std::string> our_set;
                our_set.reserve(our_hashes.size());
                for (const auto& h : our_hashes) {
                    our_set.insert(std::string(reinterpret_cast<const char*>(h.data()), h.size()));
                }
                for (const auto& h : peer_items) {
                    std::string key(reinterpret_cast<const char*>(h.data()), h.size());
                    if (our_set.find(key) == our_set.end()) {
                        missing.push_back(h);
                    }
                }
            }
            if (!missing.empty()) {
                missing_per_ns[ns] = std::move(missing);
            }
        }
        total_stats.namespaces_synced++;
    }

    // Signal end of Phase B: all namespaces reconciled
    if (!co_await conn->send_message(wire::TransportMsgType_SyncComplete, empty)) {
        peer = find_peer(conn);
        if (peer) peer->syncing = false;
        co_return;
    }
    peer = find_peer(conn);
    if (!peer) co_return;

    // Phase C: Exchange blobs one at a time using existing BlobRequest/BlobTransfer
    spdlog::debug("sync initiator {}: Phase C start, missing {} namespaces",
                  conn->remote_address(), missing_per_ns.size());
    for (const auto& [ns, missing] : missing_per_ns) {
        spdlog::debug("sync initiator {}: requesting {} blobs for ns {}",
                      conn->remote_address(), missing.size(), to_hex(ns, 4));
        for (size_t i = 0; i < missing.size(); i += MAX_HASHES_PER_REQUEST) {
            size_t batch_end = std::min(i + static_cast<size_t>(MAX_HASHES_PER_REQUEST),
                                        missing.size());
            std::vector<std::array<uint8_t, 32>> batch(
                missing.begin() + static_cast<ptrdiff_t>(i),
                missing.begin() + static_cast<ptrdiff_t>(batch_end));

            auto req_payload = sync::SyncProtocol::encode_blob_request(ns, batch);
            if (!co_await conn->send_message(wire::TransportMsgType_BlobRequest, req_payload)) {
                peer = find_peer(conn);
                if (peer) peer->syncing = false;
                co_return;
            }
            peer = find_peer(conn);
            if (!peer) co_return;

            uint32_t expected = static_cast<uint32_t>(batch.size());
            uint32_t received = 0;
            while (received < expected) {
                auto msg = co_await recv_sync_msg(conn, BLOB_TRANSFER_TIMEOUT);
                peer = find_peer(conn);
                if (!peer) co_return;
                if (!msg) {
                    spdlog::warn("sync: timeout waiting for blob transfer from {}",
                                 conn->remote_address());
                    break;
                }

                spdlog::debug("sync initiator {}: Phase C got msg type={}",
                              conn->remote_address(), static_cast<int>(msg->type));
                if (msg->type == wire::TransportMsgType_BlobTransfer) {
                    auto blobs = sync::SyncProtocol::decode_blob_transfer(msg->payload);
                    auto s = co_await sync_proto_.ingest_blobs(blobs, conn);
                    peer = find_peer(conn);
                    if (!peer) co_return;
                    total_stats.blobs_received += s.blobs_received;
                    total_stats.storage_full_count += s.storage_full_count;
                    total_stats.quota_exceeded_count += s.quota_exceeded_count;
                    received++;
                } else if (msg->type == wire::TransportMsgType_BlobRequest) {
                    if (peer->peer_is_full) {
                        spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                        continue;
                    }
                    auto [req_ns, requested_hashes] =
                        sync::SyncProtocol::decode_blob_request(msg->payload);
                    for (const auto& hash : requested_hashes) {
                        auto blob = engine_.get_blob(req_ns, hash);
                        if (blob.has_value()) {
                            auto bt_payload =
                                sync::SyncProtocol::encode_single_blob_transfer(*blob);
                            co_await conn->send_message(
                                wire::TransportMsgType_BlobTransfer, bt_payload);
                            peer = find_peer(conn);
                            if (!peer) co_return;
                            total_stats.blobs_sent++;
                        }
                    }
                }
            }
        }
    }

    // Handle remaining BlobRequests from peer (they may still need our blobs)
    while (true) {
        auto msg = co_await recv_sync_msg(conn, std::chrono::seconds(2));
        peer = find_peer(conn);
        if (!peer) co_return;
        if (!msg) break;
        if (msg->type == wire::TransportMsgType_BlobRequest) {
            if (peer->peer_is_full) {
                spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                continue;
            }
            auto [req_ns, requested_hashes] =
                sync::SyncProtocol::decode_blob_request(msg->payload);
            for (const auto& hash : requested_hashes) {
                auto blob = engine_.get_blob(req_ns, hash);
                if (blob.has_value()) {
                    auto bt_payload =
                        sync::SyncProtocol::encode_single_blob_transfer(*blob);
                    co_await conn->send_message(
                        wire::TransportMsgType_BlobTransfer, bt_payload);
                    peer = find_peer(conn);
                    if (!peer) co_return;
                    total_stats.blobs_sent++;
                }
            }
        } else {
            break;
        }
    }

    // Post-sync cursor update: only update after successful sync completion (Pitfall 1)
    for (const auto& pns : peer_namespaces) {
        auto old_cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
        storage::SyncCursor updated;
        updated.seq_num = pns.latest_seq_num;
        updated.round_count = (old_cursor ? old_cursor->round_count : 0) + 1;
        updated.last_sync_timestamp = now_ts;
        storage_.set_sync_cursor(peer_hash, pns.namespace_id, updated);
    }

    spdlog::info("Synced with peer {}: received {} blobs, sent {} blobs, {} namespaces "
                 "(cursor: {} hits, {} misses{})",
                 conn->remote_address(),
                 total_stats.blobs_received, total_stats.blobs_sent,
                 total_stats.namespaces_synced,
                 cursor_hits_this_round, cursor_misses_this_round,
                 sync_is_full_resync ? ", full resync" : "");

    // Post-sync StorageFull signal
    if (total_stats.storage_full_count > 0) {
        std::span<const uint8_t> empty{};
        co_await conn->send_message(wire::TransportMsgType_StorageFull, empty);
        spdlog::warn("Sent StorageFull to sync peer {} ({} blobs rejected)",
                     peer_display_name(conn), total_stats.storage_full_count);
    }

    // Post-sync QuotaExceeded signal
    if (total_stats.quota_exceeded_count > 0) {
        std::span<const uint8_t> empty{};
        co_await conn->send_message(wire::TransportMsgType_QuotaExceeded, empty);
        spdlog::warn("Sent QuotaExceeded to sync peer {} ({} blobs rejected)",
                     peer_display_name(conn), total_stats.quota_exceeded_count);
        metrics_.quota_rejections += total_stats.quota_exceeded_count;
    }

    // PEX exchange: send PeerListRequest and wait for response (inline, no concurrent send)
    // Skip in closed mode -- don't advertise or discover peers
    // Note: acl_ is not available here; the PEX callback is injected by facade
    // Instead, always do PEX after sync -- the facade can suppress if needed
    // Actually, sync protocol always does inline PEX. The acl check was in PeerManager.
    // We need the acl ref. For now, just always do PEX as before -- the plan says
    // PEX inline after sync uses pex_.build_peer_list_response and PeerManager::encode_peer_list.
    // But we don't have a direct acl_ reference. Let's add a flag or callback.
    // Simplest: pass a "pex_enabled" bool or callback. Actually, we need acl for this.
    // The original code checked acl_.is_peer_closed_mode(). Let's store a reference.
    // DEVIATION: We'll accept the PEX part inline using callbacks.

    // PEX: inline PEX exchange after sync (initiator sends request)
    if (pex_request_) {
        co_await pex_request_(conn);
    }

    ++metrics_.syncs;
    peer = find_peer(conn);
    if (peer) peer->syncing = false;
}

asio::awaitable<void> SyncOrchestrator::handle_sync_as_responder(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer) co_return;
    peer->syncing = true;

    sync::SyncStats total_stats;
    constexpr auto SYNC_TIMEOUT = std::chrono::seconds(30);

    // Send SyncAccept
    std::span<const uint8_t> empty{};
    co_await conn->send_message(wire::TransportMsgType_SyncAccept, empty);
    peer = find_peer(conn);
    if (!peer) co_return;

    // Phase A: Send our NamespaceList (filtered by sync_namespaces)
    auto our_namespaces = engine_.list_namespaces();
    if (!sync_namespaces_.empty()) {
        std::erase_if(our_namespaces, [this](const storage::NamespaceInfo& ns) {
            return sync_namespaces_.find(ns.namespace_id) == sync_namespaces_.end();
        });
    }
    // Phase 86: Also filter by remote peer's announced namespaces (D-04, D-05)
    {
        auto* p = find_peer(conn);
        if (p && !p->announced_namespaces.empty()) {
            std::erase_if(our_namespaces, [&p](const storage::NamespaceInfo& ns) {
                return p->announced_namespaces.count(ns.namespace_id) == 0;
            });
        }
    }
    auto ns_payload = sync::SyncProtocol::encode_namespace_list(our_namespaces);
    co_await conn->send_message(wire::TransportMsgType_NamespaceList, ns_payload);

    // Phase A (continued): Receive peer's NamespaceList
    auto ns_msg = co_await recv_sync_msg(conn, SYNC_TIMEOUT);
    peer = find_peer(conn);
    if (!peer) co_return;
    if (!ns_msg || ns_msg->type != wire::TransportMsgType_NamespaceList) {
        spdlog::warn("sync responder {}: expected NamespaceList", conn->remote_address());
        peer->syncing = false;
        co_return;
    }
    auto peer_namespaces = sync::SyncProtocol::decode_namespace_list(ns_msg->payload);

    // Cursor decision (same logic as initiator, independently applied)
    auto peer_hash = crypto::sha3_256(conn->peer_pubkey());
    auto now_ts = static_cast<uint64_t>(std::time(nullptr));
    std::set<std::array<uint8_t, 32>> cursor_skip_namespaces;
    bool sync_is_full_resync = false;
    uint64_t cursor_hits_this_round = 0;
    uint64_t cursor_misses_this_round = 0;

    for (const auto& pns : peer_namespaces) {
        auto cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
        if (cursor.has_value()) {
            auto reason = check_full_resync(*cursor, now_ts);
            if (reason != FullResyncReason::None) {
                sync_is_full_resync = true;
                if (reason == FullResyncReason::Periodic) {
                    spdlog::debug("sync responder {}: full resync (periodic, round {})",
                                 conn->remote_address(), cursor->round_count);
                } else {
                    spdlog::warn("sync responder {}: full resync (time gap, last={}s ago)",
                                 conn->remote_address(),
                                 now_ts - cursor->last_sync_timestamp);
                }
                break;
            }
        }
    }

    if (!sync_is_full_resync) {
        for (const auto& pns : peer_namespaces) {
            auto cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
            if (cursor.has_value()) {
                if (cursor->seq_num == pns.latest_seq_num) {
                    cursor_skip_namespaces.insert(pns.namespace_id);
                    cursor_hits_this_round++;
                    spdlog::debug("sync responder cursor hit: ns={} seq={}", to_hex(pns.namespace_id, 8), pns.latest_seq_num);
                } else if (pns.latest_seq_num < cursor->seq_num) {
                    spdlog::warn("sync responder cursor mismatch: ns={} remote_seq={} stored_seq={}, resetting",
                                 to_hex(pns.namespace_id, 8), pns.latest_seq_num, cursor->seq_num);
                    storage_.delete_sync_cursor(peer_hash, pns.namespace_id);
                    cursor_misses_this_round++;
                } else {
                    cursor_misses_this_round++;
                }
            } else {
                cursor_misses_this_round++;
            }
        }
    } else {
        cursor_misses_this_round = peer_namespaces.size();
        ++metrics_.full_resyncs;
    }

    // Phase B: Respond to initiator-driven reconciliation
    std::map<std::array<uint8_t, 32>, std::vector<std::array<uint8_t, 32>>> missing_per_ns;

    // Cache sorted hash vectors per namespace (Pitfall 6: snapshot once)
    std::map<std::array<uint8_t, 32>, std::vector<sync::Hash32>> ns_hash_cache;

    while (true) {
        // Byte budget check (responder): silently stop if budget exhausted
        if (rate_limit_bytes_per_sec_ > 0 && peer->bucket_tokens == 0) {
            spdlog::debug("sync responder {}: byte budget exhausted, stopping silently",
                         conn->remote_address());
            peer->syncing = false;
            co_return;
        }

        auto msg = co_await recv_sync_msg(conn, SYNC_TIMEOUT);
        peer = find_peer(conn);
        if (!peer) co_return;
        if (!msg) {
            spdlog::warn("sync responder {}: timeout during Phase B", conn->remote_address());
            peer->syncing = false;
            co_return;
        }

        if (msg->type == wire::TransportMsgType_SyncComplete) break;

        if (msg->type == wire::TransportMsgType_ReconcileInit) {
            auto init = sync::decode_reconcile_init(msg->payload);
            if (!init) {
                spdlog::warn("sync responder {}: invalid ReconcileInit", conn->remote_address());
                continue;
            }

            std::array<uint8_t, 32> ns = init->namespace_id;

            // Phase 86: Remote peer namespace filter (D-04, D-05)
            {
                auto* p = find_peer(conn);
                if (p && !p->announced_namespaces.empty() &&
                    p->announced_namespaces.count(ns) == 0) {
                    continue;
                }
            }

            bool cursor_hit = cursor_skip_namespaces.count(ns) > 0;
            if (cursor_hit) {
                ++metrics_.cursor_hits;
            } else {
                ++metrics_.cursor_misses;
            }

            // Snapshot our hashes for this namespace (sorted)
            auto& our_hashes = ns_hash_cache[ns];
            if (our_hashes.empty()) {
                auto raw = sync_proto_.collect_namespace_hashes(ns);
                our_hashes.assign(raw.begin(), raw.end());
                std::sort(our_hashes.begin(), our_hashes.end());
            }

            // Build initial full-range from the init message
            sync::Hash32 max_hash;
            max_hash.fill(0xFF);

            sync::RangeEntry init_range;
            init_range.upper_bound = max_hash;
            init_range.mode = sync::RangeMode::Fingerprint;
            init_range.count = init->count;
            init_range.fingerprint = init->fingerprint;

            auto result = sync::process_ranges(our_hashes, {init_range});

            std::vector<sync::Hash32> peer_items;
            peer_items.insert(peer_items.end(),
                              result.have_items.begin(), result.have_items.end());

            if (result.complete) {
                auto done_payload = sync::encode_reconcile_ranges(ns, {});
                co_await conn->send_message(wire::TransportMsgType_ReconcileRanges, done_payload);
                peer = find_peer(conn);
                if (!peer) co_return;
            } else {
                auto resp_payload = sync::encode_reconcile_ranges(ns, result.response_ranges);
                co_await conn->send_message(wire::TransportMsgType_ReconcileRanges, resp_payload);
                peer = find_peer(conn);
                if (!peer) co_return;

                // Multi-round loop within this namespace
                for (uint32_t round = 0; round < sync::MAX_RECONCILE_ROUNDS; ++round) {
                    auto rmsg = co_await recv_sync_msg(conn, SYNC_TIMEOUT);
                    peer = find_peer(conn);
                    if (!peer) co_return;
                    if (!rmsg) {
                        spdlog::warn("sync responder {}: timeout during reconciliation for ns={}",
                                     conn->remote_address(), to_hex(ns, 8));
                        peer->syncing = false;
                        co_return;
                    }

                    if (rmsg->type == wire::TransportMsgType_ReconcileRanges) {
                        auto decoded = sync::decode_reconcile_ranges(rmsg->payload);
                        if (!decoded) {
                            spdlog::warn("sync responder {}: invalid ReconcileRanges",
                                         conn->remote_address());
                            break;
                        }

                        if (decoded->ranges.empty()) break;

                        bool has_fingerprint = false;
                        for (const auto& r : decoded->ranges) {
                            if (r.mode == sync::RangeMode::Fingerprint) {
                                has_fingerprint = true;
                                break;
                            }
                        }

                        if (!has_fingerprint) {
                            for (const auto& r : decoded->ranges) {
                                if (r.mode == sync::RangeMode::ItemList) {
                                    peer_items.insert(peer_items.end(),
                                                      r.items.begin(), r.items.end());
                                }
                            }
                            sync::Hash32 lower = sync::Hash32{};
                            std::vector<sync::Hash32> our_range_items;
                            for (const auto& r : decoded->ranges) {
                                if (r.mode == sync::RangeMode::ItemList) {
                                    auto [b, e] = sync::range_indices(our_hashes, lower,
                                                                      r.upper_bound);
                                    for (size_t idx = b; idx < e; ++idx) {
                                        our_range_items.push_back(our_hashes[idx]);
                                    }
                                }
                                lower = r.upper_bound;
                            }
                            auto items_payload = sync::encode_reconcile_items(ns,
                                                                              our_range_items);
                            co_await conn->send_message(wire::TransportMsgType_ReconcileItems,
                                                        items_payload);
                            peer = find_peer(conn);
                            if (!peer) co_return;
                            break;
                        }

                        auto rresult = sync::process_ranges(our_hashes, decoded->ranges);
                        peer_items.insert(peer_items.end(),
                                          rresult.have_items.begin(), rresult.have_items.end());

                        auto rp = sync::encode_reconcile_ranges(ns, rresult.response_ranges);
                        co_await conn->send_message(wire::TransportMsgType_ReconcileRanges, rp);
                        peer = find_peer(conn);
                        if (!peer) co_return;
                    } else if (rmsg->type == wire::TransportMsgType_ReconcileItems) {
                        auto decoded = sync::decode_reconcile_items(rmsg->payload);
                        if (decoded) {
                            peer_items.insert(peer_items.end(),
                                              decoded->items.begin(), decoded->items.end());
                        }
                        break;
                    } else {
                        break;
                    }
                }
            }

            // Compute diff: peer items we don't have (only request if not cursor-hit)
            if (!cursor_hit) {
                std::vector<std::array<uint8_t, 32>> missing;
                {
                    std::unordered_set<std::string> our_set;
                    our_set.reserve(our_hashes.size());
                    for (const auto& h : our_hashes) {
                        our_set.insert(std::string(reinterpret_cast<const char*>(h.data()), h.size()));
                    }
                    for (const auto& h : peer_items) {
                        std::string key(reinterpret_cast<const char*>(h.data()), h.size());
                        if (our_set.find(key) == our_set.end()) {
                            missing.push_back(h);
                        }
                    }
                }
                if (!missing.empty()) {
                    missing_per_ns[ns] = std::move(missing);
                }
            }
            total_stats.namespaces_synced++;
        }
    }

    // Phase C: Exchange blobs one at a time
    spdlog::debug("sync responder {}: Phase C start, missing {} namespaces",
                  conn->remote_address(), missing_per_ns.size());
    for (const auto& [ns, missing] : missing_per_ns) {
        spdlog::debug("sync responder {}: requesting {} blobs for ns {}",
                      conn->remote_address(), missing.size(), to_hex(ns, 4));
        for (size_t i = 0; i < missing.size(); i += MAX_HASHES_PER_REQUEST) {
            size_t batch_end = std::min(i + static_cast<size_t>(MAX_HASHES_PER_REQUEST),
                                        missing.size());
            std::vector<std::array<uint8_t, 32>> batch(
                missing.begin() + static_cast<ptrdiff_t>(i),
                missing.begin() + static_cast<ptrdiff_t>(batch_end));

            auto req_payload = sync::SyncProtocol::encode_blob_request(ns, batch);
            co_await conn->send_message(wire::TransportMsgType_BlobRequest, req_payload);
            peer = find_peer(conn);
            if (!peer) co_return;

            uint32_t expected = static_cast<uint32_t>(batch.size());
            uint32_t received = 0;
            while (received < expected) {
                auto msg = co_await recv_sync_msg(conn, BLOB_TRANSFER_TIMEOUT);
                peer = find_peer(conn);
                if (!peer) co_return;
                if (!msg) {
                    spdlog::warn("sync responder: timeout waiting for blob transfer from {}",
                                 conn->remote_address());
                    break;
                }

                if (msg->type == wire::TransportMsgType_BlobTransfer) {
                    auto blobs = sync::SyncProtocol::decode_blob_transfer(msg->payload);
                    auto s = co_await sync_proto_.ingest_blobs(blobs, conn);
                    peer = find_peer(conn);
                    if (!peer) co_return;
                    total_stats.blobs_received += s.blobs_received;
                    total_stats.storage_full_count += s.storage_full_count;
                    total_stats.quota_exceeded_count += s.quota_exceeded_count;
                    received++;
                } else if (msg->type == wire::TransportMsgType_BlobRequest) {
                    if (peer->peer_is_full) {
                        spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                        continue;
                    }
                    auto [req_ns, requested_hashes] =
                        sync::SyncProtocol::decode_blob_request(msg->payload);
                    for (const auto& hash : requested_hashes) {
                        auto blob = engine_.get_blob(req_ns, hash);
                        if (blob.has_value()) {
                            auto bt_payload =
                                sync::SyncProtocol::encode_single_blob_transfer(*blob);
                            co_await conn->send_message(
                                wire::TransportMsgType_BlobTransfer, bt_payload);
                            peer = find_peer(conn);
                            if (!peer) co_return;
                            total_stats.blobs_sent++;
                        }
                    }
                }
            }
        }
    }

    // Handle remaining BlobRequests from peer
    spdlog::debug("sync responder {}: entering remaining BlobRequests handler", conn->remote_address());
    while (true) {
        auto msg = co_await recv_sync_msg(conn, std::chrono::seconds(2));
        peer = find_peer(conn);
        if (!peer) co_return;
        if (!msg) {
            spdlog::debug("sync responder {}: remaining handler timeout, done", conn->remote_address());
            break;
        }
        spdlog::debug("sync responder {}: remaining handler got type={}", conn->remote_address(), static_cast<int>(msg->type));
        if (msg->type == wire::TransportMsgType_BlobRequest) {
            if (peer->peer_is_full) {
                spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                continue;
            }
            auto [req_ns, requested_hashes] =
                sync::SyncProtocol::decode_blob_request(msg->payload);
            spdlog::debug("sync responder {}: remaining handler serving {} hashes", conn->remote_address(), requested_hashes.size());
            for (const auto& hash : requested_hashes) {
                auto blob = engine_.get_blob(req_ns, hash);
                if (blob.has_value()) {
                    auto bt_payload =
                        sync::SyncProtocol::encode_single_blob_transfer(*blob);
                    co_await conn->send_message(
                        wire::TransportMsgType_BlobTransfer, bt_payload);
                    peer = find_peer(conn);
                    if (!peer) co_return;
                    total_stats.blobs_sent++;
                }
            }
        } else {
            break;
        }
    }

    // Post-sync cursor update (responder side, same as initiator)
    for (const auto& pns : peer_namespaces) {
        auto old_cursor = storage_.get_sync_cursor(peer_hash, pns.namespace_id);
        storage::SyncCursor updated;
        updated.seq_num = pns.latest_seq_num;
        updated.round_count = (old_cursor ? old_cursor->round_count : 0) + 1;
        updated.last_sync_timestamp = now_ts;
        storage_.set_sync_cursor(peer_hash, pns.namespace_id, updated);
    }

    spdlog::info("Sync responder {}: received {} blobs, sent {} blobs, {} namespaces "
                 "(cursor: {} hits, {} misses{})",
                 conn->remote_address(),
                 total_stats.blobs_received, total_stats.blobs_sent,
                 total_stats.namespaces_synced,
                 cursor_hits_this_round, cursor_misses_this_round,
                 sync_is_full_resync ? ", full resync" : "");

    if (total_stats.storage_full_count > 0) {
        std::span<const uint8_t> empty{};
        co_await conn->send_message(wire::TransportMsgType_StorageFull, empty);
        spdlog::warn("Sent StorageFull to sync peer {} ({} blobs rejected)",
                     peer_display_name(conn), total_stats.storage_full_count);
    }

    if (total_stats.quota_exceeded_count > 0) {
        std::span<const uint8_t> empty{};
        co_await conn->send_message(wire::TransportMsgType_QuotaExceeded, empty);
        spdlog::warn("Sent QuotaExceeded to sync peer {} ({} blobs rejected)",
                     peer_display_name(conn), total_stats.quota_exceeded_count);
        metrics_.quota_rejections += total_stats.quota_exceeded_count;
    }

    // PEX exchange (responder side)
    if (pex_respond_) {
        co_await pex_respond_(conn);
    }

    ++metrics_.syncs;
    peer = find_peer(conn);
    if (peer) peer->syncing = false;
}

asio::awaitable<void> SyncOrchestrator::sync_all_peers() {
    // Take a snapshot of connection pointers to avoid iterator invalidation
    std::vector<net::Connection::Ptr> connections;
    for (const auto& peer : peers_) {
        connections.push_back(peer->connection);
    }
    for (const auto& conn : connections) {
        auto* peer = find_peer(conn);
        if (peer && peer->connection->is_authenticated() && !peer->syncing) {
            co_await run_sync_with_peer(peer->connection);
        }
    }
}

asio::awaitable<void> SyncOrchestrator::sync_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        sync_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(safety_net_interval_seconds_));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        sync_timer_ = nullptr;
        if (ec || stopping_) co_return;

        co_await sync_all_peers();

        // Phase 82 D-02: Clean stale disconnected peer entries during safety-net cycle.
        {
            auto now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            for (auto it = disconnected_peers_.begin(); it != disconnected_peers_.end(); ) {
                if (now_ms - it->second.disconnect_time > ConnectionManager::CURSOR_GRACE_PERIOD_MS) {
                    storage_.delete_peer_cursors(it->first);
                    it = disconnected_peers_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}

// =============================================================================
// Sync rejection
// =============================================================================

void SyncOrchestrator::send_sync_rejected(net::Connection::Ptr conn, uint8_t reason) {
    std::vector<uint8_t> payload = { reason };
    asio::co_spawn(ioc_, [conn, payload = std::move(payload)]() -> asio::awaitable<void> {
        co_await conn->send_message(wire::TransportMsgType_SyncRejected,
                                     std::span<const uint8_t>(payload));
    }, asio::detached);
}

// =============================================================================
// Expiry scanning
// =============================================================================

void SyncOrchestrator::rearm_expiry_timer(uint64_t expiry_time) {
    if (next_expiry_target_ == 0 || expiry_time < next_expiry_target_) {
        next_expiry_target_ = expiry_time;
        if (expiry_loop_running_) {
            if (expiry_timer_) expiry_timer_->cancel();
        } else {
            asio::co_spawn(ioc_, expiry_scan_loop(), asio::detached);
        }
    }
}

asio::awaitable<void> SyncOrchestrator::expiry_scan_loop() {
    expiry_loop_running_ = true;
    while (!stopping_) {
        uint64_t target = next_expiry_target_;
        if (target == 0) break;

        uint64_t now = storage::system_clock_seconds();
        auto duration = (target > now)
            ? std::chrono::seconds(target - now)
            : std::chrono::seconds(0);

        asio::steady_timer timer(ioc_);
        expiry_timer_ = &timer;
        timer.expires_after(duration);
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        expiry_timer_ = nullptr;
        if (stopping_) break;
        if (ec) continue;

        auto purged = storage_.run_expiry_scan();
        if (purged > 0) {
            spdlog::info("expiry scan: purged {} blobs", purged);
        }

        auto next = storage_.get_earliest_expiry();
        if (next.has_value()) {
            next_expiry_target_ = *next;
        } else {
            next_expiry_target_ = 0;
            break;
        }
    }
    expiry_loop_running_ = false;
}

// =============================================================================
// Cursor compaction
// =============================================================================

asio::awaitable<void> SyncOrchestrator::cursor_compaction_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        cursor_compaction_timer_ = &timer;
        timer.expires_after(std::chrono::hours(6));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        cursor_compaction_timer_ = nullptr;
        if (ec || stopping_) co_return;

        std::vector<std::array<uint8_t, 32>> connected;
        for (const auto& peer : peers_) {
            if (peer->connection && !peer->connection->peer_pubkey().empty()) {
                auto hash = crypto::sha3_256(peer->connection->peer_pubkey());
                connected.push_back(hash);
            }
        }
        // Phase 82: Also preserve cursors for recently-disconnected peers (grace period)
        {
            auto now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            for (const auto& [hash, state] : disconnected_peers_) {
                if (now_ms - state.disconnect_time <= ConnectionManager::CURSOR_GRACE_PERIOD_MS) {
                    connected.push_back(hash);
                }
            }
        }
        auto removed = storage_.cleanup_stale_cursors(connected);
        if (removed > 0) {
            spdlog::info("cursor compaction: removed {} entries for disconnected peers", removed);
        }
    }
}

// =============================================================================
// Storage compaction
// =============================================================================

asio::awaitable<void> SyncOrchestrator::compaction_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        compaction_timer_ = &timer;
        timer.expires_after(std::chrono::hours(compaction_interval_hours_));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        compaction_timer_ = nullptr;
        if (ec || stopping_) co_return;

        if (compaction_interval_hours_ == 0) co_return;

        auto result = storage_.compact();
        if (result.success) {
            auto before_mib = static_cast<double>(result.before_bytes) / (1024.0 * 1024.0);
            auto after_mib = static_cast<double>(result.after_bytes) / (1024.0 * 1024.0);
            double reduction = (result.before_bytes > 0)
                ? (1.0 - static_cast<double>(result.after_bytes) /
                         static_cast<double>(result.before_bytes)) * 100.0
                : 0.0;
            spdlog::info("compaction complete: {:.1f}MiB -> {:.1f}MiB ({:.1f}%) in {}ms",
                         before_mib, after_mib, reduction, result.duration_ms);
            last_compaction_time_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            ++compaction_count_;
        }
    }
}

// =============================================================================
// Timer cancellation
// =============================================================================

void SyncOrchestrator::cancel_timers() {
    if (sync_timer_) sync_timer_->cancel();
    if (expiry_timer_) expiry_timer_->cancel();
    if (cursor_compaction_timer_) cursor_compaction_timer_->cancel();
    if (compaction_timer_) compaction_timer_->cancel();
}

// =============================================================================
// Config reload
// =============================================================================

void SyncOrchestrator::set_sync_config(uint32_t cooldown, uint32_t max_sessions,
                                        uint32_t safety_net_interval) {
    sync_cooldown_seconds_ = cooldown;
    max_sync_sessions_ = max_sessions;
    safety_net_interval_seconds_ = safety_net_interval;
}

void SyncOrchestrator::set_rate_limit(uint64_t bytes_per_sec) {
    rate_limit_bytes_per_sec_ = bytes_per_sec;
}

void SyncOrchestrator::set_compaction_interval(uint32_t hours) {
    compaction_interval_hours_ = hours;
}

void SyncOrchestrator::set_cursor_config(uint32_t full_resync_interval, uint64_t stale_seconds) {
    full_resync_interval_ = full_resync_interval;
    cursor_stale_seconds_ = stale_seconds;
}

} // namespace chromatindb::peer
