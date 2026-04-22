#pragma once

#include "db/identity/identity.h"
#include "db/net/framing.h"
#include "db/net/handshake.h"
#include "db/net/protocol.h"
#include "db/net/role.h"

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/as_tuple.hpp>
#include <asio/generic/stream_protocol.hpp>
#include <asio/local/stream_protocol.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
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
        Connection::Ptr, wire::TransportMsgType, std::vector<uint8_t>, uint32_t)>;

    /// Callback for connection close/error.
    using CloseCallback = std::function<void(Connection::Ptr, bool /*graceful*/)>;

    /// Create a connection from an accepted inbound socket (responder role).
    static Ptr create_inbound(asio::ip::tcp::socket socket,
                               const identity::NodeIdentity& identity);

    /// Create a connection for outbound (initiator role).
    /// Socket must already be connected.
    static Ptr create_outbound(asio::ip::tcp::socket socket,
                                const identity::NodeIdentity& identity);

    /// Create a UDS inbound connection (responder, from UdsAcceptor).
    static Ptr create_uds_inbound(asio::local::stream_protocol::socket socket,
                                   const identity::NodeIdentity& identity);

    /// Create a UDS outbound connection (initiator, for UDS clients).
    static Ptr create_uds_outbound(asio::local::stream_protocol::socket socket,
                                    const identity::NodeIdentity& identity);

    ~Connection();

    /// Result of reassembling a chunked sub-frame sequence.
    struct ReassembledChunked {
        uint8_t type;
        uint32_t request_id;
        std::vector<uint8_t> payload;
    };

    /// Run the connection lifecycle: handshake -> message loop -> cleanup.
    asio::awaitable<bool> run();

    /// Send a transport message (encrypted). Automatically uses chunked mode
    /// for payloads >= STREAMING_THRESHOLD.
    asio::awaitable<bool> send_message(wire::TransportMsgType type,
                                        std::span<const uint8_t> payload,
                                        uint32_t request_id = 0);

    /// Send goodbye and close gracefully.
    asio::awaitable<void> close_gracefully();

    /// Force close (cancel all pending ops).
    void close();

    /// Whether handshake has completed successfully.
    bool is_authenticated() const { return authenticated_; }

    /// Whether this is the initiator (outbound) side of the connection.
    bool is_initiator() const { return is_initiator_; }

    /// Whether this connection uses a Unix domain socket transport.
    bool is_uds() const { return is_uds_; }

    /// Peer's signing public key (available after auth).
    const std::vector<uint8_t>& peer_pubkey() const { return peer_pubkey_; }

    /// Role this side declares in its own AuthSignature payload.
    /// Default Role::Peer (node-daemon default). Callers acting as clients
    /// (e.g. cdb CLI, peer-management UDS clients) must set this to
    /// Role::Client before the handshake starts.
    void set_local_role(Role r) { local_role_ = r; }
    Role local_role() const { return local_role_; }

    /// Role the remote end declared in its AuthSignature payload.
    /// Available only after the handshake has completed; callers should
    /// check `is_authenticated()` first. This is what ACL/classifier code
    /// should consult, never the ACL-list membership.
    Role peer_role() const { return peer_role_; }

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

    /// Update blob data cap for chunked reassembly enforcement (seeded by PeerManager
    /// on connect + SIGHUP reload). BLOB-01/BLOB-03.
    void set_blob_max_bytes(uint64_t cap) { blob_max_bytes_ = cap; }

    /// Trust-check function type: returns true if the IP address is trusted.
    using TrustCheck = std::function<bool(const asio::ip::address&)>;

    /// Set trust-check for lightweight handshake path.
    void set_trust_check(TrustCheck check) { trust_check_ = std::move(check); }

    /// Whether this connection was received as goodbye (graceful peer shutdown).
    bool received_goodbye() const { return received_goodbye_; }

    /// Timestamp of last received message (for keepalive liveness check).
    std::chrono::steady_clock::time_point last_recv_time() const { return last_recv_time_; }

    /// Remote peer address string (set at construction time).
    const std::string& remote_address() const { return remote_addr_; }

    /// Original connect address (e.g., "host:port" from bootstrap/PEX).
    /// Empty for inbound connections.
    void set_connect_address(const std::string& a) { connect_address_ = a; }
    const std::string& connect_address() const { return connect_address_; }

