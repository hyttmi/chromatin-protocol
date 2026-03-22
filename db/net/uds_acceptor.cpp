#include "db/net/uds_acceptor.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

namespace chromatindb::net {

UdsAcceptor::UdsAcceptor(const std::string& path,
                         const identity::NodeIdentity& identity,
                         asio::io_context& ioc)
    : path_(path)
    , identity_(identity)
    , ioc_(ioc)
    , acceptor_(ioc) {}

void UdsAcceptor::start() {
    if (running_) return;

    asio::local::stream_protocol::endpoint ep(path_);

    // Try to bind; if stale socket exists, unlink and retry
    acceptor_.open();
    asio::error_code ec;
    acceptor_.bind(ep, ec);
    if (ec == asio::error::address_in_use) {
        spdlog::info("unlinking stale UDS socket: {}", path_);
        ::unlink(path_.c_str());
        acceptor_.bind(ep);  // throws on failure
    } else if (ec) {
        throw std::system_error(ec, "UDS bind failed on " + path_);
    }

    // Set socket file permissions: owner + group read/write
    ::chmod(path_.c_str(), 0660);

    acceptor_.listen();
    running_ = true;

    spdlog::info("UDS listening on {}", path_);

    asio::co_spawn(ioc_, accept_loop(), asio::detached);
}

void UdsAcceptor::stop() {
    if (!running_) return;
    running_ = false;

    // Cancel pending accept
    asio::error_code ec;
    acceptor_.close(ec);

    // Close all active connections
    auto snapshot = connections_;
    for (auto& conn : snapshot) {
        conn->close();
    }
    connections_.clear();

    // Clean up socket file
    ::unlink(path_.c_str());

    spdlog::info("UDS acceptor stopped");
}

asio::awaitable<void> UdsAcceptor::accept_loop() {
    while (running_) {
        auto [ec, socket] = co_await acceptor_.async_accept(use_nothrow);
        if (ec) {
            if (!running_) break;
            spdlog::warn("UDS accept error: {}", ec.message());
            continue;
        }

        // Check accept filter (connection limit)
        if (accept_filter_ && !accept_filter_()) {
            spdlog::info("rejected UDS connection (max peers reached)");
            asio::error_code close_ec;
            socket.close(close_ec);
            continue;
        }

        spdlog::info("accepted UDS connection");

        auto conn = Connection::create_uds_inbound(std::move(socket), identity_);
        // UDS connections do not need trust_check (is_uds_ = true bypasses it)
        if (pool_) conn->set_pool(*pool_);
        connections_.push_back(conn);

        conn->on_close([this](Connection::Ptr c, bool /*graceful*/) {
            remove_connection(c);
            if (on_disconnected_) on_disconnected_(c);
        });

        conn->on_ready([this](Connection::Ptr c) {
            if (on_connected_) on_connected_(c);
        });

        // Spawn run coroutine
        asio::co_spawn(ioc_, [conn]() -> asio::awaitable<void> {
            co_await conn->run();
        }, asio::detached);
    }
}

void UdsAcceptor::remove_connection(Connection::Ptr conn) {
    connections_.erase(
        std::remove(connections_.begin(), connections_.end(), conn),
        connections_.end());
}

} // namespace chromatindb::net
