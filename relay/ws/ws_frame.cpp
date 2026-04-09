#include "relay/ws/ws_frame.h"

#include <algorithm>
#include <cstring>

namespace chromatindb::relay::ws {

std::string encode_frame(uint8_t opcode, std::span<const uint8_t> payload, bool fin) {
    std::string frame;

    // Byte 0: FIN bit + opcode
    uint8_t byte0 = opcode & 0x0F;
    if (fin) byte0 |= 0x80;

    frame.push_back(static_cast<char>(byte0));

    // Byte 1+: payload length (server frames are NEVER masked, so MASK bit = 0)
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 65535) {
        frame.push_back(static_cast<char>(126));
        // 16-bit big-endian length
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        // 64-bit big-endian length
        uint64_t len = payload.size();
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
        }
    }

    // Payload data
    frame.append(reinterpret_cast<const char*>(payload.data()), payload.size());

    return frame;
}

std::string encode_close_frame(uint16_t status_code, std::string_view reason) {
    std::vector<uint8_t> payload;
    payload.reserve(2 + reason.size());
    // Big-endian status code
    payload.push_back(static_cast<uint8_t>((status_code >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(status_code & 0xFF));
    // Optional reason
    payload.insert(payload.end(), reason.begin(), reason.end());

    return encode_frame(OPCODE_CLOSE, payload);
}

std::optional<FrameHeader> parse_frame_header(std::span<const uint8_t> buf) {
    if (buf.size() < 2) return std::nullopt;

    FrameHeader hdr{};
    hdr.fin = (buf[0] & 0x80) != 0;
    hdr.opcode = buf[0] & 0x0F;
    hdr.masked = (buf[1] & 0x80) != 0;

    uint64_t len = buf[1] & 0x7F;
    size_t pos = 2;

    if (len == 126) {
        // 16-bit extended length
        if (buf.size() < pos + 2) return std::nullopt;
        len = (static_cast<uint64_t>(buf[pos]) << 8) | buf[pos + 1];
        pos += 2;
    } else if (len == 127) {
        // 64-bit extended length
        if (buf.size() < pos + 8) return std::nullopt;
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | buf[pos + i];
        }
        pos += 8;
    }

    hdr.payload_length = len;

    // Mask key (4 bytes, only if masked)
    if (hdr.masked) {
        if (buf.size() < pos + 4) return std::nullopt;
        std::memcpy(hdr.mask_key, buf.data() + pos, 4);
        pos += 4;
    } else {
        std::memset(hdr.mask_key, 0, 4);
    }

    hdr.header_size = pos;
    return hdr;
}

void apply_mask(std::span<uint8_t> payload, const uint8_t mask[4]) {
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] ^= mask[i % 4];
    }
}

bool is_control_opcode(uint8_t opcode) {
    return opcode == OPCODE_CLOSE || opcode == OPCODE_PING || opcode == OPCODE_PONG;
}

AssembledMessage FragmentAssembler::feed(uint8_t opcode, bool fin,
                                          std::span<const uint8_t> payload) {
    // Control frames are returned immediately without disturbing fragment state
    if (is_control_opcode(opcode)) {
        AssembledMessage msg;
        msg.result = AssemblyResult::CONTROL;
        msg.opcode = opcode;
        msg.data.assign(payload.begin(), payload.end());
        msg.close_code = 0;
        if (opcode == OPCODE_CLOSE && payload.size() >= 2) {
            msg.close_code = (static_cast<uint16_t>(payload[0]) << 8) | payload[1];
        }
        return msg;
    }

    // Continuation frame without active fragment is an error
    if (opcode == OPCODE_CONTINUATION && !in_fragment_) {
        return {AssemblyResult::ERROR, 0, {}, 0, "unexpected continuation frame"};
    }

    // Non-continuation, non-control frame while fragment in progress is an error
    if (opcode != OPCODE_CONTINUATION && in_fragment_) {
        return {AssemblyResult::ERROR, 0, {}, 0, "new data frame during fragmentation"};
    }

    // Start of a new message (first frame)
    if (opcode != OPCODE_CONTINUATION) {
        start_opcode_ = opcode;
        buffer_.clear();
    }

    // Accumulate payload
    buffer_.insert(buffer_.end(), payload.begin(), payload.end());

    // Check size limit
    size_t max_size = (start_opcode_ == OPCODE_TEXT) ? MAX_TEXT_MESSAGE_SIZE : MAX_BINARY_MESSAGE_SIZE;
    if (buffer_.size() > max_size) {
        reset();
        return {AssemblyResult::ERROR, 0, {}, 0, "message exceeds maximum size"};
    }

    if (fin) {
        // Message complete
        AssembledMessage msg;
        msg.result = AssemblyResult::COMPLETE;
        msg.opcode = start_opcode_;
        msg.data = std::move(buffer_);
        msg.close_code = 0;
        in_fragment_ = false;
        buffer_.clear();
        start_opcode_ = 0;
        return msg;
    }

    // More fragments expected
    in_fragment_ = true;
    return {AssemblyResult::INCOMPLETE, start_opcode_, {}, 0, ""};
}

void FragmentAssembler::reset() {
    buffer_.clear();
    start_opcode_ = 0;
    in_fragment_ = false;
}

} // namespace chromatindb::relay::ws
