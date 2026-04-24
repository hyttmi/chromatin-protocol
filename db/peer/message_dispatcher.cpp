#include "db/peer/message_dispatcher.h"
#include "db/peer/blob_push_manager.h"
#include "db/peer/connection_manager.h"
#include "db/peer/error_codes.h"
#include "db/peer/peer_manager.h"  // for static encode/decode methods
#include "db/peer/pex_manager.h"
#include "db/peer/sync_orchestrator.h"
#include "db/peer/sync_reject.h"
#include "db/crypto/verify_helpers.h"
#include "db/engine/engine.h"
#include "db/logging/logging.h"
#include "db/net/framing.h"
#include "db/storage/storage.h"
#include "db/util/blob_helpers.h"
#include "db/util/endian.h"
#include "db/util/hex.h"
#include "db/version.h"
#include "db/wire/codec.h"
#include "db/wire/transport_generated.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <ctime>

namespace chromatindb::peer {

namespace {

using chromatindb::util::to_hex;

// Wire protocol response status bytes.
constexpr uint8_t STATUS_FOUND     = 0x01;
constexpr uint8_t STATUS_NOT_FOUND = 0x00;
constexpr uint8_t STATUS_EXISTS    = 0x01;
constexpr uint8_t STATUS_MISSING   = 0x00;

/// Token bucket rate limiter: returns true if tokens consumed, false if rate exceeded.
/// Caller must check rate_bytes_per_sec > 0 before calling (0 = disabled).
bool try_consume_tokens(PeerInfo& peer, uint64_t bytes,
                        uint64_t rate_bytes_per_sec, uint64_t burst_bytes) {
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    uint64_t elapsed_ms = now_ms - peer.bucket_last_refill;
    // Cap elapsed to prevent intermediate overflow in multiplication
    uint64_t max_elapsed = (burst_bytes * 1000) / rate_bytes_per_sec + 1;
    elapsed_ms = std::min(elapsed_ms, max_elapsed);
    uint64_t refill = (rate_bytes_per_sec * elapsed_ms) / 1000;
    peer.bucket_tokens = std::min(peer.bucket_tokens + refill, burst_bytes);
    peer.bucket_last_refill = now_ms;
    if (bytes > peer.bucket_tokens) {
        return false;
    }
    peer.bucket_tokens -= bytes;
    return true;
}

} // anonymous namespace

/// Send ErrorResponse(63) with a 2-byte payload [error_code:1][original_type:1].
/// Static free function -- safe to call from coroutine lambdas (no this capture needed).
static asio::awaitable<void> send_error_response(
    net::Connection::Ptr conn,
    uint8_t error_code,
    wire::TransportMsgType original_type,
    uint32_t request_id,
    NodeMetrics& metrics)
{
    std::array<uint8_t, 2> payload = {
        error_code,
        static_cast<uint8_t>(original_type)
    };
    co_await conn->send_message(
        wire::TransportMsgType_ErrorResponse,
        std::span<const uint8_t>(payload.data(), payload.size()),
        request_id);
    ++metrics.error_responses;
}

MessageDispatcher::MessageDispatcher(
    asio::io_context& ioc,
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
    MaxStorageCallback config_max_storage_bytes)
    : ioc_(ioc)
    , pool_(pool)
    , engine_(engine)
    , storage_(storage)
    , metrics_(metrics)
    , stopping_(stopping)
    , sync_namespaces_(sync_namespaces)
    , peers_(peers)
    , sync_(sync)
    , pex_(pex)
    , blob_push_(blob_push)
    , conn_mgr_(conn_mgr)
    , record_strike_(std::move(record_strike))
    , on_blob_ingested_(std::move(on_blob_ingested))
    , metrics_collector_uptime_(std::move(metrics_collector_uptime))
    , config_max_storage_bytes_(std::move(config_max_storage_bytes)) {
}

PeerInfo* MessageDispatcher::find_peer(const net::Connection::Ptr& conn) {
    for (auto& p : peers_) {
        if (p->connection == conn) return p.get();
    }
    return nullptr;
}

std::string MessageDispatcher::peer_display_name(const net::Connection::Ptr& conn) {
    auto* peer = find_peer(conn);
    if (peer) return peer->address.empty() ? conn->remote_address() : peer->address;
    return conn->remote_address();
}

void MessageDispatcher::set_rate_limits(uint64_t bytes_per_sec, uint64_t burst) {
    rate_limit_bytes_per_sec_ = bytes_per_sec;
    rate_limit_burst_ = burst;
}

void MessageDispatcher::on_peer_message(net::Connection::Ptr conn,
                                         wire::TransportMsgType type,
                                         std::vector<uint8_t> payload,
                                         uint32_t request_id) {
    // Track last message time for inactivity detection (CONN-03).
    {
        auto* peer = find_peer(conn);
        if (peer) {
            peer->last_message_time = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
        }
    }

    // Universal byte accounting: all message types consume token bucket bytes.
    if (rate_limit_bytes_per_sec_ > 0) {
        auto* peer = find_peer(conn);
        if (peer && !try_consume_tokens(*peer, payload.size(),
                                         rate_limit_bytes_per_sec_, rate_limit_burst_)) {
            if (type == wire::TransportMsgType_BlobWrite || type == wire::TransportMsgType_Delete) {
                ++metrics_.rate_limited;
                spdlog::warn("rate limit exceeded by peer {} ({} bytes, limit {}B/s), disconnecting",
                             conn->remote_address(), payload.size(), rate_limit_bytes_per_sec_);
                asio::co_spawn(ioc_, conn->close_gracefully(), asio::detached);
                return;
            }
            if (type == wire::TransportMsgType_SyncRequest) {
                auto* sync_peer = find_peer(conn);
                if (sync_peer && sync_peer->syncing) {
                    ++metrics_.sync_rejections;
                    spdlog::debug("sync request from {} dropped: byte rate exceeded + session active",
                                  conn->remote_address());
                    return;
                }
                sync_.send_sync_rejected(conn, SYNC_REJECT_BYTE_RATE);
                ++metrics_.sync_rejections;
                spdlog::debug("sync request from {} rejected: {}",
                              conn->remote_address(),
                              sync_reject_reason_string(SYNC_REJECT_BYTE_RATE));
                return;
            }
            spdlog::debug("byte budget exceeded for peer {} (type={}), routing anyway",
                          conn->remote_address(), static_cast<int>(type));
        }
    }

    // Handle sync messages
    if (type == wire::TransportMsgType_SyncRequest) {
        auto* peer = find_peer(conn);
        if (!peer) return;

        if (peer->syncing) {
            ++metrics_.sync_rejections;
            spdlog::debug("sync request from {} dropped: session active (avoiding concurrent write)",
                          conn->remote_address());
            return;
        }

        if (sync_.sync_cooldown_seconds() > 0) {
            auto now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            uint64_t elapsed_ms = now_ms - peer->last_sync_initiated;
            uint64_t cooldown_ms = static_cast<uint64_t>(sync_.sync_cooldown_seconds()) * 1000;
            if (peer->last_sync_initiated > 0 && elapsed_ms < cooldown_ms) {
                sync_.send_sync_rejected(conn, SYNC_REJECT_COOLDOWN);
                ++metrics_.sync_rejections;
                spdlog::debug("sync request from {} rejected: {} ({} ms remaining)",
                              conn->remote_address(),
                              sync_reject_reason_string(SYNC_REJECT_COOLDOWN),
                              cooldown_ms - elapsed_ms);
                return;
            }
        }

        peer->last_sync_initiated = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        peer->sync_inbox.clear();
        asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
            co_await sync_.handle_sync_as_responder(conn);
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_PeerListRequest) {
        auto* peer = find_peer(conn);
        if (peer) {
            if (peer->syncing) {
                sync_.route_sync_message(peer, type, std::move(payload));
            } else {
                sync_.route_sync_message(peer, type, std::move(payload));
                asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
                    co_await pex_.handle_pex_as_responder(conn);
                }, asio::detached);
            }
        }
        return;
    }

    if (type == wire::TransportMsgType_SyncAccept ||
        type == wire::TransportMsgType_SyncRejected ||
        type == wire::TransportMsgType_NamespaceList ||
        type == wire::TransportMsgType_ReconcileInit ||
        type == wire::TransportMsgType_ReconcileRanges ||
        type == wire::TransportMsgType_ReconcileItems ||
        type == wire::TransportMsgType_BlobRequest ||
        type == wire::TransportMsgType_BlobTransfer ||
        type == wire::TransportMsgType_SyncComplete ||
        type == wire::TransportMsgType_PeerListResponse) {
        auto* peer = find_peer(conn);
        if (peer) {
            sync_.route_sync_message(peer, type, std::move(payload));
        }
        return;
    }

    // Peer-initiated NodeInfoResponse: decode max_blob_data_bytes and snapshot into
    // PeerInfo::advertised_blob_cap (SYNC-01). Only peer-role connections participate
    // in this capability exchange; clients use the same message type synchronously via
    // Connection::recv() and MUST NOT have their response consumed here.
    //
    // Wire layout (from Phase 127 encoder, message_dispatcher.cpp NodeInfoRequest handler):
    //   [version_len:1][version:N][uptime:8][peer_count:4][namespace_count:4]
    //   [total_blobs:8][storage_used:8][storage_max:8]
    //   [max_blob_data_bytes:8 BE]           <-- what we read
    //   [max_frame_bytes:4 BE][rate_limit_bytes_per_sec:8 BE][max_subscriptions:4 BE]
    //   [types_count:1][supported_types:N]
    if (type == wire::TransportMsgType_NodeInfoResponse) {
        auto* peer = find_peer(conn);
        if (!peer || peer->role != net::Role::Peer) {
            // Ignore: either connection vanished, or this response arrived on a
            // non-peer connection (e.g., client role) where we never issued a peer
            // NodeInfoRequest. No strike -- lenient on unexpected responses.
            return;
        }
        try {
            if (payload.size() < 1) {
                throw std::runtime_error("NodeInfoResponse truncated (version_len)");
            }
            size_t off = 0;
            uint8_t ver_len = payload[off++];
            // Skip version, uptime(8) + peer_count(4) + namespace_count(4)
            //      + total_blobs(8) + storage_used(8) + storage_max(8)
            // = 40 fixed bytes between version and max_blob_data_bytes.
            size_t cap_off = static_cast<size_t>(ver_len) + 40 + 1;  // +1 accounts for the already-consumed version_len byte
            // Re-compute off for clarity (equivalent to cap_off - 1 because off already == 1 after version_len).
            off += ver_len;  // skip version string
            off += 8 + 4 + 4 + 8 + 8 + 8;  // skip uptime, peer_count, namespace_count, total_blobs, storage_used, storage_max
            if (off != cap_off) {
                // Sanity check -- must stay in sync with encoder layout.
                throw std::runtime_error("NodeInfoResponse offset math mismatch");
            }
            if (off + 8 > payload.size()) {
                throw std::runtime_error("NodeInfoResponse truncated (max_blob_data_bytes)");
            }
            uint64_t advertised = chromatindb::util::read_u64_be(payload.data() + off);
            if (peer->advertised_blob_cap != 0 && peer->advertised_blob_cap != advertised) {
                // Session-constant per spec; overwrite anyway as a cheap consistency guard.
                spdlog::warn("peer {} re-advertised blob cap: {} -> {}",
                             peer_display_name(conn),
                             peer->advertised_blob_cap, advertised);
            }
            peer->advertised_blob_cap = advertised;
            spdlog::debug("peer {} advertised blob_max_bytes={}",
                          peer_display_name(conn), advertised);
        } catch (const std::exception& e) {
            spdlog::warn("malformed NodeInfoResponse from peer {}: {}",
                         conn->remote_address(), e.what());
            // Do not strike -- malformed cap advertisement does not halt replication
            // (cap stays 0 = "unknown", filter MUST NOT skip per D-01).
        }
        return;
    }

    if (type == wire::TransportMsgType_Subscribe) {
        auto* peer = find_peer(conn);
        if (peer) {
            auto namespaces = PeerManager::decode_namespace_list(payload);
            // RES-01 (D-06/D-07): Per-connection subscription limit
            if (max_subscriptions_ > 0 &&
                peer->subscribed_namespaces.size() + namespaces.size() > max_subscriptions_) {
                spdlog::warn("Subscription limit exceeded for peer {}: {} existing + {} requested > {} max",
                             peer_display_name(conn),
                             peer->subscribed_namespaces.size(),
                             namespaces.size(), max_subscriptions_);
                // D-08: Send rejection using existing QuotaExceeded message type.
                // Must co_spawn because send_message is awaitable and Subscribe handler is inline.
                asio::co_spawn(ioc_, [conn, request_id]() -> asio::awaitable<void> {
                    std::span<const uint8_t> empty{};
                    co_await conn->send_message(wire::TransportMsgType_QuotaExceeded, empty, request_id);
                }, asio::detached);
                return;
            }
            for (const auto& ns : namespaces) {
                peer->subscribed_namespaces.insert(ns);
            }
            spdlog::debug("Peer {} subscribed to {} namespaces (total: {})",
                         peer_display_name(conn), namespaces.size(),
                         peer->subscribed_namespaces.size());
        }
        return;
    }

    if (type == wire::TransportMsgType_Unsubscribe) {
        auto* peer = find_peer(conn);
        if (peer) {
            auto namespaces = PeerManager::decode_namespace_list(payload);
            for (const auto& ns : namespaces) {
                peer->subscribed_namespaces.erase(ns);
            }
            spdlog::debug("Peer {} unsubscribed from {} namespaces (total: {})",
                         peer_display_name(conn), namespaces.size(),
                         peer->subscribed_namespaces.size());
        }
        return;
    }

    // Namespace announce (inline dispatch, not sync inbox -- Pitfall 4)
    if (type == wire::TransportMsgType_SyncNamespaceAnnounce) {
        auto* peer = find_peer(conn);
        if (peer) {
            auto namespaces = PeerManager::decode_namespace_list(payload);
            peer->announced_namespaces.clear();
            for (const auto& ns : namespaces) {
                peer->announced_namespaces.insert(ns);
            }
            peer->announce_received = true;
            if (peer->announce_notify) peer->announce_notify->cancel();
            spdlog::info("peer {} announced {} sync namespaces",
                         peer_display_name(conn),
                         peer->announced_namespaces.empty() ? std::string("all") :
                         std::to_string(peer->announced_namespaces.size()));
        }
        return;
    }

    // Targeted blob fetch (PUSH-05, PUSH-06)
    if (type == wire::TransportMsgType_BlobNotify) {
        blob_push_.on_blob_notify(conn, std::move(payload));
        return;
    }

    if (type == wire::TransportMsgType_BlobFetch) {
        blob_push_.handle_blob_fetch(conn, std::move(payload));
        return;
    }

    if (type == wire::TransportMsgType_BlobFetchResponse) {
        blob_push_.handle_blob_fetch_response(conn, std::move(payload));
        return;
    }

    if (type == wire::TransportMsgType_Delete) {
        // D-07: Delete reuses the BlobWriteBody envelope — the inner Blob
        // is a tombstone (data = [magic:4][target_hash:32]), structurally identical
        // to a regular Blob; only `data` magic differs. target_namespace sits at the
        // envelope layer, not inside the signed Blob (which no longer carries namespace_id).
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                // Verify envelope shape via FlatBuffers verifier.
                auto verifier = flatbuffers::Verifier(payload.data(), payload.size());
                if (!verifier.VerifyBuffer<wire::BlobWriteBody>(nullptr)) {
                    spdlog::warn("malformed Delete envelope from {}: verifier rejected",
                                 conn->remote_address());
                    record_strike_(conn, "Delete: verifier rejected");
                    catch_error = ERROR_DECODE_FAILED;
                    throw std::runtime_error("verifier rejected");
                }
                auto* body = flatbuffers::GetRoot<wire::BlobWriteBody>(payload.data());
                if (!body || !body->target_namespace() ||
                    body->target_namespace()->size() != 32 || !body->blob()) {
                    spdlog::warn("Delete from {} has malformed envelope",
                                 conn->remote_address());
                    record_strike_(conn, "Delete: malformed envelope");
                    catch_error = ERROR_DECODE_FAILED;
                    throw std::runtime_error("malformed envelope");
                }
                std::array<uint8_t, 32> target_namespace;
                std::memcpy(target_namespace.data(), body->target_namespace()->data(), 32);

                auto blob = wire::decode_blob_from_fb(body->blob());

                if (!sync_namespaces_.empty() &&
                    sync_namespaces_.find(target_namespace) == sync_namespaces_.end()) {
                    spdlog::debug("dropping delete for filtered namespace from {}",
                                  conn->remote_address());
                    co_return;
                }
                auto result = co_await engine_.delete_blob(target_namespace, blob, conn);
                co_await asio::post(ioc_, asio::use_awaitable);
                if (result.accepted && result.ack.has_value()) {
                    auto ack = result.ack.value();
                    std::vector<uint8_t> ack_payload(41);
                    std::memcpy(ack_payload.data(), ack.blob_hash.data(), 32);
                    chromatindb::util::store_u64_be(ack_payload.data() + 32, ack.seq_num);
                    ack_payload[40] = (ack.status == engine::IngestStatus::stored) ? 0 : 1;
                    co_await conn->send_message(wire::TransportMsgType_DeleteAck,
                                                 std::span<const uint8_t>(ack_payload), request_id);
                    if (ack.status == engine::IngestStatus::stored) {
                        uint64_t expiry_time = wire::saturating_expiry(blob.timestamp, blob.ttl);
                        on_blob_ingested_(
                            target_namespace,
                            ack.blob_hash,
                            ack.seq_num,
                            static_cast<uint32_t>(blob.data.size()),
                            true,
                            expiry_time,
                            nullptr);
                    }
                } else if (result.error.has_value()) {
                    spdlog::warn("delete rejected from {}: {}",
                                 conn->remote_address(), result.error_detail);
                    record_strike_(conn, result.error_detail);
                    uint8_t err_code = ERROR_VALIDATION_FAILED;
                    if (*result.error == engine::IngestError::pubk_first_violation) {
                        err_code = ERROR_PUBK_FIRST_VIOLATION;
                    } else if (*result.error == engine::IngestError::pubk_mismatch) {
                        err_code = ERROR_PUBK_MISMATCH;
                    } else if (*result.error == engine::IngestError::bomb_ttl_nonzero) {
                        err_code = ERROR_BOMB_TTL_NONZERO;
                    } else if (*result.error == engine::IngestError::bomb_malformed) {
                        err_code = ERROR_BOMB_MALFORMED;
                    } else if (*result.error == engine::IngestError::bomb_delegate_not_allowed) {
                        err_code = ERROR_BOMB_DELEGATE_NOT_ALLOWED;
                    }
                    co_await send_error_response(conn, err_code, wire::TransportMsgType_Delete, request_id, metrics_);
                }
            } catch (const std::exception& e) {
                if (catch_error == 0) {
                    spdlog::warn("malformed delete from {}: {}",
                                 conn->remote_address(), e.what());
                    record_strike_(conn, e.what());
                    catch_error = ERROR_DECODE_FAILED;
                }
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_Delete, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_StorageFull) {
        auto* peer = find_peer(conn);
        if (peer) {
            peer->peer_is_full = true;
            spdlog::info("Peer {} reported storage full, suppressing sync pushes",
                         peer_display_name(conn));
        }
        return;
    }

    if (type == wire::TransportMsgType_QuotaExceeded) {
        spdlog::info("Peer {} reported namespace quota exceeded",
                     peer_display_name(conn));
        return;
    }

    if (type == wire::TransportMsgType_ReadRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                if (payload.size() < 64) {
                    record_strike_(conn, "ReadRequest too short");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_ReadRequest, request_id, metrics_);
                    co_return;
                }
                auto [ns, hash] = chromatindb::util::extract_namespace_hash(std::span{payload});

                auto blob = engine_.get_blob(ns, hash);
                if (blob.has_value()) {
                    if (wire::is_blob_expired(*blob, static_cast<uint64_t>(std::time(nullptr)))) {
                        spdlog::debug("filtered expired blob in ReadRequest");
                        std::vector<uint8_t> response = {STATUS_NOT_FOUND};
                        co_await conn->send_message(wire::TransportMsgType_ReadResponse,
                                                     std::span<const uint8_t>(response), request_id);
                        co_return;
                    }
                    auto encoded = wire::encode_blob(*blob);
                    std::vector<uint8_t> response(1 + encoded.size());
                    response[0] = STATUS_FOUND;
                    std::memcpy(response.data() + 1, encoded.data(), encoded.size());
                    co_await conn->send_message(wire::TransportMsgType_ReadResponse,
                                                 std::span<const uint8_t>(response), request_id);
                } else {
                    std::vector<uint8_t> response = {STATUS_NOT_FOUND};
                    co_await conn->send_message(wire::TransportMsgType_ReadResponse,
                                                 std::span<const uint8_t>(response), request_id);
                }
            } catch (const std::exception& e) {
                spdlog::warn("malformed ReadRequest from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_DECODE_FAILED;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_ReadRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_ListRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                if (payload.size() < 44) {
                    record_strike_(conn, "ListRequest too short");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_ListRequest, request_id, metrics_);
                    co_return;
                }
                auto ns = chromatindb::util::extract_namespace(std::span{payload});
                uint64_t since_seq = chromatindb::util::read_u64_be(payload.data() + 32);
                uint32_t limit = chromatindb::util::read_u32_be(payload.data() + 40);

                // Optional flags byte at offset 44 (length-based detection)
                uint8_t flags = 0;
                std::array<uint8_t, 4> type_filter{};
                bool has_type_filter = false;
                bool include_all = false;

                if (payload.size() >= 45) {
                    flags = payload[44];
                    include_all = (flags & 0x01) != 0;  // bit 0: include_all
                }
                if (payload.size() >= 49 && (flags & 0x02) != 0) {
                    std::memcpy(type_filter.data(), payload.data() + 45, 4);
                    has_type_filter = true;  // bit 1: type_filter present
                }

                constexpr uint32_t MAX_LIST_LIMIT = 100;
                if (limit == 0 || limit > MAX_LIST_LIMIT)
                    limit = MAX_LIST_LIMIT;

                auto refs = storage_.get_blob_refs_since(ns, since_seq, limit + 1);
                bool has_more = (refs.size() > limit);
                if (has_more)
                    refs.resize(limit);

                // Filter (type / expiry / delegation / tombstone) and — for
                // every surviving entry — decode the stored blob to populate
                // data_size / timestamp / ttl for ListResponse metadata. The
                // non-raw path was already loading each blob for TTL expiry;
                // we extend the raw path the same way so ls can surface size
                // and timestamp without a second round-trip.
                uint64_t now = static_cast<uint64_t>(std::time(nullptr));
                std::vector<storage::BlobRef> filtered_refs;
                filtered_refs.reserve(refs.size());
                for (auto& ref : refs) {
                    if (!include_all) {
                        // Skip tombstones / delegations by type prefix alone.
                        if (std::memcmp(ref.blob_type.data(),
                                        wire::TOMBSTONE_MAGIC.data(), 4) == 0) {
                            continue;
                        }
                        if (std::memcmp(ref.blob_type.data(),
                                        wire::DELEGATION_MAGIC.data(), 4) == 0) {
                            continue;
                        }
                    }
                    if (has_type_filter) {
                        if (std::memcmp(ref.blob_type.data(), type_filter.data(), 4) != 0) {
                            continue;
                        }
                    }

                    // One MDBX read per surviving entry to pick up the author
                    // timestamp and ciphertext size. Also serves as the expiry
                    // check in non-raw mode.
                    auto blob = storage_.get_blob(ns, ref.blob_hash);
                    if (!blob) continue;
                    if (!include_all && wire::is_blob_expired(*blob, now)) {
                        continue;
                    }
                    ref.data_size = static_cast<uint64_t>(blob->data.size());
                    ref.timestamp = blob->timestamp;
                    ref.ttl = blob->ttl;
                    filtered_refs.push_back(ref);
                }

                // Wire entry (60 bytes):
                //   hash:32 | seq:8BE | type:4 | size:8BE | timestamp:8BE
                // TTL is intentionally omitted; expiry can be derived when
                // needed via ReadRequest for the specific blob.
                constexpr size_t LIST_ENTRY_SIZE = 60;
                uint32_t count = static_cast<uint32_t>(filtered_refs.size());
                auto body_size = chromatindb::util::checked_mul(static_cast<size_t>(count), LIST_ENTRY_SIZE);
                if (!body_size) { record_strike_(conn, "ListResponse overflow"); co_await send_error_response(conn, ERROR_INTERNAL, wire::TransportMsgType_ListRequest, request_id, metrics_); co_return; }
                auto resp_size = chromatindb::util::checked_add(*body_size, size_t{5});
                if (!resp_size) { record_strike_(conn, "ListResponse overflow"); co_await send_error_response(conn, ERROR_INTERNAL, wire::TransportMsgType_ListRequest, request_id, metrics_); co_return; }
                std::vector<uint8_t> response(*resp_size);
                chromatindb::util::store_u32_be(response.data(), count);
                for (uint32_t i = 0; i < count; ++i) {
                    size_t off = 4 + i * LIST_ENTRY_SIZE;
                    std::memcpy(response.data() + off, filtered_refs[i].blob_hash.data(), 32);
                    chromatindb::util::store_u64_be(response.data() + off + 32, filtered_refs[i].seq_num);
                    std::memcpy(response.data() + off + 40, filtered_refs[i].blob_type.data(), 4);
                    chromatindb::util::store_u64_be(response.data() + off + 44, filtered_refs[i].data_size);
                    chromatindb::util::store_u64_be(response.data() + off + 52, filtered_refs[i].timestamp);
                }
                response[*resp_size - 1] = has_more ? 1 : 0;

                co_await conn->send_message(wire::TransportMsgType_ListResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("malformed ListRequest from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_DECODE_FAILED;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_ListRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_StatsRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                if (payload.size() < 32) {
                    record_strike_(conn, "StatsRequest too short");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_StatsRequest, request_id, metrics_);
                    co_return;
                }
                auto ns = chromatindb::util::extract_namespace(std::span{payload});

                auto quota = storage_.get_namespace_quota(ns);
                auto [byte_limit, count_limit] = engine_.effective_quota(ns);

                std::vector<uint8_t> response(24);
                chromatindb::util::store_u64_be(response.data(), quota.blob_count);
                chromatindb::util::store_u64_be(response.data() + 8, quota.total_bytes);
                chromatindb::util::store_u64_be(response.data() + 16, byte_limit);

                co_await conn->send_message(wire::TransportMsgType_StatsResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("malformed StatsRequest from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_DECODE_FAILED;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_StatsRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_ExistsRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                if (payload.size() < 64) {
                    record_strike_(conn, "ExistsRequest too short");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_ExistsRequest, request_id, metrics_);
                    co_return;
                }
                auto [ns, hash] = chromatindb::util::extract_namespace_hash(std::span{payload});

                auto blob = storage_.get_blob(ns, hash);
                bool exists = blob.has_value() && !wire::is_blob_expired(*blob, static_cast<uint64_t>(std::time(nullptr)));
                if (blob.has_value() && !exists) {
                    spdlog::debug("filtered expired blob in ExistsRequest");
                }

                std::vector<uint8_t> response(33);
                response[0] = exists ? STATUS_EXISTS : STATUS_MISSING;
                std::memcpy(response.data() + 1, hash.data(), 32);

                co_await conn->send_message(wire::TransportMsgType_ExistsResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("malformed ExistsRequest from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_DECODE_FAILED;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_ExistsRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_NodeInfoRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                std::string version = CHROMATINDB_VERSION;
                uint64_t uptime = metrics_collector_uptime_();
                uint32_t peers = static_cast<uint32_t>(peers_.size());
                auto namespaces = storage_.list_namespaces();
                uint32_t ns_count = static_cast<uint32_t>(namespaces.size());

                uint64_t total_blobs = 0;
                for (const auto& ns_info : namespaces)
                    total_blobs += ns_info.latest_seq_num;

                uint64_t storage_used = storage_.used_data_bytes();
                uint64_t storage_max = config_max_storage_bytes_();

                static constexpr uint8_t supported[] = {
                    5, 6, 7, 8,
                    17, 18, 19, 20, 21,
                    30, 31, 32, 33, 34, 35, 36,
                    37, 38, 39, 40,
                    41, 42, 43, 44, 45, 46,
                    47, 48, 49, 50, 51, 52,
                    53, 54, 55, 56, 57, 58,
                    63  // ErrorResponse
                };
                uint8_t types_count = static_cast<uint8_t>(sizeof(supported));

                size_t resp_size = 1 + version.size()
                                 + 8 + 4 + 4 + 8 + 8 + 8   // uptime + peers + ns + total_blobs + storage_used + storage_max
                                 + 8 + 4 + 8 + 4            // NODEINFO-01..04: blob + frame + rate + subs
                                 + 1 + types_count;         // types_count + supported[] tail
                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                response[off++] = static_cast<uint8_t>(version.size());
                std::memcpy(response.data() + off, version.data(), version.size());
                off += version.size();

                chromatindb::util::store_u64_be(response.data() + off, uptime);
                off += 8;

                chromatindb::util::store_u32_be(response.data() + off, peers);
                off += 4;

                chromatindb::util::store_u32_be(response.data() + off, ns_count);
                off += 4;

                chromatindb::util::store_u64_be(response.data() + off, total_blobs);
                off += 8;

                chromatindb::util::store_u64_be(response.data() + off, storage_used);
                off += 8;

                chromatindb::util::store_u64_be(response.data() + off, storage_max);
                off += 8;

                // NODEINFO-01: max_blob_data_bytes (u64 BE) — sourced from live seeded blob_max_bytes_ per Phase 128 D-04.
                chromatindb::util::store_u64_be(response.data() + off, blob_max_bytes_);
                off += 8;

                // NODEINFO-02: max_frame_bytes (u32 BE, per D-04 sourced from framing.h)
                chromatindb::util::store_u32_be(response.data() + off, chromatindb::net::MAX_FRAME_SIZE);
                off += 4;

                // NODEINFO-03: rate_limit_bytes_per_sec (u64 BE, per D-03 — renamed/re-typed from REQ's u32 messages/sec)
                chromatindb::util::store_u64_be(response.data() + off, rate_limit_bytes_per_sec_);
                off += 8;

                // NODEINFO-04: max_subscriptions_per_connection (u32 BE)
                chromatindb::util::store_u32_be(response.data() + off, max_subscriptions_);
                off += 4;

                response[off++] = types_count;
                std::memcpy(response.data() + off, supported, types_count);

                co_await conn->send_message(wire::TransportMsgType_NodeInfoResponse,
                                             std::span<const uint8_t>(response), request_id);

                // Phase 129 SYNC-01 symmetry fix (VERI-05 regression caught
                // 2026-04-24): the SyncTrigger-based NodeInfoRequest only fires
                // on the initiator side, leaving the responder's PeerInfo[peer].
                // advertised_blob_cap = 0 (unknown). That makes the sync-out
                // cap-divergence filter one-sided — correctness is preserved
                // by Phase 128 ingest enforcement, but the skip counter
                // chromatindb_sync_skipped_oversized_total never increments on
                // the responder side, breaking operator visibility (SYNC-04).
                // Reciprocate here when the requester is a peer we don't yet
                // know the cap for. The advertised_blob_cap == 0 guard
                // prevents ping-pong after first exchange.
                auto* peer = find_peer(conn);
                if (peer && peer->role == net::Role::Peer &&
                    peer->advertised_blob_cap == 0) {
                    std::span<const uint8_t> empty{};
                    co_await conn->send_message(
                        wire::TransportMsgType_NodeInfoRequest, empty);
                }
            } catch (const std::exception& e) {
                spdlog::warn("NodeInfoRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_INTERNAL;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_NodeInfoRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_NamespaceListRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                if (payload.size() < 36) {
                    record_strike_(conn, "NamespaceListRequest too short");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_NamespaceListRequest, request_id, metrics_);
                    co_return;
                }

                auto after_ns = chromatindb::util::extract_namespace(std::span{payload});
                uint32_t limit = chromatindb::util::read_u32_be(payload.data() + 32);
                if (limit == 0 || limit > 1000) limit = 100;

                auto all_ns = storage_.list_namespaces();

                std::sort(all_ns.begin(), all_ns.end(),
                    [](const auto& a, const auto& b) {
                        return a.namespace_id < b.namespace_id;
                    });

                static const std::array<uint8_t, 32> zero_ns{};
                auto it = all_ns.begin();
                if (after_ns != zero_ns) {
                    it = std::upper_bound(all_ns.begin(), all_ns.end(), after_ns,
                        [](const auto& cursor, const auto& info) {
                            return cursor < info.namespace_id;
                        });
                }

                std::vector<std::pair<std::array<uint8_t, 32>, uint64_t>> entries;
                uint32_t count = 0;
                for (; it != all_ns.end() && count < limit; ++it, ++count) {
                    auto quota = storage_.get_namespace_quota(it->namespace_id);
                    entries.emplace_back(it->namespace_id, quota.blob_count);
                }
                bool has_more = (it != all_ns.end());

                auto body = chromatindb::util::checked_mul(entries.size(), size_t{40});
                if (!body) { record_strike_(conn, "NamespaceListResponse overflow"); co_await send_error_response(conn, ERROR_INTERNAL, wire::TransportMsgType_NamespaceListRequest, request_id, metrics_); co_return; }
                auto resp_size_opt = chromatindb::util::checked_add(*body, size_t{5});
                if (!resp_size_opt) { record_strike_(conn, "NamespaceListResponse overflow"); co_await send_error_response(conn, ERROR_INTERNAL, wire::TransportMsgType_NamespaceListRequest, request_id, metrics_); co_return; }
                size_t resp_size = *resp_size_opt;
                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                uint32_t entry_count = static_cast<uint32_t>(entries.size());
                chromatindb::util::store_u32_be(response.data() + off, entry_count);
                off += 4;

                response[off++] = has_more ? 0x01 : 0x00;

                for (const auto& [ns_id, blob_count] : entries) {
                    std::memcpy(response.data() + off, ns_id.data(), 32);
                    off += 32;
                    chromatindb::util::store_u64_be(response.data() + off, blob_count);
                    off += 8;
                }

                co_await conn->send_message(wire::TransportMsgType_NamespaceListResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("NamespaceListRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_INTERNAL;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_NamespaceListRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_StorageStatusRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                uint64_t used_data = storage_.used_data_bytes();
                uint64_t max_storage = config_max_storage_bytes_();
                uint64_t tombstones = storage_.count_tombstones();
                auto namespaces = storage_.list_namespaces();
                uint32_t ns_count = static_cast<uint32_t>(namespaces.size());
                uint64_t total_blobs = 0;
                for (const auto& ns_info : namespaces)
                    total_blobs += ns_info.latest_seq_num;
                uint64_t mmap_bytes = storage_.used_bytes();

                std::vector<uint8_t> response(44);
                size_t off = 0;

                chromatindb::util::store_u64_be(response.data() + off, used_data);
                off += 8;
                chromatindb::util::store_u64_be(response.data() + off, max_storage);
                off += 8;
                chromatindb::util::store_u64_be(response.data() + off, tombstones);
                off += 8;
                chromatindb::util::store_u32_be(response.data() + off, ns_count);
                off += 4;
                chromatindb::util::store_u64_be(response.data() + off, total_blobs);
                off += 8;
                chromatindb::util::store_u64_be(response.data() + off, mmap_bytes);
                off += 8;

                co_await conn->send_message(wire::TransportMsgType_StorageStatusResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("StorageStatusRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_INTERNAL;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_StorageStatusRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_NamespaceStatsRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                if (payload.size() < 32) {
                    record_strike_(conn, "NamespaceStatsRequest too short");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_NamespaceStatsRequest, request_id, metrics_);
                    co_return;
                }

                auto ns = chromatindb::util::extract_namespace(std::span{payload});

                auto all_ns = storage_.list_namespaces();
                bool found = false;
                for (const auto& info : all_ns) {
                    if (info.namespace_id == ns) {
                        found = true;
                        break;
                    }
                }

                std::vector<uint8_t> response(41, 0);
                size_t off = 0;

                response[off++] = found ? 0x01 : 0x00;

                if (found) {
                    auto quota = storage_.get_namespace_quota(ns);
                    uint64_t delegation_count = storage_.count_delegations(ns);
                    auto [quota_bytes_limit, quota_count_limit] = engine_.effective_quota(ns);

                    chromatindb::util::store_u64_be(response.data() + off, quota.blob_count);
                    off += 8;
                    chromatindb::util::store_u64_be(response.data() + off, quota.total_bytes);
                    off += 8;
                    chromatindb::util::store_u64_be(response.data() + off, delegation_count);
                    off += 8;
                    chromatindb::util::store_u64_be(response.data() + off, quota_bytes_limit);
                    off += 8;
                    chromatindb::util::store_u64_be(response.data() + off, quota_count_limit);
                    off += 8;
                }

                co_await conn->send_message(wire::TransportMsgType_NamespaceStatsResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("NamespaceStatsRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_DECODE_FAILED;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_NamespaceStatsRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_MetadataRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                if (payload.size() < 64) {
                    record_strike_(conn, "MetadataRequest too short");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_MetadataRequest, request_id, metrics_);
                    co_return;
                }
                auto [ns, hash] = chromatindb::util::extract_namespace_hash(std::span{payload});

                auto blob_opt = storage_.get_blob(ns, hash);
                if (!blob_opt) {
                    std::vector<uint8_t> response(1, STATUS_NOT_FOUND);
                    co_await conn->send_message(wire::TransportMsgType_MetadataResponse,
                                                 std::span<const uint8_t>(response), request_id);
                    co_return;
                }

                if (wire::is_blob_expired(*blob_opt, static_cast<uint64_t>(std::time(nullptr)))) {
                    spdlog::debug("filtered expired blob in MetadataRequest");
                    std::vector<uint8_t> response(1, STATUS_NOT_FOUND);
                    co_await conn->send_message(wire::TransportMsgType_MetadataResponse,
                                                 std::span<const uint8_t>(response), request_id);
                    co_return;
                }

                const auto& blob = *blob_opt;
                uint64_t data_size = blob.data.size();
                // the signed blob no longer carries the full 2592-byte
                // pubkey inline — post-schema-change, the caller gets the 32-byte
                // signer_hint (SHA3 of signing pubkey). To retrieve the full
                // 2592-byte pubkey, clients fetch the PUBK blob from the namespace.
                constexpr uint16_t signer_hint_len = 32;

                uint64_t seq_num = 0;
                auto refs = storage_.get_blob_refs_since(ns, 0, UINT32_MAX);
                for (const auto& ref : refs) {
                    if (ref.blob_hash == hash) {
                        seq_num = ref.seq_num;
                        break;
                    }
                }

                size_t resp_size = 1 + 32 + 8 + 4 + 8 + 8 + 2 + signer_hint_len;
                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                response[off++] = STATUS_FOUND;

                std::memcpy(response.data() + off, hash.data(), 32);
                off += 32;

                chromatindb::util::store_u64_be(response.data() + off, blob.timestamp);
                off += 8;

                chromatindb::util::store_u32_be(response.data() + off, blob.ttl);
                off += 4;

                chromatindb::util::store_u64_be(response.data() + off, data_size);
                off += 8;

                chromatindb::util::store_u64_be(response.data() + off, seq_num);
                off += 8;

                response[off++] = static_cast<uint8_t>(signer_hint_len >> 8);
                response[off++] = static_cast<uint8_t>(signer_hint_len & 0xFF);

                std::memcpy(response.data() + off, blob.signer_hint.data(), signer_hint_len);
                off += signer_hint_len;

                co_await conn->send_message(wire::TransportMsgType_MetadataResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("MetadataRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_DECODE_FAILED;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_MetadataRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_BatchExistsRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                if (payload.size() < 36) {
                    record_strike_(conn, "BatchExistsRequest too short");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_BatchExistsRequest, request_id, metrics_);
                    co_return;
                }
                auto ns = chromatindb::util::extract_namespace(std::span{payload});

                uint32_t count = chromatindb::util::read_u32_be(payload.data() + 32);

                if (count == 0 || count > 1024) {
                    record_strike_(conn, "BatchExistsRequest invalid count");
                    co_await send_error_response(conn, ERROR_VALIDATION_FAILED, wire::TransportMsgType_BatchExistsRequest, request_id, metrics_);
                    co_return;
                }

                size_t expected_size = 36 + static_cast<size_t>(count) * 32;
                if (payload.size() < expected_size) {
                    record_strike_(conn, "BatchExistsRequest payload too short for count");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_BatchExistsRequest, request_id, metrics_);
                    co_return;
                }

                uint64_t now = static_cast<uint64_t>(std::time(nullptr));
                std::vector<uint8_t> response(count);
                for (uint32_t i = 0; i < count; ++i) {
                    std::array<uint8_t, 32> hash{};
                    std::memcpy(hash.data(), payload.data() + 36 + i * 32, 32);
                    auto blob = storage_.get_blob(ns, hash);
                    response[i] = (blob.has_value() && !wire::is_blob_expired(*blob, now)) ? 0x01 : 0x00;
                }

                co_await conn->send_message(wire::TransportMsgType_BatchExistsResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("BatchExistsRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_DECODE_FAILED;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_BatchExistsRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_DelegationListRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                if (payload.size() < 32) {
                    record_strike_(conn, "DelegationListRequest too short");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_DelegationListRequest, request_id, metrics_);
                    co_return;
                }
                auto ns = chromatindb::util::extract_namespace(std::span{payload});

                auto entries = storage_.list_delegations(ns);

                uint32_t entry_count = static_cast<uint32_t>(entries.size());
                auto body = chromatindb::util::checked_mul(static_cast<size_t>(entry_count), size_t{64});
                if (!body) { record_strike_(conn, "DelegationListResponse overflow"); co_await send_error_response(conn, ERROR_INTERNAL, wire::TransportMsgType_DelegationListRequest, request_id, metrics_); co_return; }
                auto resp_size_opt = chromatindb::util::checked_add(size_t{4}, *body);
                if (!resp_size_opt) { record_strike_(conn, "DelegationListResponse overflow"); co_await send_error_response(conn, ERROR_INTERNAL, wire::TransportMsgType_DelegationListRequest, request_id, metrics_); co_return; }
                size_t resp_size = *resp_size_opt;
                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                chromatindb::util::store_u32_be(response.data() + off, entry_count);
                off += 4;

                for (const auto& entry : entries) {
                    std::memcpy(response.data() + off, entry.delegate_pk_hash.data(), 32);
                    off += 32;
                    std::memcpy(response.data() + off, entry.delegation_blob_hash.data(), 32);
                    off += 32;
                }

                co_await conn->send_message(wire::TransportMsgType_DelegationListResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("DelegationListRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_DECODE_FAILED;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_DelegationListRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_BatchReadRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                if (payload.size() < 40) {
                    record_strike_(conn, "BatchReadRequest too short");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_BatchReadRequest, request_id, metrics_);
                    co_return;
                }

                auto ns = chromatindb::util::extract_namespace(std::span{payload});

                uint32_t cap_bytes = chromatindb::util::read_u32_be(payload.data() + 32);

                static constexpr uint32_t MAX_CAP = 4194304;
                if (cap_bytes == 0 || cap_bytes > MAX_CAP)
                    cap_bytes = MAX_CAP;

                uint32_t count = chromatindb::util::read_u32_be(payload.data() + 36);

                if (count == 0 || count > 256) {
                    record_strike_(conn, "BatchReadRequest invalid count");
                    co_await send_error_response(conn, ERROR_VALIDATION_FAILED, wire::TransportMsgType_BatchReadRequest, request_id, metrics_);
                    co_return;
                }

                size_t expected_size = 40 + static_cast<size_t>(count) * 32;
                if (payload.size() < expected_size) {
                    record_strike_(conn, "BatchReadRequest payload too short for count");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_BatchReadRequest, request_id, metrics_);
                    co_return;
                }

                uint8_t truncated = 0x00;
                uint32_t result_count = 0;
                uint64_t cumulative_size = 0;
                struct Entry {
                    uint8_t status;
                    std::array<uint8_t, 32> hash;
                    std::vector<uint8_t> encoded;
                };
                std::vector<Entry> entries;
                entries.reserve(count);

                uint64_t now = static_cast<uint64_t>(std::time(nullptr));
                for (uint32_t i = 0; i < count; ++i) {
                    std::array<uint8_t, 32> hash{};
                    std::memcpy(hash.data(), payload.data() + 40 + i * 32, 32);

                    auto blob = storage_.get_blob(ns, hash);
                    if (!blob) {
                        entries.push_back({0x00, hash, {}});
                        ++result_count;
                        continue;
                    }

                    if (wire::is_blob_expired(*blob, now)) {
                        spdlog::debug("filtered expired blob in BatchReadRequest");
                        entries.push_back({0x00, hash, {}});
                        ++result_count;
                        continue;
                    }

                    auto encoded = wire::encode_blob(*blob);
                    uint64_t blob_size = encoded.size();

                    cumulative_size += blob_size;
                    entries.push_back({0x01, hash, std::move(encoded)});
                    ++result_count;

                    if (cumulative_size >= cap_bytes) {
                        if (i + 1 < count)
                            truncated = 0x01;
                        break;
                    }
                }

                size_t resp_size = 5;
                for (const auto& e : entries) {
                    resp_size += 1 + 32;
                    if (e.status == 0x01)
                        resp_size += 8 + e.encoded.size();
                }

                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                response[off++] = truncated;

                chromatindb::util::store_u32_be(response.data() + off, result_count);
                off += 4;

                for (const auto& e : entries) {
                    response[off++] = e.status;
                    std::memcpy(response.data() + off, e.hash.data(), 32);
                    off += 32;

                    if (e.status == 0x01) {
                        uint64_t sz = e.encoded.size();
                        chromatindb::util::store_u64_be(response.data() + off, sz);
                        off += 8;
                        std::memcpy(response.data() + off, e.encoded.data(), e.encoded.size());
                        off += e.encoded.size();
                    }
                }

                co_await conn->send_message(wire::TransportMsgType_BatchReadResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("BatchReadRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_DECODE_FAILED;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_BatchReadRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_PeerInfoRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                bool trusted = conn->is_uds();
                if (!trusted) {
                    auto addr_str = conn->remote_address();
                    auto colon_pos = addr_str.rfind(':');
                    if (colon_pos != std::string::npos) {
                        asio::error_code ec;
                        auto addr = asio::ip::make_address(addr_str.substr(0, colon_pos), ec);
                        if (!ec)
                            trusted = conn_mgr_.is_trusted_address(addr);
                    }
                }

                uint32_t pc = static_cast<uint32_t>(peers_.size());
                uint32_t bc = 0;
                for (const auto& p : peers_) {
                    if (p->is_bootstrap) ++bc;
                }

                if (!trusted) {
                    std::vector<uint8_t> response(8);
                    chromatindb::util::store_u32_be(response.data(), pc);
                    chromatindb::util::store_u32_be(response.data() + 4, bc);
                    co_await conn->send_message(wire::TransportMsgType_PeerInfoResponse,
                                                 std::span<const uint8_t>(response), request_id);
                    co_return;
                }

                auto now_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());

                size_t resp_size = 8;
                for (const auto& peer : peers_) {
                    resp_size += 2 + peer->address.size() + 1 + 1 + 1 + 8;
                }

                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                chromatindb::util::store_u32_be(response.data() + off, pc);
                off += 4;
                chromatindb::util::store_u32_be(response.data() + off, bc);
                off += 4;

                for (const auto& peer : peers_) {
                    uint16_t addr_len = static_cast<uint16_t>(peer->address.size());
                    response[off++] = static_cast<uint8_t>(addr_len >> 8);
                    response[off++] = static_cast<uint8_t>(addr_len & 0xFF);
                    std::memcpy(response.data() + off, peer->address.data(), addr_len);
                    off += addr_len;
                    response[off++] = peer->is_bootstrap ? 0x01 : 0x00;
                    response[off++] = peer->syncing ? 0x01 : 0x00;
                    response[off++] = peer->peer_is_full ? 0x01 : 0x00;
                    uint64_t duration_ms = (peer->last_message_time > 0) ? (now_ms - peer->last_message_time) : 0;
                    chromatindb::util::store_u64_be(response.data() + off, duration_ms);
                    off += 8;
                }

                co_await conn->send_message(wire::TransportMsgType_PeerInfoResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("PeerInfoRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_INTERNAL;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_PeerInfoRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_TimeRangeRequest) {
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                if (payload.size() < 52) {
                    record_strike_(conn, "TimeRangeRequest too short");
                    co_await send_error_response(conn, ERROR_MALFORMED_PAYLOAD, wire::TransportMsgType_TimeRangeRequest, request_id, metrics_);
                    co_return;
                }

                auto ns = chromatindb::util::extract_namespace(std::span{payload});

                uint64_t start_ts = chromatindb::util::read_u64_be(payload.data() + 32);
                uint64_t end_ts = chromatindb::util::read_u64_be(payload.data() + 40);

                if (start_ts > end_ts) {
                    record_strike_(conn, "TimeRangeRequest invalid range (start > end)");
                    co_await send_error_response(conn, ERROR_VALIDATION_FAILED, wire::TransportMsgType_TimeRangeRequest, request_id, metrics_);
                    co_return;
                }

                uint32_t limit = chromatindb::util::read_u32_be(payload.data() + 48);

                static constexpr uint32_t MAX_LIMIT = 100;
                if (limit == 0 || limit > MAX_LIMIT)
                    limit = MAX_LIMIT;

                static constexpr uint32_t SCAN_LIMIT = 10000;
                auto refs = storage_.get_blob_refs_since(ns, 0, SCAN_LIMIT);

                struct ResultEntry {
                    std::array<uint8_t, 32> blob_hash;
                    uint64_t seq_num;
                    uint64_t timestamp;
                };
                std::vector<ResultEntry> results;
                results.reserve(limit);

                uint8_t truncated = 0x00;

                uint64_t now = static_cast<uint64_t>(std::time(nullptr));
                for (const auto& ref : refs) {
                    // Skip tombstones and delegations using type prefix
                    if (std::memcmp(ref.blob_type.data(), wire::TOMBSTONE_MAGIC.data(), 4) == 0) continue;
                    if (std::memcmp(ref.blob_type.data(), wire::DELEGATION_MAGIC.data(), 4) == 0) continue;

                    auto blob = storage_.get_blob(ns, ref.blob_hash);
                    if (!blob) continue;

                    if (wire::is_blob_expired(*blob, now)) continue;

                    if (blob->timestamp >= start_ts && blob->timestamp <= end_ts) {
                        results.push_back({ref.blob_hash, ref.seq_num, blob->timestamp});
                        if (results.size() >= limit) {
                            truncated = 0x01;
                            break;
                        }
                    }
                }

                if (refs.size() >= SCAN_LIMIT && results.size() < limit)
                    truncated = 0x01;

                uint32_t result_count = static_cast<uint32_t>(results.size());
                auto body = chromatindb::util::checked_mul(static_cast<size_t>(result_count), size_t{48});
                if (!body) { record_strike_(conn, "TimeRangeResponse overflow"); co_await send_error_response(conn, ERROR_INTERNAL, wire::TransportMsgType_TimeRangeRequest, request_id, metrics_); co_return; }
                auto resp_size_opt = chromatindb::util::checked_add(size_t{5}, *body);
                if (!resp_size_opt) { record_strike_(conn, "TimeRangeResponse overflow"); co_await send_error_response(conn, ERROR_INTERNAL, wire::TransportMsgType_TimeRangeRequest, request_id, metrics_); co_return; }
                size_t resp_size = *resp_size_opt;
                std::vector<uint8_t> response(resp_size);
                size_t off = 0;

                response[off++] = truncated;

                chromatindb::util::store_u32_be(response.data() + off, result_count);
                off += 4;

                for (const auto& r : results) {
                    std::memcpy(response.data() + off, r.blob_hash.data(), 32);
                    off += 32;
                    chromatindb::util::store_u64_be(response.data() + off, r.seq_num);
                    off += 8;
                    chromatindb::util::store_u64_be(response.data() + off, r.timestamp);
                    off += 8;
                }

                co_await conn->send_message(wire::TransportMsgType_TimeRangeResponse,
                                             std::span<const uint8_t>(response), request_id);
            } catch (const std::exception& e) {
                spdlog::warn("TimeRangeRequest handler error from {}: {}", conn->remote_address(), e.what());
                record_strike_(conn, e.what());
                catch_error = ERROR_DECODE_FAILED;
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_TimeRangeRequest, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    if (type == wire::TransportMsgType_BlobWrite) {
        // D-07/D-08: BlobWrite envelope carries target_namespace alongside
        // the signed Blob (the inner Blob no longer has namespace_id).
        asio::co_spawn(ioc_, [this, conn, request_id, payload = std::move(payload)]() -> asio::awaitable<void> {
            uint8_t catch_error = 0;
            try {
                // Verify envelope shape via FlatBuffers verifier.
                auto verifier = flatbuffers::Verifier(payload.data(), payload.size());
                if (!verifier.VerifyBuffer<wire::BlobWriteBody>(nullptr)) {
                    spdlog::warn("malformed BlobWrite envelope from {}: verifier rejected",
                                 conn->remote_address());
                    record_strike_(conn, "BlobWrite: verifier rejected");
                    catch_error = ERROR_DECODE_FAILED;
                    throw std::runtime_error("verifier rejected");
                }
                auto* body = flatbuffers::GetRoot<wire::BlobWriteBody>(payload.data());
                if (!body || !body->target_namespace() ||
                    body->target_namespace()->size() != 32 || !body->blob()) {
                    spdlog::warn("BlobWrite from {} has malformed envelope",
                                 conn->remote_address());
                    record_strike_(conn, "BlobWrite: malformed envelope");
                    catch_error = ERROR_DECODE_FAILED;
                    throw std::runtime_error("malformed envelope");
                }
                std::array<uint8_t, 32> target_namespace;
                std::memcpy(target_namespace.data(), body->target_namespace()->data(), 32);

                auto blob = wire::decode_blob_from_fb(body->blob());

                if (!sync_namespaces_.empty() &&
                    sync_namespaces_.find(target_namespace) == sync_namespaces_.end()) {
                    spdlog::debug("dropping BlobWrite for filtered namespace from {}",
                                  conn->remote_address());
                    co_return;
                }
                auto result = co_await engine_.ingest(target_namespace, blob, conn);
                co_await asio::post(ioc_, asio::use_awaitable);
                if (result.accepted && result.ack.has_value()) {
                    auto ack = result.ack.value();
                    std::vector<uint8_t> ack_payload(41);
                    std::memcpy(ack_payload.data(), ack.blob_hash.data(), 32);
                    chromatindb::util::store_u64_be(ack_payload.data() + 32, ack.seq_num);
                    ack_payload[40] = (ack.status == engine::IngestStatus::stored) ? 0 : 1;
                    co_await conn->send_message(wire::TransportMsgType_WriteAck,
                                                 std::span<const uint8_t>(ack_payload), request_id);
                }
                if (result.accepted && result.ack.has_value() &&
                    result.ack->status == engine::IngestStatus::stored) {
                    uint64_t expiry_time = wire::saturating_expiry(blob.timestamp, blob.ttl);
                    on_blob_ingested_(
                        target_namespace,
                        result.ack->blob_hash,
                        result.ack->seq_num,
                        static_cast<uint32_t>(blob.data.size()),
                        wire::is_tombstone(blob.data),
                        expiry_time,
                        nullptr);
                    ++metrics_.ingests;
                } else if (result.accepted) {
                    ++metrics_.ingests;
                } else if (!result.accepted && result.error.has_value()) {
                    ++metrics_.rejections;
                    if (*result.error == engine::IngestError::storage_full) {
                        spdlog::warn("Storage full, notifying peer {}", peer_display_name(conn));
                        std::span<const uint8_t> empty{};
                        co_await conn->send_message(wire::TransportMsgType_StorageFull, empty, request_id);
                    } else if (*result.error == engine::IngestError::quota_exceeded) {
                        spdlog::warn("Namespace quota exceeded, notifying peer {}",
                                     peer_display_name(conn));
                        ++metrics_.quota_rejections;
                        std::span<const uint8_t> empty{};
                        co_await conn->send_message(wire::TransportMsgType_QuotaExceeded, empty, request_id);
                    } else if (*result.error == engine::IngestError::timestamp_rejected) {
                        spdlog::debug("BlobWrite from {}: timestamp rejected ({})",
                                      conn->remote_address(),
                                      result.error_detail.empty() ? "unknown" : result.error_detail);
                    } else if (*result.error == engine::IngestError::pubk_first_violation) {
                        spdlog::warn("BlobWrite from {}: PUBK-first violation ({})",
                                     conn->remote_address(), result.error_detail);
                        record_strike_(conn, result.error_detail);
                        co_await send_error_response(conn, ERROR_PUBK_FIRST_VIOLATION, wire::TransportMsgType_BlobWrite, request_id, metrics_);
                    } else if (*result.error == engine::IngestError::pubk_mismatch) {
                        spdlog::warn("BlobWrite from {}: PUBK mismatch ({})",
                                     conn->remote_address(), result.error_detail);
                        record_strike_(conn, result.error_detail);
                        co_await send_error_response(conn, ERROR_PUBK_MISMATCH, wire::TransportMsgType_BlobWrite, request_id, metrics_);
                    } else if (*result.error == engine::IngestError::bomb_ttl_nonzero) {
                        spdlog::warn("BlobWrite from {}: BOMB ttl!=0 ({})",
                                     conn->remote_address(), result.error_detail);
                        record_strike_(conn, result.error_detail);
                        co_await send_error_response(conn, ERROR_BOMB_TTL_NONZERO, wire::TransportMsgType_BlobWrite, request_id, metrics_);
                    } else if (*result.error == engine::IngestError::bomb_malformed) {
                        spdlog::warn("BlobWrite from {}: BOMB malformed ({})",
                                     conn->remote_address(), result.error_detail);
                        record_strike_(conn, result.error_detail);
                        co_await send_error_response(conn, ERROR_BOMB_MALFORMED, wire::TransportMsgType_BlobWrite, request_id, metrics_);
                    } else if (*result.error == engine::IngestError::bomb_delegate_not_allowed) {
                        spdlog::warn("BlobWrite from {}: delegate BOMB rejected ({})",
                                     conn->remote_address(), result.error_detail);
                        record_strike_(conn, result.error_detail);
                        co_await send_error_response(conn, ERROR_BOMB_DELEGATE_NOT_ALLOWED, wire::TransportMsgType_BlobWrite, request_id, metrics_);
                    } else {
                        spdlog::warn("invalid blob from peer {}: {}",
                                     conn->remote_address(),
                                     result.error_detail.empty() ? "validation failed" : result.error_detail);
                        record_strike_(conn, result.error_detail);
                        co_await send_error_response(conn, ERROR_VALIDATION_FAILED, wire::TransportMsgType_BlobWrite, request_id, metrics_);
                    }
                }
            } catch (const std::exception& e) {
                if (catch_error == 0) {
                    spdlog::warn("malformed BlobWrite from peer {}: {}",
                                 conn->remote_address(), e.what());
                    record_strike_(conn, e.what());
                    catch_error = ERROR_DECODE_FAILED;
                }
            }
            if (catch_error) {
                co_await send_error_response(conn, catch_error, wire::TransportMsgType_BlobWrite, request_id, metrics_);
            }
        }, asio::detached);
        return;
    }

    // Ping/Pong/Goodbye handled by Connection before dispatch.
    // Log anything else so protocol mismatches don't disappear silently.
    spdlog::debug("unrecognized message type {} from {}",
                  static_cast<int>(type), conn->remote_address());
    record_strike_(conn, "unrecognized message type");
    asio::co_spawn(ioc_, [conn, type, request_id, &metrics = metrics_]() -> asio::awaitable<void> {
        co_await send_error_response(conn, ERROR_UNKNOWN_TYPE, type, request_id, metrics);
    }, asio::detached);
}

} // namespace chromatindb::peer
