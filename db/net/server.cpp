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

    // Cancel all sleeping reconnect timers so reconnect_loop coroutines exit
    for (auto& [addr, timer] : reconnect_timers_) {
        if (timer) timer->cancel();
    }

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

asio::awaitable<void> Server::connect_to_peer(std::string address) {
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
    conn->set_connect_address(address);
    if (trust_check_) conn->set_trust_check(trust_check_);
    if (pool_) conn->set_pool(*pool_);
    connections_.push_back(conn);

    // Set on_close WITHOUT reconnect -- we handle reconnect after run() returns
    conn->on_close([this](Connection::Ptr c, bool graceful) {
        remove_connection(c);
        if (on_disconnected_) on_disconnected_(c);
    });

    // Snapshot ACL rejection count (no reference held across co_await)
    int prev_acl_count = reconnect_state_[address].acl_rejection_count;

    // Notify on_connected after handshake succeeds (before message loop)
    bool handshake_ok = false;
    conn->on_ready([this, &handshake_ok](Connection::Ptr c) {
        handshake_ok = true;
        if (on_connected_) on_connected_(c);
    });

    auto ok = co_await conn->run();

    // Check if ACL rejection happened during this attempt
    bool was_acl_rejected =
        reconnect_state_[address].acl_rejection_count > prev_acl_count;

    if (ok && !was_acl_rejected) {
        // Connection ran and closed normally. Enter reconnect loop.
        if (!draining_) {
            co_await reconnect_loop(address);
        }
    } else if (!handshake_ok && !draining_) {
        // Handshake failed -- reconnect
        remove_connection(conn);
        co_await reconnect_loop(address);
    } else if (was_acl_rejected && !draining_) {
        // ACL rejected -- enter reconnect loop (with extended backoff tracking)
        co_await reconnect_loop(address);
    } else if (!draining_) {
        // Handshake succeeded but connection closed (not ACL) -- reconnect
        co_await reconnect_loop(address);
    }
}

