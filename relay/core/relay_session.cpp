#include "relay/core/relay_session.h"
#include "relay/core/message_filter.h"
#include "db/crypto/hash.h"
#include "db/util/hex.h"

#include <spdlog/spdlog.h>

using chromatindb::util::to_hex;

namespace chromatindb::relay::core {

RelaySession::RelaySession(chromatindb::net::Connection::Ptr client_conn,
                           std::string uds_path,
                           const chromatindb::identity::NodeIdentity& identity,
                           asio::io_context& ioc)
    : client_conn_(std::move(client_conn))
    , uds_path_(std::move(uds_path))
    , identity_(identity)
    , ioc_(ioc) {
    // Compute client public key hash hex for logging (D-06)
    auto pk_hash = crypto::sha3_256(client_conn_->peer_pubkey());
    client_pk_hex_ = to_hex(pk_hash);
}

RelaySession::Ptr RelaySession::create(
    chromatindb::net::Connection::Ptr client_conn,
    const std::string& uds_path,
    const chromatindb::identity::NodeIdentity& identity,
    asio::io_context& ioc) {
    return Ptr(new RelaySession(std::move(client_conn), uds_path, identity, ioc));
}

asio::awaitable<bool> RelaySession::start() {
    // 1. Open UDS socket and connect to chromatindb node
    asio::local::stream_protocol::socket uds_socket(ioc_);
    auto [ec] = co_await uds_socket.async_connect(
        asio::local::stream_protocol::endpoint(uds_path_),
        chromatindb::net::use_nothrow);

    if (ec) {
        spdlog::error("failed to connect to node UDS {}: {} (client {})",
                      uds_path_, ec.message(), client_pk_hex_);
        co_return false;  // Per D-03: refuse client
    }

    // 2. Create node connection (UDS outbound = TrustedHello initiator)
    node_conn_ = chromatindb::net::Connection::create_uds_outbound(
        std::move(uds_socket), identity_);

    // 3. Set up close callbacks
    auto self = shared_from_this();

    client_conn_->on_close([self](chromatindb::net::Connection::Ptr conn, bool graceful) {
        self->handle_client_close(conn, graceful);
    });

    node_conn_->on_close([self](chromatindb::net::Connection::Ptr conn, bool graceful) {
        self->handle_node_close(conn, graceful);
    });

    // 4. Set node on_ready: wire message forwarding only after TrustedHello completes
    node_conn_->on_ready([self](chromatindb::net::Connection::Ptr /*conn*/) {
        // Now it's safe to forward messages in both directions
        self->client_conn_->on_message(
            [self](chromatindb::net::Connection::Ptr conn,
                   chromatindb::wire::TransportMsgType type,
                   std::vector<uint8_t> payload) {
                self->handle_client_message(conn, type, std::move(payload));
            });

        self->node_conn_->on_message(
            [self](chromatindb::net::Connection::Ptr conn,
                   chromatindb::wire::TransportMsgType type,
                   std::vector<uint8_t> payload) {
                self->handle_node_message(conn, type, std::move(payload));
            });

        spdlog::info("session active: client {} from {}",
                     self->client_pk_hex_, self->client_conn_->remote_address());
    });

    // 5. Spawn node connection run (TrustedHello handshake + message loop)
    asio::co_spawn(ioc_, node_conn_->run(), asio::detached);

    co_return true;
}

void RelaySession::stop() {
    teardown("stop requested");
}

const std::string& RelaySession::client_address() const {
    return client_conn_->remote_address();
}

void RelaySession::handle_client_message(
    chromatindb::net::Connection::Ptr /*conn*/,
    chromatindb::wire::TransportMsgType type,
    std::vector<uint8_t> payload) {

    if (!is_client_allowed(type)) {
        // Per D-07: warn with type name and client pubkey hash
        spdlog::warn("blocked message type {} from client {}",
                     type_name(type), client_pk_hex_);
        // Per D-01: disconnect client on blocked type
        teardown("blocked message type");
        return;
    }

    // Forward allowed message to node
    auto self = shared_from_this();
    auto t = type;
    auto p = std::move(payload);
    auto conn = node_conn_;
    asio::co_spawn(ioc_, [self, conn, t, p = std::move(p)]() -> asio::awaitable<void> {
        co_await conn->send_message(t, p);
    }, asio::detached);
}

void RelaySession::handle_node_message(
    chromatindb::net::Connection::Ptr /*conn*/,
    chromatindb::wire::TransportMsgType type,
    std::vector<uint8_t> payload) {

    // No filtering on node->client direction -- node only sends client-understood types
    auto self = shared_from_this();
    auto t = type;
    auto p = std::move(payload);
    auto conn = client_conn_;
    asio::co_spawn(ioc_, [self, conn, t, p = std::move(p)]() -> asio::awaitable<void> {
        co_await conn->send_message(t, p);
    }, asio::detached);
}

void RelaySession::handle_client_close(
    chromatindb::net::Connection::Ptr /*conn*/, bool graceful) {
    // Per D-06: info level with reason
    spdlog::info("client disconnected: {} ({})",
                 client_pk_hex_,
                 graceful ? "graceful" : "error");
    teardown("client disconnected");
}

void RelaySession::handle_node_close(
    chromatindb::net::Connection::Ptr /*conn*/, bool /*graceful*/) {
    // Per D-04: disconnect client immediately on node UDS loss
    spdlog::info("node UDS connection lost for client {}", client_pk_hex_);
    teardown("node disconnected");
}

void RelaySession::teardown(const std::string& reason) {
    if (stopped_) return;
    stopped_ = true;

    spdlog::info("session teardown: {} (client {})", reason, client_pk_hex_);

    // Close both connections
    if (client_conn_) client_conn_->close();
    if (node_conn_) node_conn_->close();

    // Fire close callback
    if (close_cb_) {
        close_cb_(shared_from_this());
    }
}

} // namespace chromatindb::relay::core
