#pragma once

#include <cstdint>
#include <string_view>

namespace chromatindb::peer {

// Error code bytes for ErrorResponse(63) payload.
// Shared between sender (MessageDispatcher) and relay translator.
constexpr uint8_t ERROR_MALFORMED_PAYLOAD  = 0x01;
constexpr uint8_t ERROR_UNKNOWN_TYPE       = 0x02;
constexpr uint8_t ERROR_DECODE_FAILED      = 0x03;
constexpr uint8_t ERROR_VALIDATION_FAILED  = 0x04;
constexpr uint8_t ERROR_INTERNAL           = 0x05;

/// Map an error code byte to a human-readable string.
/// Returns "unknown" for unrecognized values.
constexpr std::string_view error_code_string(uint8_t code) {
    switch (code) {
        case ERROR_MALFORMED_PAYLOAD:  return "malformed_payload";
        case ERROR_UNKNOWN_TYPE:       return "unknown_type";
        case ERROR_DECODE_FAILED:      return "decode_failed";
        case ERROR_VALIDATION_FAILED:  return "validation_failed";
        case ERROR_INTERNAL:           return "internal_error";
        default:                       return "unknown";
    }
}

} // namespace chromatindb::peer