asio::awaitable<void> Server::reconnect_loop(std::string address) {
    // Ensure entry exists (no reference held across co_await -- avoids dangling on destruction)
    reconnect_state_[address];
    bool first_attempt = true;
    // reconnect_loop active for this address

    while (!draining_) {
        // If stop_reconnect() erased our entry, exit the loop (connection dedup).
        if (!reconnect_state_.count(address)) co_return;
        // Read state by value at each iteration (safe across co_await)
        auto cur = reconnect_state_[address];

        // First attempt connects immediately (delay_sec=1 means no wait on first try).
        // Subsequent attempts wait with jitter.
        if (!first_attempt || cur.delay_sec > 1 ||
            cur.acl_rejection_count >= ACL_REJECTION_THRESHOLD) {

            // Compute effective delay with jitter
            int effective_delay = cur.delay_sec;
            if (cur.acl_rejection_count >= ACL_REJECTION_THRESHOLD) {
                effective_delay = EXTENDED_BACKOFF_SEC;
            } else if (cur.delay_sec > 1) {
                std::uniform_int_distribution<int> dist(0, cur.delay_sec / 2);
                int jitter = dist(rng_);
                effective_delay = std::min(cur.delay_sec + jitter, MAX_BACKOFF_SEC);
            }

            spdlog::info("reconnecting to {} in {}s", address, effective_delay);

            asio::steady_timer timer(ioc_);
            reconnect_timers_[address] = &timer;
            timer.expires_after(std::chrono::seconds(effective_delay));
            auto [ec] = co_await timer.async_wait(use_nothrow);
            reconnect_timers_[address] = nullptr;
            if (ec || draining_) co_return;
        }
        first_attempt = false;

        // Try to connect
        auto [host, port] = parse_address(address);
        asio::ip::tcp::resolver resolver(ioc_);
        auto [ec_resolve, endpoints] = co_await resolver.async_resolve(
            host, port, use_nothrow);
        if (draining_) co_return;
        if (ec_resolve) {
            reconnect_state_[address].delay_sec = std::min(
                reconnect_state_[address].delay_sec * 2, MAX_BACKOFF_SEC);
            continue;
        }

        asio::ip::tcp::socket socket(ioc_);
        auto [ec_connect, ep] = co_await asio::async_connect(
            socket, endpoints, use_nothrow);
        if (draining_) co_return;
        if (ec_connect) {
            reconnect_state_[address].delay_sec = std::min(
                reconnect_state_[address].delay_sec * 2, MAX_BACKOFF_SEC);
            continue;
        }

        spdlog::info("reconnected to {}", address);

        auto conn = Connection::create_outbound(std::move(socket), identity_);
        conn->set_connect_address(address);
        if (trust_check_) conn->set_trust_check(trust_check_);
        if (pool_) conn->set_pool(*pool_);
        connections_.push_back(conn);

        conn->on_close([this](Connection::Ptr c, bool /*graceful*/) {
            remove_connection(c);
            if (on_disconnected_) on_disconnected_(c);
        });

        // Snapshot ACL rejection count before running connection
        int prev_acl_count = reconnect_state_[address].acl_rejection_count;

        // Notify on_connected after handshake succeeds (before message loop)
        bool handshake_ok = false;
        conn->on_ready([this, &handshake_ok](Connection::Ptr c) {
            handshake_ok = true;
            if (on_connected_) on_connected_(c);
        });

        auto ok = co_await conn->run();

        if (draining_) co_return;

        // Check if ACL rejection happened during this attempt
        bool was_acl_rejected =
            reconnect_state_[address].acl_rejection_count > prev_acl_count;

        if ((ok || handshake_ok) && !was_acl_rejected) {
            // Successful connection ran and closed normally. Reset backoff.
            reconnect_state_[address].delay_sec = 1;
            reconnect_state_[address].acl_rejection_count = 0;
            continue;
        }

        if (!ok && !handshake_ok) {
            remove_connection(conn);
        }

        // ACL rejection or handshake failure: increase backoff (unless already in extended)
        if (reconnect_state_[address].acl_rejection_count < ACL_REJECTION_THRESHOLD) {
            reconnect_state_[address].delay_sec = std::min(
                reconnect_state_[address].delay_sec * 2, MAX_BACKOFF_SEC);
        }
    }
}

void Server::notify_acl_rejected(const std::string& address) {
    auto& state = reconnect_state_[address];
    ++state.acl_rejection_count;
    if (state.acl_rejection_count >= ACL_REJECTION_THRESHOLD) {
        state.delay_sec = EXTENDED_BACKOFF_SEC;
        spdlog::warn("ACL rejection threshold reached for {}, extended backoff {}s",
                     address, EXTENDED_BACKOFF_SEC);
    }
}

void Server::clear_reconnect_state() {
    reconnect_state_.clear();
    // Cancel all sleeping reconnect timers to force immediate retry
    for (auto& [addr, timer] : reconnect_timers_) {
        if (timer) timer->cancel();
    }
}

void Server::stop_reconnect(const std::string& address) {
    reconnect_state_.erase(address);
    auto it = reconnect_timers_.find(address);
    if (it != reconnect_timers_.end() && it->second) {
        it->second->cancel();
    }
}

void Server::delay_reconnect(const std::string& address) {
    auto it = reconnect_state_.find(address);
    if (it != reconnect_state_.end()) {
        it->second.delay_sec = MAX_BACKOFF_SEC;
    }
}

// =============================================================================
// Outbound connection with reconnect (for discovered/persisted peers)
// =============================================================================

void Server::connect_once(const std::string& address) {
    // Note: despite name, now reconnects. Name preserved for call-site compatibility.
    if (draining_) return;
    // Skip if a reconnect loop is already active for this address
    if (reconnect_state_.count(address)) return;
    // Mark as active immediately to prevent duplicate spawns before coroutine starts
    reconnect_state_[address];
    asio::co_spawn(ioc_, reconnect_loop(address), asio::detached);
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
