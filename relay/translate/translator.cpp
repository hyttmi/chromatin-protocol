#include "relay/translate/translator.h"
#include "relay/translate/json_schema.h"
#include "relay/translate/type_registry.h"
#include "relay/util/base64.h"
#include "relay/util/endian.h"
#include "relay/util/hex.h"
#include "relay/wire/blob_codec.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace chromatindb::relay::translate {

// =============================================================================
// json_to_binary: JSON request -> binary payload
// =============================================================================

/// Encode a single field from JSON into the binary payload.
static bool encode_field(const FieldSpec& field, const nlohmann::json& msg,
                         std::vector<uint8_t>& out) {
    bool has_field = msg.contains(field.json_name);

    switch (field.encoding) {
    case FieldEncoding::REQUEST_ID:
        // request_id is in transport envelope, not payload. Skip.
        return true;

    case FieldEncoding::HEX_32: {
        if (!has_field) {
            if (field.optional) {
                // Zero-fill for optional missing fields.
                out.resize(out.size() + 32, 0);
                return true;
            }
            return false;
        }
        auto hex = msg[field.json_name].get<std::string>();
        auto bytes = util::from_hex(hex);
        if (!bytes || bytes->size() != 32) return false;
        out.insert(out.end(), bytes->begin(), bytes->end());
        return true;
    }

    case FieldEncoding::HEX_PUBKEY: {
        if (!has_field) {
            if (field.optional) {
                out.resize(out.size() + 2592, 0);
                return true;
            }
            return false;
        }
        auto hex = msg[field.json_name].get<std::string>();
        auto bytes = util::from_hex(hex);
        if (!bytes || bytes->size() != 2592) return false;
        out.insert(out.end(), bytes->begin(), bytes->end());
        return true;
    }

    case FieldEncoding::BASE64: {
        if (!has_field) {
            if (field.optional) return true;
            return false;
        }
        auto b64 = msg[field.json_name].get<std::string>();
        auto bytes = util::base64_decode(b64);
        if (!bytes) return false;
        out.insert(out.end(), bytes->begin(), bytes->end());
        return true;
    }

    case FieldEncoding::UINT64_STRING: {
        uint64_t val = 0;
        if (has_field) {
            if (msg[field.json_name].is_string()) {
                val = std::stoull(msg[field.json_name].get<std::string>());
            } else if (msg[field.json_name].is_number_unsigned()) {
                val = msg[field.json_name].get<uint64_t>();
            }
        }
        uint8_t buf[8];
        util::store_u64_be(buf, val);
        out.insert(out.end(), buf, buf + 8);
        return true;
    }

    case FieldEncoding::UINT32_NUMBER: {
        uint32_t val = 0;
        if (has_field) {
            val = msg[field.json_name].get<uint32_t>();
        }
        uint8_t buf[4];
        util::store_u32_be(buf, val);
        out.insert(out.end(), buf, buf + 4);
        return true;
    }

    case FieldEncoding::UINT16_NUMBER: {
        uint16_t val = 0;
        if (has_field) {
            val = msg[field.json_name].get<uint16_t>();
        }
        uint8_t buf[2];
        util::store_u16_be(buf, val);
        out.insert(out.end(), buf, buf + 2);
        return true;
    }

    case FieldEncoding::UINT8_NUMBER: {
        uint8_t val = 0;
        if (has_field) {
            val = msg[field.json_name].get<uint8_t>();
        }
        out.push_back(val);
        return true;
    }

    case FieldEncoding::BOOL: {
        uint8_t val = 0;
        if (has_field) {
            val = msg[field.json_name].get<bool>() ? 1 : 0;
        }
        out.push_back(val);
        return true;
    }

    case FieldEncoding::STRING: {
        std::string str;
        if (has_field) {
            str = msg[field.json_name].get<std::string>();
        }
        uint16_t len = static_cast<uint16_t>(str.size());
        uint8_t buf[2];
        util::store_u16_be(buf, len);
        out.insert(out.end(), buf, buf + 2);
        out.insert(out.end(), str.begin(), str.end());
        return true;
    }

    case FieldEncoding::STRING_ARRAY: {
        if (!has_field || !msg[field.json_name].is_array()) {
            // Empty array: u16BE count = 0
            out.push_back(0);
            out.push_back(0);
            return true;
        }
        auto& arr = msg[field.json_name];
        uint16_t count = static_cast<uint16_t>(arr.size());
        uint8_t buf[2];
        util::store_u16_be(buf, count);
        out.insert(out.end(), buf, buf + 2);
        for (const auto& item : arr) {
            auto s = item.get<std::string>();
            uint16_t slen = static_cast<uint16_t>(s.size());
            util::store_u16_be(buf, slen);
            out.insert(out.end(), buf, buf + 2);
            out.insert(out.end(), s.begin(), s.end());
        }
        return true;
    }

    case FieldEncoding::HEX_32_ARRAY: {
        if (!has_field || !msg[field.json_name].is_array()) {
            // Empty array: u32BE count = 0
            uint8_t buf[4] = {0, 0, 0, 0};
            out.insert(out.end(), buf, buf + 4);
            return true;
        }
        auto& arr = msg[field.json_name];
        uint32_t count = static_cast<uint32_t>(arr.size());
        uint8_t buf[4];
        util::store_u32_be(buf, count);
        out.insert(out.end(), buf, buf + 4);
        for (const auto& item : arr) {
            auto hex = item.get<std::string>();
            auto bytes = util::from_hex(hex);
            if (!bytes || bytes->size() != 32) return false;
            out.insert(out.end(), bytes->begin(), bytes->end());
        }
        return true;
    }
    }

    return false;
}

