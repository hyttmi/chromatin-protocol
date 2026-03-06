#include <catch2/catch_test_macros.hpp>
#include "db/crypto/kem.h"
#include <cstring>

using namespace chromatin::crypto;

TEST_CASE("ML-KEM-1024 keypair generation produces correct sizes", "[kem]") {
    KEM kem;
    kem.generate_keypair();

    REQUIRE(kem.export_public_key().size() == KEM::PUBLIC_KEY_SIZE);
    REQUIRE(kem.export_secret_key().size() == KEM::SECRET_KEY_SIZE);
}

TEST_CASE("ML-KEM-1024 encaps produces correct sizes", "[kem]") {
    KEM kem;
    kem.generate_keypair();

    auto [ciphertext, shared_secret] = kem.encaps(kem.export_public_key());

    REQUIRE(ciphertext.size() == KEM::CIPHERTEXT_SIZE);
    REQUIRE(shared_secret.size() == KEM::SHARED_SECRET_SIZE);
}

TEST_CASE("ML-KEM-1024 encaps/decaps round-trip produces matching secrets", "[kem]") {
    KEM kem;
    kem.generate_keypair();

    auto [ciphertext, shared_secret_e] = kem.encaps(kem.export_public_key());
    auto shared_secret_d = kem.decaps(ciphertext, kem.export_secret_key());

    REQUIRE(shared_secret_e.size() == shared_secret_d.size());
    REQUIRE(shared_secret_e == shared_secret_d);
}

TEST_CASE("ML-KEM-1024 decaps with wrong secret key produces different secret", "[kem]") {
    KEM kem1;
    kem1.generate_keypair();

    KEM kem2;
    kem2.generate_keypair();

    // Encapsulate with kem1's public key
    auto [ciphertext, shared_secret_e] = kem1.encaps(kem1.export_public_key());

    // Decapsulate with kem2's secret key (wrong key -- ML-KEM implicit reject)
    auto shared_secret_wrong = kem2.decaps(ciphertext, kem2.export_secret_key());

    // ML-KEM provides implicit rejection: returns a pseudorandom value instead of failing
    REQUIRE_FALSE(shared_secret_e == shared_secret_wrong);
}

TEST_CASE("ML-KEM-1024 move semantics", "[kem]") {
    KEM kem;
    kem.generate_keypair();

    auto pubkey = std::vector<uint8_t>(
        kem.export_public_key().begin(),
        kem.export_public_key().end());

    KEM moved(std::move(kem));
    REQUIRE(moved.has_keypair());
    REQUIRE(moved.export_public_key().size() == KEM::PUBLIC_KEY_SIZE);
}
