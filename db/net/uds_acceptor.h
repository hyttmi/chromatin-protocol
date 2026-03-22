#pragma once

#include "db/identity/identity.h"
#include "db/net/connection.h"

#include <asio.hpp>
#include <asio/local/stream_protocol.hpp>

#include <functional>
#include <string>
#include <vector>

namespace chromatindb::net {

/// Unix Domain Socket acceptor: listens on a local socket path.
/// Produces Connection objects using TrustedHello (always-trusted, no PQ).
class UdsAcceptor {
public:
    using ConnectionCallback = std::function<void(Connection::Ptr)>;
    using AcceptFilter = std::function<bool()>;

    UdsAcceptor(const std::string& path,
                const identity::NodeIdentity& identity,
                asio::io_context& ioc);

    /// Start accepting UDS connections. Unlinks stale socket if needed.
    void start();

    /// Stop accepting and unlink the socket file.
    void stop();

    /// Set callback for new authenticated connections.
    void set_on_connected(ConnectionCallback cb) { on_connected_ = std::move(cb); }

    /// Set callback for disconnected connections.
    void set_on_disconnected(ConnectionCallback cb) { on_disconnected_ = std::move(cb); }

    /// Set accept filter (connection limit).
    void set_accept_filter(AcceptFilter cb) { accept_filter_ = std::move(cb); }

    /// Set thread pool for crypto offload (lightweight keys still need HKDF).
    void set_pool(asio::thread_pool& pool) { pool_ = &pool; }

    /// Number of active UDS connections.
    size_t connection_count() const { return connections_.size(); }

    /// Whether acceptor is running.
    bool is_running() const { return running_; }

private:
    asio::awaitable<void> accept_loop();
    void remove_connection(Connection::Ptr conn);

    std::string path_;
    const identity::NodeIdentity& identity_;
    asio::io_context& ioc_;
    asio::local::stream_protocol::acceptor acceptor_;
    std::vector<Connection::Ptr> connections_;
    bool running_ = false;

    ConnectionCallback on_connected_;
    ConnectionCallback on_disconnected_;
    AcceptFilter accept_filter_;
    asio::thread_pool* pool_ = nullptr;
};

} // namespace chromatindb::net
