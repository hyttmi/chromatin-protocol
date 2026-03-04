#pragma once

#include "config/config.h"
#include "identity/identity.h"
#include "net/connection.h"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace chromatin::net {

/// TCP server: accepts inbound connections, connects to peers, manages lifecycles.
class Server {
public:
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
};

} // namespace chromatin::net
