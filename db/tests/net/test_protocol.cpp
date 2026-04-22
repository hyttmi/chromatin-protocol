#include <catch2/catch_test_macros.hpp>
#include "db/net/protocol.h"

using namespace chromatindb::net;
using namespace chromatindb::wire;

TEST_CASE("TransportCodec encode/decode round-trip", "[protocol]") {
    SECTION("KemPubkey round-trips") {
        std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
        auto encoded = TransportCodec::encode(TransportMsgType_KemPubkey, payload);
        REQUIRE(!encoded.empty());

        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_KemPubkey);
        REQUIRE(decoded->payload == payload);
    }

    SECTION("KemCiphertext round-trips") {
        std::vector<uint8_t> payload(1568, 0xAB); // KEM ciphertext size
        auto encoded = TransportCodec::encode(TransportMsgType_KemCiphertext, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_KemCiphertext);
        REQUIRE(decoded->payload == payload);
    }

    SECTION("AuthSignature round-trips") {
        std::vector<uint8_t> payload(4627, 0xCD); // max signature size
        auto encoded = TransportCodec::encode(TransportMsgType_AuthSignature, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_AuthSignature);
        REQUIRE(decoded->payload == payload);
    }

    SECTION("AuthPubkey round-trips") {
        std::vector<uint8_t> payload(2592, 0xEF); // ML-DSA-87 pubkey size
        auto encoded = TransportCodec::encode(TransportMsgType_AuthPubkey, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_AuthPubkey);
        REQUIRE(decoded->payload == payload);
    }

    SECTION("Ping round-trips with empty payload") {
        std::span<const uint8_t> empty{};
        auto encoded = TransportCodec::encode(TransportMsgType_Ping, empty);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_Ping);
        REQUIRE(decoded->payload.empty());
    }

    SECTION("Pong round-trips with empty payload") {
        std::span<const uint8_t> empty{};
        auto encoded = TransportCodec::encode(TransportMsgType_Pong, empty);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_Pong);
    }

    SECTION("Goodbye round-trips") {
        std::span<const uint8_t> empty{};
        auto encoded = TransportCodec::encode(TransportMsgType_Goodbye, empty);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_Goodbye);
    }

    SECTION("BlobWrite round-trips with large payload") {
        std::vector<uint8_t> payload(65536, 0x42); // 64KB
        auto encoded = TransportCodec::encode(TransportMsgType_BlobWrite, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_BlobWrite);
        REQUIRE(decoded->payload == payload);
    }
}

TEST_CASE("TransportCodec new message types round-trip", "[protocol]") {
    SECTION("WriteAck round-trips") {
        // 41 bytes: [blob_hash:32][seq_num_be:8][status:1]
        std::vector<uint8_t> payload(41, 0x00);
        payload[0] = 0xAA;   // first byte of hash
        payload[31] = 0xBB;  // last byte of hash
        payload[32] = 0x00; payload[33] = 0x00; payload[34] = 0x00; payload[35] = 0x00;
        payload[36] = 0x00; payload[37] = 0x00; payload[38] = 0x00; payload[39] = 0x2A; // seq=42
        payload[40] = 0x00;  // status=stored
        auto encoded = TransportCodec::encode(TransportMsgType_WriteAck, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_WriteAck);
        REQUIRE(decoded->payload == payload);
    }

    SECTION("ReadRequest round-trips") {
        // 64 bytes: [namespace_id:32][blob_hash:32]
        std::vector<uint8_t> payload(64, 0xCC);
        auto encoded = TransportCodec::encode(TransportMsgType_ReadRequest, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_ReadRequest);
        REQUIRE(decoded->payload == payload);
    }

    SECTION("ReadResponse round-trips with found blob") {
        // [found:1=0x01][flatbuffer_blob:variable]
        std::vector<uint8_t> payload = {0x01};
        // Append some fake blob bytes
        for (int i = 0; i < 100; ++i) payload.push_back(static_cast<uint8_t>(i));
        auto encoded = TransportCodec::encode(TransportMsgType_ReadResponse, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_ReadResponse);
        REQUIRE(decoded->payload == payload);
    }

    SECTION("ReadResponse round-trips with not-found") {
        std::vector<uint8_t> payload = {0x00};
        auto encoded = TransportCodec::encode(TransportMsgType_ReadResponse, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_ReadResponse);
        REQUIRE(decoded->payload == payload);
    }

    SECTION("ListRequest round-trips") {
        // 44 bytes: [namespace_id:32][since_seq_be:8][limit_be:4]
        std::vector<uint8_t> payload(44, 0xDD);
        auto encoded = TransportCodec::encode(TransportMsgType_ListRequest, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_ListRequest);
        REQUIRE(decoded->payload == payload);
    }

    SECTION("ListResponse round-trips") {
        // [count_be:4][{hash:32, seq_be:8}*2][has_more:1]
        // count=2, 2 entries of 40 bytes each, has_more=1
        std::vector<uint8_t> payload(4 + 2 * 40 + 1, 0xEE);
        payload[0] = 0; payload[1] = 0; payload[2] = 0; payload[3] = 2;  // count=2
        payload.back() = 1;  // has_more=true
        auto encoded = TransportCodec::encode(TransportMsgType_ListResponse, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_ListResponse);
        REQUIRE(decoded->payload == payload);
    }

    SECTION("StatsRequest round-trips") {
        // 32 bytes: [namespace_id:32]
        std::vector<uint8_t> payload(32, 0xFF);
        auto encoded = TransportCodec::encode(TransportMsgType_StatsRequest, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_StatsRequest);
        REQUIRE(decoded->payload == payload);
    }

    SECTION("StatsResponse round-trips") {
        // 24 bytes: [blob_count_be:8][total_bytes_be:8][quota_bytes_be:8]
        std::vector<uint8_t> payload(24, 0x00);
        payload[7] = 42;   // blob_count=42
        payload[15] = 100;  // total_bytes=100
        payload[23] = 0;    // quota_bytes=0 (unlimited)
        auto encoded = TransportCodec::encode(TransportMsgType_StatsResponse, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_StatsResponse);
        REQUIRE(decoded->payload == payload);
    }
}

