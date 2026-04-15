#include <catch2/catch_test_macros.hpp>
#include "cli/src/wire.h"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <cstring>

using namespace chromatindb::cli;

TEST_CASE("wire: BE encode/decode roundtrip", "[wire]") {
    // u16
    {
        uint8_t buf[2];
        store_u16_be(buf, 0x1234);
        REQUIRE(buf[0] == 0x12);
        REQUIRE(buf[1] == 0x34);
        REQUIRE(load_u16_be(buf) == 0x1234);
    }
    // u16 boundary
    {
        uint8_t buf[2];
        store_u16_be(buf, 0);
        REQUIRE(load_u16_be(buf) == 0);
        store_u16_be(buf, 0xFFFF);
        REQUIRE(load_u16_be(buf) == 0xFFFF);
    }
    // u32
    {
        uint8_t buf[4];
        store_u32_be(buf, 0xDEADBEEF);
        REQUIRE(buf[0] == 0xDE);
        REQUIRE(buf[1] == 0xAD);
        REQUIRE(buf[2] == 0xBE);
        REQUIRE(buf[3] == 0xEF);
        REQUIRE(load_u32_be(buf) == 0xDEADBEEF);
    }
    // u64
    {
        uint8_t buf[8];
        store_u64_be(buf, 0x0102030405060708ULL);
        REQUIRE(buf[0] == 0x01);
        REQUIRE(buf[1] == 0x02);
        REQUIRE(buf[2] == 0x03);
        REQUIRE(buf[3] == 0x04);
        REQUIRE(buf[4] == 0x05);
        REQUIRE(buf[5] == 0x06);
        REQUIRE(buf[6] == 0x07);
        REQUIRE(buf[7] == 0x08);
        REQUIRE(load_u64_be(buf) == 0x0102030405060708ULL);
    }
}

TEST_CASE("wire: encode_transport roundtrips", "[wire]") {
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t type = static_cast<uint8_t>(MsgType::ReadRequest);
    uint32_t req_id = 42;

    auto encoded = encode_transport(type, payload, req_id);
    REQUIRE(!encoded.empty());

    auto decoded = decode_transport(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->type == type);
    REQUIRE(decoded->request_id == req_id);
    REQUIRE(decoded->payload == payload);
}

TEST_CASE("wire: encode_transport with empty payload", "[wire]") {
    auto encoded = encode_transport(
        static_cast<uint8_t>(MsgType::StatsRequest), {}, 99);
    auto decoded = decode_transport(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->type == static_cast<uint8_t>(MsgType::StatsRequest));
    REQUIRE(decoded->payload.empty());
    REQUIRE(decoded->request_id == 99);
}

