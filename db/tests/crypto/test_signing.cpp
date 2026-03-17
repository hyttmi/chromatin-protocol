#include <catch2/catch_test_macros.hpp>
#include "db/crypto/signing.h"
#include <vector>
#include <cstring>

using namespace chromatindb::crypto;

TEST_CASE("ML-DSA-87 keypair generation produces correct sizes", "[signing]") {
    Signer signer;
    signer.generate_keypair();

    REQUIRE(signer.export_public_key().size() == Signer::PUBLIC_KEY_SIZE);
    REQUIRE(signer.export_secret_key().size() == Signer::SECRET_KEY_SIZE);
}

TEST_CASE("ML-DSA-87 sign produces valid signature", "[signing]") {
    Signer signer;
    signer.generate_keypair();

    std::vector<uint8_t> message = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"
    auto signature = signer.sign(message);

    REQUIRE(signature.size() <= Signer::MAX_SIGNATURE_SIZE);
    REQUIRE(signature.size() > 0);
}

TEST_CASE("ML-DSA-87 verify returns true for valid signature", "[signing]") {
    Signer signer;
    signer.generate_keypair();

    std::vector<uint8_t> message = {1, 2, 3, 4, 5, 6, 7, 8};
    auto signature = signer.sign(message);
    auto pubkey = signer.export_public_key();

    REQUIRE(Signer::verify(message, signature, pubkey));
}

TEST_CASE("ML-DSA-87 verify returns false for tampered message", "[signing]") {
    Signer signer;
    signer.generate_keypair();

    std::vector<uint8_t> message = {1, 2, 3, 4, 5};
    auto signature = signer.sign(message);
    auto pubkey = signer.export_public_key();

    // Tamper with message
    std::vector<uint8_t> tampered = {1, 2, 3, 4, 6};
    REQUIRE_FALSE(Signer::verify(tampered, signature, pubkey));
}

TEST_CASE("ML-DSA-87 verify returns false for wrong public key", "[signing]") {
    Signer signer1;
    signer1.generate_keypair();

    Signer signer2;
    signer2.generate_keypair();

    std::vector<uint8_t> message = {10, 20, 30};
    auto signature = signer1.sign(message);
    auto wrong_pubkey = signer2.export_public_key();

    REQUIRE_FALSE(Signer::verify(message, signature, wrong_pubkey));
}

TEST_CASE("ML-DSA-87 from_keypair reconstructs signer", "[signing]") {
    Signer signer;
    signer.generate_keypair();

    auto pubkey = signer.export_public_key();
    auto seckey = signer.export_secret_key();

    // Save key material
    std::vector<uint8_t> pub_copy(pubkey.begin(), pubkey.end());
    std::vector<uint8_t> sec_copy(seckey.begin(), seckey.end());

    // Reconstruct
    auto signer2 = Signer::from_keypair(pub_copy, sec_copy);

    // Sign with reconstructed signer, verify with original pubkey
    std::vector<uint8_t> message = {42, 43, 44};
    auto sig = signer2.sign(message);
    REQUIRE(Signer::verify(message, sig, pub_copy));
}

TEST_CASE("ML-DSA-87 move semantics", "[signing]") {
    Signer signer;
    signer.generate_keypair();

    auto pubkey_before = std::vector<uint8_t>(
        signer.export_public_key().begin(),
        signer.export_public_key().end());

    Signer moved(std::move(signer));
    REQUIRE(moved.has_keypair());

    auto pubkey_after = moved.export_public_key();
    REQUIRE(std::equal(pubkey_before.begin(), pubkey_before.end(),
                       pubkey_after.begin(), pubkey_after.end()));
}