TEST_CASE("Client protocol payload formats", "[protocol]") {
    SECTION("ReadRequest payload is 64 bytes") {
        std::vector<uint8_t> payload(64);
        std::fill(payload.begin(), payload.begin() + 32, 0xAA);
        std::fill(payload.begin() + 32, payload.end(), 0xBB);
        auto encoded = TransportCodec::encode(TransportMsgType_ReadRequest, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->payload.size() == 64);
        REQUIRE(decoded->payload[0] == 0xAA);
        REQUIRE(decoded->payload[31] == 0xAA);
        REQUIRE(decoded->payload[32] == 0xBB);
        REQUIRE(decoded->payload[63] == 0xBB);
    }

    SECTION("ListRequest payload encodes since_seq and limit in big-endian") {
        std::vector<uint8_t> payload(44, 0x00);
        std::fill(payload.begin(), payload.begin() + 32, 0x11);
        // since_seq = 256 (0x0000000000000100)
        payload[38] = 0x01; payload[39] = 0x00;
        // limit = 50 (0x00000032)
        payload[43] = 0x32;
        auto encoded = TransportCodec::encode(TransportMsgType_ListRequest, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->payload.size() == 44);
        uint64_t since_seq = 0;
        for (int i = 0; i < 8; ++i)
            since_seq = (since_seq << 8) | decoded->payload[32 + i];
        REQUIRE(since_seq == 256);
        uint32_t limit = 0;
        for (int i = 0; i < 4; ++i)
            limit = (limit << 8) | decoded->payload[40 + i];
        REQUIRE(limit == 50);
    }

    SECTION("ListResponse payload with zero entries") {
        std::vector<uint8_t> payload = {0x00, 0x00, 0x00, 0x00, 0x00};
        auto encoded = TransportCodec::encode(TransportMsgType_ListResponse, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->payload.size() == 5);
        uint32_t count = 0;
        for (int i = 0; i < 4; ++i)
            count = (count << 8) | decoded->payload[i];
        REQUIRE(count == 0);
        REQUIRE(decoded->payload[4] == 0);
    }

    SECTION("StatsResponse payload encodes three uint64 big-endian values") {
        std::vector<uint8_t> payload(24, 0x00);
        // blob_count = 1000 (0x3E8) -- big-endian uint64 at offset 0
        payload[6] = 0x03; payload[7] = 0xE8;
        // total_bytes = 1048576 (0x100000) -- big-endian uint64 at offset 8
        payload[13] = 0x10;
        // quota_bytes = 0 (unlimited)
        auto encoded = TransportCodec::encode(TransportMsgType_StatsResponse, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->payload.size() == 24);
        uint64_t blob_count = 0;
        for (int i = 0; i < 8; ++i)
            blob_count = (blob_count << 8) | decoded->payload[i];
        REQUIRE(blob_count == 1000);
        uint64_t total_bytes = 0;
        for (int i = 0; i < 8; ++i)
            total_bytes = (total_bytes << 8) | decoded->payload[8 + i];
        REQUIRE(total_bytes == 1048576);
        uint64_t quota_bytes = 0;
        for (int i = 0; i < 8; ++i)
            quota_bytes = (quota_bytes << 8) | decoded->payload[16 + i];
        REQUIRE(quota_bytes == 0);
    }
}

TEST_CASE("TransportCodec decode rejects corrupt data", "[protocol]") {
    SECTION("empty buffer") {
        std::span<const uint8_t> empty{};
        auto decoded = TransportCodec::decode(empty);
        REQUIRE_FALSE(decoded.has_value());
    }

    SECTION("garbage bytes") {
        std::vector<uint8_t> garbage = {0xFF, 0xFE, 0xFD, 0xFC};
        auto decoded = TransportCodec::decode(garbage);
        REQUIRE_FALSE(decoded.has_value());
    }

    SECTION("truncated buffer") {
        std::vector<uint8_t> payload = {1, 2, 3};
        auto encoded = TransportCodec::encode(TransportMsgType_BlobWrite, payload);
        // Truncate to half
        encoded.resize(encoded.size() / 2);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE_FALSE(decoded.has_value());
    }
}

TEST_CASE("TransportCodec preserves request_id", "[protocol]") {
    SECTION("non-zero request_id round-trips") {
        std::vector<uint8_t> payload = {1, 2, 3};
        auto encoded = TransportCodec::encode(TransportMsgType_Ping, payload, 42);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_Ping);
        REQUIRE(decoded->payload == payload);
        REQUIRE(decoded->request_id == 42);
    }

    SECTION("max uint32 request_id round-trips") {
        std::vector<uint8_t> payload = {1, 2, 3};
        auto encoded = TransportCodec::encode(TransportMsgType_Ping, payload, 0xFFFFFFFF);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_Ping);
        REQUIRE(decoded->payload == payload);
        REQUIRE(decoded->request_id == 0xFFFFFFFF);
    }

    SECTION("default request_id is 0") {
        std::vector<uint8_t> payload = {1, 2, 3};
        auto encoded = TransportCodec::encode(TransportMsgType_Ping, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_Ping);
        REQUIRE(decoded->payload == payload);
        REQUIRE(decoded->request_id == 0);
    }
}
