#include "db/net/auth_helpers.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

using namespace chromatindb::net;

TEST_CASE("encode_auth_payload round-trips through decode_auth_payload",
          "[auth_helpers]") {
    std::vector<uint8_t> pubkey(64, 0xAA);
    std::vector<uint8_t> signature(128, 0xBB);

    auto encoded = encode_auth_payload(pubkey, signature);
    auto decoded = decode_auth_payload(encoded);

    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pubkey == pubkey);
    REQUIRE(decoded->signature == signature);
}

TEST_CASE("encode_auth_payload produces LE pubkey_size + pubkey + signature layout",
          "[auth_helpers]") {
    std::vector<uint8_t> pubkey(10, 0x01);
    std::vector<uint8_t> signature(20, 0x02);

    auto encoded = encode_auth_payload(pubkey, signature);

    // Total: 4 (size) + 10 (pubkey) + 20 (signature) = 34
    REQUIRE(encoded.size() == 34);

    // First 4 bytes: LE encoding of 10 (0x0A, 0x00, 0x00, 0x00)
    REQUIRE(encoded[0] == 0x0A);
    REQUIRE(encoded[1] == 0x00);
    REQUIRE(encoded[2] == 0x00);
    REQUIRE(encoded[3] == 0x00);

    // Bytes 4-13: pubkey
    for (size_t i = 0; i < 10; ++i) {
        REQUIRE(encoded[4 + i] == 0x01);
    }

    // Bytes 14-33: signature
    for (size_t i = 0; i < 20; ++i) {
        REQUIRE(encoded[14 + i] == 0x02);
    }
}

TEST_CASE("encode_auth_payload with ML-DSA-87 key size encodes correctly",
          "[auth_helpers]") {
    // ML-DSA-87 public key is 2592 bytes
    std::vector<uint8_t> pubkey(2592, 0xCC);
    std::vector<uint8_t> signature(4627, 0xDD);

    auto encoded = encode_auth_payload(pubkey, signature);

    REQUIRE(encoded.size() == 4 + 2592 + 4627);

    // 2592 = 0x00000A20, LE = [0x20, 0x0A, 0x00, 0x00]
    REQUIRE(encoded[0] == 0x20);
    REQUIRE(encoded[1] == 0x0A);
    REQUIRE(encoded[2] == 0x00);
    REQUIRE(encoded[3] == 0x00);

    auto decoded = decode_auth_payload(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pubkey.size() == 2592);
    REQUIRE(decoded->signature.size() == 4627);
}

TEST_CASE("decode_auth_payload with empty input returns nullopt",
          "[auth_helpers]") {
    std::span<const uint8_t> empty{};
    auto result = decode_auth_payload(empty);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("decode_auth_payload with only 3 bytes returns nullopt",
          "[auth_helpers]") {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    auto result = decode_auth_payload(data);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("decode_auth_payload with pubkey_size larger than remaining data returns nullopt",
          "[auth_helpers]") {
    // Claim pubkey_size = 100 (LE: 0x64, 0x00, 0x00, 0x00) but only 5 bytes after header
    std::vector<uint8_t> data = {0x64, 0x00, 0x00, 0x00,
                                  0x01, 0x02, 0x03, 0x04, 0x05};
    auto result = decode_auth_payload(data);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("decode_auth_payload with pubkey_size=0 returns empty pubkey",
          "[auth_helpers]") {
    // pubkey_size=0 (LE: 0x00, 0x00, 0x00, 0x00), rest is signature
    std::vector<uint8_t> data = {0x00, 0x00, 0x00, 0x00,
                                  0xAA, 0xBB, 0xCC};
    auto result = decode_auth_payload(data);
    REQUIRE(result.has_value());
    REQUIRE(result->pubkey.empty());
    REQUIRE(result->signature.size() == 3);
    REQUIRE(result->signature[0] == 0xAA);
    REQUIRE(result->signature[1] == 0xBB);
    REQUIRE(result->signature[2] == 0xCC);
}

TEST_CASE("encode_auth_payload byte-level LE verification",
          "[auth_helpers]") {
    // pubkey size = 2592 = 0x00000A20
    // LE encoding: [0x20, 0x0A, 0x00, 0x00]
    std::vector<uint8_t> pubkey(2592, 0xFF);
    std::vector<uint8_t> signature(100, 0xEE);

    auto encoded = encode_auth_payload(pubkey, signature);

    uint32_t pk_size = static_cast<uint32_t>(pubkey.size());
    REQUIRE(encoded[0] == static_cast<uint8_t>(pk_size & 0xFF));
    REQUIRE(encoded[1] == static_cast<uint8_t>((pk_size >> 8) & 0xFF));
    REQUIRE(encoded[2] == static_cast<uint8_t>((pk_size >> 16) & 0xFF));
    REQUIRE(encoded[3] == static_cast<uint8_t>((pk_size >> 24) & 0xFF));
}
