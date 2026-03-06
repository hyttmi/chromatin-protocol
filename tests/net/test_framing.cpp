#include <catch2/catch_test_macros.hpp>
#include "db/net/framing.h"
#include "db/net/protocol.h"
#include "db/crypto/aead.h"

using namespace chromatindb::net;
using namespace chromatindb::crypto;
using namespace chromatindb::wire;

TEST_CASE("make_nonce produces correct nonces", "[framing]") {
    SECTION("counter 0 produces all zeros") {
        auto nonce = make_nonce(0);
        REQUIRE(nonce.size() == AEAD::NONCE_SIZE);
        for (size_t i = 0; i < nonce.size(); ++i) {
            REQUIRE(nonce[i] == 0);
        }
    }

    SECTION("counter 1 produces correct encoding") {
        auto nonce = make_nonce(1);
        // First 4 bytes: zero
        for (int i = 0; i < 4; ++i) {
            REQUIRE(nonce[i] == 0);
        }
        // Last 8 bytes: big-endian 1
        for (int i = 4; i < 11; ++i) {
            REQUIRE(nonce[i] == 0);
        }
        REQUIRE(nonce[11] == 1);
    }

    SECTION("counter 256 encodes correctly") {
        auto nonce = make_nonce(256);
        REQUIRE(nonce[10] == 1);
        REQUIRE(nonce[11] == 0);
    }

    SECTION("UINT64_MAX encodes correctly") {
        auto nonce = make_nonce(UINT64_MAX);
        for (int i = 4; i < 12; ++i) {
            REQUIRE(nonce[i] == 0xFF);
        }
    }

    SECTION("sequential nonces are unique") {
        auto n0 = make_nonce(0);
        auto n1 = make_nonce(1);
        auto n2 = make_nonce(2);
        REQUIRE(n0 != n1);
        REQUIRE(n1 != n2);
        REQUIRE(n0 != n2);
    }
}

TEST_CASE("write_frame produces correct format", "[framing]") {
    auto key = AEAD::keygen();

    SECTION("frame has 4-byte header + ciphertext") {
        std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03, 0x04};
        auto frame = write_frame(plaintext, key.span(), 0);

        // Expected: 4 header + (4 plaintext + 16 tag) = 24 bytes
        REQUIRE(frame.size() == FRAME_HEADER_SIZE + plaintext.size() + AEAD::TAG_SIZE);
    }

    SECTION("length prefix is big-endian ciphertext length") {
        std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03};
        auto frame = write_frame(plaintext, key.span(), 0);

        uint32_t expected_ct_len = static_cast<uint32_t>(plaintext.size() + AEAD::TAG_SIZE);
        uint32_t actual_len = (static_cast<uint32_t>(frame[0]) << 24) |
                              (static_cast<uint32_t>(frame[1]) << 16) |
                              (static_cast<uint32_t>(frame[2]) << 8) |
                              static_cast<uint32_t>(frame[3]);
        REQUIRE(actual_len == expected_ct_len);
    }

    SECTION("empty plaintext produces valid frame") {
        std::span<const uint8_t> empty{};
        auto frame = write_frame(empty, key.span(), 0);
        // 4 header + 16 tag (ciphertext of empty plaintext is just the tag)
        REQUIRE(frame.size() == FRAME_HEADER_SIZE + AEAD::TAG_SIZE);
    }
}

TEST_CASE("read_frame round-trip with write_frame", "[framing]") {
    auto key = AEAD::keygen();

    SECTION("basic round-trip") {
        std::vector<uint8_t> plaintext = {0xDE, 0xAD, 0xBE, 0xEF};
        auto frame = write_frame(plaintext, key.span(), 0);
        auto result = read_frame(frame, key.span(), 0);

        REQUIRE(result.has_value());
        REQUIRE(result->plaintext == plaintext);
        REQUIRE(result->bytes_consumed == frame.size());
    }

    SECTION("empty plaintext round-trip") {
        std::span<const uint8_t> empty{};
        auto frame = write_frame(empty, key.span(), 0);
        auto result = read_frame(frame, key.span(), 0);

        REQUIRE(result.has_value());
        REQUIRE(result->plaintext.empty());
    }

    SECTION("large payload round-trip") {
        std::vector<uint8_t> plaintext(8192, 0x42);
        auto frame = write_frame(plaintext, key.span(), 0);
        auto result = read_frame(frame, key.span(), 0);

        REQUIRE(result.has_value());
        REQUIRE(result->plaintext == plaintext);
    }

    SECTION("sequential counters round-trip") {
        for (uint64_t i = 0; i < 10; ++i) {
            std::vector<uint8_t> plaintext = {static_cast<uint8_t>(i)};
            auto frame = write_frame(plaintext, key.span(), i);
            auto result = read_frame(frame, key.span(), i);

            REQUIRE(result.has_value());
            REQUIRE(result->plaintext == plaintext);
        }
    }
}

TEST_CASE("read_frame rejects invalid frames", "[framing]") {
    auto key = AEAD::keygen();

    SECTION("wrong key fails decrypt") {
        auto other_key = AEAD::keygen();
        std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03};
        auto frame = write_frame(plaintext, key.span(), 0);

        auto result = read_frame(frame, other_key.span(), 0);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("wrong counter (nonce desync) fails decrypt") {
        std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03};
        auto frame = write_frame(plaintext, key.span(), 0);

        auto result = read_frame(frame, key.span(), 1); // wrong counter
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("frame exceeding MAX_FRAME_SIZE throws") {
        // Craft a frame header claiming > 16MB
        std::vector<uint8_t> bad_frame = {0x01, 0x00, 0x00, 0x01}; // 16MB + 1
        REQUIRE_THROWS_AS(read_frame(bad_frame, key.span(), 0), std::runtime_error);
    }

    SECTION("buffer too short for header throws") {
        std::vector<uint8_t> short_buf = {0x00, 0x00};
        REQUIRE_THROWS_AS(read_frame(short_buf, key.span(), 0), std::runtime_error);
    }

    SECTION("buffer too short for declared frame throws") {
        // Header says 100 bytes but buffer is only header + 10 bytes
        std::vector<uint8_t> bad_frame = {0x00, 0x00, 0x00, 0x64}; // 100 bytes
        bad_frame.resize(14); // only 10 bytes of payload
        REQUIRE_THROWS_AS(read_frame(bad_frame, key.span(), 0), std::runtime_error);
    }

    SECTION("tampered ciphertext fails decrypt") {
        std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03};
        auto frame = write_frame(plaintext, key.span(), 0);

        // Flip a byte in the ciphertext portion
        frame[FRAME_HEADER_SIZE + 2] ^= 0xFF;

        auto result = read_frame(frame, key.span(), 0);
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("Integration: protocol encode -> frame -> read -> decode", "[framing][protocol]") {
    auto key = AEAD::keygen();

    std::vector<uint8_t> payload = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"
    auto encoded = TransportCodec::encode(TransportMsgType_Data, payload);

    // Write as encrypted frame
    auto frame = write_frame(encoded, key.span(), 0);

    // Read back
    auto result = read_frame(frame, key.span(), 0);
    REQUIRE(result.has_value());

    // Decode the transport message
    auto decoded = TransportCodec::decode(result->plaintext);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->type == TransportMsgType_Data);
    REQUIRE(decoded->payload == payload);
}
