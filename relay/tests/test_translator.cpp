#include "relay/translate/translator.h"
#include "relay/translate/json_schema.h"
#include "relay/translate/type_registry.h"
#include "relay/util/base64.h"
#include "relay/util/endian.h"
#include "relay/util/hex.h"
#include "relay/wire/blob_codec.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace chromatindb::relay::translate;
using namespace chromatindb::relay::util;
using namespace chromatindb::relay::wire;

// Helper: create a 32-byte pattern filled with a specific byte.
static std::vector<uint8_t> make_bytes32(uint8_t fill) {
    return std::vector<uint8_t>(32, fill);
}

static std::string hex32(uint8_t fill) {
    return to_hex(make_bytes32(fill));
}

// =============================================================================
// json_to_binary tests
// =============================================================================

TEST_CASE("json_to_binary: ReadRequest encodes namespace + hash", "[translator]") {
    nlohmann::json msg = {
        {"type", "read_request"},
        {"namespace", hex32(0xAA)},
        {"hash", hex32(0xBB)},
        {"request_id", 42}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 31);
    REQUIRE(result->payload.size() == 64);
    // First 32 bytes: namespace (0xAA fill)
    for (int i = 0; i < 32; ++i) {
        REQUIRE(result->payload[i] == 0xAA);
    }
    // Next 32 bytes: hash (0xBB fill)
    for (int i = 32; i < 64; ++i) {
        REQUIRE(result->payload[i] == 0xBB);
    }
}

TEST_CASE("json_to_binary: ListRequest with since_seq", "[translator]") {
    nlohmann::json msg = {
        {"type", "list_request"},
        {"namespace", hex32(0x11)},
        {"since_seq", "100"},
        {"limit", 50}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 33);
    // namespace(32) + since_seq(8) + limit(4) = 44
    REQUIRE(result->payload.size() == 44);
    // since_seq at offset 32
    REQUIRE(read_u64_be(result->payload.data() + 32) == 100);
    // limit at offset 40
    REQUIRE(read_u32_be(result->payload.data() + 40) == 50);
}

TEST_CASE("json_to_binary: ExistsRequest", "[translator]") {
    nlohmann::json msg = {
        {"type", "exists_request"},
        {"namespace", hex32(0x22)},
        {"hash", hex32(0x33)}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 37);
    REQUIRE(result->payload.size() == 64);
}

TEST_CASE("json_to_binary: StatsRequest (empty payload)", "[translator]") {
    nlohmann::json msg = {{"type", "stats_request"}};
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 35);
    REQUIRE(result->payload.empty());
}

TEST_CASE("json_to_binary: DeleteRequest", "[translator]") {
    nlohmann::json msg = {
        {"type", "delete"},
        {"namespace", hex32(0x44)},
        {"hash", hex32(0x55)}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 17);
    REQUIRE(result->payload.size() == 64);
}

TEST_CASE("json_to_binary: Data (FlatBuffer blob)", "[translator]") {
    auto ns_hex = hex32(0xCC);
    auto pk_bytes = std::vector<uint8_t>(2592, 0xDD);
    auto pk_hex = to_hex(pk_bytes);
    auto data = std::vector<uint8_t>{1, 2, 3, 4};
    auto data_b64 = base64_encode(data);
    auto sig = std::vector<uint8_t>(4627, 0xEE);
    auto sig_b64 = base64_encode(sig);

    nlohmann::json msg = {
        {"type", "data"},
        {"namespace", ns_hex},
        {"pubkey", pk_hex},
        {"data", data_b64},
        {"ttl", 3600},
        {"timestamp", "1000000"},
        {"signature", sig_b64}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 8);
    // Should be a valid FlatBuffer blob
    auto decoded = decode_blob(result->payload);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->namespace_id.size() == 32);
    REQUIRE(decoded->namespace_id[0] == 0xCC);
    REQUIRE(decoded->ttl == 3600);
    REQUIRE(decoded->timestamp == 1000000);
    REQUIRE(decoded->data.size() == 4);
}

TEST_CASE("json_to_binary: Subscribe with namespace array", "[translator]") {
    nlohmann::json msg = {
        {"type", "subscribe"},
        {"namespaces", {hex32(0x11), hex32(0x22)}}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 19);
    // count(4) + 2*32 = 68
    REQUIRE(result->payload.size() == 68);
    REQUIRE(read_u32_be(result->payload.data()) == 2);
}