/// Encode a Data message (FlatBuffer blob) from JSON.
static std::optional<TranslateResult> encode_data_blob(const nlohmann::json& msg) {
    wire::DecodedBlob blob;

    // namespace
    if (!msg.contains("namespace")) return std::nullopt;
    auto ns = util::from_hex(msg["namespace"].get<std::string>());
    if (!ns || ns->size() != 32) return std::nullopt;
    blob.namespace_id = std::move(*ns);

    // pubkey
    if (!msg.contains("pubkey")) return std::nullopt;
    auto pk = util::from_hex(msg["pubkey"].get<std::string>());
    if (!pk) return std::nullopt;
    blob.pubkey = std::move(*pk);

    // data
    if (!msg.contains("data")) return std::nullopt;
    auto data = util::base64_decode(msg["data"].get<std::string>());
    if (!data) return std::nullopt;
    blob.data = std::move(*data);

    // ttl
    blob.ttl = msg.value("ttl", 0u);

    // timestamp
    if (msg.contains("timestamp")) {
        if (msg["timestamp"].is_string()) {
            blob.timestamp = std::stoull(msg["timestamp"].get<std::string>());
        } else {
            blob.timestamp = msg["timestamp"].get<uint64_t>();
        }
    }

    // signature
    if (!msg.contains("signature")) return std::nullopt;
    auto sig = util::base64_decode(msg["signature"].get<std::string>());
    if (!sig) return std::nullopt;
    blob.signature = std::move(*sig);

    auto encoded = wire::encode_blob(blob);
    return TranslateResult{8, std::move(encoded)};
}

