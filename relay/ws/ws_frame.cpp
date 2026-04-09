#include "relay/ws/ws_frame.h"

namespace chromatindb::relay::ws {

std::string encode_frame(uint8_t /*opcode*/, std::span<const uint8_t> /*payload*/, bool /*fin*/) {
    return {};  // Stub
}

std::string encode_close_frame(uint16_t /*status_code*/, std::string_view /*reason*/) {
    return {};  // Stub
}

std::optional<FrameHeader> parse_frame_header(std::span<const uint8_t> /*buf*/) {
    return std::nullopt;  // Stub
}

void apply_mask(std::span<uint8_t> /*payload*/, const uint8_t /*mask*/[4]) {
    // Stub
}

bool is_control_opcode(uint8_t /*opcode*/) {
    return false;  // Stub
}

AssembledMessage FragmentAssembler::feed(uint8_t /*opcode*/, bool /*fin*/,
                                          std::span<const uint8_t> /*payload*/) {
    return {AssemblyResult::ERROR, 0, {}, 0, "not implemented"};  // Stub
}

void FragmentAssembler::reset() {
    buffer_.clear();
    start_opcode_ = 0;
    in_fragment_ = false;
}

} // namespace chromatindb::relay::ws
