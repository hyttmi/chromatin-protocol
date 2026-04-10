#include <catch2/catch_test_macros.hpp>

#include "relay/util/base64.h"

#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

using chromatindb::relay::util::base64_decode;
using chromatindb::relay::util::base64_encode;

TEST_CASE("base64 roundtrip empty data", "[base64]") {
    std::vector<uint8_t> empty;
    auto encoded = base64_encode(empty);
    REQUIRE(encoded.empty());

    auto decoded = base64_decode("");
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->empty());
}

TEST_CASE("base64 roundtrip known string", "[base64]") {
    // "Hello" -> "SGVsbG8="
    std::string input = "Hello";
    std::vector<uint8_t> data(input.begin(), input.end());

    auto encoded = base64_encode(data);
    REQUIRE(encoded == "SGVsbG8=");

    auto decoded = base64_decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == data);
}

TEST_CASE("base64 roundtrip binary data with all byte values", "[base64]") {
    std::vector<uint8_t> data(256);
    std::iota(data.begin(), data.end(), static_cast<uint8_t>(0));

    auto encoded = base64_encode(data);
    REQUIRE(!encoded.empty());

    auto decoded = base64_decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == data);
}

TEST_CASE("base64 decode invalid input returns nullopt", "[base64]") {
    // Invalid character
    auto result = base64_decode("SGVs!G8=");
    REQUIRE(!result.has_value());

    // Not a multiple of 4
    result = base64_decode("SGVsbG8");
    REQUIRE(!result.has_value());
}

TEST_CASE("base64 roundtrip large data", "[base64]") {
    // 1 MiB of data
    std::vector<uint8_t> data(1024 * 1024);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto encoded = base64_encode(data);
    REQUIRE(!encoded.empty());

    auto decoded = base64_decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == data);
}

TEST_CASE("base64 known RFC 4648 test vectors", "[base64]") {
    auto check = [](std::string_view input, std::string_view expected) {
        std::vector<uint8_t> data(input.begin(), input.end());
        auto encoded = base64_encode(data);
        REQUIRE(encoded == expected);

        auto decoded = base64_decode(encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(*decoded == data);
    };

    check("f", "Zg==");
    check("fo", "Zm8=");
    check("foo", "Zm9v");
    check("foob", "Zm9vYg==");
    check("fooba", "Zm9vYmE=");
    check("foobar", "Zm9vYmFy");
}
