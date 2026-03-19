#pragma once

#include "db/config/config.h"
#include "db/identity/identity.h"
#include "db/net/connection.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace chromatindb::net {

/// TCP server: accepts inbound connections, connects to peers, manages lifecycles.
class Server {
public:
    /// Callback types for PeerManager integration.
    using ConnectionCallback = std::function<void(Connection::Ptr)>;
    using AcceptFilter = std::function<bool()>;
    using ShutdownCallback = std::function<void()>;

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

    /// Set callback invoked before drain starts (PeerManager saves peers here).
    void set_on_shutdown(ShutdownCallback cb) { on_shutdown_ = std::move(cb); }

    /// Set thread pool reference for crypto offload (forwarded to connections).
    void set_pool(asio::thread_pool& pool) { pool_ = &pool; }

    /// Trust-check function type for lightweight handshake.
    using TrustCheck = std::function<bool(const asio::ip::address&)>;

    /// Set trust-check function, passed to each Connection for handshake branching.
    void set_trust_check(TrustCheck cb) { trust_check_ = std::move(cb); }

    /// Exit code: 0 = clean shutdown, 1 = forced/timeout.
    int exit_code() const { return exit_code_; }

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

    /// Arm (or re-arm) the SIGINT/SIGTERM signal handler.
    void arm_signal_handler();

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
    ShutdownCallback on_shutdown_;
    TrustCheck trust_check_;
    asio::thread_pool* pool_ = nullptr;
    int exit_code_ = 0;
};

} // namespace chromatindb::net
