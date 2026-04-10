#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <string_view>

namespace chromatindb::relay::translate {

/// Field encoding rules for JSON <-> binary translation.
/// Phase 103 uses these to drive the conversion.
enum class FieldEncoding : uint8_t {
    HEX_32,         // 32-byte hash/namespace -> 64-char hex string (per D-19)
    HEX_PUBKEY,     // 2592-byte ML-DSA-87 pubkey -> 5184-char hex string (per D-19)
    BASE64,         // Variable-length binary (blob data, signatures) -> base64 (per D-19)
    UINT64_STRING,  // uint64 -> JSON string "12345" (per D-20, PROT-03)
    UINT32_NUMBER,  // uint32 -> JSON number (fits in JS Number safely)
    UINT16_NUMBER,  // uint16 -> JSON number
    UINT8_NUMBER,   // uint8 -> JSON number
    BOOL,           // uint8 0/1 -> JSON boolean
    STRING,         // Length-prefixed UTF-8 string -> JSON string
    STRING_ARRAY,   // Array of strings -> JSON array of strings
    HEX_32_ARRAY,   // Array of 32-byte hashes -> JSON array of hex strings
    REQUEST_ID,     // uint32 request_id -> JSON number (per D-22, optional field)
};

/// A single field in a message schema.
struct FieldSpec {
    std::string_view json_name;    // JSON field name (e.g., "namespace", "hash", "data")
    FieldEncoding encoding;        // How to encode/decode this field
    bool optional = false;         // If true, omit from JSON when default/zero (per D-24)
};

/// Schema for one message type.
struct MessageSchema {
    std::string_view type_name;              // "read_request"
    uint8_t wire_type;                       // 31
    bool is_flatbuffer;                      // true for Data(8), ReadResponse(32), BatchReadResponse(54)
    bool is_compound;                        // true for compound binary responses needing custom decode
    std::span<const FieldSpec> fields;       // Field definitions (empty for FB types)
};

// =============================================================================
// Per-type field definitions (non-FlatBuffer message types)
// FlatBuffer types (Data, ReadResponse, BatchReadResponse) have is_flatbuffer=true
// and their fields are defined by the .fbs schema, not here.
// =============================================================================

// --- Ping (5), Pong (6), Goodbye (7) ---
inline constexpr FieldSpec PING_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
};
inline constexpr FieldSpec PONG_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
};
inline constexpr FieldSpec GOODBYE_FIELDS[] = {};

// --- Delete (17) ---
inline constexpr FieldSpec DELETE_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"namespace",   FieldEncoding::HEX_32},
    {"hash",        FieldEncoding::HEX_32},
};

// --- DeleteAck (18) ---
inline constexpr FieldSpec DELETE_ACK_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"status",      FieldEncoding::UINT8_NUMBER},
};

// --- Subscribe (19) ---
inline constexpr FieldSpec SUBSCRIBE_FIELDS[] = {
    {"request_id",   FieldEncoding::REQUEST_ID,     true},
    {"namespaces",   FieldEncoding::HEX_32_ARRAY},
};

// --- Unsubscribe (20) ---
inline constexpr FieldSpec UNSUBSCRIBE_FIELDS[] = {
    {"request_id",   FieldEncoding::REQUEST_ID,     true},
    {"namespaces",   FieldEncoding::HEX_32_ARRAY},
};

// --- Notification (21) ---
inline constexpr FieldSpec NOTIFICATION_FIELDS[] = {
    {"namespace",    FieldEncoding::HEX_32},
    {"hash",         FieldEncoding::HEX_32},
    {"seq_num",      FieldEncoding::UINT64_STRING},
    {"size",         FieldEncoding::UINT32_NUMBER},
    {"is_tombstone", FieldEncoding::BOOL},
};

// --- StorageFull (22), QuotaExceeded (25) --- node->client signals
inline constexpr FieldSpec STORAGE_FULL_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
};
inline constexpr FieldSpec QUOTA_EXCEEDED_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
};

// --- WriteAck (30) ---
inline constexpr FieldSpec WRITE_ACK_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"hash",        FieldEncoding::HEX_32},
    {"seq_num",     FieldEncoding::UINT64_STRING},
    {"status",      FieldEncoding::UINT8_NUMBER},
};

