#pragma once

#include "config/config.h"
#include "identity/identity.h"
#include "net/connection.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace chromatin::net {

/// TCP server: accepts inbound connections, connects to peers, manages lifecycles.
class Server {
public:
    /// Callback types for PeerManager integration.
    using ConnectionCallback = std::function<void(Connection::Ptr)>;
    using AcceptFilter = std::function<bool()>;

    Server(const config::Config& config,
           const identity::NodeIdentity& identity,
           asio::io_context& ioc);

    /// Start accepting connections and connect to bootstrap peers.
    void start();

    /// Trigger graceful shutdown (non-blocking, starts drain coroutine).
    void stop();

    /// Number of active connections.
    size_t connection_count() const;

    /// Whether the server is in shutdown/draining state.
    bool is_draining() const { return draining_; }

    /// Set callback for when a peer successfully connects and authenticates.
    void set_on_connected(ConnectionCallback cb) { on_connected_ = std::move(cb); }

    /// Set callback for when a peer disconnects.
    void set_on_disconnected(ConnectionCallback cb) { on_disconnected_ = std::move(cb); }

    /// Set filter for accepting inbound connections (return false to reject).
    void set_accept_filter(AcceptFilter cb) { accept_filter_ = std::move(cb); }

    /// Connect to a peer once (no reconnect on failure). For discovered peers.
    void connect_once(const std::string& address);

private:
    /// Accept inbound connections in a loop.
    asio::awaitable<void> accept_loop();

    /// Connect to a single peer address.
    asio::awaitable<void> connect_to_peer(const std::string& address);

    /// Reconnect loop with exponential backoff.
    asio::awaitable<void> reconnect_loop(const std::string& address);

    /// Drain all connections and stop.
    asio::awaitable<void> drain(std::chrono::seconds timeout);

    /// Remove a connection from the active set.
    void remove_connection(Connection::Ptr conn);

    /// Parse "host:port" string.
    static std::pair<std::string, std::string> parse_address(const std::string& addr);

    const config::Config& config_;
    const identity::NodeIdentity& identity_;
    asio::io_context& ioc_;
    asio::ip::tcp::acceptor acceptor_;
    asio::signal_set signals_;

    std::vector<Connection::Ptr> connections_;
    bool draining_ = false;
    bool started_ = false;

    // PeerManager integration callbacks
    ConnectionCallback on_connected_;
    ConnectionCallback on_disconnected_;
    AcceptFilter accept_filter_;
};

} // namespace chromatin::net
