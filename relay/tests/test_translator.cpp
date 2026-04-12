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

TEST_CASE("json_to_binary: StatsRequest with namespace", "[translator]") {
    nlohmann::json msg = {
        {"type", "stats_request"},
        {"namespace", hex32(0xAA)}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 35);
    REQUIRE(result->payload.size() == 32);
    // Verify namespace bytes
    for (int i = 0; i < 32; ++i) {
        REQUIRE(result->payload[i] == 0xAA);
    }
}

TEST_CASE("json_to_binary: DeleteRequest (FlatBuffer blob with tombstone)", "[translator]") {
    // Delete sends a full signed tombstone blob: data = DEADBEEF + 32-byte target hash
    auto ns_hex = hex32(0x44);
    auto pk_bytes = std::vector<uint8_t>(2592, 0x55);
    auto pk_hex = to_hex(pk_bytes);
    auto sig = std::vector<uint8_t>(4627, 0x66);
    auto sig_b64 = base64_encode(sig);

    // Tombstone data: 4-byte magic + 32-byte target hash = 36 bytes
    std::vector<uint8_t> tombstone_data = {0xDE, 0xAD, 0xBE, 0xEF};
    tombstone_data.insert(tombstone_data.end(), 32, 0xAA);  // target hash

    nlohmann::json msg = {
        {"type", "delete"},
        {"namespace", ns_hex},
        {"pubkey", pk_hex},
        {"data", base64_encode(tombstone_data)},
        {"ttl", 0},
        {"timestamp", "1700000000"},
        {"signature", sig_b64}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 17);
    REQUIRE(result->payload.size() > 64);
}

TEST_CASE("json_to_binary: Delete rejects empty data (no tombstone magic)", "[translator]") {
    auto ns_hex = hex32(0x44);
    auto pk_bytes = std::vector<uint8_t>(2592, 0x55);
    auto sig = std::vector<uint8_t>(4627, 0x66);

    nlohmann::json msg = {
        {"type", "delete"},
        {"namespace", ns_hex},
        {"pubkey", to_hex(pk_bytes)},
        {"data", base64_encode(std::vector<uint8_t>{})},
        {"ttl", 0},
        {"timestamp", "1700000000"},
        {"signature", base64_encode(sig)}
    };
    REQUIRE_FALSE(json_to_binary(msg).has_value());
}

TEST_CASE("json_to_binary: Delete rejects wrong tombstone magic", "[translator]") {
    auto ns_hex = hex32(0x44);
    auto pk_bytes = std::vector<uint8_t>(2592, 0x55);
    auto sig = std::vector<uint8_t>(4627, 0x66);

    std::vector<uint8_t> bad_data = {0x00, 0x00, 0x00, 0x00};
    bad_data.insert(bad_data.end(), 32, 0xAA);

    nlohmann::json msg = {
        {"type", "delete"},
        {"namespace", ns_hex},
        {"pubkey", to_hex(pk_bytes)},
        {"data", base64_encode(bad_data)},
        {"ttl", 0},
        {"timestamp", "1700000000"},
        {"signature", base64_encode(sig)}
    };
    REQUIRE_FALSE(json_to_binary(msg).has_value());
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
        {"until", "5000"},
        {"limit", 100}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 57);
    // namespace(32) + since(8) + until(8) + limit(4) = 52
    REQUIRE(result->payload.size() == 52);
    REQUIRE(read_u64_be(result->payload.data() + 32) == 999);
    REQUIRE(read_u64_be(result->payload.data() + 40) == 5000);
    REQUIRE(read_u32_be(result->payload.data() + 48) == 100);
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

