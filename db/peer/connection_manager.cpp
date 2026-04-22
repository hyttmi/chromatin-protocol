#include "db/peer/connection_manager.h"
#include "db/peer/peer_manager.h"  // for PeerManager::encode_namespace_list (static)
#include "db/acl/access_control.h"
#include "db/crypto/hash.h"
#include "db/identity/identity.h"
#include "db/net/connection.h"
#include "db/net/server.h"
#include "db/util/endian.h"
#include "db/util/hex.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace chromatindb::peer {

namespace {
using chromatindb::util::to_hex;
} // anonymous namespace

ConnectionManager::ConnectionManager(
    identity::NodeIdentity& identity,
    acl::AccessControl& acl,
    NodeMetrics& metrics,
    net::Server& server,
    asio::io_context& ioc,
    const bool& stopping,
    const std::set<std::array<uint8_t, 32>>& sync_namespaces,
    uint64_t rate_limit_burst,
    uint32_t max_peers,
    uint32_t max_clients,
    uint32_t strike_threshold,
    MessageCallback on_message,
    SyncTrigger sync_trigger,
    ConnectCallback on_connect,
    DisconnectCallback on_disconnect)
    : identity_(identity)
    , acl_(acl)
    , metrics_(metrics)
    , server_(server)
    , ioc_(ioc)
    , stopping_(stopping)
    , sync_namespaces_(sync_namespaces)
    , on_message_(std::move(on_message))
    , sync_trigger_(std::move(sync_trigger))
    , on_connect_(std::move(on_connect))
    , on_disconnect_(std::move(on_disconnect))
    , rate_limit_burst_(rate_limit_burst)
    , max_peers_(max_peers)
    , max_clients_(max_clients)
    , strike_threshold_(strike_threshold) {}

// =============================================================================
// Connection lifecycle
// =============================================================================

bool ConnectionManager::should_accept_connection() const {
    size_t peer_count = 0;
    size_t client_count = 0;
    for (const auto& p : peers_) {
        if (p->role == net::Role::Client) ++client_count;
        else ++peer_count;
    }
    return peer_count < max_peers_ && client_count < max_clients_;
}

void ConnectionManager::on_peer_connected(net::Connection::Ptr conn) {
    // ACL check: route based on the role the remote explicitly declared in
    // its AuthSignature payload. UDS overrides to Client regardless of what
    // was declared -- the UDS socket is permission-gated at the filesystem
    // level and is only ever used by local CLI tools, never for node peering.
    auto peer_ns = crypto::sha3_256(conn->peer_pubkey());

    net::Role role = conn->is_uds() ? net::Role::Client : conn->peer_role();
    bool allowed = (role == net::Role::Client)
        ? acl_.is_client_allowed(std::span<const uint8_t, 32>(peer_ns))
        : acl_.is_peer_allowed(std::span<const uint8_t, 32>(peer_ns));

    if (!allowed) {
        auto full_hex = to_hex(std::span<const uint8_t>(peer_ns.data(), peer_ns.size()), 32);
        spdlog::warn("access denied ({}): namespace={} ip={}",
                     net::role_name(role),
                     full_hex, conn->remote_address());
        if (!conn->connect_address().empty()) {
            server_.notify_acl_rejected(conn->connect_address());
        }
        ++metrics_.peers_disconnected_total;
        conn->close();
        return;
    }

    PeerInfo info;
    info.connection = conn;
    info.address = conn->remote_address();
    info.is_bootstrap = false;
    info.role = role;
    info.strike_count = 0;
    info.syncing = false;

    // Initialize token bucket for rate limiting (full burst capacity on connect)
    info.bucket_tokens = rate_limit_burst_;
    info.bucket_last_refill = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // Initialize last_message_time for inactivity detection (CONN-03)
    info.last_message_time = info.bucket_last_refill;

    // Check if this connection is to a bootstrap peer
    // RES-02 (D-09): Compare full endpoint (host:port), not just host
    for (const auto& bp : bootstrap_addresses_) {
        if (bp == info.address) {
            info.is_bootstrap = true;
            break;
        }
    }

    auto ns_hex = to_hex(conn->peer_pubkey(), 8);

    // Connection dedup: skip for non-peer roles (clients, observers, etc.).
    // Only PEER connections participate in node-to-node dedup.
    if (role == net::Role::Peer) {
        for (auto it = peers_.begin(); it != peers_.end(); ++it) {
            auto existing_ns = crypto::sha3_256((*it)->connection->peer_pubkey());
            if (existing_ns == peer_ns) {
                auto own_ns = identity_.namespace_id();
                bool own_ns_lower = std::lexicographical_compare(
                    own_ns.begin(), own_ns.end(),
                    peer_ns.data(), peer_ns.data() + 32);

                bool keep_new;
                if (own_ns_lower) {
                    keep_new = conn->is_initiator();
                } else {
                    keep_new = !conn->is_initiator();
                }

                if (keep_new) {
                    spdlog::info("duplicate connection from peer {}: closing existing, keeping new",
                                 ns_hex);
                    auto closed_addr = (*it)->connection->connect_address();
                    if (!closed_addr.empty()) {
                        server_.delay_reconnect(closed_addr);
                    }
                    (*it)->connection->close();
                    peers_.erase(it);
                } else {
                    spdlog::info("duplicate connection from peer {}: closing new, keeping existing",
                                 ns_hex);
                    auto closed_addr = conn->connect_address();
                    if (!closed_addr.empty()) {
                        server_.delay_reconnect(closed_addr);
                    }
                    conn->close();
                    return;  // Don't add to peers_
                }
                break;
            }
        }
    }  // role == Peer

    spdlog::info("Connected {} {}@{}",
                 net::role_name(role), ns_hex, info.address);

    // Set up message routing -- delegate to on_message_ callback
    conn->on_message([this](net::Connection::Ptr c, wire::TransportMsgType type,
                            std::vector<uint8_t> payload, uint32_t request_id) {
        on_message_(c, type, std::move(payload), request_id);
    });

    auto addr = info.address;
    peers_.push_back(std::make_unique<PeerInfo>(std::move(info)));
    peers_.back()->sync_inbox.clear();
    ++metrics_.peers_connected_total;

    // Notify facade for cross-component actions (PEX tracking, persisted peers).
    // Only peer-role connections belong in PEX/reconnect/sync plumbing.
    if (peers_.back()->role == net::Role::Peer) {
        on_connect_(conn, addr);
    }

    // MAINT-04/MAINT-05: Check cursor grace period for reconnecting peers
    {
        auto peer_hash = crypto::sha3_256(conn->peer_pubkey());
        auto it = disconnected_peers_.find(peer_hash);
        if (it != disconnected_peers_.end()) {
            auto now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            if (now_ms - it->second.disconnect_time <= CURSOR_GRACE_PERIOD_MS) {
                spdlog::info("peer {} reconnected within grace period, cursors preserved",
                             ns_hex);
            } else {
                spdlog::info("peer {} reconnected after grace period, cursors expired",
                             ns_hex);
            }
            disconnected_peers_.erase(it);
        }
    }

    // Both sides exchange SyncNamespaceAnnounce before sync.
    // Only peer-role connections participate in replication.
    if (peers_.back()->role == net::Role::Peer) {
        asio::co_spawn(ioc_, [this, conn]() -> asio::awaitable<void> {
            co_await announce_and_sync(conn);
        }, asio::detached);
    }
}

