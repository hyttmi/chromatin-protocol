#include "peer/peer_manager.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
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
}

void PeerManager::start() {
    // Load persisted peers before starting server
    load_persisted_peers();

    server_.start();

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
    // First, compute all our missing hashes and send all BlobRequests.
    // Then handle all incoming messages (both BlobTransfer responses and BlobRequests from peer).
    // This avoids deadlock when both sides send BlobRequest simultaneously.

    // C1: Send all BlobRequests up front
    uint32_t pending_responses = 0;
    for (const auto& [ns, their_hashes] : peer_hashes) {
        auto our_hashes = sync_proto_.collect_namespace_hashes(ns);
        auto missing = sync::SyncProtocol::diff_hashes(our_hashes, their_hashes);
        if (missing.empty()) {
            total_stats.namespaces_synced++;
            continue;
        }

        auto req_payload = sync::SyncProtocol::encode_hash_list(ns, missing);
        if (!co_await conn->send_message(wire::TransportMsgType_BlobRequest, req_payload)) {
            peer->syncing = false;
            co_return;
        }
        pending_responses++;
        total_stats.namespaces_synced++;
    }

    // C2: Process incoming messages (BlobTransfer responses + BlobRequests from peer)
    // Loop until we've received all expected BlobTransfer responses and no more BlobRequests arrive
    while (pending_responses > 0) {
        auto msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
        if (!msg) break;

        if (msg->type == wire::TransportMsgType_BlobTransfer) {
            auto blobs = sync::SyncProtocol::decode_blob_transfer(msg->payload);
            auto s = sync_proto_.ingest_blobs(blobs);
            total_stats.blobs_received += s.blobs_received;
            pending_responses--;
        } else if (msg->type == wire::TransportMsgType_BlobRequest) {
            auto [ns, requested_hashes] = sync::SyncProtocol::decode_hash_list(msg->payload);
            auto blobs = sync_proto_.get_blobs_by_hashes(ns, requested_hashes);
            auto bt_payload = sync::SyncProtocol::encode_blob_transfer(blobs);
            co_await conn->send_message(wire::TransportMsgType_BlobTransfer, bt_payload);
            total_stats.blobs_sent += static_cast<uint32_t>(blobs.size());
        }
    }

    // C3: Handle any remaining BlobRequests from peer (they may still need our blobs)
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

    spdlog::info("Synced with peer {}: received {} blobs, sent {} blobs, {} namespaces",
                 conn->remote_address(),
                 total_stats.blobs_received, total_stats.blobs_sent,
                 total_stats.namespaces_synced);

    // PEX exchange: send PeerListRequest and wait for response (inline, no concurrent send)
    {
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
    // Same as initiator: send all BlobRequests first, then process responses + requests.
    uint32_t pending_responses = 0;
    for (const auto& [ns, their_hashes] : peer_hashes) {
        auto our_hashes = sync_proto_.collect_namespace_hashes(ns);
        auto missing = sync::SyncProtocol::diff_hashes(our_hashes, their_hashes);
        if (missing.empty()) {
            total_stats.namespaces_synced++;
            continue;
        }

        auto req_payload = sync::SyncProtocol::encode_hash_list(ns, missing);
        co_await conn->send_message(wire::TransportMsgType_BlobRequest, req_payload);
        pending_responses++;
        total_stats.namespaces_synced++;
    }

    // Process incoming messages (BlobTransfer responses + BlobRequests from peer)
    while (pending_responses > 0) {
        auto msg = co_await recv_sync_msg(peer, SYNC_TIMEOUT);
        if (!msg) break;

        if (msg->type == wire::TransportMsgType_BlobTransfer) {
            auto blobs = sync::SyncProtocol::decode_blob_transfer(msg->payload);
            auto s = sync_proto_.ingest_blobs(blobs);
            total_stats.blobs_received += s.blobs_received;
            pending_responses--;
        } else if (msg->type == wire::TransportMsgType_BlobRequest) {
            auto [ns, requested_hashes] = sync::SyncProtocol::decode_hash_list(msg->payload);
            auto blobs = sync_proto_.get_blobs_by_hashes(ns, requested_hashes);
            auto bt_payload = sync::SyncProtocol::encode_blob_transfer(blobs);
            co_await conn->send_message(wire::TransportMsgType_BlobTransfer, bt_payload);
            total_stats.blobs_sent += static_cast<uint32_t>(blobs.size());
        }
    }

    // Handle any remaining BlobRequests from peer
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

    // PEX exchange: wait for PeerListRequest from initiator, then respond (inline, no concurrent send)
    {
        auto pex_msg = co_await recv_sync_msg(peer, std::chrono::seconds(5));
        if (pex_msg && pex_msg->type == wire::TransportMsgType_PeerListRequest) {
            auto addresses = build_peer_list_response(conn->remote_address());
            auto payload = encode_peer_list(addresses);
            spdlog::debug("PEX: sending {} peers to {}", addresses.size(), conn->remote_address());
            co_await conn->send_message(wire::TransportMsgType_PeerListResponse,
                                         std::span<const uint8_t>(payload));
        }
    }

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
    try {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream f(path);
        f << j.dump(2);
        spdlog::debug("saved {} persisted peers to {}", persisted_peers_.size(), path.string());
    } catch (const std::exception& e) {
        spdlog::warn("failed to save persisted peers: {}", e.what());
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

} // namespace chromatin::peer
