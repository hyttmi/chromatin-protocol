#pragma once

#include <cstdint>
#include <string_view>

namespace chromatindb::peer {

// Error code bytes for ErrorResponse(63) payload.
// Shared between sender (MessageDispatcher) and relay translator.
constexpr uint8_t ERROR_MALFORMED_PAYLOAD    = 0x01;
constexpr uint8_t ERROR_UNKNOWN_TYPE         = 0x02;
constexpr uint8_t ERROR_DECODE_FAILED        = 0x03;
constexpr uint8_t ERROR_VALIDATION_FAILED    = 0x04;
constexpr uint8_t ERROR_INTERNAL             = 0x05;
constexpr uint8_t ERROR_TIMEOUT              = 0x06;
constexpr uint8_t ERROR_PUBK_FIRST_VIOLATION = 0x07;  // Phase 122 D-03: first write to namespace must be PUBK
constexpr uint8_t ERROR_PUBK_MISMATCH        = 0x08;  // Phase 122 D-04: PUBK signing pubkey differs from registered owner

/// Map an error code byte to a human-readable string.
/// Returns "unknown" for unrecognized values.
constexpr std::string_view error_code_string(uint8_t code) {
    switch (code) {
        case ERROR_MALFORMED_PAYLOAD:    return "malformed_payload";
        case ERROR_UNKNOWN_TYPE:         return "unknown_type";
        case ERROR_DECODE_FAILED:        return "decode_failed";
        case ERROR_VALIDATION_FAILED:    return "validation_failed";
        case ERROR_INTERNAL:             return "internal_error";
        case ERROR_TIMEOUT:              return "timeout";
        case ERROR_PUBK_FIRST_VIOLATION: return "pubk_first_violation";
        case ERROR_PUBK_MISMATCH:        return "pubk_mismatch";
        default:                         return "unknown";
    }
}

/// Alias for the plan's verification grep — matches `error_code_name`
/// convention used elsewhere in the codebase.
constexpr std::string_view error_code_name(uint8_t code) {
    return error_code_string(code);
}

} // namespace chromatindb::peer
