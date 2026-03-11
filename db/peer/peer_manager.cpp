#include "db/peer/peer_manager.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fcntl.h>    // O_WRONLY, O_CREAT, O_TRUNC, O_RDONLY, O_DIRECTORY
#include <unistd.h>   // ::write, ::fsync, ::close, ::open

#include <algorithm>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace chromatindb::peer {

namespace {

/// Convert bytes to hex string (for logging).
std::string to_hex(std::span<const uint8_t> bytes, size_t max_len = 8) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    size_t len = std::min(bytes.size(), max_len);
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result += hex_chars[(bytes[i] >> 4) & 0xF];
        result += hex_chars[bytes[i] & 0xF];
    }
    return result;
}

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

/// Convert 64-char hex string to 32-byte namespace ID.
/// Caller must ensure hex.size() == 64 and all chars are valid hex.
std::array<uint8_t, 32> hex_to_namespace(const std::string& hex) {
    std::array<uint8_t, 32> result{};
    for (size_t i = 0; i < 32; ++i) {
        auto byte_str = hex.substr(i * 2, 2);
        result[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
    }
    return result;
}

} // anonymous namespace

PeerManager::PeerManager(const config::Config& config,
                         identity::NodeIdentity& identity,
                         engine::BlobEngine& engine,
                         storage::Storage& storage,
                         asio::io_context& ioc,
                         acl::AccessControl& acl,
                         const std::filesystem::path& config_path)
    : config_(config)
    , identity_(identity)
    , engine_(engine)
    , storage_(storage)
    , ioc_(ioc)
    , acl_(acl)
    , server_(config, identity, ioc)
    , sync_proto_(engine, storage)
    , sighup_signal_(ioc)
    , sigusr1_signal_(ioc)
    , config_path_(config_path) {
    // Initialize rate limit parameters from config
    rate_limit_bytes_per_sec_ = config.rate_limit_bytes_per_sec;
    rate_limit_burst_ = config.rate_limit_burst;

    // Initialize namespace filter from config
    for (const auto& hex : config.sync_namespaces) {
        sync_namespaces_.insert(hex_to_namespace(hex));
    }

    // Track bootstrap addresses
    for (const auto& addr : config.bootstrap_peers) {
        bootstrap_addresses_.insert(addr);
        known_addresses_.insert(addr);
    }

    // Wire server callbacks
    server_.set_accept_filter([this]() { return should_accept_connection(); });

    server_.set_on_connected([this](net::Connection::Ptr conn) {
        on_peer_connected(conn);
    });

    server_.set_on_disconnected([this](net::Connection::Ptr conn) {
        on_peer_disconnected(conn);
    });

    // Set up sync-received blob notification callback
    sync_proto_.set_on_blob_ingested(
        [this](const std::array<uint8_t, 32>& ns, const std::array<uint8_t, 32>& hash,
               uint64_t seq, uint32_t size, bool tombstone) {
            notify_subscribers(ns, hash, seq, size, tombstone);
        });
}

void PeerManager::start() {
    start_time_ = std::chrono::steady_clock::now();

    // Log access control mode
    if (acl_.is_closed_mode()) {
        spdlog::info("access control: closed mode ({} allowed keys)", acl_.allowed_count());
    } else {
        spdlog::info("access control: open mode");
    }

    // Set up SIGHUP handler for config reload (only if config path was provided)
    if (!config_path_.empty()) {
        setup_sighup_handler();
        spdlog::info("SIGHUP reload: enabled (config: {})", config_path_.string());
    } else {
        spdlog::info("SIGHUP reload: disabled (no --config provided)");
    }

    // Set up SIGUSR1 handler for metrics dump (always enabled, no config dependency)
    setup_sigusr1_handler();

    // Load persisted peers before starting server
    load_persisted_peers();

    server_.start();

    // Register shutdown callback (save peers before drain)
    server_.set_on_shutdown([this]() {
        stopping_ = true;
        save_persisted_peers();  // Save while connection list is still accurate
        sighup_signal_.cancel();
        sigusr1_signal_.cancel();
        if (expiry_timer_) expiry_timer_->cancel();
    });

    // Connect to persisted peers (in addition to bootstrap peers from server_.start())
    for (const auto& pp : persisted_peers_) {
        if (!bootstrap_addresses_.count(pp.address)) {
            known_addresses_.insert(pp.address);
            server_.connect_once(pp.address);
        }
    }

    // Start periodic sync timer
    asio::co_spawn(ioc_, sync_timer_loop(), asio::detached);

    // Start periodic peer exchange timer
    asio::co_spawn(ioc_, pex_timer_loop(), asio::detached);

    // Start periodic peer list flush timer (30s)
    asio::co_spawn(ioc_, peer_flush_timer_loop(), asio::detached);

    // Start cancellable expiry scan coroutine
    asio::co_spawn(ioc_, expiry_scan_loop(), asio::detached);

    // Start periodic metrics log timer (60s)
    asio::co_spawn(ioc_, metrics_timer_loop(), asio::detached);
}

void PeerManager::stop() {
    stopping_ = true;
    sighup_signal_.cancel();
    sigusr1_signal_.cancel();
    if (expiry_timer_) expiry_timer_->cancel();
    server_.stop();
}

int PeerManager::exit_code() const {
    return server_.exit_code();
}

size_t PeerManager::peer_count() const {
    return peers_.size();
}

size_t PeerManager::bootstrap_peer_count() const {
    size_t count = 0;
    for (const auto& p : peers_) {
        if (p.is_bootstrap) ++count;
    }
    return count;
}

// =============================================================================
// Connection callbacks
// =============================================================================

bool PeerManager::should_accept_connection() {
    return peers_.size() < config_.max_peers;
}

