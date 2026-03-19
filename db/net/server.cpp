#include "db/net/server.h"

#include <spdlog/spdlog.h>
#include <algorithm>

namespace chromatindb::net {

Server::Server(const config::Config& config,
               const identity::NodeIdentity& identity,
               asio::io_context& ioc)
    : config_(config)
    , identity_(identity)
    , ioc_(ioc)
    , acceptor_(ioc)
    , signals_(ioc, SIGINT, SIGTERM) {}

std::pair<std::string, std::string> Server::parse_address(const std::string& addr) {
    auto colon = addr.rfind(':');
    if (colon == std::string::npos) {
        return {addr, "4200"};
    }
    return {addr.substr(0, colon), addr.substr(colon + 1)};
}

void Server::start() {
    if (started_) return;
    started_ = true;

    // Setup acceptor
    auto [host, port] = parse_address(config_.bind_address);
    asio::ip::tcp::resolver resolver(ioc_);
    auto endpoints = resolver.resolve(host, port);

    auto endpoint = *endpoints.begin();
    acceptor_.open(endpoint.endpoint().protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint.endpoint());
    acceptor_.listen();

    spdlog::info("listening on {}", config_.bind_address);

    // Start accept loop
    asio::co_spawn(ioc_, accept_loop(), asio::detached);

    // Connect to bootstrap peers
    for (const auto& peer : config_.bootstrap_peers) {
        asio::co_spawn(ioc_, connect_to_peer(peer), asio::detached);
    }

    // Signal handling (re-arming for second-signal force shutdown)
    arm_signal_handler();
}

void Server::stop() {
    if (draining_) return;
    draining_ = true;

    // Invoke pre-drain callback (PeerManager saves peers here)
    if (on_shutdown_) on_shutdown_();

    // Cancel acceptor
    asio::error_code ec;
    acceptor_.close(ec);

    // Start drain coroutine
    asio::co_spawn(ioc_, drain(std::chrono::seconds(5)), asio::detached);
}

void Server::arm_signal_handler() {
    signals_.async_wait([this](asio::error_code ec, int sig) {
        if (ec) return;
        if (draining_) {
            // Second signal: force shutdown, clean exit (NOT std::_Exit)
            spdlog::info("second signal received ({}), forcing shutdown", sig);
            for (auto& conn : connections_) conn->close();
            connections_.clear();
            exit_code_ = 1;
            spdlog::default_logger()->flush();
            ioc_.stop();
            return;
        }
        spdlog::info("signal {} received, starting graceful shutdown", sig);
        stop();
        arm_signal_handler();  // Re-arm for second signal
    });
}

size_t Server::connection_count() const {
    return connections_.size();
}

void Server::remove_connection(Connection::Ptr conn) {
    connections_.erase(
        std::remove(connections_.begin(), connections_.end(), conn),
        connections_.end());
}

// =============================================================================
// Accept loop
// =============================================================================

asio::awaitable<void> Server::accept_loop() {
    while (!draining_) {
        auto [ec, socket] = co_await acceptor_.async_accept(use_nothrow);
        if (ec) {
            if (draining_) break;
            spdlog::warn("accept error: {}", ec.message());
            continue;
        }

        // Check accept filter (PeerManager connection limit)
        if (accept_filter_ && !accept_filter_()) {
            spdlog::info("rejected inbound connection (max peers reached) from {}",
                socket.remote_endpoint().address().to_string());
            asio::error_code close_ec;
            socket.close(close_ec);
            continue;
        }

        spdlog::info("accepted connection from {}",
            socket.remote_endpoint().address().to_string());

        auto conn = Connection::create_inbound(std::move(socket), identity_);
        if (trust_check_) conn->set_trust_check(trust_check_);
        if (pool_) conn->set_pool(*pool_);
        connections_.push_back(conn);

        conn->on_close([this](Connection::Ptr c, bool graceful) {
            remove_connection(c);
            if (on_disconnected_) on_disconnected_(c);
        });

        // Notify on_connected after handshake succeeds (before message loop)
        conn->on_ready([this](Connection::Ptr c) {
            if (on_connected_) on_connected_(c);
        });

        // Spawn run coroutine
        asio::co_spawn(ioc_, [conn]() -> asio::awaitable<void> {
            co_await conn->run();
        }, asio::detached);
    }
}

// =============================================================================
// Outbound connection
// =============================================================================

asio::awaitable<void> Server::connect_to_peer(const std::string& address) {
    auto [host, port] = parse_address(address);

    asio::ip::tcp::resolver resolver(ioc_);
    auto [ec_resolve, endpoints] = co_await resolver.async_resolve(
        host, port, use_nothrow);
    if (ec_resolve) {
        spdlog::warn("failed to resolve {}: {}", address, ec_resolve.message());
        // Enter reconnect loop
        co_await reconnect_loop(address);
        co_return;
    }

    asio::ip::tcp::socket socket(ioc_);
    auto [ec_connect, ep] = co_await asio::async_connect(
        socket, endpoints, use_nothrow);
    if (ec_connect) {
        spdlog::warn("failed to connect to {}: {}", address, ec_connect.message());
        co_await reconnect_loop(address);
        co_return;
    }

    spdlog::info("connected to {}", address);

    auto conn = Connection::create_outbound(std::move(socket), identity_);
    if (trust_check_) conn->set_trust_check(trust_check_);
    if (pool_) conn->set_pool(*pool_);
    connections_.push_back(conn);

    // Set on_close WITHOUT reconnect -- we handle reconnect after run() returns
    conn->on_close([this](Connection::Ptr c, bool graceful) {
        remove_connection(c);
        if (on_disconnected_) on_disconnected_(c);
    });

    // Notify on_connected after handshake succeeds (before message loop)
    bool handshake_ok = false;
    conn->on_ready([this, &handshake_ok](Connection::Ptr c) {
        handshake_ok = true;
        if (on_connected_) on_connected_(c);
    });

    auto ok = co_await conn->run();
    if (ok) {
        // Connection ran and closed normally. Set up reconnect for bootstrap peers.
        if (!draining_) {
            asio::co_spawn(ioc_, reconnect_loop(address), asio::detached);
        }
    } else if (!handshake_ok && !draining_) {
        // Handshake failed -- reconnect
        remove_connection(conn);
        co_await reconnect_loop(address);
    }
}

asio::awaitable<void> Server::reconnect_loop(const std::string& address) {
    int delay_sec = 1;
    constexpr int max_delay = 60;

    while (!draining_) {
        spdlog::info("reconnecting to {} in {}s", address, delay_sec);

        asio::steady_timer timer(ioc_);
        timer.expires_after(std::chrono::seconds(delay_sec));
        auto [ec] = co_await timer.async_wait(use_nothrow);
        if (ec || draining_) co_return;

        // Try to connect
        auto [host, port] = parse_address(address);
        asio::ip::tcp::resolver resolver(ioc_);
        auto [ec_resolve, endpoints] = co_await resolver.async_resolve(
            host, port, use_nothrow);
        if (ec_resolve) {
            delay_sec = std::min(delay_sec * 2, max_delay);
            continue;
        }

        asio::ip::tcp::socket socket(ioc_);
        auto [ec_connect, ep] = co_await asio::async_connect(
            socket, endpoints, use_nothrow);
        if (ec_connect) {
            delay_sec = std::min(delay_sec * 2, max_delay);
            continue;
        }

        spdlog::info("reconnected to {}", address);

        auto conn = Connection::create_outbound(std::move(socket), identity_);
        if (trust_check_) conn->set_trust_check(trust_check_);
        if (pool_) conn->set_pool(*pool_);
        connections_.push_back(conn);

        conn->on_close([this](Connection::Ptr c, bool /*graceful*/) {
            remove_connection(c);
            if (on_disconnected_) on_disconnected_(c);
        });

        // Notify on_connected after handshake succeeds (before message loop)
        bool handshake_ok = false;
        conn->on_ready([this, &handshake_ok](Connection::Ptr c) {
            handshake_ok = true;
            if (on_connected_) on_connected_(c);
        });

        auto ok = co_await conn->run();
        if (ok || handshake_ok) {
            // Connection ran and closed. Loop to reconnect.
            delay_sec = 1;
            continue;
        }

        remove_connection(conn);
        delay_sec = std::min(delay_sec * 2, max_delay);
    }
}

// =============================================================================
// One-shot outbound connection (no reconnect)
// =============================================================================

void Server::connect_once(const std::string& address) {
    if (draining_) return;
    asio::co_spawn(ioc_, [this, address]() -> asio::awaitable<void> {
        auto [host, port] = parse_address(address);

        asio::ip::tcp::resolver resolver(ioc_);
        auto [ec_resolve, endpoints] = co_await resolver.async_resolve(
            host, port, use_nothrow);
        if (ec_resolve) {
            spdlog::debug("connect_once: failed to resolve {}: {}", address, ec_resolve.message());
            co_return;
        }

        asio::ip::tcp::socket socket(ioc_);
        auto [ec_connect, ep] = co_await asio::async_connect(
            socket, endpoints, use_nothrow);
        if (ec_connect) {
            spdlog::debug("connect_once: failed to connect to {}: {}", address, ec_connect.message());
            co_return;
        }

        spdlog::info("connect_once: connected to {}", address);

        auto conn = Connection::create_outbound(std::move(socket), identity_);
        if (trust_check_) conn->set_trust_check(trust_check_);
        if (pool_) conn->set_pool(*pool_);
        connections_.push_back(conn);

        conn->on_close([this](Connection::Ptr c, bool /*graceful*/) {
            remove_connection(c);
            if (on_disconnected_) on_disconnected_(c);
        });

        conn->on_ready([this](Connection::Ptr c) {
            if (on_connected_) on_connected_(c);
        });

        co_await conn->run();
        // Connection ended -- no reconnect for discovered peers
    }, asio::detached);
}

// =============================================================================
// Graceful shutdown
// =============================================================================

asio::awaitable<void> Server::drain(std::chrono::seconds timeout) {
    spdlog::info("draining {} connections...", connections_.size());

    // Take a snapshot of connections to avoid iterator invalidation
    // (close_cb_ may call remove_connection() during iteration)
    auto snapshot = connections_;

    // Send goodbye to all connections
    for (auto& conn : snapshot) {
        if (conn->is_authenticated()) {
            co_await conn->close_gracefully();
        } else {
            conn->close();
        }
    }
    spdlog::info("goodbye sent to all peers");

    // Wait for drain timeout or all connections closed
    asio::steady_timer timer(ioc_);
    timer.expires_after(timeout);
    auto [ec] = co_await timer.async_wait(use_nothrow);

    // Force close any remaining (timeout = forced shutdown)
    if (!connections_.empty()) {
        exit_code_ = 1;
        for (auto& conn : connections_) {
            conn->close();
        }
        connections_.clear();
    }

    spdlog::info("shutdown complete");
    signals_.cancel();
}

} // namespace chromatindb::net