void ConnectionManager::on_peer_disconnected(net::Connection::Ptr conn) {
    auto ns_hex = to_hex(conn->peer_pubkey(), 8);
    bool graceful = conn->received_goodbye();

    // Find PeerInfo to check if the disconnecting connection was a client.
    // Client disconnects get simpler handling (no PEX cleanup, no reconnect).
    bool was_client = false;
    for (const auto& p : peers_) {
        if (p->connection == conn) {
            was_client = (p->role == net::Role::Client);
            break;
        }
    }

    spdlog::info("{} {} disconnected ({})",
                 was_client ? "Client" : "Peer", ns_hex,
                 graceful ? "graceful" : "timeout");

    // Cursor grace period only for peers, not clients
    if (!was_client) {
        auto peer_hash = crypto::sha3_256(conn->peer_pubkey());
        auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        disconnected_peers_[peer_hash] = DisconnectedPeerState{now_ms};
    }

    // Notify facade for cross-component cleanup (blob_push pending_fetches)
    on_disconnect_(conn);

    // Remove from peers
    peers_.erase(
        std::remove_if(peers_.begin(), peers_.end(),
                       [&conn](const std::unique_ptr<PeerInfo>& p) {
                           return p->connection == conn;
                       }),
        peers_.end());
    ++metrics_.peers_disconnected_total;
}

// =============================================================================
// Namespace announce exchange (FILT-01)
// =============================================================================

asio::awaitable<void> ConnectionManager::announce_and_sync(net::Connection::Ptr conn) {
    auto* peer = find_peer(conn);
    if (!peer) co_return;

    // Send our sync_namespaces (using existing encode_namespace_list)
    auto ns_list = std::vector<std::array<uint8_t, 32>>(
        sync_namespaces_.begin(), sync_namespaces_.end());
    auto payload = PeerManager::encode_namespace_list(ns_list);
    if (!co_await conn->send_message(
            wire::TransportMsgType_SyncNamespaceAnnounce, payload)) {
        co_return;
    }

    // Re-lookup after co_await -- peer may have disconnected
    peer = find_peer(conn);
    if (!peer) co_return;

    // Wait for peer's announce (timeout 5s) using timer-cancel pattern
    if (!peer->announce_received) {
        asio::steady_timer timer(ioc_);
        peer->announce_notify = &timer;
        timer.expires_after(std::chrono::seconds(5));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        // Re-lookup after co_await
        peer = find_peer(conn);
        if (!peer) co_return;
        peer->announce_notify = nullptr;
        if (!peer->announce_received) {
            spdlog::warn("peer {} did not send SyncNamespaceAnnounce within 5s, treating as replicate-all",
                         peer_display_name(conn));
        }
    }

    // Initiator triggers sync after announce exchange
    if (conn->is_initiator()) {
        co_await sync_trigger_(conn);
    }
}

