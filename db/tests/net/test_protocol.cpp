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

    SECTION("Data round-trips with large payload") {
        std::vector<uint8_t> payload(65536, 0x42); // 64KB
        auto encoded = TransportCodec::encode(TransportMsgType_Data, payload);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->type == TransportMsgType_Data);
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
        auto encoded = TransportCodec::encode(TransportMsgType_Data, payload);
        // Truncate to half
        encoded.resize(encoded.size() / 2);
        auto decoded = TransportCodec::decode(encoded);
        REQUIRE_FALSE(decoded.has_value());
    }
}