TEST_CASE("binary_to_json: StatsResponse (compound 24-byte)", "[translator]") {
    // StatsResponse(36): [blob_count:u64BE][storage_bytes:u64BE][quota_bytes_limit:u64BE]
    std::vector<uint8_t> payload(24);
    store_u64_be(payload.data(), 42);         // blob_count
    store_u64_be(payload.data() + 8, 8192);   // storage_bytes
    store_u64_be(payload.data() + 16, 1048576); // quota_bytes_limit

    auto json = binary_to_json(36, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "stats_response");
    REQUIRE((*json)["blob_count"] == "42");
    REQUIRE((*json)["storage_bytes"] == "8192");
    REQUIRE((*json)["quota_bytes_limit"] == "1048576");
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

TEST_CASE("binary_to_json: TimeRangeResponse (truncated-first, 48-byte entries)", "[translator]") {
    // TimeRangeResponse(58): [truncated:u8][count:u32BE][ [hash:32][seq_num:u64BE][timestamp:u64BE] * count ]
    uint32_t count = 1;
    std::vector<uint8_t> payload(5 + count * 48);
    payload[0] = 0;  // not truncated
    store_u32_be(payload.data() + 1, count);
    std::memset(payload.data() + 5, 0xEE, 32);        // hash
    store_u64_be(payload.data() + 37, 42);             // seq_num
    store_u64_be(payload.data() + 45, 1234567);        // timestamp

    auto json = binary_to_json(58, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "time_range_response");
    REQUIRE((*json)["truncated"] == false);
    REQUIRE((*json)["entries"].size() == 1);
    REQUIRE((*json)["entries"][0]["seq_num"] == "42");
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

TEST_CASE("json_to_binary: Delete encodes as FlatBuffer blob type 17", "[translator]") {
    // Delete is encode-only (client->node). Node sends back DeleteAck(18), not Delete(17).
    auto ns_hex = hex32(0x33);
    auto pk_bytes = std::vector<uint8_t>(2592, 0x44);
    auto pk_hex = to_hex(pk_bytes);
    auto sig = std::vector<uint8_t>(4627, 0x55);
    auto sig_b64 = base64_encode(sig);

    std::vector<uint8_t> tombstone_data = {0xDE, 0xAD, 0xBE, 0xEF};
    tombstone_data.insert(tombstone_data.end(), 32, 0xBB);

    nlohmann::json msg = {
        {"type", "delete"},
        {"namespace", ns_hex},
        {"pubkey", pk_hex},
        {"data", base64_encode(tombstone_data)},
        {"ttl", 0},
        {"timestamp", "1700000000"},
        {"signature", sig_b64}
    };
    auto binary = json_to_binary(msg);
    REQUIRE(binary.has_value());
    REQUIRE(binary->wire_type == 17);
    // Verify the FlatBuffer can be decoded back as a blob
    auto blob = decode_blob(binary->payload);
    REQUIRE(blob.has_value());
    REQUIRE(blob->namespace_id == make_bytes32(0x33));
    REQUIRE(blob->data.size() == 36);
    REQUIRE(blob->data[0] == 0xDE);
    REQUIRE(blob->ttl == 0);
}

TEST_CASE("binary_to_json: DeleteAck (41-byte format)", "[translator]") {
    // Node sends [hash:32][seq_num:8BE][status:1] = 41 bytes (same as WriteAck)
    std::vector<uint8_t> payload(41);
    std::memset(payload.data(), 0xAA, 32);     // hash
    store_u64_be(payload.data() + 32, 42);     // seq_num
    payload[40] = 0;                            // status = stored
    auto json = binary_to_json(18, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "delete_ack");
    REQUIRE((*json)["hash"] == hex32(0xAA));
    REQUIRE((*json)["seq_num"] == "42");
    REQUIRE((*json)["status"] == 0);
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
    uint8_t compound_types[] = {34, 36, 40, 42, 44, 46, 48, 50, 52, 56, 58};
    for (auto wt : compound_types) {
        auto s = schema_for_type(wt);
        INFO("wire_type=" << static_cast<int>(wt));
        REQUIRE(s != nullptr);
        REQUIRE(s->is_compound == true);
    }
}

// =============================================================================
// Comprehensive decoder tests for fixed compound types (106-01)
// =============================================================================

TEST_CASE("decode NodeInfoResponse: u8 string lengths and raw type bytes", "[translator][compound]") {
    // Build payload matching node's u8-prefixed format:
    // [ver_len:u8][version][gh_len:u8][git_hash]
    // [uptime:u64BE][peer_count:u32BE][namespace_count:u32BE]
    // [total_blobs:u64BE][storage_used:u64BE][storage_max:u64BE]
    // [types_count:u8][type_bytes...]
    std::string version = "1.0.0";
    std::string git_hash = "abc123";

    std::vector<uint8_t> payload;
    // version: u8 len + string
    payload.push_back(static_cast<uint8_t>(version.size()));
    payload.insert(payload.end(), version.begin(), version.end());
    // git_hash: u8 len + string
    payload.push_back(static_cast<uint8_t>(git_hash.size()));
    payload.insert(payload.end(), git_hash.begin(), git_hash.end());

    // Fixed fields
    size_t fixed_off = payload.size();
    payload.resize(payload.size() + 8 + 4 + 4 + 8 + 8 + 8);
    store_u64_be(payload.data() + fixed_off, 1000);       // uptime
    store_u32_be(payload.data() + fixed_off + 8, 2);      // peer_count
    store_u32_be(payload.data() + fixed_off + 12, 3);     // namespace_count
    store_u64_be(payload.data() + fixed_off + 16, 100);   // total_blobs
    store_u64_be(payload.data() + fixed_off + 24, 5000);  // storage_used
    store_u64_be(payload.data() + fixed_off + 32, 10000); // storage_max

    // supported_types: u8 count + raw type bytes
    // Ping=5, Pong=6, Goodbye=7
    payload.push_back(3);  // types_count
    payload.push_back(5);  // Ping
    payload.push_back(6);  // Pong
    payload.push_back(7);  // Goodbye

    auto json = binary_to_json(40, payload);
    REQUIRE(json.has_value());

    SECTION("string fields decoded correctly") {
        REQUIRE((*json)["version"] == "1.0.0");
        REQUIRE((*json)["git_hash"] == "abc123");
    }

    SECTION("numeric fields decoded correctly") {
        REQUIRE((*json)["uptime"] == "1000");
        REQUIRE((*json)["peer_count"] == 2);
        REQUIRE((*json)["namespace_count"] == 3);
        REQUIRE((*json)["total_blobs"] == "100");
        REQUIRE((*json)["storage_used"] == "5000");
        REQUIRE((*json)["storage_max"] == "10000");
    }

    SECTION("supported_types translated via type_to_string") {
        REQUIRE((*json)["supported_types"].is_array());
        REQUIRE((*json)["supported_types"].size() == 3);
        REQUIRE((*json)["supported_types"][0] == "ping");
        REQUIRE((*json)["supported_types"][1] == "pong");
        REQUIRE((*json)["supported_types"][2] == "goodbye");
    }
}

TEST_CASE("decode NodeInfoResponse: unknown type byte falls back to numeric string", "[translator][compound]") {
    std::vector<uint8_t> payload;
    // version: empty string
    payload.push_back(0);
    // git_hash: empty string
    payload.push_back(0);
    // Fixed fields (all zeros)
    payload.resize(payload.size() + 8 + 4 + 4 + 8 + 8 + 8, 0);
    // 1 type with unknown value 255
    payload.push_back(1);
    payload.push_back(255);

    auto json = binary_to_json(40, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["supported_types"].size() == 1);
    REQUIRE((*json)["supported_types"][0] == "255");
}

TEST_CASE("decode NodeInfoResponse: truncated input returns nullopt", "[translator][compound]") {
    SECTION("empty payload") {
        auto json = binary_to_json(40, std::span<const uint8_t>{});
        REQUIRE_FALSE(json.has_value());
    }

    SECTION("version only, no git_hash") {
        std::vector<uint8_t> payload = {3, 'a', 'b', 'c'};
        auto json = binary_to_json(40, payload);
        REQUIRE_FALSE(json.has_value());
    }

    SECTION("strings complete but fixed fields too short") {
        std::vector<uint8_t> payload = {1, 'x', 1, 'y'};
        // Missing 40 bytes of fixed fields
        auto json = binary_to_json(40, payload);
        REQUIRE_FALSE(json.has_value());
    }

    SECTION("header complete but types section short") {
        std::vector<uint8_t> payload;
        payload.push_back(0);  // empty version
        payload.push_back(0);  // empty git_hash
        payload.resize(payload.size() + 40, 0);  // fixed fields
        payload.push_back(3);  // claims 3 types
        payload.push_back(5);  // but only 1 byte
        auto json = binary_to_json(40, payload);
        REQUIRE_FALSE(json.has_value());
    }
}

TEST_CASE("decode StatsResponse: 24-byte compound format", "[translator][compound]") {
    std::vector<uint8_t> payload(24);
    store_u64_be(payload.data(), 42);
    store_u64_be(payload.data() + 8, 8192);
    store_u64_be(payload.data() + 16, 1048576);

    auto json = binary_to_json(36, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "stats_response");
    REQUIRE((*json)["blob_count"] == "42");
    REQUIRE((*json)["storage_bytes"] == "8192");
    REQUIRE((*json)["quota_bytes_limit"] == "1048576");
}

TEST_CASE("decode StatsResponse: truncated input returns nullopt", "[translator][compound]") {
    SECTION("empty payload") {
        auto json = binary_to_json(36, std::span<const uint8_t>{});
        REQUIRE_FALSE(json.has_value());
    }

    SECTION("23 bytes (one short)") {
        std::vector<uint8_t> payload(23, 0);
        auto json = binary_to_json(36, payload);
        REQUIRE_FALSE(json.has_value());
    }
}

TEST_CASE("decode TimeRangeResponse: truncated-first with seq_num", "[translator][compound]") {
    // [truncated:u8][count:u32BE][ [hash:32][seq_num:u64BE][timestamp:u64BE] * count ]
    uint32_t count = 2;
    std::vector<uint8_t> payload(5 + count * 48);
    payload[0] = 0x01;  // truncated
    store_u32_be(payload.data() + 1, count);

    // Entry 0: hash=0x00, seq=100, ts=1000
    std::memset(payload.data() + 5, 0x00, 32);
    store_u64_be(payload.data() + 37, 100);
    store_u64_be(payload.data() + 45, 1000);

    // Entry 1: hash=0xFF, seq=200, ts=2000
    std::memset(payload.data() + 53, 0xFF, 32);
    store_u64_be(payload.data() + 85, 200);
    store_u64_be(payload.data() + 93, 2000);

    auto json = binary_to_json(58, payload);
    REQUIRE(json.has_value());

    SECTION("truncated flag is first byte") {
        REQUIRE((*json)["truncated"] == true);
    }

    SECTION("entries have correct count") {
        REQUIRE((*json)["entries"].size() == 2);
    }

    SECTION("entry 0 fields") {
        REQUIRE((*json)["entries"][0]["seq_num"] == "100");
        REQUIRE((*json)["entries"][0]["timestamp"] == "1000");
    }

    SECTION("entry 1 fields") {
        REQUIRE((*json)["entries"][1]["seq_num"] == "200");
        REQUIRE((*json)["entries"][1]["timestamp"] == "2000");
    }
}

TEST_CASE("decode TimeRangeResponse: truncated input returns nullopt", "[translator][compound]") {
    SECTION("too short for header") {
        std::vector<uint8_t> payload = {0, 0, 0, 0};  // 4 bytes, need 5
        auto json = binary_to_json(58, payload);
        REQUIRE_FALSE(json.has_value());
    }

    SECTION("header says 2 entries but only 1 present") {
        std::vector<uint8_t> payload(5 + 48);  // header + 1 entry
        payload[0] = 0;
        store_u32_be(payload.data() + 1, 2);  // claims 2 entries
        auto json = binary_to_json(58, payload);
        REQUIRE_FALSE(json.has_value());
    }
}

TEST_CASE("decode DelegationListResponse: correct field names", "[translator][compound]") {
    uint32_t count = 2;
    std::vector<uint8_t> payload(4 + count * 64);
    store_u32_be(payload.data(), count);

    // Entry 0
    std::memset(payload.data() + 4, 0xAA, 32);     // delegate_pk_hash
    std::memset(payload.data() + 36, 0xBB, 32);    // delegation_blob_hash

    // Entry 1
    std::memset(payload.data() + 68, 0xCC, 32);    // delegate_pk_hash
    std::memset(payload.data() + 100, 0xDD, 32);   // delegation_blob_hash

    auto json = binary_to_json(52, payload);
    REQUIRE(json.has_value());
    REQUIRE((*json)["type"] == "delegation_list_response");
    REQUIRE((*json)["delegations"].size() == 2);

    SECTION("field names are delegate_pk_hash and delegation_blob_hash") {
        REQUIRE((*json)["delegations"][0].contains("delegate_pk_hash"));
        REQUIRE((*json)["delegations"][0].contains("delegation_blob_hash"));
        // Verify old field names are NOT present
        REQUIRE_FALSE((*json)["delegations"][0].contains("namespace"));
        REQUIRE_FALSE((*json)["delegations"][0].contains("pubkey_hash"));
    }

    SECTION("field values are correct hex") {
        REQUIRE((*json)["delegations"][0]["delegate_pk_hash"] == hex32(0xAA));
        REQUIRE((*json)["delegations"][0]["delegation_blob_hash"] == hex32(0xBB));
        REQUIRE((*json)["delegations"][1]["delegate_pk_hash"] == hex32(0xCC));
        REQUIRE((*json)["delegations"][1]["delegation_blob_hash"] == hex32(0xDD));
    }
}

TEST_CASE("decode DelegationListResponse: truncated input returns nullopt", "[translator][compound]") {
    SECTION("empty payload") {
        std::vector<uint8_t> payload;
        auto json = binary_to_json(52, payload);
        REQUIRE_FALSE(json.has_value());
    }

    SECTION("count says 1 but payload too short") {
        std::vector<uint8_t> payload(4 + 32);  // count + half an entry
        store_u32_be(payload.data(), 1);
        auto json = binary_to_json(52, payload);
        REQUIRE_FALSE(json.has_value());
    }
}

TEST_CASE("json_to_binary: StatsRequest encodes namespace in payload", "[translator][compound]") {
    nlohmann::json msg = {
        {"type", "stats_request"},
        {"namespace", hex32(0x55)}
    };
    auto result = json_to_binary(msg);
    REQUIRE(result.has_value());
    REQUIRE(result->wire_type == 35);
    REQUIRE(result->payload.size() == 32);
    for (int i = 0; i < 32; ++i) {
        REQUIRE(result->payload[i] == 0x55);
    }
}

// =============================================================================
// Bounds-check regression tests for existing correct compound decoders
// =============================================================================

TEST_CASE("bounds check: ListResponse empty payload returns nullopt", "[translator][bounds]") {
    auto json = binary_to_json(34, std::span<const uint8_t>{});
    REQUIRE_FALSE(json.has_value());
}

TEST_CASE("bounds check: NamespaceListResponse empty payload returns nullopt", "[translator][bounds]") {
    auto json = binary_to_json(42, std::span<const uint8_t>{});
    REQUIRE_FALSE(json.has_value());
}

TEST_CASE("bounds check: MetadataResponse empty payload returns nullopt", "[translator][bounds]") {
    auto json = binary_to_json(48, std::span<const uint8_t>{});
    REQUIRE_FALSE(json.has_value());
}

TEST_CASE("bounds check: BatchExistsResponse empty payload returns valid empty results", "[translator][bounds]") {
    // BatchExistsResponse with empty span produces empty results array (not nullopt)
    auto json = binary_to_json(50, std::span<const uint8_t>{});
    REQUIRE(json.has_value());
    REQUIRE((*json)["results"].empty());
}

TEST_CASE("bounds check: PeerInfoResponse empty payload returns nullopt", "[translator][bounds]") {
    auto json = binary_to_json(56, std::span<const uint8_t>{});
    REQUIRE_FALSE(json.has_value());
}

TEST_CASE("bounds check: NamespaceStatsResponse empty payload returns nullopt", "[translator][bounds]") {
    auto json = binary_to_json(46, std::span<const uint8_t>{});
    REQUIRE_FALSE(json.has_value());
}

TEST_CASE("bounds check: StorageStatusResponse empty payload returns nullopt", "[translator][bounds]") {
    auto json = binary_to_json(44, std::span<const uint8_t>{});
    REQUIRE_FALSE(json.has_value());
}

// =============================================================================
// ErrorResponse (63) tests -- Phase 999.2
// =============================================================================

TEST_CASE("binary_to_json: ErrorResponse (type 63)", "[translator]") {
    // Payload: [error_code:1][original_type:1]
    // malformed_payload(1) for ReadRequest(31)
    std::vector<uint8_t> payload = {0x01, 31};
    auto result = binary_to_json(63, payload);
    REQUIRE(result.has_value());

    CHECK((*result)["type"] == "error");
    CHECK((*result)["code"] == "malformed_payload");
    CHECK((*result)["original_type"] == "read_request");
}

TEST_CASE("binary_to_json: ErrorResponse all error codes", "[translator]") {
    struct TestCase {
        uint8_t code;
        std::string_view expected;
    };
    TestCase cases[] = {
        {1, "malformed_payload"},
        {2, "unknown_type"},
        {3, "decode_failed"},
        {4, "validation_failed"},
        {5, "internal_error"},
    };

    for (const auto& tc : cases) {
        SECTION(std::string(tc.expected)) {
            std::vector<uint8_t> payload = {tc.code, 35};  // StatsRequest
            auto result = binary_to_json(63, payload);
            REQUIRE(result.has_value());
            CHECK((*result)["code"] == tc.expected);
            CHECK((*result)["original_type"] == "stats_request");
        }
    }
}

TEST_CASE("binary_to_json: ErrorResponse unknown error code", "[translator]") {
    std::vector<uint8_t> payload = {0xFF, 31};  // Unknown code
    auto result = binary_to_json(63, payload);
    REQUIRE(result.has_value());
    CHECK((*result)["code"] == "unknown");
    CHECK((*result)["original_type"] == "read_request");
}

TEST_CASE("binary_to_json: ErrorResponse unknown original_type falls back to number", "[translator]") {
    std::vector<uint8_t> payload = {0x01, 0xFF};  // Unknown wire type 255
    auto result = binary_to_json(63, payload);
    REQUIRE(result.has_value());
    CHECK((*result)["code"] == "malformed_payload");
    CHECK((*result)["original_type"] == "255");
}

TEST_CASE("binary_to_json: ErrorResponse truncated payload returns nullopt", "[translator]") {
    SECTION("empty payload") {
        std::vector<uint8_t> payload = {};
        auto result = binary_to_json(63, payload);
        CHECK_FALSE(result.has_value());
    }
    SECTION("1-byte payload") {
        std::vector<uint8_t> payload = {0x01};
        auto result = binary_to_json(63, payload);
        CHECK_FALSE(result.has_value());
    }
}

TEST_CASE("binary_to_json: ErrorResponse without request_id has no request_id field", "[translator]") {
    std::vector<uint8_t> payload = {0x03, 37};  // decode_failed for ExistsRequest
    auto result = binary_to_json(63, payload);
    REQUIRE(result.has_value());
    CHECK((*result)["type"] == "error");
    CHECK((*result)["code"] == "decode_failed");
    CHECK((*result)["original_type"] == "exists_request");
    // binary_to_json does not add request_id; that's done by the caller
    CHECK_FALSE(result->contains("request_id"));
}