TEST_CASE("json_to_binary: BatchExistsRequest", "[translator]") {
    nlohmann::json msg = {
        {"type", "batch_exists_request"},
        {"namespace", hex32(0xAA)},
        {"hashes", {hex32(0xBB), hex32(0xCC)}}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 49);
    // namespace(32) + count(4) + 2*32 = 100
    REQUIRE(result->payload.size() == 100);
    REQUIRE(read_u32_be(result->payload.data() + 32) == 2);
}

TEST_CASE("json_to_binary: BatchReadRequest field order", "[translator]") {
    nlohmann::json msg = {
        {"type", "batch_read_request"},
        {"namespace", hex32(0xAA)},
        {"max_bytes", 4194304},
        {"hashes", {hex32(0xBB)}}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 53);
    // namespace(32) + max_bytes(4) + count(4) + 1*32 = 72
    REQUIRE(result->payload.size() == 72);
    // max_bytes at offset 32
    REQUIRE(read_u32_be(result->payload.data() + 32) == 4194304);
    // count at offset 36
    REQUIRE(read_u32_be(result->payload.data() + 36) == 1);
}

TEST_CASE("json_to_binary: unknown type returns nullopt", "[translator]") {
    nlohmann::json msg = {{"type", "nonexistent_type"}};
    auto result = json_to_binary(msg);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("json_to_binary: NamespaceListRequest with cursor and limit", "[translator]") {
    nlohmann::json msg = {
        {"type", "namespace_list_request"},
        {"after_namespace", hex32(0xAA)},
        {"limit", 50}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 41);
    // namespace(32) + limit(4) = 36
    REQUIRE(result->payload.size() == 36);
    REQUIRE(read_u32_be(result->payload.data() + 32) == 50);
}

TEST_CASE("json_to_binary: TimeRangeRequest", "[translator]") {
    nlohmann::json msg = {
        {"type", "time_range_request"},
        {"namespace", hex32(0x11)},
        {"since", "999"},
        {"limit", 100}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 57);
    // namespace(32) + since(8) + limit(4) = 44
    REQUIRE(result->payload.size() == 44);
    REQUIRE(read_u64_be(result->payload.data() + 32) == 999);
    REQUIRE(read_u32_be(result->payload.data() + 40) == 100);
}

// =============================================================================
// binary_to_json tests
// =============================================================================

TEST_CASE("binary_to_json: WriteAck", "[translator]") {
    // WriteAck(30): [hash:32][seq_num:u64BE][status:u8]
    std::vector<uint8_t> payload(41);
    std::memset(payload.data(), 0xAA, 32);  // hash
    store_u64_be(payload.data() + 32, 12345);  // seq_num
    payload[40] = 0;  // status

    auto json = binary_to_json(30, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "write_ack");
    REQUIRE((*json)["hash"] == hex32(0xAA));
    REQUIRE((*json)["seq_num"] == "12345");
    REQUIRE((*json)["status"] == 0);
}

TEST_CASE("binary_to_json: ExistsResponse", "[translator]") {
    std::vector<uint8_t> payload = {0x01};  // exists = true
    auto json = binary_to_json(38, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "exists_response");
    REQUIRE((*json)["exists"] == true);
}

TEST_CASE("binary_to_json: StatsResponse", "[translator]") {
    // StatsResponse(36): [total_blobs:u64BE][storage_used:u64BE][storage_max:u64BE]
    //                     [namespace_count:u32BE][peer_count:u32BE][uptime:u64BE]
    std::vector<uint8_t> payload(48);
    store_u64_be(payload.data(), 1000);       // total_blobs
    store_u64_be(payload.data() + 8, 2000);   // storage_used
    store_u64_be(payload.data() + 16, 5000);  // storage_max
    store_u32_be(payload.data() + 24, 10);    // namespace_count
    store_u32_be(payload.data() + 28, 5);     // peer_count
    store_u64_be(payload.data() + 32, 86400); // uptime

    auto json = binary_to_json(36, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "stats_response");
    REQUIRE((*json)["total_blobs"] == "1000");
    REQUIRE((*json)["storage_used"] == "2000");
    REQUIRE((*json)["peer_count"] == 5);
}

TEST_CASE("binary_to_json: ReadResponse (FlatBuffer)", "[translator]") {
    // Build a blob
    DecodedBlob blob;
    blob.namespace_id = make_bytes32(0x11);
    blob.pubkey = std::vector<uint8_t>(2592, 0x22);
    blob.data = {1, 2, 3};
    blob.ttl = 3600;
    blob.timestamp = 1000;
    blob.signature = std::vector<uint8_t>(4627, 0x33);

    auto fb = encode_blob(blob);
    // ReadResponse: [status:1][fb_blob]
    std::vector<uint8_t> payload(1 + fb.size());
    payload[0] = 0x01;  // STATUS_FOUND
    std::memcpy(payload.data() + 1, fb.data(), fb.size());

    auto json = binary_to_json(32, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "read_response");
    REQUIRE((*json)["status"] == 1);
    REQUIRE((*json)["ttl"] == 3600);
    REQUIRE((*json)["timestamp"] == "1000");
}

TEST_CASE("binary_to_json: ReadResponse not found", "[translator]") {
    std::vector<uint8_t> payload = {0x00};  // STATUS_NOT_FOUND
    auto json = binary_to_json(32, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "read_response");
    REQUIRE((*json)["status"] == 0);
    REQUIRE_FALSE(json->contains("namespace"));
}

TEST_CASE("binary_to_json: ListResponse (compound)", "[translator]") {
    // ListResponse: [count:u32BE][ [hash:32][seq_num:u64BE] * count ][truncated:u8]
    uint32_t count = 2;
    std::vector<uint8_t> payload(4 + count * 40 + 1);
    store_u32_be(payload.data(), count);
    // Entry 0
    std::memset(payload.data() + 4, 0xAA, 32);
    store_u64_be(payload.data() + 36, 100);
    // Entry 1
    std::memset(payload.data() + 44, 0xBB, 32);
    store_u64_be(payload.data() + 76, 200);
    // Truncated
    payload.back() = 1;

    auto json = binary_to_json(34, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "list_response");
    REQUIRE((*json)["truncated"] == true);
    REQUIRE((*json)["entries"].size() == 2);
    REQUIRE((*json)["entries"][0]["seq_num"] == "100");
    REQUIRE((*json)["entries"][1]["seq_num"] == "200");
}

TEST_CASE("binary_to_json: MetadataResponse found", "[translator]") {
    // [status:1][hash:32][timestamp:8][ttl:4][data_size:8][seq_num:8][pubkey_len:2][pubkey:N]
    uint16_t pk_len = 4;
    std::vector<uint8_t> payload(1 + 32 + 8 + 4 + 8 + 8 + 2 + pk_len);
    payload[0] = 0x01;  // found
    std::memset(payload.data() + 1, 0xCC, 32);  // hash
    store_u64_be(payload.data() + 33, 1000);   // timestamp
    store_u32_be(payload.data() + 41, 3600);   // ttl
    store_u64_be(payload.data() + 45, 512);    // data_size
    store_u64_be(payload.data() + 53, 42);     // seq_num
    store_u16_be(payload.data() + 61, pk_len); // pubkey_len
    std::memset(payload.data() + 63, 0xDD, pk_len);  // pubkey

    auto json = binary_to_json(48, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "metadata_response");
    REQUIRE((*json)["found"] == true);
    REQUIRE((*json)["ttl"] == 3600);
    REQUIRE((*json)["data_size"] == "512");
    REQUIRE((*json)["seq_num"] == "42");
}

TEST_CASE("binary_to_json: MetadataResponse not found", "[translator]") {
    std::vector<uint8_t> payload = {0x00};
    auto json = binary_to_json(48, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["found"] == false);
}

TEST_CASE("binary_to_json: BatchExistsResponse", "[translator]") {
    std::vector<uint8_t> payload = {0x01, 0x00, 0x01};
    auto json = binary_to_json(50, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "batch_exists_response");
    REQUIRE((*json)["results"].size() == 3);
    REQUIRE((*json)["results"][0] == true);
    REQUIRE((*json)["results"][1] == false);
    REQUIRE((*json)["results"][2] == true);
}

TEST_CASE("binary_to_json: NamespaceListResponse", "[translator]") {
    // [count:u32BE][has_more:u8][ [ns:32][blob_count:u64BE] * count ]
    uint32_t count = 1;
    std::vector<uint8_t> payload(5 + count * 40);
    store_u32_be(payload.data(), count);
    payload[4] = 0x01;  // has_more
    std::memset(payload.data() + 5, 0xAA, 32);
    store_u64_be(payload.data() + 37, 999);

    auto json = binary_to_json(42, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "namespace_list_response");
    REQUIRE((*json)["has_more"] == true);
    REQUIRE((*json)["namespaces"].size() == 1);
    REQUIRE((*json)["namespaces"][0]["blob_count"] == "999");
}

TEST_CASE("binary_to_json: PeerInfoResponse trusted format", "[translator]") {
    // Minimal: 1 peer with address "1.2.3.4:5678"
    std::string addr = "1.2.3.4:5678";
    // [peer_count:u32BE][bootstrap_count:u32BE][addr_len:u16BE][addr][is_bootstrap:u8][syncing:u8][is_full:u8][duration_ms:u64BE]
    std::vector<uint8_t> payload(8 + 2 + addr.size() + 3 + 8);
    store_u32_be(payload.data(), 1);      // peer_count
    store_u32_be(payload.data() + 4, 0);  // bootstrap_count
    store_u16_be(payload.data() + 8, static_cast<uint16_t>(addr.size()));
    std::memcpy(payload.data() + 10, addr.data(), addr.size());
    size_t off = 10 + addr.size();
    payload[off++] = 1;  // is_bootstrap
    payload[off++] = 0;  // syncing
    payload[off++] = 1;  // is_full
    store_u64_be(payload.data() + off, 60000);

    auto json = binary_to_json(56, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "peer_info_response");
    REQUIRE((*json)["peers"].size() == 1);
    REQUIRE((*json)["peers"][0]["address"] == "1.2.3.4:5678");
    REQUIRE((*json)["peers"][0]["is_bootstrap"] == true);
    REQUIRE((*json)["peers"][0]["is_full"] == true);
    REQUIRE((*json)["peers"][0]["duration_ms"] == "60000");
}

TEST_CASE("binary_to_json: TimeRangeResponse", "[translator]") {
    uint32_t count = 1;
    std::vector<uint8_t> payload(4 + count * 40 + 1);
    store_u32_be(payload.data(), count);
    std::memset(payload.data() + 4, 0xEE, 32);
    store_u64_be(payload.data() + 36, 1234567);
    payload.back() = 0;  // not truncated

    auto json = binary_to_json(58, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "time_range_response");
    REQUIRE((*json)["truncated"] == false);
    REQUIRE((*json)["entries"].size() == 1);
    REQUIRE((*json)["entries"][0]["timestamp"] == "1234567");
}

TEST_CASE("binary_to_json: DelegationListResponse", "[translator]") {
    uint32_t count = 1;
    std::vector<uint8_t> payload(4 + count * 64);
    store_u32_be(payload.data(), count);
    std::memset(payload.data() + 4, 0xAA, 32);  // namespace
    std::memset(payload.data() + 36, 0xBB, 32);  // pubkey_hash

    auto json = binary_to_json(52, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "delegation_list_response");
    REQUIRE((*json)["delegations"].size() == 1);
}

TEST_CASE("binary_to_json: NamespaceStatsResponse found", "[translator]") {
    // [found:u8][blob_count:u64BE][storage_used:u64BE][delegation_count:u64BE][quota_bytes:u64BE][quota_count:u64BE]
    std::vector<uint8_t> payload(41);
    payload[0] = 0x01;
    store_u64_be(payload.data() + 1, 100);   // blob_count
    store_u64_be(payload.data() + 9, 2000);  // storage_used
    store_u64_be(payload.data() + 17, 3);    // delegation_count
    store_u64_be(payload.data() + 25, 10000); // quota_bytes_limit
    store_u64_be(payload.data() + 33, 500);  // quota_count_limit

    auto json = binary_to_json(46, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "namespace_stats_response");
    REQUIRE((*json)["found"] == true);
    REQUIRE((*json)["blob_count"] == "100");
    REQUIRE((*json)["storage_used"] == "2000");
}

TEST_CASE("is_binary_response", "[translator]") {
    REQUIRE(is_binary_response(32) == true);   // ReadResponse
    REQUIRE(is_binary_response(54) == true);   // BatchReadResponse
    REQUIRE(is_binary_response(30) == false);  // WriteAck
    REQUIRE(is_binary_response(34) == false);  // ListResponse
    REQUIRE(is_binary_response(8) == false);   // Data
}

// =============================================================================
// Roundtrip tests
// =============================================================================

TEST_CASE("roundtrip: ReadRequest", "[translator]") {
    nlohmann::json msg = {
        {"type", "read_request"},
        {"namespace", hex32(0xAA)},
        {"hash", hex32(0xBB)}
    };
    auto binary = json_to_binary(msg);
    REQUIRE(binary.has_value());
    auto json = binary_to_json(binary->wire_type, binary->payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "read_request");
    REQUIRE((*json)["namespace"] == hex32(0xAA));
    REQUIRE((*json)["hash"] == hex32(0xBB));
}

TEST_CASE("roundtrip: ExistsRequest", "[translator]") {
    nlohmann::json msg = {
        {"type", "exists_request"},
        {"namespace", hex32(0x11)},
        {"hash", hex32(0x22)}
    };
    auto binary = json_to_binary(msg);
    REQUIRE(binary.has_value());
    auto json = binary_to_json(binary->wire_type, binary->payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["namespace"] == hex32(0x11));
    REQUIRE((*json)["hash"] == hex32(0x22));
}

TEST_CASE("roundtrip: DeleteRequest", "[translator]") {
    nlohmann::json msg = {
        {"type", "delete"},
        {"namespace", hex32(0x33)},
        {"hash", hex32(0x44)}
    };
    auto binary = json_to_binary(msg);
    REQUIRE(binary.has_value());
    auto json = binary_to_json(binary->wire_type, binary->payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "delete");
    REQUIRE((*json)["namespace"] == hex32(0x33));
}

TEST_CASE("json_to_binary: missing type returns nullopt", "[translator]") {
    nlohmann::json msg = {{"namespace", hex32(0xAA)}};
    REQUIRE_FALSE(json_to_binary(msg).has_value());
}

TEST_CASE("json_to_binary: Ping (fire-and-forget)", "[translator]") {
    nlohmann::json msg = {{"type", "ping"}};
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 5);
    REQUIRE(result->payload.empty());
}

