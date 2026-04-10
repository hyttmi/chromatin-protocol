#include <catch2/catch_test_macros.hpp>

#include "relay/wire/transport_codec.h"

#include <cstdint>
#include <vector>

using chromatindb::relay::wire::TransportCodec;
using namespace chromatindb::wire;

TEST_CASE("TransportCodec roundtrip Ping", "[transport_codec]") {
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    auto encoded = TransportCodec::encode(TransportMsgType_Ping, payload, 42);

    auto decoded = TransportCodec::decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->type == TransportMsgType_Ping);
    REQUIRE(decoded->payload == payload);
    REQUIRE(decoded->request_id == 42);
}

TEST_CASE("TransportCodec roundtrip ReadRequest", "[transport_codec]") {
    // Simulate a 64-byte payload (namespace + hash)
    std::vector<uint8_t> payload(64, 0xAB);
    auto encoded = TransportCodec::encode(TransportMsgType_ReadRequest, payload, 100);

    auto decoded = TransportCodec::decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->type == TransportMsgType_ReadRequest);
    REQUIRE(decoded->payload == payload);
    REQUIRE(decoded->request_id == 100);
}

TEST_CASE("TransportCodec roundtrip Data", "[transport_codec]") {
    std::vector<uint8_t> payload(256, 0xCD);
    auto encoded = TransportCodec::encode(TransportMsgType_Data, payload, 7);

    auto decoded = TransportCodec::decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->type == TransportMsgType_Data);
    REQUIRE(decoded->payload == payload);
    REQUIRE(decoded->request_id == 7);
}

TEST_CASE("TransportCodec request_id preservation", "[transport_codec]") {
    std::vector<uint8_t> payload;
    auto encoded = TransportCodec::encode(TransportMsgType_Ping, payload, 0xDEADBEEF);

    auto decoded = TransportCodec::decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->request_id == 0xDEADBEEF);
}

TEST_CASE("TransportCodec empty payload roundtrip", "[transport_codec]") {
    std::vector<uint8_t> payload;
    auto encoded = TransportCodec::encode(TransportMsgType_Pong, payload, 0);

    auto decoded = TransportCodec::decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->type == TransportMsgType_Pong);
    REQUIRE(decoded->payload.empty());
    REQUIRE(decoded->request_id == 0);
}

TEST_CASE("TransportCodec invalid buffer returns nullopt", "[transport_codec]") {
    std::vector<uint8_t> garbage = {0xFF, 0xFE, 0xFD, 0xFC};
    auto decoded = TransportCodec::decode(garbage);
    REQUIRE(!decoded.has_value());
}

TEST_CASE("TransportCodec large payload roundtrip", "[transport_codec]") {
    std::vector<uint8_t> payload(100000);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto encoded = TransportCodec::encode(TransportMsgType_Data, payload, 999);

    auto decoded = TransportCodec::decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->payload == payload);
    REQUIRE(decoded->request_id == 999);
}

TEST_CASE("TransportCodec request_id zero default", "[transport_codec]") {
    std::vector<uint8_t> payload = {0x01};
    auto encoded = TransportCodec::encode(TransportMsgType_Ping, payload);

    auto decoded = TransportCodec::decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->request_id == 0);
}
