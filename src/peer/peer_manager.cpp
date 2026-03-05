#include "peer/peer_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>

namespace chromatin::peer {

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

} // anonymous namespace

PeerManager::PeerManager(const config::Config& config,
                         identity::NodeIdentity& identity,
                         engine::BlobEngine& engine,
                         storage::Storage& storage,
                         asio::io_context& ioc)
    : config_(config)
    , identity_(identity)
    , engine_(engine)
    , storage_(storage)
    , ioc_(ioc)
    , server_(config, identity, ioc)
    , sync_proto_(engine) {
    // Track bootstrap addresses
    for (const auto& addr : config.bootstrap_peers) {
        bootstrap_addresses_.insert(addr);
    }

    // Wire server callbacks
    server_.set_accept_filter([this]() { return should_accept_connection(); });

    server_.set_on_connected([this](net::Connection::Ptr conn) {
        on_peer_connected(conn);
    });

    server_.set_on_disconnected([this](net::Connection::Ptr conn) {
        on_peer_disconnected(conn);
    });
}

void PeerManager::start() {
    server_.start();

    // Start periodic sync timer
    asio::co_spawn(ioc_, sync_timer_loop(), asio::detached);
}

void PeerManager::stop() {
    stopping_ = true;
    server_.stop();
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
    PeerInfo info;
    info.connection = conn;
    info.address = conn->remote_address();
    info.is_bootstrap = false;
    info.strike_count = 0;
    info.syncing = false;

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

    // Trigger sync on connect
    asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
        co_await run_sync_with_peer(conn);
    }, asio::detached);
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
        if (peer) {
            peer->sync_inbox.clear();  // Fresh sync session
            asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
                co_await handle_sync_as_responder(conn);
            }, asio::detached);
        }
        return;
    }

    if (type == wire::TransportMsgType_SyncAccept ||
        type == wire::TransportMsgType_NamespaceList ||
        type == wire::TransportMsgType_HashList ||
        type == wire::TransportMsgType_BlobRequest ||
        type == wire::TransportMsgType_BlobTransfer ||
        type == wire::TransportMsgType_SyncComplete) {
        auto* peer = find_peer(conn);
        if (peer) {
            route_sync_message(peer, type, std::move(payload));
        }
        return;
    }

    if (type == wire::TransportMsgType_Data) {
        // Data message -- try to ingest as a blob
        try {
            auto blob = wire::decode_blob(payload);
            auto result = engine_.ingest(blob);
            if (!result.accepted && result.error.has_value()) {
                spdlog::warn("invalid blob from peer {}: {}",
                             conn->remote_address(),
                             result.error_detail.empty() ? "validation failed" : result.error_detail);
                record_strike(conn, result.error_detail);
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

    // Phase C: Compute diffs and exchange blobs
    // C1: Request blobs we're missing from peer
    for (const auto& [ns, their_hashes] : peer_hashes) {
        auto our_hashes = sync_proto_.collect_namespace_hashes(ns);
        auto missing = sync::SyncProtocol::diff_hashes(our_hashes, their_hashes);
        if (missing.empty()) {
            total_stats.namespaces_synced++;
            continue;
        }

        // Send BlobRequest (reuse hash_list encoding -- same wire format)
        auto req_payload = sync::SyncProtocol::encode_hash_list(ns, missing);
        if (!co_await conn->send_message(wire::TransportMsgType_BlobRequest, req_payload)) {
            peer->syncing = false;
            co_return;
        }

        // Wait for BlobTransfer response
        auto bt_msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
        if (bt_msg && bt_msg->type == wire::TransportMsgType_BlobTransfer) {
            auto blobs = sync::SyncProtocol::decode_blob_transfer(bt_msg->payload);
            auto s = sync_proto_.ingest_blobs(blobs);
            total_stats.blobs_received += s.blobs_received;
        }
        total_stats.namespaces_synced++;
    }

    // C2: Handle incoming BlobRequests from peer (they may need our blobs)
    // Drain any remaining messages in the inbox (BlobRequests from peer)
    while (!peer->sync_inbox.empty()) {
        auto msg = std::move(peer->sync_inbox.front());
        peer->sync_inbox.pop_front();
        if (msg.type == wire::TransportMsgType_BlobRequest) {
            auto [ns, requested_hashes] = sync::SyncProtocol::decode_hash_list(msg.payload);
            auto blobs = sync_proto_.get_blobs_by_hashes(ns, requested_hashes);
            auto bt_payload = sync::SyncProtocol::encode_blob_transfer(blobs);
            co_await conn->send_message(wire::TransportMsgType_BlobTransfer, bt_payload);
            total_stats.blobs_sent += static_cast<uint32_t>(blobs.size());
        }
    }

    // Also poll briefly for late-arriving BlobRequests
    while (true) {
        auto msg = co_await recv_sync_msg(peer, std::chrono::seconds(2));
        if (!msg) break;  // No more messages
        if (msg->type == wire::TransportMsgType_BlobRequest) {
            auto [ns, requested_hashes] = sync::SyncProtocol::decode_hash_list(msg->payload);
            auto blobs = sync_proto_.get_blobs_by_hashes(ns, requested_hashes);
            auto bt_payload = sync::SyncProtocol::encode_blob_transfer(blobs);
            co_await conn->send_message(wire::TransportMsgType_BlobTransfer, bt_payload);
            total_stats.blobs_sent += static_cast<uint32_t>(blobs.size());
        } else {
            break;  // Unexpected message type, done
        }
    }

    spdlog::info("Synced with peer {}: received {} blobs, sent {} blobs, {} namespaces",
                 conn->remote_address(),
                 total_stats.blobs_received, total_stats.blobs_sent,
                 total_stats.namespaces_synced);

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

    // Phase C: Compute diffs, exchange blobs
    for (const auto& [ns, their_hashes] : peer_hashes) {
        auto our_hashes = sync_proto_.collect_namespace_hashes(ns);
        auto missing = sync::SyncProtocol::diff_hashes(our_hashes, their_hashes);
        if (missing.empty()) {
            total_stats.namespaces_synced++;
            continue;
        }

        auto req_payload = sync::SyncProtocol::encode_hash_list(ns, missing);
        co_await conn->send_message(wire::TransportMsgType_BlobRequest, req_payload);

        auto bt_msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
        if (bt_msg && bt_msg->type == wire::TransportMsgType_BlobTransfer) {
            auto blobs = sync::SyncProtocol::decode_blob_transfer(bt_msg->payload);
            auto s = sync_proto_.ingest_blobs(blobs);
            total_stats.blobs_received += s.blobs_received;
        }
        total_stats.namespaces_synced++;
    }

    // Handle incoming BlobRequests from peer
    while (!peer->sync_inbox.empty()) {
        auto msg = std::move(peer->sync_inbox.front());
        peer->sync_inbox.pop_front();
        if (msg.type == wire::TransportMsgType_BlobRequest) {
            auto [ns, requested_hashes] = sync::SyncProtocol::decode_hash_list(msg.payload);
            auto blobs = sync_proto_.get_blobs_by_hashes(ns, requested_hashes);
            auto bt_payload = sync::SyncProtocol::encode_blob_transfer(blobs);
            co_await conn->send_message(wire::TransportMsgType_BlobTransfer, bt_payload);
            total_stats.blobs_sent += static_cast<uint32_t>(blobs.size());
        }
    }

    while (true) {
        auto msg = co_await recv_sync_msg(peer, std::chrono::seconds(2));
        if (!msg) break;
        if (msg->type == wire::TransportMsgType_BlobRequest) {
            auto [ns, requested_hashes] = sync::SyncProtocol::decode_hash_list(msg->payload);
            auto blobs = sync_proto_.get_blobs_by_hashes(ns, requested_hashes);
            auto bt_payload = sync::SyncProtocol::encode_blob_transfer(blobs);
            co_await conn->send_message(wire::TransportMsgType_BlobTransfer, bt_payload);
            total_stats.blobs_sent += static_cast<uint32_t>(blobs.size());
        } else {
            break;
        }
    }

    spdlog::info("Sync responder {}: received {} blobs, sent {} blobs, {} namespaces",
                 conn->remote_address(),
                 total_stats.blobs_received, total_stats.blobs_sent,
                 total_stats.namespaces_synced);

    peer->syncing = false;
}

asio::awaitable<void> PeerManager::sync_all_peers() {
    for (auto& peer : peers_) {
        if (peer.connection->is_authenticated() && !peer.syncing) {
            co_await run_sync_with_peer(peer.connection);
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

} // namespace chromatin::peer
