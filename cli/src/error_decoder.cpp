// =============================================================================
// D-05 ErrorResponse decoder — extracted into its own TU so the [error_decoder]
// unit TEST_CASE in cli/tests/test_wire.cpp can link against it without pulling
// in the rest of commands.cpp (which depends on asio/spdlog/nlohmann_json and
// the full CLI command surface).
//
// The definition is byte-identical to the previous in-line version in
// cli/src/commands.cpp — this is a pure linkage refactor that does not change
// the production cdb binary's behavior. See plan 05 SUMMARY for the motivation.
// =============================================================================

#include "cli/src/commands_internal.h"
#include "cli/src/wire.h"  // to_hex

#include <cstdio>

namespace chromatindb::cli {

// D-05: decode an ErrorResponse payload into user-facing wording.
// Payload layout (post-Phase 122/123): [error_code:1][original_type:1].
// Defensive short-reads (<2 bytes) return a generic message; unknown codes
// format as "(code 0x%02X)" — opaque, non-identifying.
//
// NEVER leaks internal tokens (PUBK_FIRST_VIOLATION / PUBK_MISMATCH) or phase
// numbers — memory: feedback_no_phase_leaks_in_user_strings.md. Wording and
// case coverage per PATTERNS.md §"Pattern 3" and RESEARCH §Q7.
std::string decode_error_response(std::span<const uint8_t> payload,
                                   const std::string& host_hint,
                                   std::span<const uint8_t, 32> ns_hint) {
    if (payload.size() < 2) {
        return "Error: node returned malformed response";
    }

    uint8_t code = payload[0];

    // Short 8-byte prefix of the namespace for a non-identifying handle.
    auto ns_short = to_hex(std::span<const uint8_t>(ns_hint.data(), 8));

    switch (code) {
        case 0x07:
            return "Error: namespace not yet initialized on node " + host_hint +
                   ". Auto-PUBK failed; try running 'cdb publish' first.";
        case 0x08:
            return "Error: namespace " + ns_short +
                   " is owned by a different key on node " + host_hint +
                   ". Cannot write.";
        case 0x09:
            return "Error: batch deletion rejected (BOMB must be permanent).";
        case 0x0A:
            return "Error: batch deletion rejected (malformed BOMB payload).";
        case 0x0B:
            return "Error: delegates cannot perform batch deletion on this node.";
        default: {
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "Error: node rejected request (code 0x%02X)", code);
            return buf;
        }
    }
}

} // namespace chromatindb::cli