void PeerManager::on_peer_connected(net::Connection::Ptr conn) {
    // ACL check: derive peer namespace hash and verify against allowed list.
    // Must happen BEFORE adding to peers_ so unauthorized peers never see data.
    auto peer_ns = crypto::sha3_256(conn->peer_pubkey());
    if (!acl_.is_allowed(std::span<const uint8_t, 32>(peer_ns))) {
        auto full_hex = to_hex(std::span<const uint8_t>(peer_ns.data(), peer_ns.size()), 32);
        spdlog::warn("access denied: namespace={} ip={}", full_hex, conn->remote_address());
        conn->close();  // Silent close, no goodbye
        return;
    }

    PeerInfo info;
    info.connection = conn;
    info.address = conn->remote_address();
    info.is_bootstrap = false;
    info.strike_count = 0;
    info.syncing = false;

    // Initialize token bucket for rate limiting (full burst capacity on connect)
    info.bucket_tokens = rate_limit_burst_;
    info.bucket_last_refill = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // Check if this connection is to a bootstrap peer
    // Note: remote_address includes port, bootstrap_addresses may not match exactly
    // We check if any bootstrap address is a prefix of the remote address
    for (const auto& bp : bootstrap_addresses_) {
        // Simple check: bootstrap is "host:port", remote is "host:port"
        // They may differ in port (bootstrap is listen port, remote is ephemeral)
        // So we check host part only
        auto bp_colon = bp.rfind(':');
        auto ra_colon = info.address.rfind(':');
        if (bp_colon != std::string::npos && ra_colon != std::string::npos) {
            auto bp_host = bp.substr(0, bp_colon);
            auto ra_host = info.address.substr(0, ra_colon);
            if (bp_host == ra_host) {
                info.is_bootstrap = true;
                break;
            }
        }
    }

    auto ns_hex = to_hex(conn->peer_pubkey());
    spdlog::info("Connected to peer {}@{}", ns_hex, info.address);

    // Set up message routing
    conn->on_message([this](net::Connection::Ptr c, wire::TransportMsgType type,
                            std::vector<uint8_t> payload) {
        on_peer_message(c, type, std::move(payload));
    });

    peers_.push_back(info);
    peers_.back().sync_inbox.clear();
    ++metrics_.peers_connected_total;

    // Track this peer's address
    known_addresses_.insert(info.address);

    // Persist successful connection
    update_persisted_peer(info.address, true);

    // Only the initiator (outbound) side triggers sync on connect.
    // The responder (inbound) side waits for SyncRequest from the peer.
    // This avoids both sides sending SyncRequest simultaneously.
    //
    // PEX exchange happens inline after sync completes (within the same coroutine)
    // to avoid concurrent sends that would desync AEAD nonces.
    if (conn->is_initiator()) {
        asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
            co_await run_sync_with_peer(conn);
        }, asio::detached);
    }
}

void PeerManager::on_peer_disconnected(net::Connection::Ptr conn) {
    auto ns_hex = to_hex(conn->peer_pubkey());
    bool graceful = conn->received_goodbye();
    spdlog::info("Peer {} disconnected ({})", ns_hex,
                 graceful ? "graceful" : "timeout");

    // Remove from peers
    peers_.erase(
        std::remove_if(peers_.begin(), peers_.end(),
                       [&conn](const PeerInfo& p) {
                           return p.connection == conn;
                       }),
        peers_.end());
    ++metrics_.peers_disconnected_total;
}

// =============================================================================
// Message routing
// =============================================================================

