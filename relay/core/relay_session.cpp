#include "relay/core/relay_session.h"
#include "relay/core/message_filter.h"
#include "db/crypto/hash.h"
#include "db/peer/peer_manager.h"
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
        self->wire_node_handlers();
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
    std::vector<uint8_t> payload,
    uint32_t request_id) {

    if (!is_client_allowed(type)) {
        // Per D-07: warn with type name and client pubkey hash
        spdlog::warn("blocked message type {} from client {}",
                     type_name(type), client_pk_hex_);
        // Per D-01: disconnect client on blocked type
        teardown("blocked message type");
        return;
    }

    // Per D-09/FILT-03: Intercept Subscribe/Unsubscribe to track subscriptions locally
    if (type == chromatindb::wire::TransportMsgType_Subscribe) {
        auto namespaces = chromatindb::peer::PeerManager::decode_namespace_list(payload);
        for (const auto& ns : namespaces) {
            if (subscribed_namespaces_.size() >= MAX_SUBSCRIPTIONS) {
                spdlog::warn("client {} exceeded subscription cap ({}), rejecting",
                             client_pk_hex_, MAX_SUBSCRIPTIONS);
                break;  // Per D-08: reject beyond cap
            }
            subscribed_namespaces_.insert(ns);
        }
        spdlog::debug("client {} subscribed: {} new, {} total",
                      client_pk_hex_, namespaces.size(), subscribed_namespaces_.size());
    }
    if (type == chromatindb::wire::TransportMsgType_Unsubscribe) {
        auto namespaces = chromatindb::peer::PeerManager::decode_namespace_list(payload);
        for (const auto& ns : namespaces) {
            subscribed_namespaces_.erase(ns);
        }
        spdlog::debug("client {} unsubscribed: {} removed, {} remaining",
                      client_pk_hex_, namespaces.size(), subscribed_namespaces_.size());
    }

    // Per D-04: Drop client->node messages silently during RECONNECTING/DEAD
    // Per D-14: Also drop during replay_pending_ (replay in progress)
    if (state_ != SessionState::ACTIVE || replay_pending_) {
        return;
    }

    // Forward allowed message to node
    auto self = shared_from_this();
    auto t = type;
    auto p = std::move(payload);
    auto conn = node_conn_;
    asio::co_spawn(ioc_, [self, conn, t, p = std::move(p), rid = request_id]() -> asio::awaitable<void> {
        co_await conn->send_message(t, p, rid);
    }, asio::detached);
}

