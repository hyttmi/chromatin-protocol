#include <catch2/catch_test_macros.hpp>
#include "crypto/aead.h"
#include <sodium.h>
#include <cstring>

using namespace chromatin::crypto;

static std::array<uint8_t, AEAD::NONCE_SIZE> make_nonce(uint8_t val = 0) {
    std::array<uint8_t, AEAD::NONCE_SIZE> nonce{};
    nonce[0] = val;
    return nonce;
}

TEST_CASE("AEAD keygen produces correct size", "[aead]") {
    auto key = AEAD::keygen();
    REQUIRE(key.size() == AEAD::KEY_SIZE);
}

TEST_CASE("AEAD encrypt then decrypt round-trips", "[aead]") {
    auto key = AEAD::keygen();
    auto nonce = make_nonce(1);
    std::vector<uint8_t> plaintext = {72, 101, 108, 108, 111};  // "Hello"
    std::vector<uint8_t> ad = {};

    auto ct = AEAD::encrypt(plaintext, ad, nonce, key.span());
    REQUIRE(ct.size() == plaintext.size() + AEAD::TAG_SIZE);

    auto pt = AEAD::decrypt(ct, ad, nonce, key.span());
    REQUIRE(pt.has_value());
    REQUIRE(pt.value() == plaintext);
}

TEST_CASE("AEAD encrypt with AD, decrypt verifies AD", "[aead]") {
    auto key = AEAD::keygen();
    auto nonce = make_nonce(2);
    std::vector<uint8_t> plaintext = {1, 2, 3};
    std::vector<uint8_t> ad = {0xAA, 0xBB, 0xCC};

    auto ct = AEAD::encrypt(plaintext, ad, nonce, key.span());

    // Correct AD decrypts
    auto pt = AEAD::decrypt(ct, ad, nonce, key.span());
    REQUIRE(pt.has_value());
    REQUIRE(pt.value() == plaintext);

    // Wrong AD fails
    std::vector<uint8_t> wrong_ad = {0xAA, 0xBB, 0xCD};
    auto bad = AEAD::decrypt(ct, wrong_ad, nonce, key.span());
    REQUIRE_FALSE(bad.has_value());
}

TEST_CASE("AEAD tampered ciphertext fails decryption", "[aead]") {
    auto key = AEAD::keygen();
    auto nonce = make_nonce(3);
    std::vector<uint8_t> plaintext = {10, 20, 30};
    std::vector<uint8_t> ad = {};

    auto ct = AEAD::encrypt(plaintext, ad, nonce, key.span());

    // Tamper with ciphertext
    ct[0] ^= 0xFF;
    auto bad = AEAD::decrypt(ct, ad, nonce, key.span());
    REQUIRE_FALSE(bad.has_value());
}

TEST_CASE("AEAD wrong key fails decryption", "[aead]") {
    auto key1 = AEAD::keygen();
    auto key2 = AEAD::keygen();
    auto nonce = make_nonce(4);
    std::vector<uint8_t> plaintext = {42};
    std::vector<uint8_t> ad = {};

    auto ct = AEAD::encrypt(plaintext, ad, nonce, key1.span());
    auto bad = AEAD::decrypt(ct, ad, nonce, key2.span());
    REQUIRE_FALSE(bad.has_value());
}

TEST_CASE("AEAD different nonces produce different ciphertext", "[aead]") {
    auto key = AEAD::keygen();
    std::vector<uint8_t> plaintext = {1, 2, 3, 4, 5};
    std::vector<uint8_t> ad = {};

    auto ct1 = AEAD::encrypt(plaintext, ad, make_nonce(10), key.span());
    auto ct2 = AEAD::encrypt(plaintext, ad, make_nonce(11), key.span());

    REQUIRE(ct1 != ct2);
}

TEST_CASE("AEAD ciphertext size is plaintext + tag", "[aead]") {
    auto key = AEAD::keygen();
    auto nonce = make_nonce(5);

    for (size_t len : {0, 1, 16, 100, 1000}) {
        std::vector<uint8_t> plaintext(len, 0x42);
        std::vector<uint8_t> ad = {};
        auto ct = AEAD::encrypt(plaintext, ad, nonce, key.span());
        REQUIRE(ct.size() == len + AEAD::TAG_SIZE);
    }
}