// --- ReadRequest (31) ---
inline constexpr FieldSpec READ_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"namespace",   FieldEncoding::HEX_32},
    {"hash",        FieldEncoding::HEX_32},
};

// --- ListRequest (33) ---
inline constexpr FieldSpec LIST_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"namespace",   FieldEncoding::HEX_32},
    {"since_seq",   FieldEncoding::UINT64_STRING,  true},  // defaults to 0
    {"limit",       FieldEncoding::UINT32_NUMBER,   true},
};

// --- ListResponse (34) ---
inline constexpr FieldSpec LIST_RESPONSE_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,      true},
    {"hashes",      FieldEncoding::HEX_32_ARRAY},
    {"truncated",   FieldEncoding::BOOL,             true},
};

// --- StatsRequest (35) ---
inline constexpr FieldSpec STATS_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
};

// --- StatsResponse (36) ---
inline constexpr FieldSpec STATS_RESPONSE_FIELDS[] = {
    {"request_id",     FieldEncoding::REQUEST_ID,     true},
    {"total_blobs",    FieldEncoding::UINT64_STRING},
    {"storage_used",   FieldEncoding::UINT64_STRING},
    {"storage_max",    FieldEncoding::UINT64_STRING},
    {"namespace_count",FieldEncoding::UINT32_NUMBER},
    {"peer_count",     FieldEncoding::UINT32_NUMBER},
    {"uptime",         FieldEncoding::UINT64_STRING},
};

// --- ExistsRequest (37) ---
inline constexpr FieldSpec EXISTS_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"namespace",   FieldEncoding::HEX_32},
    {"hash",        FieldEncoding::HEX_32},
};

// --- ExistsResponse (38) ---
inline constexpr FieldSpec EXISTS_RESPONSE_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"exists",      FieldEncoding::BOOL},
};

// --- NodeInfoRequest (39) ---
inline constexpr FieldSpec NODE_INFO_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
};

// --- NodeInfoResponse (40) ---
inline constexpr FieldSpec NODE_INFO_RESPONSE_FIELDS[] = {
    {"request_id",       FieldEncoding::REQUEST_ID,     true},
    {"version",          FieldEncoding::STRING},
    {"git_hash",         FieldEncoding::STRING},
    {"uptime",           FieldEncoding::UINT64_STRING},
    {"peer_count",       FieldEncoding::UINT32_NUMBER},
    {"namespace_count",  FieldEncoding::UINT32_NUMBER},
    {"total_blobs",      FieldEncoding::UINT64_STRING},
    {"storage_used",     FieldEncoding::UINT64_STRING},
    {"storage_max",      FieldEncoding::UINT64_STRING},
    {"supported_types",  FieldEncoding::STRING_ARRAY},
};

// --- NamespaceListRequest (41) ---
inline constexpr FieldSpec NAMESPACE_LIST_REQUEST_FIELDS[] = {
    {"request_id",        FieldEncoding::REQUEST_ID,     true},
    {"after_namespace",   FieldEncoding::HEX_32,         true},  // 32-byte cursor
    {"limit",             FieldEncoding::UINT32_NUMBER,   true},
};

// --- NamespaceListResponse (42) ---
inline constexpr FieldSpec NAMESPACE_LIST_RESPONSE_FIELDS[] = {
    {"request_id",   FieldEncoding::REQUEST_ID,     true},
    {"namespaces",   FieldEncoding::HEX_32_ARRAY},
};

// --- StorageStatusRequest (43) ---
inline constexpr FieldSpec STORAGE_STATUS_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
};

// --- StorageStatusResponse (44) ---
inline constexpr FieldSpec STORAGE_STATUS_RESPONSE_FIELDS[] = {
    {"request_id",       FieldEncoding::REQUEST_ID,     true},
    {"capacity_bytes",   FieldEncoding::UINT64_STRING},
    {"used_bytes",       FieldEncoding::UINT64_STRING},
    {"blob_count",       FieldEncoding::UINT64_STRING},
    {"namespace_count",  FieldEncoding::UINT32_NUMBER},
};

// --- NamespaceStatsRequest (45) ---
inline constexpr FieldSpec NAMESPACE_STATS_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"namespace",   FieldEncoding::HEX_32},
};

