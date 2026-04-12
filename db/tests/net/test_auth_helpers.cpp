#include "db/net/auth_helpers.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

using namespace chromatindb::net;

TEST_CASE("encode_auth_payload round-trips through decode_auth_payload",
          "[auth_helpers]") {
    std::vector<uint8_t> pubkey(2592, 0xAA);  // Must be Signer::PUBLIC_KEY_SIZE
    std::vector<uint8_t> signature(128, 0xBB);

    auto encoded = encode_auth_payload(pubkey, signature);
    auto decoded = decode_auth_payload(encoded);

    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pubkey == pubkey);
    REQUIRE(decoded->signature == signature);
}

TEST_CASE("encode_auth_payload produces BE pubkey_size + pubkey + signature layout",
          "[auth_helpers]") {
    std::vector<uint8_t> pubkey(10, 0x01);
    std::vector<uint8_t> signature(20, 0x02);

    auto encoded = encode_auth_payload(pubkey, signature);

    // Total: 4 (size) + 10 (pubkey) + 20 (signature) = 34
    REQUIRE(encoded.size() == 34);

    // First 4 bytes: BE encoding of 10 (0x00, 0x00, 0x00, 0x0A)
    REQUIRE(encoded[0] == 0x00);
    REQUIRE(encoded[1] == 0x00);
    REQUIRE(encoded[2] == 0x00);
    REQUIRE(encoded[3] == 0x0A);

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

    // 2592 = 0x00000A20, BE = [0x00, 0x00, 0x0A, 0x20]
    REQUIRE(encoded[0] == 0x00);
    REQUIRE(encoded[1] == 0x00);
    REQUIRE(encoded[2] == 0x0A);
    REQUIRE(encoded[3] == 0x20);

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
    // Claim pubkey_size = 100 (BE: 0x00, 0x00, 0x00, 0x64) but only 5 bytes after header
    std::vector<uint8_t> data = {0x00, 0x00, 0x00, 0x64,
                                  0x01, 0x02, 0x03, 0x04, 0x05};
    auto result = decode_auth_payload(data);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("decode_auth_payload with pubkey_size=0 returns nullopt (wrong size)",
          "[auth_helpers]") {
    // pubkey_size=0 != Signer::PUBLIC_KEY_SIZE (2592), rejected by PROTO-02
    std::vector<uint8_t> data = {0x00, 0x00, 0x00, 0x00,
                                  0xAA, 0xBB, 0xCC};
    auto result = decode_auth_payload(data);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("decode_auth_payload rejects wrong pubkey size", "[auth_helpers]") {
    // Encode with a 64-byte pubkey (not 2592)
    std::vector<uint8_t> pubkey(64, 0xAA);
    std::vector<uint8_t> signature(128, 0xBB);
    auto encoded = encode_auth_payload(pubkey, signature);
    auto decoded = decode_auth_payload(encoded);
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE("decode_auth_payload accepts correct ML-DSA-87 pubkey size", "[auth_helpers]") {
    std::vector<uint8_t> pubkey(2592, 0xAA);  // Signer::PUBLIC_KEY_SIZE
    std::vector<uint8_t> signature(4627, 0xBB);
    auto encoded = encode_auth_payload(pubkey, signature);
    auto decoded = decode_auth_payload(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->pubkey.size() == 2592);
}

TEST_CASE("encode_auth_payload byte-level BE verification",
          "[auth_helpers]") {
    // pubkey size = 2592 = 0x00000A20
    // BE encoding: [0x00, 0x00, 0x0A, 0x20]
    std::vector<uint8_t> pubkey(2592, 0xFF);
    std::vector<uint8_t> signature(100, 0xEE);

    auto encoded = encode_auth_payload(pubkey, signature);

    uint32_t pk_size = static_cast<uint32_t>(pubkey.size());
    REQUIRE(encoded[0] == static_cast<uint8_t>((pk_size >> 24) & 0xFF));
    REQUIRE(encoded[1] == static_cast<uint8_t>((pk_size >> 16) & 0xFF));
    REQUIRE(encoded[2] == static_cast<uint8_t>((pk_size >> 8) & 0xFF));
    REQUIRE(encoded[3] == static_cast<uint8_t>(pk_size & 0xFF));
}
