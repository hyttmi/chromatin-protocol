#pragma once

#include "db/identity/identity.h"
#include "db/net/framing.h"
#include "db/net/handshake.h"
#include "db/net/protocol.h"

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/as_tuple.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace chromatindb::net {

/// Completion token for non-throwing async operations.
constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);

/// A single peer connection: handles handshake, encrypted IO, heartbeat.
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using Ptr = std::shared_ptr<Connection>;

    /// Callback for received data messages.
    using MessageCallback = std::function<void(
        Connection::Ptr, wire::TransportMsgType, std::vector<uint8_t>)>;

    /// Callback for connection close/error.
    using CloseCallback = std::function<void(Connection::Ptr, bool /*graceful*/)>;

    /// Create a connection from an accepted inbound socket (responder role).
    static Ptr create_inbound(asio::ip::tcp::socket socket,
                               const identity::NodeIdentity& identity);

    /// Create a connection for outbound (initiator role).
    /// Socket must already be connected.
    static Ptr create_outbound(asio::ip::tcp::socket socket,
                                const identity::NodeIdentity& identity);

    ~Connection();

    /// Run the connection lifecycle: handshake -> message loop -> cleanup.
    asio::awaitable<bool> run();

    /// Send a transport message (encrypted).
    asio::awaitable<bool> send_message(wire::TransportMsgType type,
                                        std::span<const uint8_t> payload);

    /// Send goodbye and close gracefully.
    asio::awaitable<void> close_gracefully();

    /// Force close (cancel all pending ops).
    void close();

    /// Whether handshake has completed successfully.
    bool is_authenticated() const { return authenticated_; }

    /// Whether this is the initiator (outbound) side of the connection.
    bool is_initiator() const { return is_initiator_; }

    /// Peer's signing public key (available after auth).
    const std::vector<uint8_t>& peer_pubkey() const { return peer_pubkey_; }

    /// Set message callback.
    void on_message(MessageCallback cb) { message_cb_ = std::move(cb); }

    /// Set close callback.
    void on_close(CloseCallback cb) { close_cb_ = std::move(cb); }

    /// Callback for post-handshake (before message loop starts).
    using ReadyCallback = std::function<void(Connection::Ptr)>;

    /// Set callback fired after handshake succeeds, before message loop.
    /// This is where PeerManager should set up message routing and start sync.
    void on_ready(ReadyCallback cb) { ready_cb_ = std::move(cb); }

    /// Set thread pool reference for crypto offload.
    void set_pool(asio::thread_pool& pool) { pool_ = &pool; }

    /// Trust-check function type: returns true if the IP address is trusted.
    using TrustCheck = std::function<bool(const asio::ip::address&)>;

    /// Set trust-check for lightweight handshake path.
    void set_trust_check(TrustCheck check) { trust_check_ = std::move(check); }

    /// Whether this connection was received as goodbye (graceful peer shutdown).
    bool received_goodbye() const { return received_goodbye_; }

    /// Remote peer address string (set at construction time).
    const std::string& remote_address() const { return remote_addr_; }

private:
    Connection(asio::ip::tcp::socket socket,
               const identity::NodeIdentity& identity,
               bool is_initiator);

    /// Perform the full handshake (KEM + auth, or lightweight for trusted peers).
    asio::awaitable<bool> do_handshake();

    /// Handshake sub-paths
    asio::awaitable<bool> do_handshake_initiator_trusted();
    asio::awaitable<bool> do_handshake_initiator_pq();
    asio::awaitable<bool> do_handshake_responder_trusted(std::vector<uint8_t> payload);
    asio::awaitable<bool> do_handshake_responder_pq_fallback(std::vector<uint8_t> payload);
    asio::awaitable<bool> do_handshake_responder_pq(std::vector<uint8_t> first_msg);

    /// Run the message read loop after handshake.
    asio::awaitable<void> message_loop();

    /// Send a single encrypted frame over the socket.
    asio::awaitable<bool> send_raw(std::span<const uint8_t> data);

    /// Read a single raw message from the socket (length-prefixed).
    asio::awaitable<std::optional<std::vector<uint8_t>>> recv_raw();

    /// Send an encrypted frame.
    asio::awaitable<bool> send_encrypted(std::span<const uint8_t> plaintext);

    /// Receive and decrypt a frame.
    asio::awaitable<std::optional<std::vector<uint8_t>>> recv_encrypted();

    asio::ip::tcp::socket socket_;
    const identity::NodeIdentity& identity_;
    bool is_initiator_;
    bool authenticated_ = false;
    bool closed_ = false;
    bool received_goodbye_ = false;

    SessionKeys session_keys_;
    uint64_t send_counter_ = 0;
    uint64_t recv_counter_ = 0;
    std::vector<uint8_t> peer_pubkey_;

    std::string remote_addr_;

    asio::thread_pool* pool_ = nullptr;

    MessageCallback message_cb_;
    CloseCallback close_cb_;
    ReadyCallback ready_cb_;
    TrustCheck trust_check_;
};

} // namespace chromatindb::net