std::optional<TranslateResult> json_to_binary(const nlohmann::json& msg) {
    if (!msg.contains("type") || !msg["type"].is_string()) {
        return std::nullopt;
    }

    auto type_str = msg["type"].get<std::string>();
    auto wire_type_opt = type_from_string(type_str);
    if (!wire_type_opt) return std::nullopt;

    auto schema = schema_for_name(type_str);
    if (!schema) return std::nullopt;

    // FlatBuffer special case: Data (type 8) and Delete (type 17)
    // Both send a full signed blob to the node (node calls decode_blob on payload)
    if (schema->is_flatbuffer && (schema->wire_type == 8 || schema->wire_type == 17)) {
        // Delete requires tombstone data: [0xDE,0xAD,0xBE,0xEF][target_hash:32] = 36 bytes
        if (schema->wire_type == 17 && msg.contains("data")) {
            auto data = util::base64_decode(msg["data"].get<std::string>());
            if (!data || data->size() != 36 ||
                (*data)[0] != 0xDE || (*data)[1] != 0xAD ||
                (*data)[2] != 0xBE || (*data)[3] != 0xEF) {
                return std::nullopt;
            }
        }
        auto result = encode_data_blob(msg);
        if (result) result->wire_type = schema->wire_type;
        return result;
    }

    // Clients don't send ReadResponse(32) or BatchReadResponse(54).
    if (schema->is_flatbuffer) {
        return std::nullopt;
    }

    // Non-FlatBuffer: iterate FieldSpec array, encode each field.
    std::vector<uint8_t> payload;
    for (const auto& field : schema->fields) {
        if (!encode_field(field, msg, payload)) {
            return std::nullopt;
        }
    }

    return TranslateResult{schema->wire_type, std::move(payload)};
}

// =============================================================================
// binary_to_json: binary response payload -> JSON
// =============================================================================

/// Decode a flat binary payload into JSON using FieldSpec metadata.
static std::optional<nlohmann::json> decode_flat(const MessageSchema& schema,
                                                  std::span<const uint8_t> payload) {
    nlohmann::json j;
    j["type"] = std::string(schema.type_name);
    size_t offset = 0;

    for (const auto& field : schema.fields) {
        switch (field.encoding) {
        case FieldEncoding::REQUEST_ID:
            // request_id is in transport envelope, added separately.
            continue;

        case FieldEncoding::HEX_32: {
            if (offset + 32 > payload.size()) {
                if (field.optional) continue;
                return std::nullopt;
            }
            j[std::string(field.json_name)] = util::to_hex(payload.subspan(offset, 32));
            offset += 32;
            break;
        }

        case FieldEncoding::HEX_PUBKEY: {
            if (offset + 2592 > payload.size()) {
                if (field.optional) continue;
                return std::nullopt;
            }
            j[std::string(field.json_name)] = util::to_hex(payload.subspan(offset, 2592));
            offset += 2592;
            break;
        }

        case FieldEncoding::BASE64: {
            // For responses, base64 fields are variable-length.
            // Remaining bytes are the data.
            auto remaining = payload.subspan(offset);
            j[std::string(field.json_name)] = util::base64_encode(remaining);
            offset = payload.size();
            break;
        }

        case FieldEncoding::UINT64_STRING: {
            if (offset + 8 > payload.size()) {
                if (field.optional) continue;
                return std::nullopt;
            }
            uint64_t val = util::read_u64_be(payload.data() + offset);
            j[std::string(field.json_name)] = std::to_string(val);
            offset += 8;
            break;
        }

        case FieldEncoding::UINT32_NUMBER: {
            if (offset + 4 > payload.size()) {
                if (field.optional) continue;
                return std::nullopt;
            }
            j[std::string(field.json_name)] = util::read_u32_be(payload.data() + offset);
            offset += 4;
            break;
        }

        case FieldEncoding::UINT16_NUMBER: {
            if (offset + 2 > payload.size()) {
                if (field.optional) continue;
                return std::nullopt;
            }
            j[std::string(field.json_name)] = util::read_u16_be(payload.data() + offset);
            offset += 2;
            break;
        }

        case FieldEncoding::UINT8_NUMBER: {
            if (offset + 1 > payload.size()) {
                if (field.optional) continue;
                return std::nullopt;
            }
            j[std::string(field.json_name)] = payload[offset++];
            break;
        }

        case FieldEncoding::BOOL: {
            if (offset + 1 > payload.size()) {
                if (field.optional) continue;
                return std::nullopt;
            }
            j[std::string(field.json_name)] = (payload[offset++] != 0);
            break;
        }

        case FieldEncoding::STRING: {
            if (offset + 2 > payload.size()) {
                if (field.optional) continue;
                return std::nullopt;
            }
            uint16_t len = util::read_u16_be(payload.data() + offset);
            offset += 2;
            if (offset + len > payload.size()) return std::nullopt;
            j[std::string(field.json_name)] = std::string(
                reinterpret_cast<const char*>(payload.data() + offset), len);
            offset += len;
            break;
        }

        case FieldEncoding::STRING_ARRAY: {
            if (offset + 2 > payload.size()) {
                if (field.optional) continue;
                return std::nullopt;
            }
            uint16_t count = util::read_u16_be(payload.data() + offset);
            offset += 2;
            auto arr = nlohmann::json::array();
            for (uint16_t i = 0; i < count; ++i) {
                if (offset + 2 > payload.size()) return std::nullopt;
                uint16_t slen = util::read_u16_be(payload.data() + offset);
                offset += 2;
                if (offset + slen > payload.size()) return std::nullopt;
                arr.push_back(std::string(
                    reinterpret_cast<const char*>(payload.data() + offset), slen));
                offset += slen;
            }
            j[std::string(field.json_name)] = std::move(arr);
            break;
        }

        case FieldEncoding::HEX_32_ARRAY: {
            if (offset + 4 > payload.size()) {
                if (field.optional) continue;
                return std::nullopt;
            }
            uint32_t count = util::read_u32_be(payload.data() + offset);
            offset += 4;
            auto arr = nlohmann::json::array();
            for (uint32_t i = 0; i < count; ++i) {
                if (offset + 32 > payload.size()) return std::nullopt;
                arr.push_back(util::to_hex(payload.subspan(offset, 32)));
                offset += 32;
            }
            j[std::string(field.json_name)] = std::move(arr);
            break;
        }
        }
    }

    return j;
}

