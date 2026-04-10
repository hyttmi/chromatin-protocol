#include <catch2/catch_test_macros.hpp>

#include "relay/wire/aead.h"
#include "relay/util/endian.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace chromatindb::relay::wire;

TEST_CASE("make_nonce produces correct 12-byte format", "[aead]") {
    auto nonce = make_nonce(0);
    REQUIRE(nonce.size() == AEAD_NONCE_SIZE);
    // All zeros for counter=0
    for (auto b : nonce) {
        REQUIRE(b == 0);
    }

    auto nonce1 = make_nonce(1);
    // First 4 bytes zero
    REQUIRE(nonce1[0] == 0);
    REQUIRE(nonce1[1] == 0);
    REQUIRE(nonce1[2] == 0);
    REQUIRE(nonce1[3] == 0);
    // Last byte should be 1 (BE encoding of counter=1)
    REQUIRE(nonce1[11] == 1);

    auto nonce_large = make_nonce(0x0102030405060708ULL);
    REQUIRE(nonce_large[0] == 0);
    REQUIRE(nonce_large[1] == 0);
    REQUIRE(nonce_large[2] == 0);
    REQUIRE(nonce_large[3] == 0);
    REQUIRE(nonce_large[4] == 0x01);
    REQUIRE(nonce_large[5] == 0x02);
    REQUIRE(nonce_large[6] == 0x03);
    REQUIRE(nonce_large[7] == 0x04);
    REQUIRE(nonce_large[8] == 0x05);
    REQUIRE(nonce_large[9] == 0x06);
    REQUIRE(nonce_large[10] == 0x07);
    REQUIRE(nonce_large[11] == 0x08);
}

TEST_CASE("AEAD encrypt/decrypt roundtrip", "[aead]") {
    std::array<uint8_t, AEAD_KEY_SIZE> key{};
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i);
    }

    std::vector<uint8_t> plaintext = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"
    auto ciphertext = aead_encrypt(plaintext, key, 0);
    REQUIRE(ciphertext.size() == plaintext.size() + AEAD_TAG_SIZE);

    auto decrypted = aead_decrypt(ciphertext, key, 0);
    REQUIRE(decrypted.has_value());
    REQUIRE(*decrypted == plaintext);
}

TEST_CASE("AEAD decrypt fails with wrong key", "[aead]") {
    std::array<uint8_t, AEAD_KEY_SIZE> key{};
    std::array<uint8_t, AEAD_KEY_SIZE> wrong_key{};
    wrong_key.fill(0xFF);

    std::vector<uint8_t> plaintext = {1, 2, 3, 4, 5};
    auto ciphertext = aead_encrypt(plaintext, key, 0);

    auto result = aead_decrypt(ciphertext, wrong_key, 0);
    REQUIRE(!result.has_value());
}

TEST_CASE("AEAD decrypt fails with wrong counter", "[aead]") {
    std::array<uint8_t, AEAD_KEY_SIZE> key{};
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i);
    }

    std::vector<uint8_t> plaintext = {0xDE, 0xAD, 0xBE, 0xEF};
    auto ciphertext = aead_encrypt(plaintext, key, 42);

    auto result = aead_decrypt(ciphertext, key, 43);
    REQUIRE(!result.has_value());
}

TEST_CASE("AEAD different counters produce different ciphertext", "[aead]") {
    std::array<uint8_t, AEAD_KEY_SIZE> key{};
    key.fill(0x42);

    std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03};
    auto ct0 = aead_encrypt(plaintext, key, 0);
    auto ct1 = aead_encrypt(plaintext, key, 1);

    REQUIRE(ct0 != ct1);
}

TEST_CASE("AEAD empty plaintext roundtrip", "[aead]") {
    std::array<uint8_t, AEAD_KEY_SIZE> key{};
    key.fill(0xAA);

    std::vector<uint8_t> plaintext;
    auto ciphertext = aead_encrypt(plaintext, key, 0);
    REQUIRE(ciphertext.size() == AEAD_TAG_SIZE);

    auto decrypted = aead_decrypt(ciphertext, key, 0);
    REQUIRE(decrypted.has_value());
    REQUIRE(decrypted->empty());
}

TEST_CASE("HKDF extract produces 32-byte output", "[aead]") {
    std::vector<uint8_t> salt(32, 0x01);
    std::vector<uint8_t> ikm(32, 0x02);

    auto prk = hkdf_extract(salt, ikm);
    REQUIRE(prk.size() == 32);

    // Same inputs produce same PRK
    auto prk2 = hkdf_extract(salt, ikm);
    REQUIRE(prk == prk2);
}

TEST_CASE("HKDF expand produces correct length output", "[aead]") {
    std::vector<uint8_t> salt(32, 0x01);
    std::vector<uint8_t> ikm(32, 0x02);
    auto prk = hkdf_extract(salt, ikm);

    auto okm = hkdf_expand(prk, "test-info", 32);
    REQUIRE(okm.size() == 32);

    // Different info strings produce different keys
    auto okm2 = hkdf_expand(prk, "other-info", 32);
    REQUIRE(okm != okm2);
}

TEST_CASE("HKDF with chromatin context strings", "[aead]") {
    std::vector<uint8_t> salt(32, 0x00);
    std::vector<uint8_t> ikm(32, 0xFF);
    auto prk = hkdf_extract(salt, ikm);

    auto key_i2r = hkdf_expand(prk, "chromatin-init-to-resp-v1", 32);
    auto key_r2i = hkdf_expand(prk, "chromatin-resp-to-init-v1", 32);

    // Directional keys must differ
    REQUIRE(key_i2r != key_r2i);

    // Deterministic
    auto key_i2r_again = hkdf_expand(prk, "chromatin-init-to-resp-v1", 32);
    REQUIRE(key_i2r == key_i2r_again);
}

TEST_CASE("HKDF extract with empty salt", "[aead]") {
    std::vector<uint8_t> empty_salt;
    std::vector<uint8_t> ikm(32, 0x42);

    auto prk = hkdf_extract(empty_salt, ikm);
    REQUIRE(prk.size() == 32);
}