private:
    Connection(asio::generic::stream_protocol::socket socket,
               const identity::NodeIdentity& identity,
               bool is_initiator,
               bool is_uds);

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

    /// Reassemble a chunked sub-frame sequence into a single message.
    /// Called when message_loop detects CHUNKED_BEGIN (0x01) flag byte.
    asio::awaitable<std::optional<ReassembledChunked>> recv_chunked(
        const std::vector<uint8_t>& first_frame);

    /// Send a large message as chunked sub-frames.
    /// Used for payloads >= STREAMING_THRESHOLD.
    asio::awaitable<bool> send_message_chunked(wire::TransportMsgType type,
                                                std::span<const uint8_t> payload,
                                                uint32_t request_id = 0,
                                                std::span<const uint8_t> extra_metadata = {});

    /// Enqueue an encoded message and wait for the drain coroutine to send it.
    asio::awaitable<bool> enqueue_send(std::vector<uint8_t> encoded);

    /// Single drain coroutine that serializes all outbound writes.
    asio::awaitable<void> drain_send_queue();

    /// Send a single encrypted frame over the socket.
    asio::awaitable<bool> send_raw(std::span<const uint8_t> data);

    /// Read a single raw message from the socket (length-prefixed).
    asio::awaitable<std::optional<std::vector<uint8_t>>> recv_raw();

    /// Send an encrypted frame.
    asio::awaitable<bool> send_encrypted(std::span<const uint8_t> plaintext);

    /// Receive and decrypt a frame.
    asio::awaitable<std::optional<std::vector<uint8_t>>> recv_encrypted();

    asio::generic::stream_protocol::socket socket_;
    const identity::NodeIdentity& identity_;
    bool is_initiator_;
    bool is_uds_ = false;
    bool authenticated_ = false;
    bool closed_ = false;
    bool received_goodbye_ = false;
    bool closing_ = false;  // Set after Goodbye enqueued, rejects new sends
    std::chrono::steady_clock::time_point last_recv_time_{std::chrono::steady_clock::now()};

    // Send queue (PUSH-04)
    struct PendingMessage {
        std::vector<uint8_t> encoded;     // TransportCodec::encode() result (or chunked header)
        asio::steady_timer* completion;   // Owned by enqueue_send coroutine's stack
        bool* result_ptr;                 // Points to local in enqueue_send
        bool is_chunked = false;          // if true, chunked_payload has data chunks
        std::vector<uint8_t> chunked_payload;  // full payload split into chunks by drain
    };
    std::deque<PendingMessage> send_queue_;
    asio::steady_timer send_signal_{socket_.get_executor()};
    bool drain_running_ = false;
    static constexpr size_t MAX_SEND_QUEUE = 1024;

    SessionKeys session_keys_;
    uint64_t send_counter_ = 0;
    uint64_t recv_counter_ = 0;

    // Test support: allow setting counters to test nonce exhaustion.
    // Only accessible via friend class, never compiled into public API.
    friend class ConnectionTestAccess;
    void set_send_counter_for_test(uint64_t v) { send_counter_ = v; }
    void set_recv_counter_for_test(uint64_t v) { recv_counter_ = v; }

    std::vector<uint8_t> peer_pubkey_;
    Role local_role_ = Role::Peer;
    Role peer_role_ = Role::Peer;

    std::string remote_addr_;
    std::string connect_address_;

    asio::thread_pool* pool_ = nullptr;

    uint64_t blob_max_bytes_ = 4ULL * 1024 * 1024;  // BLOB-01: seeded by PeerManager

    MessageCallback message_cb_;
    CloseCallback close_cb_;
    ReadyCallback ready_cb_;
    TrustCheck trust_check_;
};

} // namespace chromatindb::net
