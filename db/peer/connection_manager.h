#pragma once

#include "db/peer/peer_types.h"

#include <asio.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace chromatindb::identity { class NodeIdentity; }
namespace chromatindb::acl { class AccessControl; }
namespace chromatindb::config { struct Config; }
namespace chromatindb::net { class Server; }

namespace chromatindb::peer {

/// Owns the peers_ deque and handles all connection lifecycle: connect,
/// disconnect, dedup, keepalive, strike.  Extracted from PeerManager
/// (Phase 96 ARCH-01, component D-01/D-11).
class ConnectionManager {
public:
    /// Callback invoked when a peer sends a message.
    using MessageCallback = std::function<void(net::Connection::Ptr,
        wire::TransportMsgType, std::vector<uint8_t>, uint32_t)>;
    /// Callback invoked after announce exchange to trigger sync.
    using SyncTrigger = std::function<asio::awaitable<void>(net::Connection::Ptr)>;
    /// Callback invoked after a peer is added to peers_ (for PEX tracking).
    using ConnectCallback = std::function<void(net::Connection::Ptr, const std::string&)>;
    /// Callback invoked when a peer disconnects (for cross-component cleanup).
    using DisconnectCallback = std::function<void(net::Connection::Ptr)>;

    ConnectionManager(identity::NodeIdentity& identity,
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
                      DisconnectCallback on_disconnect);

    // Connection lifecycle (D-01)
    void on_peer_connected(net::Connection::Ptr conn);
    void on_peer_disconnected(net::Connection::Ptr conn);
    bool should_accept_connection() const;

    // Peer access (D-11: other components access peers through here)
    PeerInfo* find_peer(const net::Connection::Ptr& conn);
    std::string peer_display_name(const net::Connection::Ptr& conn);
    std::deque<std::unique_ptr<PeerInfo>>& peers() { return peers_; }
    const std::deque<std::unique_ptr<PeerInfo>>& peers() const { return peers_; }
    size_t peer_count() const { return peers_.size(); }

    // Bootstrap tracking
    const std::set<std::string>& bootstrap_addresses() const { return bootstrap_addresses_; }
    std::set<std::string>& bootstrap_addresses() { return bootstrap_addresses_; }
    const std::set<std::string>& trusted_peers() const { return trusted_peers_; }

    // Disconnected peer tracking (cursor grace period)
    std::unordered_map<std::array<uint8_t, 32>, DisconnectedPeerState, ArrayHash32>&
        disconnected_peers() { return disconnected_peers_; }
    static constexpr uint64_t CURSOR_GRACE_PERIOD_MS = 5 * 60 * 1000;

    // Trust check (passed to Connection as function)
    bool is_trusted_address(const asio::ip::address& addr) const;

    // Strike system
    void record_strike(net::Connection::Ptr conn, const std::string& reason);

    // Strike threshold accessor
    uint32_t strike_threshold() const { return strike_threshold_; }

    // Disconnect unauthorized (called from reload_config)
    void disconnect_unauthorized_peers();

    // Timer loops
    asio::awaitable<void> keepalive_loop();
    void cancel_timers();

    // Config reload
    void set_max_peers(uint32_t max_peers) { max_peers_ = max_peers; }
    void set_max_clients(uint32_t max_clients) { max_clients_ = max_clients; }
    void set_trusted_peers(std::set<std::string> peers) { trusted_peers_ = std::move(peers); }

private:
    asio::awaitable<void> announce_and_sync(net::Connection::Ptr conn);
    asio::awaitable<std::optional<SyncMessage>> recv_sync_msg(
        const net::Connection::Ptr& conn, std::chrono::seconds timeout);

    identity::NodeIdentity& identity_;
    acl::AccessControl& acl_;
    NodeMetrics& metrics_;
    net::Server& server_;
    asio::io_context& ioc_;
    const bool& stopping_;
    const std::set<std::array<uint8_t, 32>>& sync_namespaces_;
    MessageCallback on_message_;
    SyncTrigger sync_trigger_;
    ConnectCallback on_connect_;
    DisconnectCallback on_disconnect_;
    uint64_t rate_limit_burst_;

    std::deque<std::unique_ptr<PeerInfo>> peers_;
    std::set<std::string> bootstrap_addresses_;
    std::set<std::string> trusted_peers_;
    std::unordered_map<std::array<uint8_t, 32>, DisconnectedPeerState, ArrayHash32>
        disconnected_peers_;

    asio::steady_timer* keepalive_timer_ = nullptr;
    uint32_t max_peers_;
    uint32_t max_clients_ = 128;
    uint32_t strike_threshold_;
};

} // namespace chromatindb::peer
