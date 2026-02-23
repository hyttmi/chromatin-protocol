#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "crypto/crypto.h"
#include "kademlia/node_id.h"
#include "kademlia/tcp_encryption.h"

namespace chromatin::kademlia {

// Protocol constants from PROTOCOL-SPEC.md section 1
inline constexpr uint8_t CHRM_MAGIC[] = {'C', 'H', 'R', 'M'};
inline constexpr uint8_t PROTOCOL_VERSION = 0x01;
// magic(4) + version(1) + type(1) + sender_port(2) + sender_id(32) + payload_length(4) = 44
inline constexpr size_t HEADER_SIZE = 4 + 1 + 1 + 2 + 32 + 4;

enum class MessageType : uint8_t {
    PING       = 0x00,
    PONG       = 0x01,
    FIND_NODE  = 0x02,
    NODES      = 0x03,
    STORE      = 0x04,
    FIND_VALUE = 0x05,
    VALUE      = 0x06,
    SYNC_REQ   = 0x07,
    SYNC_RESP  = 0x08,
    STORE_ACK  = 0x09,
    SEQ_REQ    = 0x0A,
    SEQ_RESP   = 0x0B
};

struct Message {
    MessageType type;
    NodeId sender;
    uint16_t sender_port = 0; // sender's listening port (carried in header)
    std::vector<uint8_t> payload;
    std::vector<uint8_t> signature;
};

// Serialize a message into the CHRM binary wire format:
// [4 bytes CHRM magic][1 byte version][1 byte type][2 bytes BE sender_port]
// [32 bytes sender_id][4 bytes BE payload_length][payload]
// [2 bytes BE signature_length][signature]
std::vector<uint8_t> serialize_message(const Message& msg);

// Deserialize a message from the CHRM binary wire format.
// Returns nullopt on invalid magic, version, or truncated data.
std::optional<Message> deserialize_message(std::span<const uint8_t> data);

// Build the signed_data buffer from a message:
// magic || version || type || sender_port || sender_id || payload_length || payload
// This is everything before the signature_length field in the wire format.
std::vector<uint8_t> build_signed_data(const Message& msg);

// Sign a message with ML-DSA-87. Sets msg.signature.
void sign_message(Message& msg, std::span<const uint8_t> secret_key);

// Verify a message's ML-DSA-87 signature.
bool verify_message(const Message& msg, std::span<const uint8_t> public_key);

// TCP transport for node-to-node communication.
// Reuses connections via an internal pool to avoid per-message TCP handshakes.
// All connections use ML-KEM-1024 + ChaCha20-Poly1305 encryption.
class TcpTransport {
public:
    TcpTransport(const std::string& bind_addr, uint16_t port,
                 uint16_t connect_timeout = 5, uint16_t read_timeout = 5,
                 size_t max_pool_size = 64, uint16_t conn_idle_seconds = 60,
                 uint64_t max_message_size = 50ULL * 1024 * 1024,
                 size_t max_tcp_clients = 256);
    ~TcpTransport();

    // Non-copyable
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // Set signing keypair for handshake authentication.
    void set_signing_keypair(const crypto::KeyPair& kp);

    // Set node ID for handshake.
    void set_node_id(const NodeId& id);

    // Callback to look up a node's ML-DSA-87 public key by node ID.
    // Returns the pubkey if known, nullopt otherwise.
    using PubkeyLookup = std::function<std::optional<std::vector<uint8_t>>(const NodeId&)>;
    void set_pubkey_lookup(PubkeyLookup fn);

    // Send a serialized message to a remote address, reusing pooled connections.
    void send(const std::string& addr, uint16_t port, const Message& msg);

    // Handler callback: called for each received message with sender address info.
    using Handler = std::function<void(const Message& msg, const std::string& from_addr, uint16_t from_port)>;

    // Blocking accept loop using poll() with 100ms timeout.
    // Accepts connections, reads multiple framed messages per connection.
    void run(Handler handler);

    // Signal the accept loop to stop.
    void stop();

    // Return the local port (useful when bound to port 0 for ephemeral port).
    uint16_t local_port() const;

    // Close pooled connections that have been idle longer than CONN_MAX_IDLE.
    // Safe to call periodically (e.g. from Kademlia::tick()).
    void cleanup_idle_connections();

private:
    int listen_fd_ = -1;
    uint16_t port_;
    uint16_t connect_timeout_;
    uint16_t read_timeout_;
    size_t max_pool_size_;
    std::chrono::seconds conn_max_idle_;
    uint64_t max_message_size_;
    size_t max_tcp_clients_;
    std::atomic<bool> running_{true};

    // Encryption state
    NodeId local_node_id_{};
    const crypto::KeyPair* signing_keypair_ = nullptr;
    PubkeyLookup pubkey_lookup_;

    // Connection pool: keyed by "[addr]:port"
    struct PooledConnection {
        int fd = -1;
        std::chrono::steady_clock::time_point last_used;
        std::optional<tcp_encryption::SessionKeys> session;  // nullopt = plaintext
    };
    std::unordered_map<std::string, PooledConnection> conn_pool_;
    std::mutex pool_mutex_;

    // Active encrypted client sessions on the accept side: fd → SessionKeys
    std::unordered_map<int, tcp_encryption::SessionKeys> client_sessions_;

    // Perform handshake as initiator on a new outgoing connection.
    // Returns true if encryption was established, false on failure.
    bool perform_initiator_handshake(int fd, const std::string& pool_key);

    // Send data on connection, encrypting if session exists.
    bool send_on_connection(int fd, const std::vector<uint8_t>& data,
                           std::optional<tcp_encryption::SessionKeys>* session);

    // Read one message from connection, decrypting if needed.
    std::optional<Message> read_message(int fd, tcp_encryption::SessionKeys* session);
};

} // namespace chromatin::kademlia