void PeerManager::on_peer_message(net::Connection::Ptr conn,
                                   wire::TransportMsgType type,
                                   std::vector<uint8_t> payload) {
    // Handle sync messages
    if (type == wire::TransportMsgType_SyncRequest) {
        // Peer wants to sync with us -- handle as responder
        auto* peer = find_peer(conn);
        if (peer && !peer->syncing) {
            peer->sync_inbox.clear();  // Fresh sync session
            asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
                co_await handle_sync_as_responder(conn);
            }, asio::detached);
        }
        return;
    }

    // PeerListRequest: if peer is not syncing, start a PEX responder coroutine.
    // If syncing, route through the inbox (handled inline by the sync coroutine).
    if (type == wire::TransportMsgType_PeerListRequest) {
        auto* peer = find_peer(conn);
        if (peer) {
            if (peer->syncing) {
                route_sync_message(peer, type, std::move(payload));
            } else {
                // Start PEX responder: push the message into inbox first, then spawn handler
                route_sync_message(peer, type, std::move(payload));
                asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
                    co_await handle_pex_as_responder(conn);
                }, asio::detached);
            }
        }
        return;
    }

    if (type == wire::TransportMsgType_SyncAccept ||
        type == wire::TransportMsgType_NamespaceList ||
        type == wire::TransportMsgType_HashList ||
        type == wire::TransportMsgType_BlobRequest ||
        type == wire::TransportMsgType_BlobTransfer ||
        type == wire::TransportMsgType_SyncComplete ||
        type == wire::TransportMsgType_PeerListResponse) {
        auto* peer = find_peer(conn);
        if (peer) {
            route_sync_message(peer, type, std::move(payload));
        }
        return;
    }

    if (type == wire::TransportMsgType_Subscribe) {
        auto* peer = find_peer(conn);
        if (peer) {
            auto namespaces = decode_namespace_list(payload);
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
            auto namespaces = decode_namespace_list(payload);
            for (const auto& ns : namespaces) {
                peer->subscribed_namespaces.erase(ns);
            }
            spdlog::debug("Peer {} unsubscribed from {} namespaces (total: {})",
                         peer_display_name(conn), namespaces.size(),
                         peer->subscribed_namespaces.size());
        }
        return;
    }

    // Rate limiting: check before Data/Delete processing (Step 0 pattern).
    // Sync messages (BlobTransfer, SyncRequest, etc.) are never rate-checked.
    if ((type == wire::TransportMsgType_Data || type == wire::TransportMsgType_Delete) &&
        rate_limit_bytes_per_sec_ > 0) {
        auto* peer = find_peer(conn);
        if (peer && !try_consume_tokens(*peer, payload.size(),
                                         rate_limit_bytes_per_sec_, rate_limit_burst_)) {
            ++metrics_.rate_limited;
            spdlog::warn("rate limit exceeded by peer {} ({} bytes, limit {}B/s), disconnecting",
                         conn->remote_address(), payload.size(), rate_limit_bytes_per_sec_);
            asio::co_spawn(ioc_, conn->close_gracefully(), asio::detached);
            return;
        }
    }

    if (type == wire::TransportMsgType_Delete) {
        // Delete message -- process as blob deletion, send ack via coroutine
        try {
            auto blob = wire::decode_blob(payload);
            auto result = engine_.delete_blob(blob);
            if (result.accepted && result.ack.has_value()) {
                // Build DeleteAck payload: [blob_hash:32][seq_num_be:8][status:1]
                auto ack = result.ack.value();
                asio::co_spawn(ioc_, [conn, ack]() -> asio::awaitable<void> {
                    std::vector<uint8_t> ack_payload(41);
                    std::memcpy(ack_payload.data(), ack.blob_hash.data(), 32);
                    for (int i = 7; i >= 0; --i) {
                        ack_payload[32 + (7 - i)] = static_cast<uint8_t>(
                            ack.seq_num >> (i * 8));
                    }
                    ack_payload[40] = (ack.status == engine::IngestStatus::stored) ? 0 : 1;
                    co_await conn->send_message(wire::TransportMsgType_DeleteAck,
                                                 std::span<const uint8_t>(ack_payload));
                }, asio::detached);
                // Notify subscribers about tombstone
                if (ack.status == engine::IngestStatus::stored) {
                    notify_subscribers(
                        blob.namespace_id,
                        ack.blob_hash,
                        ack.seq_num,
                        static_cast<uint32_t>(blob.data.size()),
                        true);  // tombstone notification
                }
            } else if (result.error.has_value()) {
                spdlog::warn("delete rejected from {}: {}",
                             conn->remote_address(), result.error_detail);
                record_strike(conn, result.error_detail);
            }
        } catch (const std::exception& e) {
            spdlog::warn("malformed delete from {}: {}",
                         conn->remote_address(), e.what());
            record_strike(conn, e.what());
        }
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

    if (type == wire::TransportMsgType_Data) {
        // Data message -- try to ingest as a blob
        try {
            auto blob = wire::decode_blob(payload);
            auto result = engine_.ingest(blob);
            if (result.accepted && result.ack.has_value() &&
                result.ack->status == engine::IngestStatus::stored) {
                notify_subscribers(
                    blob.namespace_id,
                    result.ack->blob_hash,
                    result.ack->seq_num,
                    static_cast<uint32_t>(blob.data.size()),
                    wire::is_tombstone(blob.data));
                ++metrics_.ingests;
            } else if (result.accepted) {
                ++metrics_.ingests;  // Duplicate or already-known blob
            } else if (!result.accepted && result.error.has_value()) {
                ++metrics_.rejections;
                if (*result.error == engine::IngestError::storage_full) {
                    // Send StorageFull to inform peer we cannot accept blobs
                    spdlog::warn("Storage full, notifying peer {}", peer_display_name(conn));
                    asio::co_spawn(ioc_, [conn]() -> asio::awaitable<void> {
                        std::span<const uint8_t> empty{};
                        co_await conn->send_message(wire::TransportMsgType_StorageFull, empty);
                    }, asio::detached);
                } else {
                    spdlog::warn("invalid blob from peer {}: {}",
                                 conn->remote_address(),
                                 result.error_detail.empty() ? "validation failed" : result.error_detail);
                    record_strike(conn, result.error_detail);
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("malformed data message from peer {}: {}",
                         conn->remote_address(), e.what());
            record_strike(conn, e.what());
        }
        return;
    }

    // Other message types ignored (Ping/Pong/Goodbye handled by Connection)
}

// =============================================================================
// Sync message queue
// =============================================================================

void PeerManager::route_sync_message(PeerInfo* peer, wire::TransportMsgType type,
                                      std::vector<uint8_t> payload) {
    peer->sync_inbox.push_back({type, std::move(payload)});
    if (peer->sync_notify) {
        peer->sync_notify->cancel();  // Wake up waiting coroutine
    }
}

asio::awaitable<std::optional<SyncMessage>>
PeerManager::recv_sync_msg(PeerInfo* peer, std::chrono::seconds timeout) {
    if (!peer->sync_inbox.empty()) {
        auto msg = std::move(peer->sync_inbox.front());
        peer->sync_inbox.pop_front();
        co_return msg;
    }
    asio::steady_timer timer(ioc_);
    peer->sync_notify = &timer;
    timer.expires_after(timeout);
    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
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

asio::awaitable<void> PeerManager::run_sync_with_peer(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer || peer->syncing) co_return;
    peer->syncing = true;
    peer->sync_inbox.clear();  // Fresh sync session -- set up BEFORE sending

    sync::SyncStats total_stats;
    constexpr auto SYNC_TIMEOUT = std::chrono::seconds(30);

    // Send SyncRequest
    std::span<const uint8_t> empty{};
    if (!co_await conn->send_message(wire::TransportMsgType_SyncRequest, empty)) {
        peer->syncing = false;
        co_return;
    }

    // Wait for SyncAccept
    auto accept_msg = co_await recv_sync_msg(peer, std::chrono::seconds(5));
    if (!accept_msg || accept_msg->type != wire::TransportMsgType_SyncAccept) {
        spdlog::warn("sync with {}: no SyncAccept received", conn->remote_address());
        peer->syncing = false;
        co_return;
    }

    // Phase A: Send our data
    auto our_namespaces = engine_.list_namespaces();
    auto ns_payload = sync::SyncProtocol::encode_namespace_list(our_namespaces);
    if (!co_await conn->send_message(wire::TransportMsgType_NamespaceList, ns_payload)) {
        peer->syncing = false;
        co_return;
    }

    for (const auto& ns_info : our_namespaces) {
        auto hashes = sync_proto_.collect_namespace_hashes(ns_info.namespace_id);
        auto hl_payload = sync::SyncProtocol::encode_hash_list(ns_info.namespace_id, hashes);
        if (!co_await conn->send_message(wire::TransportMsgType_HashList, hl_payload)) {
            peer->syncing = false;
            co_return;
        }
    }

    if (!co_await conn->send_message(wire::TransportMsgType_SyncComplete, empty)) {
        peer->syncing = false;
        co_return;
    }

    // Phase B: Receive peer's data
    auto ns_msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
    if (!ns_msg || ns_msg->type != wire::TransportMsgType_NamespaceList) {
        spdlog::warn("sync with {}: expected NamespaceList", conn->remote_address());
        peer->syncing = false;
        co_return;
    }
    auto peer_namespaces = sync::SyncProtocol::decode_namespace_list(ns_msg->payload);

    // Collect peer's hash lists
    std::map<std::array<uint8_t, 32>, std::vector<std::array<uint8_t, 32>>> peer_hashes;
    while (true) {
        auto msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
        if (!msg) {
            spdlog::warn("sync with {}: timeout waiting for HashList/SyncComplete",
                         conn->remote_address());
            peer->syncing = false;
            co_return;
        }
        if (msg->type == wire::TransportMsgType_SyncComplete) break;
        if (msg->type == wire::TransportMsgType_HashList) {
            auto [ns, hashes] = sync::SyncProtocol::decode_hash_list(msg->payload);
            peer_hashes[ns] = std::move(hashes);
        }
    }

    // Phase C: Compute diffs and exchange blobs one at a time.
    // Batches BlobRequests to MAX_HASHES_PER_REQUEST hashes per message.
    // Each BlobTransfer carries exactly one blob to keep memory bounded.
    // Uses BLOB_TRANSFER_TIMEOUT (120s) for blob transfers vs SYNC_TIMEOUT (30s) for control.

    for (const auto& [ns, their_hashes] : peer_hashes) {
        auto our_hashes = sync_proto_.collect_namespace_hashes(ns);
        auto missing = sync::SyncProtocol::diff_hashes(our_hashes, their_hashes);
        if (missing.empty()) {
            total_stats.namespaces_synced++;
            continue;
        }

        // Send BlobRequests in batches of MAX_HASHES_PER_REQUEST
        for (size_t i = 0; i < missing.size(); i += MAX_HASHES_PER_REQUEST) {
            size_t batch_end = std::min(i + static_cast<size_t>(MAX_HASHES_PER_REQUEST),
                                        missing.size());
            std::vector<std::array<uint8_t, 32>> batch(
                missing.begin() + static_cast<ptrdiff_t>(i),
                missing.begin() + static_cast<ptrdiff_t>(batch_end));

            auto req_payload = sync::SyncProtocol::encode_hash_list(ns, batch);
            if (!co_await conn->send_message(wire::TransportMsgType_BlobRequest, req_payload)) {
                peer->syncing = false;
                co_return;
            }

            // Receive individual BlobTransfers for this batch.
            // Also handle interleaved BlobRequests from peer.
            uint32_t expected = static_cast<uint32_t>(batch.size());
            uint32_t received = 0;
            while (received < expected) {
                auto msg = co_await recv_sync_msg(peer, BLOB_TRANSFER_TIMEOUT);
                if (!msg) {
                    spdlog::warn("sync: timeout waiting for blob transfer from {}",
                                 conn->remote_address());
                    break;  // Skip remaining, continue next batch
                }

                if (msg->type == wire::TransportMsgType_BlobTransfer) {
                    auto blobs = sync::SyncProtocol::decode_blob_transfer(msg->payload);
                    auto s = sync_proto_.ingest_blobs(blobs);
                    total_stats.blobs_received += s.blobs_received;
                    total_stats.storage_full_count += s.storage_full_count;
                    received++;
                } else if (msg->type == wire::TransportMsgType_BlobRequest) {
                    // Skip outbound blob pushes if peer is full
                    if (peer->peer_is_full) {
                        spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                        continue;
                    }
                    auto [req_ns, requested_hashes] =
                        sync::SyncProtocol::decode_hash_list(msg->payload);
                    for (const auto& hash : requested_hashes) {
                        auto blob = engine_.get_blob(req_ns, hash);
                        if (blob.has_value()) {
                            auto bt_payload =
                                sync::SyncProtocol::encode_single_blob_transfer(*blob);
                            co_await conn->send_message(
                                wire::TransportMsgType_BlobTransfer, bt_payload);
                            total_stats.blobs_sent++;
                        }
                    }
                }
            }
        }

        total_stats.namespaces_synced++;
    }

    // Handle remaining BlobRequests from peer (they may still need our blobs)
    while (true) {
        auto msg = co_await recv_sync_msg(peer, std::chrono::seconds(2));
        if (!msg) break;
        if (msg->type == wire::TransportMsgType_BlobRequest) {
            // Skip outbound blob pushes if peer is full
            if (peer->peer_is_full) {
                spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                continue;
            }
            auto [req_ns, requested_hashes] =
                sync::SyncProtocol::decode_hash_list(msg->payload);
            for (const auto& hash : requested_hashes) {
                auto blob = engine_.get_blob(req_ns, hash);
                if (blob.has_value()) {
                    auto bt_payload =
                        sync::SyncProtocol::encode_single_blob_transfer(*blob);
                    co_await conn->send_message(
                        wire::TransportMsgType_BlobTransfer, bt_payload);
                    total_stats.blobs_sent++;
                }
            }
        } else {
            break;
        }
    }

    spdlog::info("Synced with peer {}: received {} blobs, sent {} blobs, {} namespaces",
                 conn->remote_address(),
                 total_stats.blobs_received, total_stats.blobs_sent,
                 total_stats.namespaces_synced);

    // Post-sync StorageFull signal: inform peer if we rejected blobs for capacity
    if (total_stats.storage_full_count > 0) {
        std::span<const uint8_t> empty{};
        co_await conn->send_message(wire::TransportMsgType_StorageFull, empty);
        spdlog::warn("Sent StorageFull to sync peer {} ({} blobs rejected)",
                     peer_display_name(conn), total_stats.storage_full_count);
    }

    // PEX exchange: send PeerListRequest and wait for response (inline, no concurrent send)
    // Skip in closed mode -- don't advertise or discover peers
    if (!acl_.is_closed_mode()) {
        std::span<const uint8_t> empty{};
        if (!co_await conn->send_message(wire::TransportMsgType_PeerListRequest, empty)) {
            spdlog::debug("PEX: failed to send PeerListRequest to {}", conn->remote_address());
            peer->syncing = false;
            co_return;
        }

        // Wait for PeerListResponse (with timeout)
        auto pex_msg = co_await recv_sync_msg(peer, std::chrono::seconds(5));
        if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListResponse) {
            handle_peer_list_response(conn, std::move(pex_msg->payload));
        }
    }

    ++metrics_.syncs;
    peer->syncing = false;
}

asio::awaitable<void> PeerManager::handle_sync_as_responder(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer) co_return;
    peer->syncing = true;

    sync::SyncStats total_stats;
    constexpr auto SYNC_TIMEOUT = std::chrono::seconds(30);

    // Send SyncAccept
    std::span<const uint8_t> empty{};
    co_await conn->send_message(wire::TransportMsgType_SyncAccept, empty);

    // Phase A: Send our data
    auto our_namespaces = engine_.list_namespaces();
    auto ns_payload = sync::SyncProtocol::encode_namespace_list(our_namespaces);
    co_await conn->send_message(wire::TransportMsgType_NamespaceList, ns_payload);

    for (const auto& ns_info : our_namespaces) {
        auto hashes = sync_proto_.collect_namespace_hashes(ns_info.namespace_id);
        auto hl_payload = sync::SyncProtocol::encode_hash_list(ns_info.namespace_id, hashes);
        co_await conn->send_message(wire::TransportMsgType_HashList, hl_payload);
    }

    co_await conn->send_message(wire::TransportMsgType_SyncComplete, empty);

    // Phase B: Receive peer's data
    auto ns_msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
    if (!ns_msg || ns_msg->type != wire::TransportMsgType_NamespaceList) {
        spdlog::warn("sync responder {}: expected NamespaceList", conn->remote_address());
        peer->syncing = false;
        co_return;
    }
    auto peer_namespaces = sync::SyncProtocol::decode_namespace_list(ns_msg->payload);

    std::map<std::array<uint8_t, 32>, std::vector<std::array<uint8_t, 32>>> peer_hashes;
    while (true) {
        auto msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
        if (!msg) {
            spdlog::warn("sync responder {}: timeout waiting for HashList/SyncComplete",
                         conn->remote_address());
            peer->syncing = false;
            co_return;
        }
        if (msg->type == wire::TransportMsgType_SyncComplete) break;
        if (msg->type == wire::TransportMsgType_HashList) {
            auto [ns, hashes] = sync::SyncProtocol::decode_hash_list(msg->payload);
            peer_hashes[ns] = std::move(hashes);
        }
    }

    // Phase C: Compute diffs and exchange blobs one at a time.
    // Same structure as initiator: batched requests, individual transfers, adaptive timeout.
    for (const auto& [ns, their_hashes] : peer_hashes) {
        auto our_hashes = sync_proto_.collect_namespace_hashes(ns);
        auto missing = sync::SyncProtocol::diff_hashes(our_hashes, their_hashes);
        if (missing.empty()) {
            total_stats.namespaces_synced++;
            continue;
        }

        for (size_t i = 0; i < missing.size(); i += MAX_HASHES_PER_REQUEST) {
            size_t batch_end = std::min(i + static_cast<size_t>(MAX_HASHES_PER_REQUEST),
                                        missing.size());
            std::vector<std::array<uint8_t, 32>> batch(
                missing.begin() + static_cast<ptrdiff_t>(i),
                missing.begin() + static_cast<ptrdiff_t>(batch_end));

            auto req_payload = sync::SyncProtocol::encode_hash_list(ns, batch);
            co_await conn->send_message(wire::TransportMsgType_BlobRequest, req_payload);

            uint32_t expected = static_cast<uint32_t>(batch.size());
            uint32_t received = 0;
            while (received < expected) {
                auto msg = co_await recv_sync_msg(peer, BLOB_TRANSFER_TIMEOUT);
                if (!msg) {
                    spdlog::warn("sync responder: timeout waiting for blob transfer from {}",
                                 conn->remote_address());
                    break;
                }

                if (msg->type == wire::TransportMsgType_BlobTransfer) {
                    auto blobs = sync::SyncProtocol::decode_blob_transfer(msg->payload);
                    auto s = sync_proto_.ingest_blobs(blobs);
                    total_stats.blobs_received += s.blobs_received;
                    total_stats.storage_full_count += s.storage_full_count;
                    received++;
                } else if (msg->type == wire::TransportMsgType_BlobRequest) {
                    // Skip outbound blob pushes if peer is full
                    if (peer->peer_is_full) {
                        spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                        continue;
                    }
                    auto [req_ns, requested_hashes] =
                        sync::SyncProtocol::decode_hash_list(msg->payload);
                    for (const auto& hash : requested_hashes) {
                        auto blob = engine_.get_blob(req_ns, hash);
                        if (blob.has_value()) {
                            auto bt_payload =
                                sync::SyncProtocol::encode_single_blob_transfer(*blob);
                            co_await conn->send_message(
                                wire::TransportMsgType_BlobTransfer, bt_payload);
                            total_stats.blobs_sent++;
                        }
                    }
                }
            }
        }

        total_stats.namespaces_synced++;
    }

    // Handle remaining BlobRequests from peer
    while (true) {
        auto msg = co_await recv_sync_msg(peer, std::chrono::seconds(2));
        if (!msg) break;
        if (msg->type == wire::TransportMsgType_BlobRequest) {
            // Skip outbound blob pushes if peer is full
            if (peer->peer_is_full) {
                spdlog::debug("Skipping blob push to full peer {}", peer_display_name(conn));
                continue;
            }
            auto [req_ns, requested_hashes] =
                sync::SyncProtocol::decode_hash_list(msg->payload);
            for (const auto& hash : requested_hashes) {
                auto blob = engine_.get_blob(req_ns, hash);
                if (blob.has_value()) {
                    auto bt_payload =
                        sync::SyncProtocol::encode_single_blob_transfer(*blob);
                    co_await conn->send_message(
                        wire::TransportMsgType_BlobTransfer, bt_payload);
                    total_stats.blobs_sent++;
                }
            }
        } else {
            break;
        }
    }

    spdlog::info("Sync responder {}: received {} blobs, sent {} blobs, {} namespaces",
                 conn->remote_address(),
                 total_stats.blobs_received, total_stats.blobs_sent,
                 total_stats.namespaces_synced);

    // Post-sync StorageFull signal: inform peer if we rejected blobs for capacity
    if (total_stats.storage_full_count > 0) {
        std::span<const uint8_t> empty{};
        co_await conn->send_message(wire::TransportMsgType_StorageFull, empty);
        spdlog::warn("Sent StorageFull to sync peer {} ({} blobs rejected)",
                     peer_display_name(conn), total_stats.storage_full_count);
    }

    // PEX exchange: wait for PeerListRequest from initiator, then respond (inline, no concurrent send)
    // Skip in closed mode -- don't share peer addresses
    if (!acl_.is_closed_mode()) {
        auto pex_msg = co_await recv_sync_msg(peer, std::chrono::seconds(5));
        if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListRequest) {
            auto addresses = build_peer_list_response(conn->remote_address());
            auto payload = encode_peer_list(addresses);
            spdlog::debug("PEX: sending {} peers to {}", addresses.size(), conn->remote_address());
            co_await conn->send_message(wire::TransportMsgType_PeerListResponse,
                                         std::span<const uint8_t>(payload));
        }
    }

    ++metrics_.syncs;
    peer->syncing = false;
}

