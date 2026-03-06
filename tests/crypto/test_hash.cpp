#include <catch2/catch_test_macros.hpp>
#include "db/crypto/hash.h"
#include "db/crypto/secure_bytes.h"
#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace chromatin::crypto;

TEST_CASE("SHA3-256 of empty input produces known digest", "[hash]") {
    // Known SHA3-256("") = a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a
    std::span<const uint8_t> empty{};
    auto hash = sha3_256(empty);

    REQUIRE(hash.size() == 32);

    // Verify against known test vector
    const uint8_t expected[] = {
        0xa7, 0xff, 0xc6, 0xf8, 0xbf, 0x1e, 0xd7, 0x66,
        0x51, 0xc1, 0x47, 0x56, 0xa0, 0x61, 0xd6, 0x62,
        0xf5, 0x80, 0xff, 0x4d, 0xe4, 0x3b, 0x49, 0xfa,
        0x82, 0xd8, 0x0a, 0x4b, 0x80, 0xf8, 0x43, 0x4a
    };
    REQUIRE(std::memcmp(hash.data(), expected, 32) == 0);
}

TEST_CASE("SHA3-256 of known string produces deterministic hash", "[hash]") {
    std::string input = "chromatindb";
    auto hash1 = sha3_256(input.data(), input.size());
    auto hash2 = sha3_256(input.data(), input.size());

    REQUIRE(hash1.size() == 32);
    REQUIRE(hash1 == hash2);  // Deterministic
}

TEST_CASE("SHA3-256 same input always produces same hash", "[hash]") {
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    auto h1 = sha3_256(std::span<const uint8_t>(data));
    auto h2 = sha3_256(std::span<const uint8_t>(data));
    REQUIRE(h1 == h2);
}

TEST_CASE("SHA3-256 different inputs produce different hashes", "[hash]") {
    std::vector<uint8_t> data1 = {1, 2, 3};
    std::vector<uint8_t> data2 = {1, 2, 4};
    auto h1 = sha3_256(std::span<const uint8_t>(data1));
    auto h2 = sha3_256(std::span<const uint8_t>(data2));
    REQUIRE(h1 != h2);
}

TEST_CASE("SHA3-256 pointer overload matches span overload", "[hash]") {
    std::vector<uint8_t> data = {10, 20, 30, 40, 50};
    auto h1 = sha3_256(std::span<const uint8_t>(data));
    auto h2 = sha3_256(data.data(), data.size());
    REQUIRE(h1 == h2);
}

// SecureBytes tests

TEST_CASE("SecureBytes default construction is empty", "[secure_bytes]") {
    SecureBytes sb;
    REQUIRE(sb.empty());
    REQUIRE(sb.size() == 0);
    REQUIRE(sb.data() == nullptr);
}

TEST_CASE("SecureBytes sized construction", "[secure_bytes]") {
    SecureBytes sb(32);
    REQUIRE(sb.size() == 32);
    REQUIRE(sb.data() != nullptr);
    // Should be zero-initialized
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(sb[i] == 0);
    }
}

TEST_CASE("SecureBytes from data", "[secure_bytes]") {
    uint8_t src[] = {0xDE, 0xAD, 0xBE, 0xEF};
    SecureBytes sb(src, 4);
    REQUIRE(sb.size() == 4);
    REQUIRE(sb[0] == 0xDE);
    REQUIRE(sb[1] == 0xAD);
    REQUIRE(sb[2] == 0xBE);
    REQUIRE(sb[3] == 0xEF);
}

TEST_CASE("SecureBytes move semantics", "[secure_bytes]") {
    SecureBytes sb1(16);
    sb1[0] = 0x42;
    auto* ptr = sb1.data();

    SecureBytes sb2(std::move(sb1));
    REQUIRE(sb2.data() == ptr);
    REQUIRE(sb2[0] == 0x42);
    REQUIRE(sb2.size() == 16);

    // Moved-from is empty
    REQUIRE(sb1.empty());  // NOLINT(bugprone-use-after-move)
    REQUIRE(sb1.data() == nullptr);
}

TEST_CASE("SecureBytes move assignment", "[secure_bytes]") {
    SecureBytes sb1(8);
    sb1[0] = 0xFF;

    SecureBytes sb2;
    sb2 = std::move(sb1);
    REQUIRE(sb2.size() == 8);
    REQUIRE(sb2[0] == 0xFF);
    REQUIRE(sb1.empty());  // NOLINT(bugprone-use-after-move)
}

TEST_CASE("SecureBytes equality comparison", "[secure_bytes]") {
    uint8_t data[] = {1, 2, 3, 4};
    SecureBytes sb1(data, 4);
    SecureBytes sb2(data, 4);
    REQUIRE(sb1 == sb2);

    uint8_t data2[] = {1, 2, 3, 5};
    SecureBytes sb3(data2, 4);
    REQUIRE_FALSE(sb1 == sb3);
}

TEST_CASE("SecureBytes span access", "[secure_bytes]") {
    uint8_t data[] = {10, 20, 30};
    SecureBytes sb(data, 3);
    auto sp = sb.span();
    REQUIRE(sp.size() == 3);
    REQUIRE(sp[0] == 10);
    REQUIRE(sp[1] == 20);
    REQUIRE(sp[2] == 30);
}
