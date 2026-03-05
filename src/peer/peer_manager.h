#pragma once

#include "config/config.h"
#include "engine/engine.h"
#include "identity/identity.h"
#include "net/connection.h"
#include "net/server.h"
#include "storage/storage.h"
#include "sync/sync_protocol.h"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace chromatin::peer {

/// A sync message received from a peer, queued for processing.
struct SyncMessage {
    wire::TransportMsgType type;
    std::vector<uint8_t> payload;
};

/// Info about a connected peer.
struct PeerInfo {
    net::Connection::Ptr connection;
    std::string address;
    bool is_bootstrap = false;
    uint32_t strike_count = 0;
    bool syncing = false;
    // Sync message queue (timer-cancel pattern)
    std::deque<SyncMessage> sync_inbox;
    asio::steady_timer* sync_notify = nullptr;
};

/// Manages peer connections, sync scheduling, and connection policies.
///
/// Wraps Server and adds:
/// - Connection limit enforcement (max_peers)
/// - Bootstrap vs non-bootstrap peer tracking
/// - Sync-on-connect and periodic sync timer
/// - Strike system for misbehaving peers
///
/// Thread safety: NOT thread-safe. Runs on single io_context thread.
class PeerManager {
public:
    PeerManager(const config::Config& config,
                identity::NodeIdentity& identity,
                engine::BlobEngine& engine,
                storage::Storage& storage,
                asio::io_context& ioc);

    /// Start the server and sync timer.
    void start();

    /// Stop the server and cancel timers.
    void stop();

    /// Number of currently connected peers.
    size_t peer_count() const;

    /// Number of connected bootstrap peers.
    size_t bootstrap_peer_count() const;

    /// Strike threshold (public for testing).
    static constexpr uint32_t STRIKE_THRESHOLD = 10;
    static constexpr uint32_t STRIKE_COOLDOWN_SEC = 300;  // 5 minutes

private:
    // Server callbacks
    void on_peer_connected(net::Connection::Ptr conn);
    void on_peer_disconnected(net::Connection::Ptr conn);
    bool should_accept_connection();

    // Message routing
    void on_peer_message(net::Connection::Ptr conn,
                         wire::TransportMsgType type,
                         std::vector<uint8_t> payload);

    // Sync orchestration
    asio::awaitable<void> run_sync_with_peer(net::Connection::Ptr conn);
    asio::awaitable<void> sync_all_peers();
    asio::awaitable<void> sync_timer_loop();

    // Sync message handling (responder side)
    asio::awaitable<void> handle_sync_as_responder(net::Connection::Ptr conn);

    // Sync message queue
    void route_sync_message(PeerInfo* peer, wire::TransportMsgType type, std::vector<uint8_t> payload);
    asio::awaitable<std::optional<SyncMessage>> recv_sync_msg(PeerInfo* peer, std::chrono::seconds timeout);

    // Strike system
    void record_strike(net::Connection::Ptr conn, const std::string& reason);

    // Helpers
    PeerInfo* find_peer(const net::Connection::Ptr& conn);
    std::string peer_display_name(const net::Connection::Ptr& conn);

    const config::Config& config_;
    identity::NodeIdentity& identity_;
    engine::BlobEngine& engine_;
    storage::Storage& storage_;
    asio::io_context& ioc_;

    net::Server server_;
    sync::SyncProtocol sync_proto_;

    std::vector<PeerInfo> peers_;
    std::set<std::string> bootstrap_addresses_;
    bool stopping_ = false;
};

} // namespace chromatin::peer
