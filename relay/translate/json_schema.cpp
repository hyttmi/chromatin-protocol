#include "relay/translate/json_schema.h"
#include "relay/translate/type_registry.h"

#include <algorithm>
#include <array>

namespace chromatindb::relay::translate {

// Helper to create a span from a constexpr array.
template <size_t N>
static constexpr std::span<const FieldSpec> fields_of(const FieldSpec (&arr)[N]) {
    return {arr, N};
}

// Empty span for FlatBuffer types and types with no fields.
static constexpr std::span<const FieldSpec> NO_FIELDS{};

/// Full schema table for all 40 registered types.
/// Sorted by wire_type for binary search.
static const std::array<MessageSchema, TYPE_REGISTRY_SIZE> SCHEMAS = {{
    // Ping (5)
    {"ping",                         5, false, false, fields_of(PING_FIELDS)},
    // Pong (6)
    {"pong",                         6, false, false, fields_of(PONG_FIELDS)},
    // Goodbye (7)
    {"goodbye",                      7, false, false, NO_FIELDS},
    // Data (8) -- FlatBuffer
    {"data",                         8, true,  false, NO_FIELDS},
    // Delete (17) -- FlatBuffer blob (same as Data, node calls decode_blob)
    {"delete",                      17, true,  false, NO_FIELDS},
    // DeleteAck (18)
    {"delete_ack",                  18, false, false, fields_of(DELETE_ACK_FIELDS)},
    // Subscribe (19)
    {"subscribe",                   19, false, false, fields_of(SUBSCRIBE_FIELDS)},
    // Unsubscribe (20)
    {"unsubscribe",                 20, false, false, fields_of(UNSUBSCRIBE_FIELDS)},
    // Notification (21)
    {"notification",                21, false, false, fields_of(NOTIFICATION_FIELDS)},
    // StorageFull (22) -- node->client signal
    {"storage_full",                22, false, false, fields_of(STORAGE_FULL_FIELDS)},
    // QuotaExceeded (25) -- node->client signal
    {"quota_exceeded",              25, false, false, fields_of(QUOTA_EXCEEDED_FIELDS)},
    // WriteAck (30)
    {"write_ack",                   30, false, false, fields_of(WRITE_ACK_FIELDS)},
    // ReadRequest (31)
    {"read_request",                31, false, false, fields_of(READ_REQUEST_FIELDS)},
    // ReadResponse (32) -- FlatBuffer
    {"read_response",               32, true,  false, NO_FIELDS},
    // ListRequest (33)
    {"list_request",                33, false, false, fields_of(LIST_REQUEST_FIELDS)},
    // ListResponse (34) -- compound: [count:u32BE][ [hash:32][seq_num:u64BE] * count ][truncated:u8]
    {"list_response",               34, false, true,  NO_FIELDS},
    // StatsRequest (35)
    {"stats_request",               35, false, false, fields_of(STATS_REQUEST_FIELDS)},
    // StatsResponse (36) -- compound: 24-byte per-namespace format
    {"stats_response",              36, false, true,  NO_FIELDS},
    // ExistsRequest (37)
    {"exists_request",              37, false, false, fields_of(EXISTS_REQUEST_FIELDS)},
    // ExistsResponse (38)
    {"exists_response",             38, false, false, fields_of(EXISTS_RESPONSE_FIELDS)},
    // NodeInfoRequest (39)
    {"node_info_request",           39, false, false, fields_of(NODE_INFO_REQUEST_FIELDS)},
    // NodeInfoResponse (40) -- compound: has length-prefixed strings
    {"node_info_response",          40, false, true,  NO_FIELDS},
    // NamespaceListRequest (41)
    {"namespace_list_request",      41, false, false, fields_of(NAMESPACE_LIST_REQUEST_FIELDS)},
    // NamespaceListResponse (42) -- compound: [count:u32BE][has_more:u8][ [ns:32][blob_count:u64BE] * count ]
    {"namespace_list_response",     42, false, true,  NO_FIELDS},
    // StorageStatusRequest (43)
    {"storage_status_request",      43, false, false, fields_of(STORAGE_STATUS_REQUEST_FIELDS)},
    // StorageStatusResponse (44) -- compound: 6-field flat binary (node format)
    {"storage_status_response",     44, false, true,  NO_FIELDS},
    // NamespaceStatsRequest (45)
    {"namespace_stats_request",     45, false, false, fields_of(NAMESPACE_STATS_REQUEST_FIELDS)},
    // NamespaceStatsResponse (46) -- compound: status-dependent
    {"namespace_stats_response",    46, false, true,  NO_FIELDS},
    // MetadataRequest (47)
    {"metadata_request",            47, false, false, fields_of(METADATA_REQUEST_FIELDS)},
    // MetadataResponse (48) -- compound: [status:1]... status-dependent
    {"metadata_response",           48, false, true,  NO_FIELDS},
    // BatchExistsRequest (49)
    {"batch_exists_request",        49, false, false, fields_of(BATCH_EXISTS_REQUEST_FIELDS)},
    // BatchExistsResponse (50) -- compound: [result:u8 * count]
    {"batch_exists_response",       50, false, true,  NO_FIELDS},
    // DelegationListRequest (51)
    {"delegation_list_request",     51, false, false, fields_of(DELEGATION_LIST_REQUEST_FIELDS)},
    // DelegationListResponse (52) -- compound: [count:u32BE][ [namespace:32][pubkey_hash:32] * count ]
    {"delegation_list_response",    52, false, true,  NO_FIELDS},
    // BatchReadRequest (53)
    {"batch_read_request",          53, false, false, fields_of(BATCH_READ_REQUEST_FIELDS)},
    // BatchReadResponse (54) -- FlatBuffer
    {"batch_read_response",         54, true,  false, NO_FIELDS},
    // PeerInfoRequest (55)
    {"peer_info_request",           55, false, false, fields_of(PEER_INFO_REQUEST_FIELDS)},
    // PeerInfoResponse (56) -- compound: trusted format with variable-length entries
    {"peer_info_response",          56, false, true,  NO_FIELDS},
    // TimeRangeRequest (57)
    {"time_range_request",          57, false, false, fields_of(TIME_RANGE_REQUEST_FIELDS)},
    // TimeRangeResponse (58) -- compound: [count:u32BE][ [hash:32][timestamp:u64BE] * count ][truncated:u8]
    {"time_range_response",         58, false, true,  NO_FIELDS},
}};

const MessageSchema* schema_for_type(uint8_t wire_type) {
    // Binary search on sorted-by-wire_type array.
    auto it = std::lower_bound(
        SCHEMAS.begin(), SCHEMAS.end(), wire_type,
        [](const MessageSchema& s, uint8_t wt) { return s.wire_type < wt; });
    if (it != SCHEMAS.end() && it->wire_type == wire_type) {
        return &*it;
    }
    return nullptr;
}

const MessageSchema* schema_for_name(std::string_view name) {
    // Use type_registry to get wire type, then look up schema.
    auto wt = type_from_string(name);
    if (!wt) return nullptr;
    return schema_for_type(*wt);
}

} // namespace chromatindb::relay::translate
