#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chromatindb::relay::ws {

// RFC 6455 opcodes
constexpr uint8_t OPCODE_CONTINUATION = 0x00;
constexpr uint8_t OPCODE_TEXT         = 0x01;
constexpr uint8_t OPCODE_BINARY       = 0x02;
constexpr uint8_t OPCODE_CLOSE        = 0x08;
constexpr uint8_t OPCODE_PING         = 0x09;
constexpr uint8_t OPCODE_PONG         = 0x0A;

// Maximum message sizes (per D-09)
constexpr size_t MAX_TEXT_MESSAGE_SIZE   = 1 * 1024 * 1024;    // 1 MiB
constexpr size_t MAX_BINARY_MESSAGE_SIZE = 110 * 1024 * 1024;  // 110 MiB

// Maximum control frame payload (RFC 6455 Section 5.5)
constexpr size_t MAX_CONTROL_PAYLOAD_SIZE = 125;

// Close status codes (RFC 6455 Section 7.4.1)
constexpr uint16_t CLOSE_NORMAL           = 1000;
constexpr uint16_t CLOSE_GOING_AWAY       = 1001;
constexpr uint16_t CLOSE_PROTOCOL_ERROR   = 1002;
constexpr uint16_t CLOSE_UNSUPPORTED_DATA = 1003;
constexpr uint16_t CLOSE_NO_STATUS        = 1005;

/// Parsed WebSocket frame header.
struct FrameHeader {
    bool fin;
    uint8_t opcode;
    bool masked;
    uint64_t payload_length;
    uint8_t mask_key[4];
    size_t header_size;  // Total bytes consumed by header
};

/// Encode a server-to-client WebSocket frame (never masked).
/// Handles all three payload length modes: 7-bit (0-125), 16-bit BE (126 prefix),
/// 64-bit BE (127 prefix).
std::string encode_frame(uint8_t opcode, std::span<const uint8_t> payload, bool fin = true);

/// Encode a close frame with BE status code and optional UTF-8 reason.
std::string encode_close_frame(uint16_t status_code, std::string_view reason = "");

/// Parse a WebSocket frame header from a buffer.
/// Returns nullopt if there are insufficient bytes to parse the complete header.
std::optional<FrameHeader> parse_frame_header(std::span<const uint8_t> buf);

/// XOR mask/unmask payload data. payload[i] ^= mask[i % 4].
/// Applying the same mask twice restores the original data.
void apply_mask(std::span<uint8_t> payload, const uint8_t mask[4]);

/// Returns true for control opcodes (close, ping, pong).
bool is_control_opcode(uint8_t opcode);

/// Result of fragment assembly.
enum class AssemblyResult {
    INCOMPLETE,  // More frames needed
    COMPLETE,    // Full message assembled
    CONTROL,     // Control frame received (returned immediately)
    ERROR        // Protocol error
};

/// An assembled WebSocket message (or control frame).
struct AssembledMessage {
    AssemblyResult result;
    uint8_t opcode;           // Original opcode (text/binary) or control opcode
    std::vector<uint8_t> data;
    uint16_t close_code;      // Only meaningful for close frames
    std::string error_reason;
};

/// Reassembles fragmented WebSocket messages per RFC 6455 Section 5.4.
/// Control frames interleaved in fragmented messages are returned immediately
/// without disturbing fragment state.
class FragmentAssembler {
public:
    /// Feed a decoded frame (already unmasked). Returns assembly state.
    AssembledMessage feed(uint8_t opcode, bool fin, std::span<const uint8_t> payload);

    /// Reset assembler state, discarding any partial message.
    void reset();

private:
    std::vector<uint8_t> buffer_;
    uint8_t start_opcode_ = 0;
    bool in_fragment_ = false;
};

} // namespace chromatindb::relay::ws
