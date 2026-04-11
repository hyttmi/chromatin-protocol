#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string_view>

namespace chromatindb::relay::translate {

struct TypeEntry {
    std::string_view json_name;
    uint8_t wire_type;
};

/// Full registry of 41 type entries (38 client-allowed + 3 node-originated signals),
/// sorted by json_name for binary search.  Source of truth for JSON type string <->
/// wire type integer mapping.
///
/// The 38 client types come from the node's supported_types array.
/// StorageFull(22), QuotaExceeded(25), and ErrorResponse(63) are node->client
/// signals included for outbound translation even though clients don't send them.
inline constexpr TypeEntry TYPE_REGISTRY[] = {
    {"batch_exists_request",        49},
    {"batch_exists_response",       50},
    {"batch_read_request",          53},
    {"batch_read_response",         54},
    {"data",                         8},
    {"delegation_list_request",     51},
    {"delegation_list_response",    52},
    {"delete",                      17},
    {"delete_ack",                  18},
    {"error",                       63},   // Phase 999.2: ErrorResponse
    {"exists_request",              37},
    {"exists_response",             38},
    {"goodbye",                      7},
    {"list_request",                33},
    {"list_response",               34},
    {"metadata_request",            47},
    {"metadata_response",           48},
    {"namespace_list_request",      41},
    {"namespace_list_response",     42},
    {"namespace_stats_request",     45},
    {"namespace_stats_response",    46},
    {"node_info_request",           39},
    {"node_info_response",          40},
    {"notification",                21},
    {"peer_info_request",           55},
    {"peer_info_response",          56},
    {"ping",                         5},
    {"pong",                         6},
    {"quota_exceeded",              25},
    {"read_request",                31},
    {"read_response",               32},
    {"stats_request",               35},
    {"stats_response",              36},
    {"storage_full",                22},
    {"storage_status_request",      43},
    {"storage_status_response",     44},
    {"subscribe",                   19},
    {"time_range_request",          57},
    {"time_range_response",         58},
    {"unsubscribe",                 20},
    {"write_ack",                   30},
};

inline constexpr size_t TYPE_REGISTRY_SIZE = sizeof(TYPE_REGISTRY) / sizeof(TYPE_REGISTRY[0]);

/// Look up wire type from JSON type string.  Binary search on sorted array.
/// Returns nullopt if not found.
std::optional<uint8_t> type_from_string(std::string_view name);

/// Look up JSON type string from wire type integer.
/// Returns nullopt if not found.  Linear scan (small array, infrequent calls).
std::optional<std::string_view> type_to_string(uint8_t wire_type);

/// Get the total count of registered types.
constexpr size_t registry_size() { return TYPE_REGISTRY_SIZE; }

} // namespace chromatindb::relay::translate