asio::awaitable<void> PeerManager::sync_all_peers() {
    // Take a snapshot of connection pointers to avoid iterator invalidation
    // (peers_ may be modified during co_await when new peers connect/disconnect)
    std::vector<net::Connection::Ptr> connections;
    for (const auto& peer : peers_) {
        connections.push_back(peer.connection);
    }
    for (const auto& conn : connections) {
        auto* peer = find_peer(conn);
        if (peer && peer->connection->is_authenticated() && !peer->syncing) {
            co_await run_sync_with_peer(peer->connection);
        }
    }
}

asio::awaitable<void> PeerManager::sync_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(config_.sync_interval_seconds));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;

        co_await sync_all_peers();
    }
}

// =============================================================================
// Strike system
// =============================================================================

void PeerManager::record_strike(net::Connection::Ptr conn, const std::string& reason) {
    auto* peer = find_peer(conn);
    if (!peer) return;

    peer->strike_count++;
    spdlog::warn("strike {}/{} for peer {} ({})",
                 peer->strike_count, STRIKE_THRESHOLD,
                 conn->remote_address(), reason);

    if (peer->strike_count >= STRIKE_THRESHOLD) {
        spdlog::warn("disconnecting peer {}: {} validation failures",
                     conn->remote_address(), peer->strike_count);
        asio::co_spawn(ioc_, conn->close_gracefully(), asio::detached);
    }
}