// =============================================================================
// Compound response decode helpers
// =============================================================================

/// ListResponse (34): [count:u32BE][ [hash:32][seq_num:u64BE] * count ][truncated:u8]
static std::optional<nlohmann::json> decode_list_response(std::span<const uint8_t> p) {
    if (p.size() < 5) return std::nullopt;  // count(4) + truncated(1) minimum
    uint32_t count = util::read_u32_be(p.data());
    size_t expected = 4 + count * 40 + 1;
    if (p.size() < expected) return std::nullopt;

    auto entries = nlohmann::json::array();
    for (uint32_t i = 0; i < count; ++i) {
        size_t off = 4 + i * 40;
        nlohmann::json entry;
        entry["hash"] = util::to_hex(p.subspan(off, 32));
        entry["seq_num"] = std::to_string(util::read_u64_be(p.data() + off + 32));
        entries.push_back(std::move(entry));
    }

    nlohmann::json j;
    j["type"] = "list_response";
    j["entries"] = std::move(entries);
    j["truncated"] = (p[expected - 1] != 0);
    return j;
}

/// NamespaceListResponse (42): [count:u32BE][has_more:u8][ [ns:32][blob_count:u64BE] * count ]
static std::optional<nlohmann::json> decode_namespace_list_response(std::span<const uint8_t> p) {
    if (p.size() < 5) return std::nullopt;
    uint32_t count = util::read_u32_be(p.data());
    bool has_more = (p[4] != 0);
    size_t expected = 5 + count * 40;
    if (p.size() < expected) return std::nullopt;

    auto namespaces = nlohmann::json::array();
    for (uint32_t i = 0; i < count; ++i) {
        size_t off = 5 + i * 40;
        nlohmann::json entry;
        entry["namespace"] = util::to_hex(p.subspan(off, 32));
        entry["blob_count"] = std::to_string(util::read_u64_be(p.data() + off + 32));
        namespaces.push_back(std::move(entry));
    }

    nlohmann::json j;
    j["type"] = "namespace_list_response";
    j["namespaces"] = std::move(namespaces);
    j["has_more"] = has_more;
    return j;
}

