#include "relay/core/message_filter.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace chromatindb::relay::core {

/// Sorted allowlist of the 38 client-allowed type strings.
/// These are the types from the node's supported_types array that clients
/// may send or receive.  StorageFull and QuotaExceeded are excluded here
/// because clients don't send them (they're node-originated signals).
static constexpr std::array<std::string_view, 38> ALLOWED_TYPE_STRINGS = {{
    "batch_exists_request",
    "batch_exists_response",
    "batch_read_request",
    "batch_read_response",
    "data",
    "delegation_list_request",
    "delegation_list_response",
    "delete",
    "delete_ack",
    "exists_request",
    "exists_response",
    "goodbye",
    "list_request",
    "list_response",
    "metadata_request",
    "metadata_response",
    "namespace_list_request",
    "namespace_list_response",
    "namespace_stats_request",
    "namespace_stats_response",
    "node_info_request",
    "node_info_response",
    "notification",
    "peer_info_request",
    "peer_info_response",
    "ping",
    "pong",
    "read_request",
    "read_response",
    "stats_request",
    "stats_response",
    "storage_status_request",
    "storage_status_response",
    "subscribe",
    "time_range_request",
    "time_range_response",
    "unsubscribe",
    "write_ack",
}};

/// Sorted allowlist of the 40 wire type integers allowed for outbound relay->client.
/// Includes the 38 client types plus StorageFull(22) and QuotaExceeded(25).
static constexpr std::array<uint8_t, 40> ALLOWED_WIRE_TYPES = {{
    5, 6, 7, 8,
    17, 18, 19, 20, 21, 22, 25,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58,
}};

bool is_type_allowed(std::string_view type_name) {
    return std::binary_search(
        ALLOWED_TYPE_STRINGS.begin(), ALLOWED_TYPE_STRINGS.end(), type_name);
}

bool is_wire_type_allowed(uint8_t wire_type) {
    return std::binary_search(
        ALLOWED_WIRE_TYPES.begin(), ALLOWED_WIRE_TYPES.end(), wire_type);
}

} // namespace chromatindb::relay::core
