#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kademlia/node_id.h"

namespace helix::kademlia {

// Protocol constants from PROTOCOL-SPEC.md section 1
inline constexpr uint8_t HELIX_MAGIC[] = {'H', 'E', 'L', 'I', 'X'};
inline constexpr uint8_t PROTOCOL_VERSION = 0x01;
inline constexpr size_t HEADER_SIZE = 5 + 1 + 1 + 1 + 32 + 4; // magic + version + type + reserved + sender_id + payload_length
inline constexpr size_t MAX_UDP_MESSAGE = 65507;

enum class MessageType : uint8_t {
    PING       = 0x00,
    PONG       = 0x01,
    FIND_NODE  = 0x02,
    NODES      = 0x03,
    STORE      = 0x04,
    FIND_VALUE = 0x05,
    VALUE      = 0x06,
    SYNC_REQ   = 0x07,
    SYNC_RESP  = 0x08
};

struct Message {
    MessageType type;
    NodeId sender;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> signature;
};

// Serialize a message into the HELIX binary wire format:
// [5 bytes HELIX magic][1 byte version][1 byte type][1 byte reserved=0x00]
// [32 bytes sender_id][4 bytes BE payload_length][payload]
// [2 bytes BE signature_length][signature]
std::vector<uint8_t> serialize_message(const Message& msg);

// Deserialize a message from the HELIX binary wire format.
// Returns nullopt on invalid magic, version, or truncated data.
std::optional<Message> deserialize_message(std::span<const uint8_t> data);

// Build the signed_data buffer from a message:
// magic || version || type || reserved || sender_id || payload_length || payload
// This is everything before the signature_length field in the wire format.
std::vector<uint8_t> build_signed_data(const Message& msg);

// Sign a message with ML-DSA-87. Sets msg.signature.
// signed_data = magic || version || type || reserved || sender_id || payload_length_bytes || payload
void sign_message(Message& msg, std::span<const uint8_t> secret_key);

// Verify a message's ML-DSA-87 signature.
// Reconstructs signed_data and verifies against the provided public key.
bool verify_message(const Message& msg, std::span<const uint8_t> public_key);

// UDP transport for node-to-node communication using POSIX sockets.
class UdpTransport {
public:
    UdpTransport(const std::string& bind_addr, uint16_t port);
    ~UdpTransport();

    // Non-copyable
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // Send a serialized message to a remote address.
    void send(const std::string& addr, uint16_t port, const Message& msg);

    // Handler callback: called for each received message with sender address info.
    using Handler = std::function<void(const Message& msg, const std::string& from_addr, uint16_t from_port)>;

    // Blocking receive loop using select() with 100ms timeout.
    // Calls handler for each valid received message.
    void run(Handler handler);

    // Signal the recv loop to stop.
    void stop();

    // Return the local port (useful when bound to port 0 for ephemeral port).
    uint16_t local_port() const;

private:
    int sockfd_ = -1;
    uint16_t port_;
    std::atomic<bool> running_{false};
};

} // namespace helix::kademlia