// =============================================================================
// Sync message receive (for announce exchange)
// =============================================================================

asio::awaitable<std::optional<SyncMessage>>
ConnectionManager::recv_sync_msg(const net::Connection::Ptr& conn, std::chrono::seconds timeout) {
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

    peer = find_peer(conn);
    if (!peer) co_return std::nullopt;

    peer->sync_notify = nullptr;
    if (peer->sync_inbox.empty()) {
        co_return std::nullopt;
    }
    auto msg = std::move(peer->sync_inbox.front());
    peer->sync_inbox.pop_front();
    co_return msg;
}

// =============================================================================
// Strike system
// =============================================================================

void ConnectionManager::record_strike(net::Connection::Ptr conn, const std::string& reason) {
    auto* peer = find_peer(conn);
    if (!peer) return;

    peer->strike_count++;
    spdlog::warn("strike {}/{} for peer {} ({})",
                 peer->strike_count, strike_threshold_,
                 conn->remote_address(), reason);

    if (peer->strike_count >= strike_threshold_) {
        spdlog::warn("disconnecting peer {}: {} validation failures",
                     conn->remote_address(), peer->strike_count);
        asio::co_spawn(ioc_, conn->close_gracefully(), asio::detached);
    }
}

// =============================================================================
// Trust check
// =============================================================================

bool ConnectionManager::is_trusted_address(const asio::ip::address& addr) const {
    if (addr.is_loopback()) return true;
    return trusted_peers_.count(addr.to_string()) > 0;
}

// =============================================================================
// Disconnect unauthorized peers (called from reload_config)
// =============================================================================

void ConnectionManager::disconnect_unauthorized_peers() {
    // Take snapshot -- closing connections triggers on_peer_disconnected which modifies peers_
    std::vector<net::Connection::Ptr> to_disconnect;

    for (const auto& peer : peers_) {
        auto peer_ns = crypto::sha3_256(peer->connection->peer_pubkey());
        bool peer_allowed = (peer->role == net::Role::Client)
            ? acl_.is_client_allowed(std::span<const uint8_t, 32>(peer_ns))
            : acl_.is_peer_allowed(std::span<const uint8_t, 32>(peer_ns));
        if (!peer_allowed) {
            to_disconnect.push_back(peer->connection);
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
// Keepalive: send Ping to TCP peers, disconnect silent ones
// =============================================================================

asio::awaitable<void> ConnectionManager::keepalive_loop() {
    static constexpr auto KEEPALIVE_INTERVAL = std::chrono::seconds(30);
    static constexpr auto KEEPALIVE_TIMEOUT  = std::chrono::seconds(60);

    while (!stopping_) {
        asio::steady_timer timer(ioc_);
        keepalive_timer_ = &timer;
        timer.expires_after(KEEPALIVE_INTERVAL);
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        keepalive_timer_ = nullptr;
        if (ec || stopping_) co_return;

        auto now = std::chrono::steady_clock::now();

        // Snapshot connections -- peers_ may change across co_await points.
        // Only PEER-role connections get keepalive; clients are short-lived.
        std::vector<net::Connection::Ptr> tcp_peers;
        for (const auto& peer : peers_) {
            if (peer->role == net::Role::Peer) {
                tcp_peers.push_back(peer->connection);
            }
        }

        // Check for dead peers (before sending new Pings)
        std::vector<net::Connection::Ptr> to_close;
        for (const auto& conn : tcp_peers) {
            auto silence = now - conn->last_recv_time();
            if (silence > KEEPALIVE_TIMEOUT) {
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(silence).count();
                spdlog::warn("keepalive: disconnecting {} ({}s silent)",
                             conn->remote_address(), secs);
                to_close.push_back(conn);
            }
        }
        for (auto& conn : to_close) {
            conn->close();
        }

        // Send Ping to remaining live TCP peers
        for (const auto& conn : tcp_peers) {
            if (std::find(to_close.begin(), to_close.end(), conn) != to_close.end()) {
                continue;  // Already closed
            }
            std::span<const uint8_t> empty{};
            co_await conn->send_message(wire::TransportMsgType_Ping, empty);
        }
    }
}

void ConnectionManager::cancel_timers() {
    if (keepalive_timer_) keepalive_timer_->cancel();
}

// =============================================================================
// Helpers
// =============================================================================

PeerInfo* ConnectionManager::find_peer(const net::Connection::Ptr& conn) {
    for (auto& p : peers_) {
        if (p->connection == conn) return p.get();
    }
    return nullptr;
}

std::string ConnectionManager::peer_display_name(const net::Connection::Ptr& conn) {
    auto ns_hex = to_hex(conn->peer_pubkey(), 8);
    return ns_hex + "@" + conn->remote_address();
}

} // namespace chromatindb::peer