// =============================================================================
// SIGHUP config reload
// =============================================================================

void PeerManager::setup_sighup_handler() {
    sighup_signal_.add(SIGHUP);
    asio::co_spawn(ioc_, sighup_loop(), asio::detached);
}

asio::awaitable<void> PeerManager::sighup_loop() {
    while (!stopping_) {
        auto [ec, sig] = co_await sighup_signal_.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        handle_sighup();
    }
}

void PeerManager::handle_sighup() {
    spdlog::info("SIGHUP received, reloading config...");
    reload_config();
}

void PeerManager::reload_config() {
    spdlog::info("reloading allowed_keys from {}...", config_path_.string());

    // Re-read config file
    config::Config new_cfg;
    try {
        new_cfg = config::load_config(config_path_);
    } catch (const std::exception& e) {
        spdlog::error("config reload failed (invalid JSON): {} (keeping current config)", e.what());
        return;
    }

    // Validate new allowed_keys
    try {
        config::validate_allowed_keys(new_cfg.allowed_keys);
    } catch (const std::exception& e) {
        spdlog::error("config reload rejected (malformed key): {} (keeping current config)", e.what());
        return;
    }

    // Reload ACL
    auto result = acl_.reload(new_cfg.allowed_keys);

    // Log diff summary
    spdlog::info("config reload: +{} keys, -{} keys", result.added, result.removed);

    if (acl_.is_closed_mode()) {
        spdlog::info("access control: closed mode ({} allowed keys)", acl_.allowed_count());
    } else {
        spdlog::info("access control: open mode");
    }

    // Disconnect revoked peers
    if (result.removed > 0 || acl_.is_closed_mode()) {
        disconnect_unauthorized_peers();
    }

    // Reload rate limit parameters
    rate_limit_bytes_per_sec_ = new_cfg.rate_limit_bytes_per_sec;
    rate_limit_burst_ = new_cfg.rate_limit_burst;
    if (rate_limit_bytes_per_sec_ > 0) {
        spdlog::info("config reload: rate_limit={}B/s burst={}B",
                     rate_limit_bytes_per_sec_, rate_limit_burst_);
    } else {
        spdlog::info("config reload: rate_limit=disabled");
    }

    // Reload sync_namespaces
    try {
        config::validate_allowed_keys(new_cfg.sync_namespaces);
    } catch (const std::exception& e) {
        spdlog::error("config reload rejected (malformed sync_namespace): {} (keeping current)", e.what());
        return;
    }
    sync_namespaces_.clear();
    for (const auto& hex : new_cfg.sync_namespaces) {
        sync_namespaces_.insert(hex_to_namespace(hex));
    }
    if (sync_namespaces_.empty()) {
        spdlog::info("config reload: sync_namespaces=all (unrestricted)");
    } else {
        spdlog::info("config reload: sync_namespaces={} namespaces", sync_namespaces_.size());
    }
}