// --- NamespaceStatsResponse (46) ---
inline constexpr FieldSpec NAMESPACE_STATS_RESPONSE_FIELDS[] = {
    {"request_id",    FieldEncoding::REQUEST_ID,     true},
    {"namespace",     FieldEncoding::HEX_32},
    {"blob_count",    FieldEncoding::UINT64_STRING},
    {"storage_used",  FieldEncoding::UINT64_STRING},
    {"latest_seq",    FieldEncoding::UINT64_STRING},
};

// --- MetadataRequest (47) ---
inline constexpr FieldSpec METADATA_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"namespace",   FieldEncoding::HEX_32},
    {"hash",        FieldEncoding::HEX_32},
};

// --- MetadataResponse (48) ---
inline constexpr FieldSpec METADATA_RESPONSE_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"found",       FieldEncoding::BOOL},
    {"namespace",   FieldEncoding::HEX_32,          true},
    {"hash",        FieldEncoding::HEX_32,          true},
    {"size",        FieldEncoding::UINT32_NUMBER,    true},
    {"ttl",         FieldEncoding::UINT32_NUMBER,    true},
    {"timestamp",   FieldEncoding::UINT64_STRING,    true},
    {"seq_num",     FieldEncoding::UINT64_STRING,    true},
    {"is_tombstone",FieldEncoding::BOOL,             true},
};

// --- BatchExistsRequest (49) ---
inline constexpr FieldSpec BATCH_EXISTS_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"namespace",   FieldEncoding::HEX_32},
    {"hashes",      FieldEncoding::HEX_32_ARRAY},
};

// --- BatchExistsResponse (50) ---
// Note: "results" is an array of bools -- Phase 103 handles the actual array encoding.
inline constexpr FieldSpec BATCH_EXISTS_RESPONSE_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"results",     FieldEncoding::BOOL},
};

// --- DelegationListRequest (51) ---
inline constexpr FieldSpec DELEGATION_LIST_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"namespace",   FieldEncoding::HEX_32},
};

// --- DelegationListResponse (52) ---
// Note: "delegations" is an array of pubkeys -- Phase 103 handles the actual array encoding.
inline constexpr FieldSpec DELEGATION_LIST_RESPONSE_FIELDS[] = {
    {"request_id",   FieldEncoding::REQUEST_ID,     true},
    {"delegations",  FieldEncoding::HEX_PUBKEY},
};

// --- BatchReadRequest (53) ---
inline constexpr FieldSpec BATCH_READ_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"namespace",   FieldEncoding::HEX_32},
    {"max_bytes",   FieldEncoding::UINT32_NUMBER,   true},
    {"hashes",      FieldEncoding::HEX_32_ARRAY},   // encoder adds u32BE count prefix
};

// --- PeerInfoRequest (55) ---
inline constexpr FieldSpec PEER_INFO_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
};

// --- PeerInfoResponse (56) --- (simplified -- full structure in Phase 103)
inline constexpr FieldSpec PEER_INFO_RESPONSE_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"peer_count",  FieldEncoding::UINT32_NUMBER},
};

// --- TimeRangeRequest (57) ---
inline constexpr FieldSpec TIME_RANGE_REQUEST_FIELDS[] = {
    {"request_id",  FieldEncoding::REQUEST_ID,     true},
    {"namespace",   FieldEncoding::HEX_32},
    {"since",       FieldEncoding::UINT64_STRING},
    {"limit",       FieldEncoding::UINT32_NUMBER,   true},
};

// --- TimeRangeResponse (58) ---
// Note: "entries" is an array of hash entries -- Phase 103 handles the actual array encoding.
inline constexpr FieldSpec TIME_RANGE_RESPONSE_FIELDS[] = {
    {"request_id",   FieldEncoding::REQUEST_ID,     true},
    {"entries",      FieldEncoding::HEX_32_ARRAY},
    {"truncated",    FieldEncoding::BOOL,            true},
};

/// Look up schema for a wire type.  Returns nullptr if not found.
const MessageSchema* schema_for_type(uint8_t wire_type);

/// Look up schema for a JSON type name.  Returns nullptr if not found.
const MessageSchema* schema_for_name(std::string_view name);

} // namespace chromatindb::relay::translate