TEST_CASE("wire: decode_transport rejects garbage", "[wire]") {
    std::vector<uint8_t> garbage = {0xFF, 0xFE, 0xFD};
    auto decoded = decode_transport(garbage);
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE("wire: aead_frame encrypt/decrypt roundtrip", "[wire]") {
    // Generate a random key
    std::array<uint8_t, 32> key{};
    randombytes_buf(key.data(), key.size());

    std::vector<uint8_t> plaintext = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t counter = 1;

    auto ciphertext = encrypt_frame(plaintext, key, counter);
    REQUIRE(ciphertext.size() == plaintext.size() + 16);  // +16 for tag

    auto result = decrypt_frame(ciphertext, key, counter);
    REQUIRE(result.has_value());
    REQUIRE(*result == plaintext);
}

TEST_CASE("wire: aead_frame nonce is 4 zeros + 8BE counter", "[wire]") {
    // Counter = 1
    {
        auto nonce = make_aead_nonce(1);
        REQUIRE(nonce.size() == 12);
        // First 4 bytes: zeros
        REQUIRE(nonce[0] == 0);
        REQUIRE(nonce[1] == 0);
        REQUIRE(nonce[2] == 0);
        REQUIRE(nonce[3] == 0);
        // Last 8 bytes: big-endian 1
        REQUIRE(nonce[4] == 0);
        REQUIRE(nonce[5] == 0);
        REQUIRE(nonce[6] == 0);
        REQUIRE(nonce[7] == 0);
        REQUIRE(nonce[8] == 0);
        REQUIRE(nonce[9] == 0);
        REQUIRE(nonce[10] == 0);
        REQUIRE(nonce[11] == 1);
    }
    // Counter = 256
    {
        auto nonce = make_aead_nonce(256);
        REQUIRE(nonce[4] == 0);
        REQUIRE(nonce[5] == 0);
        REQUIRE(nonce[6] == 0);
        REQUIRE(nonce[7] == 0);
        REQUIRE(nonce[8] == 0);
        REQUIRE(nonce[9] == 0);
        REQUIRE(nonce[10] == 1);
        REQUIRE(nonce[11] == 0);
    }
}

TEST_CASE("wire: aead_frame wrong counter fails decrypt", "[wire]") {
    std::array<uint8_t, 32> key{};
    randombytes_buf(key.data(), key.size());

    std::vector<uint8_t> plaintext = {10, 20, 30};

    auto ciphertext = encrypt_frame(plaintext, key, 1);
    auto result = decrypt_frame(ciphertext, key, 2);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("wire: encode_blob/decode_blob roundtrip", "[wire]") {
    BlobData blob;
    blob.namespace_id.fill(0xAB);
    blob.pubkey.resize(2592, 0x01);
    blob.data = {0xCA, 0xFE, 0xBA, 0xBE};
    blob.ttl = 3600;
    blob.timestamp = 1700000000;
    blob.signature.resize(4627, 0x02);

    auto encoded = encode_blob(blob);
    REQUIRE(!encoded.empty());

    auto decoded = decode_blob(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->namespace_id == blob.namespace_id);
    REQUIRE(decoded->pubkey == blob.pubkey);
    REQUIRE(decoded->data == blob.data);
    REQUIRE(decoded->ttl == blob.ttl);
    REQUIRE(decoded->timestamp == blob.timestamp);
    REQUIRE(decoded->signature == blob.signature);
}

TEST_CASE("wire: encode_blob/decode_blob with zero ttl", "[wire]") {
    BlobData blob;
    blob.namespace_id.fill(0x00);
    blob.pubkey.resize(10, 0x11);
    blob.data = {0x42};
    blob.ttl = 0;
    blob.timestamp = 0;
    blob.signature.resize(10, 0x22);

    auto encoded = encode_blob(blob);
    auto decoded = decode_blob(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->ttl == 0);
    REQUIRE(decoded->timestamp == 0);
}

TEST_CASE("wire: decode_blob rejects garbage", "[wire]") {
    std::vector<uint8_t> garbage = {0x00, 0x01, 0x02};
    auto decoded = decode_blob(garbage);
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE("wire: build_signing_input deterministic", "[wire]") {
    std::array<uint8_t, 32> ns{};
    ns.fill(0xAA);
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    uint32_t ttl = 3600;
    uint64_t timestamp = 1700000000;

    auto hash1 = build_signing_input(ns, data, ttl, timestamp);
    auto hash2 = build_signing_input(ns, data, ttl, timestamp);
    REQUIRE(hash1 == hash2);

    // Different data produces different hash
    std::vector<uint8_t> data2 = {1, 2, 3, 4, 6};
    auto hash3 = build_signing_input(ns, data2, ttl, timestamp);
    REQUIRE(hash1 != hash3);
}

TEST_CASE("wire: make_tombstone_data format", "[wire]") {
    std::array<uint8_t, 32> target{};
    target.fill(0xBB);

    auto tombstone = make_tombstone_data(target);
    REQUIRE(tombstone.size() == 36);

    // Magic prefix
    REQUIRE(tombstone[0] == 0xDE);
    REQUIRE(tombstone[1] == 0xAD);
    REQUIRE(tombstone[2] == 0xBE);
    REQUIRE(tombstone[3] == 0xEF);

    // Target hash follows
    for (size_t i = 0; i < 32; ++i) {
        REQUIRE(tombstone[4 + i] == 0xBB);
    }
}

TEST_CASE("wire: hex roundtrip", "[wire]") {
    std::vector<uint8_t> bytes = {0x00, 0x0A, 0xFF, 0xDE, 0xAD};
    auto hex = to_hex(bytes);
    REQUIRE(hex == "000affdead");

    auto back = from_hex(hex);
    REQUIRE(back.has_value());
    REQUIRE(*back == bytes);
}

TEST_CASE("wire: hex uppercase input accepted", "[wire]") {
    auto result = from_hex("DEADBEEF");
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 4);
    REQUIRE((*result)[0] == 0xDE);
    REQUIRE((*result)[1] == 0xAD);
    REQUIRE((*result)[2] == 0xBE);
    REQUIRE((*result)[3] == 0xEF);
}

TEST_CASE("wire: from_hex rejects odd length", "[wire]") {
    auto result = from_hex("abc");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("wire: from_hex rejects invalid chars", "[wire]") {
    auto result = from_hex("zz");
    REQUIRE_FALSE(result.has_value());
}
