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
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace chromatindb::net {

/// Per-address reconnect state: tracks backoff delay and ACL rejection count.
struct ReconnectState {
    int delay_sec = 1;
    int acl_rejection_count = 0;
};

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

    /// Connect to a peer with automatic reconnect on disconnect.
    /// Despite the name, now enters reconnect_loop. Name preserved for call-site compatibility.
    void connect_once(const std::string& address);

    /// Notify that a peer at the given address was ACL-rejected.
    /// Increments the rejection counter; triggers extended backoff after threshold.
    void notify_acl_rejected(const std::string& address);

    /// Reset all per-address reconnect state and cancel sleeping reconnect timers.
    /// Called by PeerManager on SIGHUP to allow immediate retry.
    void clear_reconnect_state();

    /// Stop reconnection attempts to a specific address (used by connection dedup).
    /// Erases reconnect state and cancels any sleeping timer, causing the
    /// reconnect_loop coroutine for that address to exit on next iteration.
    void stop_reconnect(const std::string& address);

    /// Delay reconnection to a specific address (used by connection dedup).
    /// Sets backoff to MAX_BACKOFF_SEC instead of permanently stopping.
    /// The reconnect loop stays alive and will retry after the delay,
    /// recovering connectivity if the kept connection later fails.
    void delay_reconnect(const std::string& address);

    /// ACL rejection threshold before extended backoff.
    static constexpr int ACL_REJECTION_THRESHOLD = 3;

    /// Extended backoff duration (seconds) for ACL-rejected peers.
    static constexpr int EXTENDED_BACKOFF_SEC = 600;

    /// Normal max backoff (seconds).
    static constexpr int MAX_BACKOFF_SEC = 60;

    /// Access reconnect state (for testing).
    const std::unordered_map<std::string, ReconnectState>& reconnect_state() const {
        return reconnect_state_;
    }

private:
    /// Accept inbound connections in a loop.
    asio::awaitable<void> accept_loop();

    /// Connect to a single peer address.
    asio::awaitable<void> connect_to_peer(std::string address);

    /// Reconnect loop with exponential backoff.
    asio::awaitable<void> reconnect_loop(std::string address);

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

    // Reconnect state: per-address backoff tracking and ACL rejection counts
    std::unordered_map<std::string, ReconnectState> reconnect_state_;
    std::mt19937 rng_{std::random_device{}()};
    std::unordered_map<std::string, asio::steady_timer*> reconnect_timers_;
};

} // namespace chromatindb::net