void PeerManager::disconnect_unauthorized_peers() {
    // Take snapshot -- closing connections triggers on_peer_disconnected which modifies peers_
    std::vector<net::Connection::Ptr> to_disconnect;

    for (const auto& peer : peers_) {
        auto peer_ns = crypto::sha3_256(peer.connection->peer_pubkey());
        if (!acl_.is_allowed(std::span<const uint8_t, 32>(peer_ns))) {
            to_disconnect.push_back(peer.connection);
        }
    }

    for (const auto& conn : to_disconnect) {
        auto full_hex = to_hex(std::span<const uint8_t>(conn->peer_pubkey()), 32);
        spdlog::warn("revoking peer: namespace={} ip={}", full_hex, conn->remote_address());
        conn->close();  // Immediate close, no goodbye
    }

    if (!to_disconnect.empty()) {
        spdlog::info("config reload: disconnected {} peer(s)", to_disconnect.size());
    }
}

// =============================================================================
// Expiry scanning (cancellable member coroutine)
// =============================================================================

asio::awaitable<void> PeerManager::expiry_scan_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        expiry_timer_ = &timer;
        timer.expires_after(std::chrono::seconds(60));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        expiry_timer_ = nullptr;
        if (ec || stopping_) co_return;

        auto purged = storage_.run_expiry_scan();
        if (purged > 0) {
            spdlog::info("expiry scan: purged {} blobs", purged);
        }
    }
}

// =============================================================================
// Periodic peer list flush
// =============================================================================

asio::awaitable<void> PeerManager::peer_flush_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(30));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        save_persisted_peers();
    }
}

// =============================================================================
// PEX wire encoding
// =============================================================================

std::vector<uint8_t> PeerManager::encode_peer_list(const std::vector<std::string>& addresses) {
    std::vector<uint8_t> result;
    // [uint16_t count][for each: uint16_t addr_len, utf8 addr_bytes]
    uint16_t count = static_cast<uint16_t>(addresses.size());
    result.push_back(static_cast<uint8_t>(count >> 8));
    result.push_back(static_cast<uint8_t>(count & 0xFF));

    for (const auto& addr : addresses) {
        uint16_t len = static_cast<uint16_t>(addr.size());
        result.push_back(static_cast<uint8_t>(len >> 8));
        result.push_back(static_cast<uint8_t>(len & 0xFF));
        result.insert(result.end(), addr.begin(), addr.end());
    }
    return result;
}

std::vector<std::string> PeerManager::decode_peer_list(std::span<const uint8_t> payload) {
    std::vector<std::string> result;
    if (payload.size() < 2) return result;

    size_t offset = 0;
    uint16_t count = (static_cast<uint16_t>(payload[offset]) << 8) |
                      static_cast<uint16_t>(payload[offset + 1]);
    offset += 2;

    for (uint16_t i = 0; i < count && offset + 2 <= payload.size(); ++i) {
        uint16_t len = (static_cast<uint16_t>(payload[offset]) << 8) |
                        static_cast<uint16_t>(payload[offset + 1]);
        offset += 2;
        if (offset + len > payload.size()) break;
        result.emplace_back(reinterpret_cast<const char*>(payload.data() + offset), len);
        offset += len;
    }
    return result;
}

// =============================================================================
// Pub/Sub notification dispatch
// =============================================================================

void PeerManager::notify_subscribers(
    const std::array<uint8_t, 32>& namespace_id,
    const std::array<uint8_t, 32>& blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone) {
    // Test hook -- fire notification callback if set
    if (on_notification_) {
        on_notification_(namespace_id, blob_hash, seq_num, blob_size, is_tombstone);
    }

    // Build notification payload once
    auto payload = encode_notification(namespace_id, blob_hash, seq_num, blob_size, is_tombstone);

    // Fan out to all subscribed peers
    for (auto& peer : peers_) {
        if (peer.subscribed_namespaces.count(namespace_id)) {
            auto conn = peer.connection;
            auto payload_copy = payload;  // Copy for each co_spawn
            asio::co_spawn(ioc_, [conn, payload_copy = std::move(payload_copy)]() -> asio::awaitable<void> {
                co_await conn->send_message(
                    wire::TransportMsgType_Notification,
                    std::span<const uint8_t>(payload_copy));
            }, asio::detached);
        }
    }
}

// =============================================================================
// Pub/Sub wire encoding
// =============================================================================

std::vector<uint8_t> PeerManager::encode_namespace_list(
    const std::vector<std::array<uint8_t, 32>>& namespaces) {
    std::vector<uint8_t> result(2 + namespaces.size() * 32);
    auto count = static_cast<uint16_t>(namespaces.size());
    result[0] = static_cast<uint8_t>(count >> 8);
    result[1] = static_cast<uint8_t>(count & 0xFF);
    size_t offset = 2;
    for (const auto& ns : namespaces) {
        std::memcpy(result.data() + offset, ns.data(), 32);
        offset += 32;
    }
    return result;
}

