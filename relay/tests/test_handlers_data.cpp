#include <catch2/catch_test_macros.hpp>

#include "relay/http/handlers_data.h"
#include "relay/http/http_parser.h"
#include "relay/http/http_response.h"
#include "relay/util/hex.h"

using namespace chromatindb::relay::http;
using namespace chromatindb::relay::util;

// ===========================================================================
// Path parameter extraction
// ===========================================================================

TEST_CASE("extract_blob_path_params: valid 64-char hex namespace and hash", "[http][data][path]") {
    std::string ns_hex(64, 'a');   // 64 'a' chars
    std::string hash_hex(64, 'b'); // 64 'b' chars
    std::string path = "/blob/" + ns_hex + "/" + hash_hex;

    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE(params.has_value());
    REQUIRE(params->namespace_bytes.size() == 32);
    REQUIRE(params->hash_bytes.size() == 32);

    // Verify decoded bytes.
    for (auto b : params->namespace_bytes) {
        REQUIRE(b == 0xAA);
    }
    for (auto b : params->hash_bytes) {
        REQUIRE(b == 0xBB);
    }
}

TEST_CASE("extract_blob_path_params: mixed-case hex is accepted", "[http][data][path]") {
    std::string ns_hex = "aAbBcCdDeEfF0011223344556677889900aAbBcCdDeEfF00112233445566778899";
    std::string hash_hex = "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF";
    std::string path = "/blob/" + ns_hex + "/" + hash_hex;

    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE(params.has_value());
    REQUIRE(params->namespace_bytes.size() == 32);
    REQUIRE(params->hash_bytes.size() == 32);
}

TEST_CASE("extract_blob_path_params: short namespace returns nullopt", "[http][data][path]") {
    std::string path = "/blob/abcd/0000000000000000000000000000000000000000000000000000000000000000";
    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: short hash returns nullopt", "[http][data][path]") {
    std::string path = "/blob/0000000000000000000000000000000000000000000000000000000000000000/abcd";
    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: no separator returns nullopt", "[http][data][path]") {
    // 128 hex chars but no '/' between ns and hash
    std::string path = "/blob/" + std::string(128, 'a');
    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: non-hex characters return nullopt", "[http][data][path]") {
    std::string ns_hex(64, 'g');  // 'g' is not valid hex
    std::string hash_hex(64, '0');
    std::string path = "/blob/" + ns_hex + "/" + hash_hex;

    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: wrong prefix returns nullopt", "[http][data][path]") {
    std::string ns_hex(64, 'a');
    std::string hash_hex(64, 'b');
    std::string path = "/other/" + ns_hex + "/" + hash_hex;

    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: empty path returns nullopt", "[http][data][path]") {
    auto params = extract_blob_path_params("", "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: just prefix returns nullopt", "[http][data][path]") {
    auto params = extract_blob_path_params("/blob/", "/blob/");
    REQUIRE_FALSE(params.has_value());
}

TEST_CASE("extract_blob_path_params: trailing slash ignored", "[http][data][path]") {
    std::string ns_hex(64, 'a');
    std::string hash_hex(64, 'b');
    // Trailing slash makes hash_hex appear as 64+1 chars -> invalid
    std::string path = "/blob/" + ns_hex + "/" + hash_hex + "/";

    auto params = extract_blob_path_params(path, "/blob/");
    REQUIRE_FALSE(params.has_value());
}

// ===========================================================================
// Wire type constants verification
// ===========================================================================

TEST_CASE("Wire type constants match expected values", "[http][data][wire]") {
    // Verify the wire types used by handlers match the transport_generated.h values.
    // These are the types the handlers must use for correctness.
    REQUIRE(chromatindb::wire::TransportMsgType_Data == 8);
    REQUIRE(chromatindb::wire::TransportMsgType_Delete == 17);
    REQUIRE(chromatindb::wire::TransportMsgType_DeleteAck == 18);
    REQUIRE(chromatindb::wire::TransportMsgType_WriteAck == 30);
    REQUIRE(chromatindb::wire::TransportMsgType_ReadRequest == 31);
    REQUIRE(chromatindb::wire::TransportMsgType_ReadResponse == 32);
    REQUIRE(chromatindb::wire::TransportMsgType_BatchReadRequest == 53);
    REQUIRE(chromatindb::wire::TransportMsgType_BatchReadResponse == 54);
}

// ===========================================================================
// Hex utility roundtrip (confirms hex parsing used by path extraction)
// ===========================================================================

TEST_CASE("hex roundtrip for 32-byte values", "[http][data][hex]") {
    std::vector<uint8_t> original(32);
    for (size_t i = 0; i < 32; ++i) {
        original[i] = static_cast<uint8_t>(i * 8);
    }

    auto hex = to_hex(original);
    REQUIRE(hex.size() == 64);

    auto decoded = from_hex(hex);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == original);
}

// ===========================================================================
// HttpResponse factory methods used by handlers
// ===========================================================================

TEST_CASE("HttpResponse::binary produces application/octet-stream", "[http][data][response]") {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
    auto resp = HttpResponse::binary(200, data);
    REQUIRE(resp.status == 200);
    REQUIRE(resp.body == data);

    bool found_ct = false;
    for (const auto& [key, val] : resp.headers) {
        if (key == "Content-Type") {
            REQUIRE(val == "application/octet-stream");
            found_ct = true;
        }
    }
    REQUIRE(found_ct);
}

TEST_CASE("HttpResponse::error 413 produces correct status", "[http][data][response]") {
    auto resp = HttpResponse::error(413, "payload_too_large", "too big");
    REQUIRE(resp.status == 413);

    auto body_str = std::string(resp.body.begin(), resp.body.end());
    REQUIRE(body_str.find("payload_too_large") != std::string::npos);
}

TEST_CASE("HttpResponse::not_found produces 404", "[http][data][response]") {
    auto resp = HttpResponse::not_found();
    REQUIRE(resp.status == 404);
}

TEST_CASE("HttpResponse::error 504 for timeout", "[http][data][response]") {
    auto resp = HttpResponse::error(504, "timeout", "node response timeout");
    REQUIRE(resp.status == 504);
}
