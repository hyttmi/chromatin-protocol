#pragma once

#include "db/identity/identity.h"
#include "db/net/connection.h"

#include <asio.hpp>
#include <asio/local/stream_protocol.hpp>

#include <functional>
#include <memory>
#include <string>

namespace chromatindb::relay::core {

/// Manages a single client session through the relay.
/// Pairs a PQ-authenticated client TCP connection with a TrustedHello UDS
/// connection to the chromatindb node. Forwards allowed messages bidirectionally.
class RelaySession : public std::enable_shared_from_this<RelaySession> {
public:
    using Ptr = std::shared_ptr<RelaySession>;
    using CloseCallback = std::function<void(RelaySession::Ptr)>;

    /// Create a relay session for an authenticated client connection.
    /// client_conn must already be post-handshake (authenticated).
    static Ptr create(chromatindb::net::Connection::Ptr client_conn,
                      const std::string& uds_path,
                      const chromatindb::identity::NodeIdentity& identity,
                      asio::io_context& ioc);

    /// Start the session: connect UDS to node, begin bidirectional forwarding.
    /// Returns false if UDS connection fails (per D-03: refuse client).
    asio::awaitable<bool> start();

    /// Tear down both connections.
    void stop();

    /// Set callback for when session ends (both directions closed).
    void on_close(CloseCallback cb) { close_cb_ = std::move(cb); }

    /// Client's public key hash hex string (for logging per D-06).
    const std::string& client_pubkey_hex() const { return client_pk_hex_; }

    /// Client's remote address (for logging per D-06).
    const std::string& client_address() const;

private:
    RelaySession(chromatindb::net::Connection::Ptr client_conn,
                 std::string uds_path,
                 const chromatindb::identity::NodeIdentity& identity,
                 asio::io_context& ioc);

    /// Forward messages from client to node (with filtering per RELAY-03).
    void handle_client_message(chromatindb::net::Connection::Ptr conn,
                               chromatindb::wire::TransportMsgType type,
                               std::vector<uint8_t> payload,
                               uint32_t request_id);

    /// Forward messages from node to client (no filtering -- node only sends client types).
    void handle_node_message(chromatindb::net::Connection::Ptr conn,
                             chromatindb::wire::TransportMsgType type,
                             std::vector<uint8_t> payload,
                             uint32_t request_id);

    /// Handle client disconnect.
    void handle_client_close(chromatindb::net::Connection::Ptr conn, bool graceful);

    /// Handle node UDS disconnect (per D-04: disconnect client immediately).
    void handle_node_close(chromatindb::net::Connection::Ptr conn, bool graceful);

    /// Shared teardown logic.
    void teardown(const std::string& reason);

    chromatindb::net::Connection::Ptr client_conn_;
    chromatindb::net::Connection::Ptr node_conn_;
    std::string uds_path_;
    const chromatindb::identity::NodeIdentity& identity_;
    asio::io_context& ioc_;
    std::string client_pk_hex_;
    bool stopped_ = false;

    CloseCallback close_cb_;
};

} // namespace chromatindb::relay::core
