#include <catch2/catch_test_macros.hpp>
#include "net/protocol.h"

using namespace chromatin::net;
using namespace chromatin::wire;

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