/// MetadataResponse (48): [status:1][hash:32][timestamp:u64BE][ttl:u32BE][data_size:u64BE][seq_num:u64BE][pubkey_len:u16BE][pubkey:N]
static std::optional<nlohmann::json> decode_metadata_response(std::span<const uint8_t> p) {
    if (p.empty()) return std::nullopt;

    nlohmann::json j;
    j["type"] = "metadata_response";

    uint8_t status = p[0];
    if (status == 0x00) {
        j["found"] = false;
        return j;
    }

    j["found"] = true;
    // [status:1][hash:32][timestamp:8][ttl:4][data_size:8][seq_num:8][pubkey_len:2][pubkey:N]
    if (p.size() < 1 + 32 + 8 + 4 + 8 + 8 + 2) return std::nullopt;

    size_t off = 1;
    j["hash"] = util::to_hex(p.subspan(off, 32)); off += 32;
    j["timestamp"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["ttl"] = util::read_u32_be(p.data() + off); off += 4;
    j["data_size"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["seq_num"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;

    uint16_t pk_len = util::read_u16_be(p.data() + off); off += 2;
    if (off + pk_len > p.size()) return std::nullopt;
    j["pubkey"] = util::to_hex(p.subspan(off, pk_len));

    return j;
}

/// BatchExistsResponse (50): [result:u8 * count]
/// Count is inferred from payload size since the request specifies count.
static std::optional<nlohmann::json> decode_batch_exists_response(std::span<const uint8_t> p) {
    nlohmann::json j;
    j["type"] = "batch_exists_response";
    auto results = nlohmann::json::array();
    for (auto b : p) {
        results.push_back(b != 0);
    }
    j["results"] = std::move(results);
    return j;
}

/// DelegationListResponse (52): [count:u32BE][ [delegate_pk_hash:32][delegation_blob_hash:32] * count ]
static std::optional<nlohmann::json> decode_delegation_list_response(std::span<const uint8_t> p) {
    if (p.size() < 4) return std::nullopt;
    uint32_t count = util::read_u32_be(p.data());
    size_t expected = 4 + count * 64;
    if (p.size() < expected) return std::nullopt;

    auto delegations = nlohmann::json::array();
    for (uint32_t i = 0; i < count; ++i) {
        size_t off = 4 + i * 64;
        nlohmann::json entry;
        entry["delegate_pk_hash"] = util::to_hex(p.subspan(off, 32));
        entry["delegation_blob_hash"] = util::to_hex(p.subspan(off + 32, 32));
        delegations.push_back(std::move(entry));
    }

    nlohmann::json j;
    j["type"] = "delegation_list_response";
    j["delegations"] = std::move(delegations);
    return j;
}

/// PeerInfoResponse (56): trusted format.
/// [peer_count:u32BE][bootstrap_count:u32BE][ [addr_len:u16BE][addr:N][is_bootstrap:u8][syncing:u8][is_full:u8][duration_ms:u64BE] * peer_count ]
static std::optional<nlohmann::json> decode_peer_info_response(std::span<const uint8_t> p) {
    if (p.size() < 8) return std::nullopt;
    uint32_t peer_count = util::read_u32_be(p.data());
    uint32_t bootstrap_count = util::read_u32_be(p.data() + 4);

    auto peers = nlohmann::json::array();
    size_t off = 8;
    for (uint32_t i = 0; i < peer_count; ++i) {
        if (off + 2 > p.size()) return std::nullopt;
        uint16_t addr_len = util::read_u16_be(p.data() + off); off += 2;
        if (off + addr_len + 3 + 8 > p.size()) return std::nullopt;

        std::string address(reinterpret_cast<const char*>(p.data() + off), addr_len);
        off += addr_len;

        nlohmann::json entry;
        entry["address"] = address;
        entry["is_bootstrap"] = (p[off++] != 0);
        entry["syncing"] = (p[off++] != 0);
        entry["is_full"] = (p[off++] != 0);
        entry["duration_ms"] = std::to_string(util::read_u64_be(p.data() + off));
        off += 8;
        peers.push_back(std::move(entry));
    }

    nlohmann::json j;
    j["type"] = "peer_info_response";
    j["peers"] = std::move(peers);
    j["bootstrap_count"] = bootstrap_count;
    return j;
}

/// TimeRangeResponse (58): [truncated:u8][count:u32BE][ [hash:32][seq_num:u64BE][timestamp:u64BE] * count ]
static std::optional<nlohmann::json> decode_time_range_response(std::span<const uint8_t> p) {
    if (p.size() < 5) return std::nullopt;  // truncated(1) + count(4) minimum
    bool truncated = (p[0] != 0);
    uint32_t count = util::read_u32_be(p.data() + 1);
    size_t expected = 5 + count * 48;  // 48 bytes per entry: hash(32) + seq_num(8) + timestamp(8)
    if (p.size() < expected) return std::nullopt;

    auto entries = nlohmann::json::array();
    for (uint32_t i = 0; i < count; ++i) {
        size_t off = 5 + i * 48;
        nlohmann::json entry;
        entry["hash"] = util::to_hex(p.subspan(off, 32));
        entry["seq_num"] = std::to_string(util::read_u64_be(p.data() + off + 32));
        entry["timestamp"] = std::to_string(util::read_u64_be(p.data() + off + 40));
        entries.push_back(std::move(entry));
    }

    nlohmann::json j;
    j["type"] = "time_range_response";
    j["entries"] = std::move(entries);
    j["truncated"] = truncated;
    return j;
}

/// NamespaceStatsResponse (46): [found:u8][blob_count:u64BE][storage_used:u64BE][delegation_count:u64BE][quota_bytes:u64BE][quota_count:u64BE]
static std::optional<nlohmann::json> decode_namespace_stats_response(std::span<const uint8_t> p) {
    if (p.empty()) return std::nullopt;

    nlohmann::json j;
    j["type"] = "namespace_stats_response";

    uint8_t found = p[0];
    j["found"] = (found != 0);

    if (found == 0) return j;

    if (p.size() < 41) return std::nullopt;  // 1 + 5*8

    size_t off = 1;
    j["blob_count"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["storage_used"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["delegation_count"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["quota_bytes_limit"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["quota_count_limit"] = std::to_string(util::read_u64_be(p.data() + off));

    return j;
}

/// StorageStatusResponse (44): [used_data:u64BE][max_storage:u64BE][tombstones:u64BE][ns_count:u32BE][total_blobs:u64BE][mmap_bytes:u64BE]
static std::optional<nlohmann::json> decode_storage_status_response(std::span<const uint8_t> p) {
    if (p.size() < 44) return std::nullopt;

    nlohmann::json j;
    j["type"] = "storage_status_response";
    size_t off = 0;
    j["used_bytes"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["capacity_bytes"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["tombstone_count"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["namespace_count"] = util::read_u32_be(p.data() + off); off += 4;
    j["blob_count"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["mmap_bytes"] = std::to_string(util::read_u64_be(p.data() + off));
    return j;
}

/// StatsResponse (36): [blob_count:u64BE][storage_bytes:u64BE][quota_bytes_limit:u64BE]
/// Per-namespace stats — 24 bytes total.
static std::optional<nlohmann::json> decode_stats_response(std::span<const uint8_t> p) {
    if (p.size() < 24) return std::nullopt;
    nlohmann::json j;
    j["type"] = "stats_response";
    size_t off = 0;
    j["blob_count"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["storage_bytes"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["quota_bytes_limit"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    return j;
}

/// NodeInfoResponse (40): [version_len:u8][version:N][git_hash_len:u8][git_hash:N][uptime:u64BE]
///   [peer_count:u32BE][namespace_count:u32BE][total_blobs:u64BE][storage_used:u64BE]
///   [storage_max:u64BE][types_count:u8][type_bytes:types_count raw bytes]
static std::optional<nlohmann::json> decode_node_info_response(std::span<const uint8_t> p) {
    nlohmann::json j;
    j["type"] = "node_info_response";
    size_t off = 0;

    // version: u8 length prefix
    if (off + 1 > p.size()) return std::nullopt;
    uint8_t ver_len = p[off++];
    if (off + ver_len > p.size()) return std::nullopt;
    j["version"] = std::string(reinterpret_cast<const char*>(p.data() + off), ver_len);
    off += ver_len;

    // git_hash: u8 length prefix
    if (off + 1 > p.size()) return std::nullopt;
    uint8_t gh_len = p[off++];
    if (off + gh_len > p.size()) return std::nullopt;
    j["git_hash"] = std::string(reinterpret_cast<const char*>(p.data() + off), gh_len);
    off += gh_len;

    // uptime, peer_count, namespace_count, total_blobs, storage_used, storage_max
    if (off + 8 + 4 + 4 + 8 + 8 + 8 > p.size()) return std::nullopt;
    j["uptime"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["peer_count"] = util::read_u32_be(p.data() + off); off += 4;
    j["namespace_count"] = util::read_u32_be(p.data() + off); off += 4;
    j["total_blobs"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["storage_used"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;
    j["storage_max"] = std::to_string(util::read_u64_be(p.data() + off)); off += 8;

    // supported_types: u8 count, then raw type bytes translated via type_to_string()
    if (off + 1 > p.size()) return std::nullopt;
    uint8_t st_count = p[off++];
    if (off + st_count > p.size()) return std::nullopt;
    auto supported = nlohmann::json::array();
    for (uint8_t i = 0; i < st_count; ++i) {
        uint8_t type_byte = p[off++];
        auto name = type_to_string(type_byte);
        if (name) {
            supported.push_back(std::string(*name));
        } else {
            supported.push_back(std::to_string(type_byte));
        }
    }
    j["supported_types"] = std::move(supported);

    return j;
}

/// ErrorResponse(63): 2-byte payload [error_code:1][original_type:1]
/// Translates both bytes to human-readable strings per D-11.
static std::optional<nlohmann::json> decode_error_response(std::span<const uint8_t> payload) {
    if (payload.size() < 2) return std::nullopt;

    nlohmann::json j;
    j["type"] = "error";

    // Map error code byte to name
    uint8_t code = payload[0];
    static constexpr std::pair<uint8_t, std::string_view> ERROR_CODES[] = {
        {1, "malformed_payload"},
        {2, "unknown_type"},
        {3, "decode_failed"},
        {4, "validation_failed"},
        {5, "internal_error"},
        {6, "timeout"},
    };
    std::string_view code_name = "unknown";
    for (const auto& [c, name] : ERROR_CODES) {
        if (c == code) { code_name = name; break; }
    }
    j["code"] = std::string(code_name);

    // Map original_type byte to name via type_registry
    uint8_t orig_type = payload[1];
    auto type_name = type_to_string(orig_type);
    j["original_type"] = type_name
        ? std::string(*type_name)
        : std::to_string(orig_type);

    return j;
}

// =============================================================================
// FlatBuffer response decode helpers
// =============================================================================

/// Convert a DecodedBlob to JSON.
static nlohmann::json blob_to_json(const wire::DecodedBlob& blob) {
    nlohmann::json j;
    j["namespace"] = util::to_hex(blob.namespace_id);
    j["pubkey"] = util::to_hex(blob.pubkey);
    j["data"] = util::base64_encode(blob.data);
    j["ttl"] = blob.ttl;
    j["timestamp"] = std::to_string(blob.timestamp);
    j["signature"] = util::base64_encode(blob.signature);
    return j;
}

/// ReadResponse (32): [status:1][FlatBuffer blob data]
static std::optional<nlohmann::json> decode_read_response(std::span<const uint8_t> p) {
    if (p.empty()) return std::nullopt;

    nlohmann::json j;
    j["type"] = "read_response";
    j["status"] = p[0];

    if (p[0] == 0x01) {
        // Found: remaining bytes are FlatBuffer blob
        auto blob = wire::decode_blob(p.subspan(1));
        if (!blob) return std::nullopt;
        auto blob_json = blob_to_json(*blob);
        for (auto& [key, val] : blob_json.items()) {
            j[key] = std::move(val);
        }
    }
    return j;
}

/// BatchReadResponse (54): [truncated:1][count:u32BE][ [status:1][hash:32][size:u64BE][fb_data] * count ]
static std::optional<nlohmann::json> decode_batch_read_response(std::span<const uint8_t> p) {
    if (p.size() < 5) return std::nullopt;

    nlohmann::json j;
    j["type"] = "batch_read_response";
    j["truncated"] = (p[0] != 0);

    uint32_t count = util::read_u32_be(p.data() + 1);
    auto blobs = nlohmann::json::array();
    size_t off = 5;

    for (uint32_t i = 0; i < count; ++i) {
        if (off + 1 + 32 > p.size()) return std::nullopt;
        uint8_t status = p[off++];
        auto hash_hex = util::to_hex(p.subspan(off, 32));
        off += 32;

        nlohmann::json entry;
        entry["hash"] = hash_hex;
        entry["status"] = status;

        if (status == 0x01) {
            if (off + 8 > p.size()) return std::nullopt;
            uint64_t blob_size = util::read_u64_be(p.data() + off);
            off += 8;
            if (off + blob_size > p.size()) return std::nullopt;
            auto blob = wire::decode_blob(p.subspan(off, blob_size));
            if (blob) {
                auto blob_json = blob_to_json(*blob);
                for (auto& [key, val] : blob_json.items()) {
                    entry[key] = std::move(val);
                }
            }
            off += blob_size;
        }
        blobs.push_back(std::move(entry));
    }

    j["blobs"] = std::move(blobs);
    return j;
}

// =============================================================================
// binary_to_json main function
// =============================================================================

std::optional<nlohmann::json> binary_to_json(uint8_t type,
                                              std::span<const uint8_t> payload) {
    auto schema = schema_for_type(type);
    if (!schema) return std::nullopt;

    // FlatBuffer types
    if (schema->is_flatbuffer) {
        switch (type) {
        case 32: return decode_read_response(payload);
        case 54: return decode_batch_read_response(payload);
        case 8: {
            // Data message from node (unexpected but handle gracefully)
            auto blob = wire::decode_blob(payload);
            if (!blob) return std::nullopt;
            auto j = blob_to_json(*blob);
            j["type"] = "data";
            return j;
        }
        default:
            return std::nullopt;
        }
    }

    // Compound types
    if (schema->is_compound) {
        switch (type) {
        case 34: return decode_list_response(payload);
        case 36: return decode_stats_response(payload);
        case 40: return decode_node_info_response(payload);
        case 42: return decode_namespace_list_response(payload);
        case 44: return decode_storage_status_response(payload);
        case 46: return decode_namespace_stats_response(payload);
        case 48: return decode_metadata_response(payload);
        case 50: return decode_batch_exists_response(payload);
        case 52: return decode_delegation_list_response(payload);
        case 56: return decode_peer_info_response(payload);
        case 58: return decode_time_range_response(payload);
        case 63: return decode_error_response(payload);
        default:
            return std::nullopt;
        }
    }

    // Flat non-FlatBuffer types: table-driven decode.
    return decode_flat(*schema, payload);
}

} // namespace chromatindb::relay::translate