std::vector<std::array<uint8_t, 32>> PeerManager::decode_namespace_list(
    std::span<const uint8_t> payload) {
    std::vector<std::array<uint8_t, 32>> result;
    if (payload.size() < 2) return result;
    uint16_t count = (static_cast<uint16_t>(payload[0]) << 8) |
                      static_cast<uint16_t>(payload[1]);
    if (payload.size() != 2 + static_cast<size_t>(count) * 32) return result;
    for (uint16_t i = 0; i < count; ++i) {
        std::array<uint8_t, 32> ns{};
        std::memcpy(ns.data(), payload.data() + 2 + i * 32, 32);
        result.push_back(ns);
    }
    return result;
}

std::vector<uint8_t> PeerManager::encode_notification(
    std::span<const uint8_t, 32> namespace_id,
    std::span<const uint8_t, 32> blob_hash,
    uint64_t seq_num,
    uint32_t blob_size,
    bool is_tombstone) {
    std::vector<uint8_t> result(77);
    std::memcpy(result.data(), namespace_id.data(), 32);
    std::memcpy(result.data() + 32, blob_hash.data(), 32);
    // seq_num big-endian at offset 64
    for (int i = 7; i >= 0; --i) {
        result[64 + (7 - i)] = static_cast<uint8_t>(seq_num >> (i * 8));
    }
    // blob_size big-endian at offset 72
    result[72] = static_cast<uint8_t>(blob_size >> 24);
    result[73] = static_cast<uint8_t>(blob_size >> 16);
    result[74] = static_cast<uint8_t>(blob_size >> 8);
    result[75] = static_cast<uint8_t>(blob_size & 0xFF);
    // is_tombstone at offset 76
    result[76] = is_tombstone ? 1 : 0;
    return result;
}

// =============================================================================
// PEX protocol
// =============================================================================

asio::awaitable<void> PeerManager::run_pex_with_peer(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer || peer->syncing) co_return;
    peer->syncing = true;
    peer->sync_inbox.clear();

    // Send PeerListRequest
    std::span<const uint8_t> empty{};
    if (!co_await conn->send_message(wire::TransportMsgType_PeerListRequest, empty)) {
        spdlog::debug("PEX: failed to send PeerListRequest to {}", conn->remote_address());
        peer->syncing = false;
        co_return;
    }

    // Wait for PeerListResponse
    auto pex_msg = co_await recv_sync_msg(peer, std::chrono::seconds(5));
    if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListResponse) {
        handle_peer_list_response(conn, std::move(pex_msg->payload));
    }

    peer->syncing = false;
}

std::vector<std::string> PeerManager::build_peer_list_response(const std::string& exclude_address) {
    std::vector<std::string> candidates;

    for (const auto& peer : peers_) {
        if (!peer.connection->is_authenticated()) continue;
        if (peer.address == exclude_address) continue;
        if (peer.address == config_.bind_address) continue;
        candidates.push_back(peer.address);
    }

    // Shuffle for random subset
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(candidates.begin(), candidates.end(), gen);

    // Truncate to MAX_PEERS_PER_EXCHANGE
    if (candidates.size() > MAX_PEERS_PER_EXCHANGE) {
        candidates.resize(MAX_PEERS_PER_EXCHANGE);
    }

    return candidates;
}

asio::awaitable<void> PeerManager::handle_pex_as_responder(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer || peer->syncing) co_return;

    // In closed mode, don't respond to PEX requests (no address sharing)
    if (acl_.is_closed_mode()) co_return;

    peer->syncing = true;
    // NOTE: do NOT clear sync_inbox -- PeerListRequest is already queued in it

    // Read PeerListRequest from inbox (already pushed by on_peer_message)
    auto pex_msg = co_await recv_sync_msg(peer, std::chrono::seconds(5));
    if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListRequest) {
        auto addresses = build_peer_list_response(conn->remote_address());
        auto payload = encode_peer_list(addresses);
        spdlog::debug("PEX: sending {} peers to {}", addresses.size(), conn->remote_address());
        co_await conn->send_message(wire::TransportMsgType_PeerListResponse,
                                     std::span<const uint8_t>(payload));
    }

    peer->syncing = false;
}

void PeerManager::handle_peer_list_response(net::Connection::Ptr conn,
                                             std::vector<uint8_t> payload) {
    auto addresses = decode_peer_list(payload);
    spdlog::debug("PEX: received {} peer addresses from {}",
                  addresses.size(), conn->remote_address());

    uint32_t connected = 0;
    for (const auto& addr : addresses) {
        // Skip if we already know about this address
        if (known_addresses_.count(addr)) continue;
        // Skip if it's our own address
        if (addr == config_.bind_address) continue;
        // Skip if we're at max peers
        if (peers_.size() >= config_.max_peers) break;
        // Skip if we've connected enough this round
        if (connected >= MAX_DISCOVERED_PER_ROUND) break;

        known_addresses_.insert(addr);
        server_.connect_once(addr);
        connected++;
    }

    if (connected > 0) {
        spdlog::info("PEX: connecting to {} new peers discovered from {}",
                     connected, conn->remote_address());
    }
}

asio::awaitable<void> PeerManager::pex_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(PEX_INTERVAL_SEC));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;

        // Skip PEX entirely in closed mode -- don't advertise or discover peers
        if (acl_.is_closed_mode()) continue;

        co_await request_peers_from_all();
    }
}

asio::awaitable<void> PeerManager::request_peers_from_all() {
    // Take a snapshot of connection pointers to avoid iterator invalidation
    // (peers_ may be modified during co_await when new peers connect/disconnect)
    std::vector<net::Connection::Ptr> connections;
    for (const auto& peer : peers_) {
        connections.push_back(peer.connection);
    }
    for (const auto& conn : connections) {
        auto* peer = find_peer(conn);
        if (peer && peer->connection->is_authenticated() && !peer->syncing) {
            co_await run_pex_with_peer(peer->connection);
        }
    }
}

// =============================================================================
// Peer persistence
// =============================================================================

std::filesystem::path PeerManager::peers_file_path() const {
    return std::filesystem::path(config_.data_dir) / "peers.json";
}

