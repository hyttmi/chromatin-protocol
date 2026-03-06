#pragma once

#include "db/config/config.h"
#include "db/engine/engine.h"
#include "db/identity/identity.h"
#include "db/net/connection.h"
#include "db/net/server.h"
#include "db/storage/storage.h"
#include "db/sync/sync_protocol.h"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace chromatin::peer {

/// A persisted peer address with connection tracking.
struct PersistedPeer {
    std::string address;
    uint64_t last_seen = 0;     // Unix timestamp
    uint32_t fail_count = 0;
};

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

    /// PEX constants (public for testing).
    static constexpr uint32_t PEX_INTERVAL_SEC = 300;        // 5 minutes
    static constexpr uint32_t MAX_PEERS_PER_EXCHANGE = 8;    // Max peers to share per response
    static constexpr uint32_t MAX_DISCOVERED_PER_ROUND = 3;  // Max new peers to connect per round
    static constexpr uint32_t MAX_PERSISTED_PEERS = 100;     // Max entries in peers.json
    static constexpr uint32_t MAX_PERSIST_FAILURES = 3;      // Prune after N consecutive startup failures

    /// PEX wire encoding (public for testing).
    static std::vector<uint8_t> encode_peer_list(const std::vector<std::string>& addresses);
    static std::vector<std::string> decode_peer_list(std::span<const uint8_t> payload);

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

    // PEX protocol
    asio::awaitable<void> pex_timer_loop();
    asio::awaitable<void> request_peers_from_all();
    asio::awaitable<void> run_pex_with_peer(net::Connection::Ptr conn);
    asio::awaitable<void> handle_pex_as_responder(net::Connection::Ptr conn);
    void handle_peer_list_response(net::Connection::Ptr conn, std::vector<uint8_t> payload);
    std::vector<std::string> build_peer_list_response(const std::string& exclude_address);

    // Peer persistence
    void load_persisted_peers();
    void save_persisted_peers();
    void update_persisted_peer(const std::string& address, bool success);
    std::filesystem::path peers_file_path() const;

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

    std::deque<PeerInfo> peers_;
    std::set<std::string> bootstrap_addresses_;
    std::set<std::string> known_addresses_;      // All addresses we know about
    std::vector<PersistedPeer> persisted_peers_;  // Peers persisted to disk
    bool stopping_ = false;
};

} // namespace chromatin::peer