TEST_CASE("schema fixes: ListRequest has since_seq field", "[translator][schema]") {
    auto s = schema_for_name("list_request");
    REQUIRE(s != nullptr);
    bool found_since_seq = false;
    for (const auto& f : s->fields) {
        if (f.json_name == "since_seq") {
            found_since_seq = true;
            REQUIRE(f.encoding == FieldEncoding::UINT64_STRING);
            REQUIRE(f.optional == true);
        }
    }
    REQUIRE(found_since_seq);
}

TEST_CASE("schema fixes: NamespaceListRequest has after_namespace and limit", "[translator][schema]") {
    auto s = schema_for_name("namespace_list_request");
    REQUIRE(s != nullptr);
    bool found_after_ns = false;
    bool found_limit = false;
    for (const auto& f : s->fields) {
        if (f.json_name == "after_namespace") found_after_ns = true;
        if (f.json_name == "limit") found_limit = true;
    }
    REQUIRE(found_after_ns);
    REQUIRE(found_limit);
}

TEST_CASE("schema fixes: compound types are marked is_compound", "[translator][schema]") {
    // ListResponse(34), NamespaceListResponse(42), MetadataResponse(48),
    // BatchExistsResponse(50), DelegationListResponse(52),
    // PeerInfoResponse(56), TimeRangeResponse(58)
    uint8_t compound_types[] = {34, 40, 42, 44, 46, 48, 50, 52, 56, 58};
    for (auto wt : compound_types) {
        auto s = schema_for_type(wt);
        INFO("wire_type=" << static_cast<int>(wt));
        REQUIRE(s != nullptr);
        REQUIRE(s->is_compound == true);
    }
}