void PeerManager::load_persisted_peers() {
    auto path = peers_file_path();
    if (!std::filesystem::exists(path)) return;

    try {
        std::ifstream f(path);
        auto j = nlohmann::json::parse(f);

        persisted_peers_.clear();
        for (const auto& entry : j.value("peers", nlohmann::json::array())) {
            PersistedPeer p;
            p.address = entry.value("address", "");
            p.last_seen = entry.value("last_seen", uint64_t(0));
            p.fail_count = entry.value("fail_count", uint32_t(0));
            if (!p.address.empty() && p.fail_count < MAX_PERSIST_FAILURES) {
                persisted_peers_.push_back(std::move(p));
            }
        }

        // Increment fail_count for all loaded peers (reset on successful connect)
        for (auto& p : persisted_peers_) {
            p.fail_count++;
        }

        spdlog::info("loaded {} persisted peers from {}", persisted_peers_.size(), path.string());
        save_persisted_peers();  // Persist incremented fail_counts
    } catch (const std::exception& e) {
        spdlog::warn("failed to load persisted peers: {}", e.what());
        persisted_peers_.clear();
    }
}

void PeerManager::save_persisted_peers() {
    // Prune peers with too many failures
    persisted_peers_.erase(
        std::remove_if(persisted_peers_.begin(), persisted_peers_.end(),
                       [](const PersistedPeer& p) { return p.fail_count >= MAX_PERSIST_FAILURES; }),
        persisted_peers_.end());

    // Cap at MAX_PERSISTED_PEERS (keep most recently seen)
    if (persisted_peers_.size() > MAX_PERSISTED_PEERS) {
        std::sort(persisted_peers_.begin(), persisted_peers_.end(),
                  [](const PersistedPeer& a, const PersistedPeer& b) {
                      return a.last_seen > b.last_seen;
                  });
        persisted_peers_.resize(MAX_PERSISTED_PEERS);
    }

    nlohmann::json j;
    j["peers"] = nlohmann::json::array();
    for (const auto& p : persisted_peers_) {
        j["peers"].push_back({
            {"address", p.address},
            {"last_seen", p.last_seen},
            {"fail_count", p.fail_count}
        });
    }

    auto path = peers_file_path();
    auto tmp_path = std::filesystem::path(path.string() + ".tmp");

    try {
        std::filesystem::create_directories(path.parent_path());
        auto json_str = j.dump(2);

        // Write to temp file via raw POSIX (std::ofstream cannot fsync)
        int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            spdlog::warn("failed to create temp peer file: {}", strerror(errno));
            return;
        }

        auto written = ::write(fd, json_str.data(), json_str.size());
        if (written < 0 || static_cast<size_t>(written) != json_str.size()) {
            ::close(fd);
            std::filesystem::remove(tmp_path);
            spdlog::warn("failed to write temp peer file");
            return;
        }

        if (::fsync(fd) != 0) {
            ::close(fd);
            std::filesystem::remove(tmp_path);
            spdlog::warn("failed to fsync temp peer file");
            return;
        }
        ::close(fd);

        // Atomic rename
        std::filesystem::rename(tmp_path, path);

        // Directory fsync (required on Linux for rename durability)
        int dir_fd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (dir_fd >= 0) {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }

        spdlog::debug("saved {} persisted peers", persisted_peers_.size());
    } catch (const std::exception& e) {
        spdlog::warn("failed to save persisted peers: {}", e.what());
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
    }
}

void PeerManager::update_persisted_peer(const std::string& address, bool success) {
    // Don't persist bootstrap peers (they have their own reconnect logic)
    if (bootstrap_addresses_.count(address)) return;

    auto it = std::find_if(persisted_peers_.begin(), persisted_peers_.end(),
                           [&address](const PersistedPeer& p) { return p.address == address; });

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));

    if (it != persisted_peers_.end()) {
        if (success) {
            it->last_seen = now;
            it->fail_count = 0;
        } else {
            it->fail_count++;
        }
    } else if (success) {
        persisted_peers_.push_back({address, now, 0});
    }

    save_persisted_peers();
}

// =============================================================================
// Helpers
// =============================================================================

PeerInfo* PeerManager::find_peer(const net::Connection::Ptr& conn) {
    for (auto& p : peers_) {
        if (p.connection == conn) return &p;
    }
    return nullptr;
}

std::string PeerManager::peer_display_name(const net::Connection::Ptr& conn) {
    auto ns_hex = to_hex(conn->peer_pubkey());
    return ns_hex + "@" + conn->remote_address();
}

// =============================================================================
// SIGUSR1 metrics dump
// =============================================================================

void PeerManager::setup_sigusr1_handler() {
    sigusr1_signal_.add(SIGUSR1);
    asio::co_spawn(ioc_, sigusr1_loop(), asio::detached);
}

asio::awaitable<void> PeerManager::sigusr1_loop() {
    while (!stopping_) {
        auto [ec, sig] = co_await sigusr1_signal_.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        dump_metrics();
    }
}

void PeerManager::dump_metrics() {
    spdlog::info("=== METRICS DUMP (SIGUSR1) ===");

    // Global counters (same data as periodic line)
    log_metrics_line();

    // Per-peer breakdown
    spdlog::info("  peers: {}", peers_.size());
    for (const auto& peer : peers_) {
        auto ns_hex = to_hex(peer.connection->peer_pubkey(), 4);
        spdlog::info("    {} (ns:{}...)", peer.address, ns_hex);
    }

    // Per-namespace stats via list_namespaces()
    auto namespaces = storage_.list_namespaces();
    spdlog::info("  namespaces: {}", namespaces.size());
    for (const auto& ns : namespaces) {
        auto ns_hex = to_hex(
            std::span<const uint8_t>(ns.namespace_id.data(), ns.namespace_id.size()), 4);
        spdlog::info("    ns:{:>8}... latest_seq={}", ns_hex, ns.latest_seq_num);
    }

    spdlog::info("=== END METRICS DUMP ===");
}

// =============================================================================
// Periodic metrics
// =============================================================================

asio::awaitable<void> PeerManager::metrics_timer_loop() {
    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(60));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || stopping_) co_return;
        log_metrics_line();
    }
}

void PeerManager::log_metrics_line() {
    auto storage_bytes = storage_.used_bytes();
    auto storage_mib = static_cast<double>(storage_bytes) / (1024.0 * 1024.0);
    auto uptime = compute_uptime_seconds();

    // Sum latest_seq_num across namespaces as blob count proxy
    // (O(N namespaces) not O(N blobs), acceptable for periodic logging)
    uint64_t blob_count = 0;
    auto namespaces = storage_.list_namespaces();
    for (const auto& ns : namespaces) {
        blob_count += ns.latest_seq_num;
    }

    spdlog::info("metrics: connections={} blobs={} storage={:.1f}MiB "
                 "syncs={} ingests={} rejections={} uptime={}",
                 peers_.size(),
                 blob_count,
                 storage_mib,
                 metrics_.syncs,
                 metrics_.ingests,
                 metrics_.rejections,
                 uptime);
}

uint64_t PeerManager::compute_uptime_seconds() const {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count());
}

} // namespace chromatindb::peer
