#include <catch2/catch_test_macros.hpp>
#include "db/crypto/kdf.h"
#include "db/crypto/kem.h"
#include "db/crypto/aead.h"
#include <cstring>

using namespace chromatin::crypto;

TEST_CASE("KDF extract produces PRK of correct size", "[kdf]") {
    std::vector<uint8_t> salt = {1, 2, 3, 4};
    std::vector<uint8_t> ikm = {10, 20, 30, 40, 50};

    auto prk = KDF::extract(salt, ikm);
    REQUIRE(prk.size() == KDF::PRK_SIZE);
}

TEST_CASE("KDF expand produces derived key of requested size", "[kdf]") {
    std::vector<uint8_t> salt = {1, 2, 3, 4};
    std::vector<uint8_t> ikm = {10, 20, 30, 40, 50};
    auto prk = KDF::extract(salt, ikm);

    auto key = KDF::expand(prk.span(), "test-context", 32);
    REQUIRE(key.size() == 32);

    auto key64 = KDF::expand(prk.span(), "test-context-64", 64);
    REQUIRE(key64.size() == 64);
}

TEST_CASE("KDF same inputs produce same derived key", "[kdf]") {
    std::vector<uint8_t> salt = {0xAA, 0xBB};
    std::vector<uint8_t> ikm = {0xCC, 0xDD, 0xEE};

    auto k1 = KDF::derive(salt, ikm, "deterministic", 32);
    auto k2 = KDF::derive(salt, ikm, "deterministic", 32);

    REQUIRE(k1 == k2);
}

TEST_CASE("KDF different contexts produce different derived keys", "[kdf]") {
    std::vector<uint8_t> salt = {1};
    std::vector<uint8_t> ikm = {2, 3};

    auto k1 = KDF::derive(salt, ikm, "context-a", 32);
    auto k2 = KDF::derive(salt, ikm, "context-b", 32);

    REQUIRE_FALSE(k1 == k2);
}

TEST_CASE("KDF derive convenience function works", "[kdf]") {
    std::vector<uint8_t> salt = {5, 6, 7};
    std::vector<uint8_t> ikm = {8, 9, 10, 11};

    auto key = KDF::derive(salt, ikm, "chromatindb-channel-key", 32);
    REQUIRE(key.size() == 32);

    // Verify it matches extract+expand manually
    auto prk = KDF::extract(salt, ikm);
    auto key2 = KDF::expand(prk.span(), "chromatindb-channel-key", 32);
    REQUIRE(key == key2);
}

TEST_CASE("KDF integration: ML-KEM shared secret -> KDF -> AEAD key", "[kdf]") {
    // Simulate key exchange
    KEM kem;
    kem.generate_keypair();

    auto [ciphertext, shared_secret] = kem.encaps(kem.export_public_key());

    // Derive AEAD key from shared secret
    std::vector<uint8_t> salt = {};  // No salt in initial key exchange
    auto aead_key = KDF::derive(salt, shared_secret.span(),
                                "chromatindb-aead-key", AEAD::KEY_SIZE);

    REQUIRE(aead_key.size() == AEAD::KEY_SIZE);

    // Verify the derived key works for AEAD
    std::array<uint8_t, AEAD::NONCE_SIZE> nonce{};
    nonce[0] = 1;
    std::vector<uint8_t> plaintext = {1, 2, 3, 4, 5};
    std::vector<uint8_t> ad = {};

    auto ct = AEAD::encrypt(plaintext, ad, nonce, aead_key.span());
    auto pt = AEAD::decrypt(ct, ad, nonce, aead_key.span());

    REQUIRE(pt.has_value());
    REQUIRE(pt.value() == plaintext);
}