void RelaySession::handle_node_message(
    chromatindb::net::Connection::Ptr /*conn*/,
    chromatindb::wire::TransportMsgType type,
    std::vector<uint8_t> payload,
    uint32_t request_id) {

    // Per D-10/FILT-03: Filter Notification by client subscription set
    if (type == chromatindb::wire::TransportMsgType_Notification) {
        if (payload.size() < 32) return;  // Malformed notification, drop
        std::array<uint8_t, 32> ns_id{};
        std::memcpy(ns_id.data(), payload.data(), 32);
        if (subscribed_namespaces_.find(ns_id) == subscribed_namespaces_.end()) {
            return;  // Not subscribed to this namespace, drop silently
        }
    }

    // Forward to client
    auto self = shared_from_this();
    auto t = type;
    auto p = std::move(payload);
    auto conn = client_conn_;
    asio::co_spawn(ioc_, [self, conn, t, p = std::move(p), rid = request_id]() -> asio::awaitable<void> {
        co_await conn->send_message(t, p, rid);
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
    if (state_ == SessionState::DEAD || stopped_) return;
    // Per D-02: Don't re-enter reconnect if already reconnecting
    if (state_ == SessionState::RECONNECTING) return;

    spdlog::info("node UDS connection lost for client {}, entering RECONNECTING",
                 client_pk_hex_);
    state_ = SessionState::RECONNECTING;
    node_conn_.reset();  // Per D-02: release old connection, new socket per attempt

    // Spawn reconnection coroutine
    auto self = shared_from_this();
    asio::co_spawn(ioc_, [self]() -> asio::awaitable<void> {
        co_await self->reconnect_loop();
    }, asio::detached);
}

void RelaySession::teardown(const std::string& reason) {
    if (stopped_) return;
    stopped_ = true;
    state_ = SessionState::DEAD;

    spdlog::info("session teardown: {} (client {})", reason, client_pk_hex_);

    // Close both connections
    if (client_conn_) client_conn_->close();
    if (node_conn_) node_conn_->close();

    // Fire close callback
    if (close_cb_) {
        close_cb_(shared_from_this());
    }
}

asio::awaitable<void> RelaySession::reconnect_loop() {
    auto self = shared_from_this();

    for (reconnect_attempts_ = 0; reconnect_attempts_ < MAX_RECONNECT_ATTEMPTS;
         ++reconnect_attempts_) {
        // Jittered backoff delay
        auto delay = jittered_backoff(reconnect_attempts_);
        asio::steady_timer timer(ioc_);
        timer.expires_after(delay);
        auto [ec] = co_await timer.async_wait(chromatindb::net::use_nothrow);
        if (stopped_) co_return;  // Shutdown during wait

        spdlog::debug("client {}: reconnect attempt {}/{} after {}ms",
                      client_pk_hex_, reconnect_attempts_ + 1,
                      MAX_RECONNECT_ATTEMPTS, delay.count());

        // Per D-02: new UDS socket per attempt
        asio::local::stream_protocol::socket uds_socket(ioc_);
        auto [connect_ec] = co_await uds_socket.async_connect(
            asio::local::stream_protocol::endpoint(uds_path_),
            chromatindb::net::use_nothrow);

        if (connect_ec) {
            spdlog::debug("client {}: reconnect attempt {} failed: {}",
                          client_pk_hex_, reconnect_attempts_ + 1,
                          connect_ec.message());
            continue;  // Try again with next backoff
        }

        // UDS connected -- create new Connection (per D-02: new socket, new AEAD state)
        node_conn_ = chromatindb::net::Connection::create_uds_outbound(
            std::move(uds_socket), identity_);

        // Wire up on_ready for subscription replay (per D-12)
        replay_pending_ = true;
        node_conn_->on_ready([self](chromatindb::net::Connection::Ptr /*conn*/) {
            // Per D-12/D-13: Replay all subscriptions as a single batch
            if (!self->subscribed_namespaces_.empty()) {
                std::vector<std::array<uint8_t, 32>> ns_list(
                    self->subscribed_namespaces_.begin(),
                    self->subscribed_namespaces_.end());
                auto payload = chromatindb::peer::PeerManager::encode_namespace_list(ns_list);
                asio::co_spawn(self->ioc_,
                    [self, payload = std::move(payload)]() -> asio::awaitable<void> {
                        co_await self->node_conn_->send_message(
                            chromatindb::wire::TransportMsgType_Subscribe, payload);
                        // Per D-07: successful reconnect resets counter, returns to ACTIVE
                        self->replay_pending_ = false;
                        self->state_ = SessionState::ACTIVE;
                        self->reconnect_attempts_ = 0;
                        spdlog::info("client {}: replayed {} subscriptions after UDS reconnect",
                                     self->client_pk_hex_,
                                     self->subscribed_namespaces_.size());
                    }, asio::detached);
            } else {
                // No subscriptions to replay -- go straight to ACTIVE
                self->replay_pending_ = false;
                self->state_ = SessionState::ACTIVE;
                self->reconnect_attempts_ = 0;
                spdlog::info("client {}: UDS reconnected (no subscriptions to replay)",
                             self->client_pk_hex_);
            }

            // Re-wire message forwarding (same pattern as initial start())
            self->wire_node_handlers();
        });

        // Wire on_close to re-enter reconnect if node drops again (Pitfall 2 protection)
        node_conn_->on_close([self](chromatindb::net::Connection::Ptr conn, bool graceful) {
            self->handle_node_close(conn, graceful);
        });

        // Spawn node connection run (TrustedHello + message loop)
        asio::co_spawn(self->ioc_, self->node_conn_->run(), asio::detached);

        co_return;  // on_ready will handle ACTIVE transition
    }

    // Per D-06: exhausted max attempts -> DEAD -> disconnect client TCP
    spdlog::warn("client {}: UDS reconnection failed after {} attempts, entering DEAD",
                 client_pk_hex_, MAX_RECONNECT_ATTEMPTS);
    state_ = SessionState::DEAD;
    teardown("reconnection failed after max attempts");
}

std::chrono::milliseconds RelaySession::jittered_backoff(uint32_t attempt) {
    auto exp = std::min(BACKOFF_CAP_MS,
                        BACKOFF_BASE_MS * (1u << std::min(attempt, 14u)));
    std::uniform_int_distribution<uint32_t> dist(0, exp);
    return std::chrono::milliseconds(dist(rng_));
}

void RelaySession::wire_node_handlers() {
    auto self = shared_from_this();
    client_conn_->on_message(
        [self](chromatindb::net::Connection::Ptr conn,
               chromatindb::wire::TransportMsgType type,
               std::vector<uint8_t> payload,
               uint32_t request_id) {
            self->handle_client_message(conn, type, std::move(payload), request_id);
        });
    node_conn_->on_message(
        [self](chromatindb::net::Connection::Ptr conn,
               chromatindb::wire::TransportMsgType type,
               std::vector<uint8_t> payload,
               uint32_t request_id) {
            self->handle_node_message(conn, type, std::move(payload), request_id);
        });
}

} // namespace chromatindb::relay::core
