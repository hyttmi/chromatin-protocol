#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace chromatindb::relay::core {

/// Check if a JSON message type string is allowed for client->relay messages.
/// Returns true for the 39 client-allowed types from node's supported_types
/// plus ErrorResponse(63) which is node-originated but forwarded to clients.
/// Hardcoded constexpr allowlist (per D-30).  Not operator-configurable.
bool is_type_allowed(std::string_view type_name);

/// Check if a wire type integer is allowed for relay->client (outbound filtering).
/// Includes the 38 client types + node-originated signals (StorageFull, QuotaExceeded, ErrorResponse).
bool is_wire_type_allowed(uint8_t wire_type);

/// Total number of client-allowed types (for testing).
constexpr size_t ALLOWED_TYPE_COUNT = 39;

} // namespace chromatindb::relay::core
