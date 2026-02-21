#include <gtest/gtest.h>
#include "crypto/kem.h"

namespace crypto = chromatin::crypto;

TEST(KemTest, KeypairGeneration) {
    auto kp = crypto::generate_kem_keypair();
    EXPECT_EQ(kp.public_key.size(), crypto::KEM_PUBLIC_KEY_SIZE);
    EXPECT_EQ(kp.secret_key.size(), crypto::KEM_SECRET_KEY_SIZE);
}

TEST(KemTest, EncapsulateDecapsulateRoundtrip) {
    auto kp = crypto::generate_kem_keypair();

    auto encap = crypto::kem_encapsulate(kp.public_key);
    EXPECT_EQ(encap.ciphertext.size(), crypto::KEM_CIPHERTEXT_SIZE);
    EXPECT_EQ(encap.shared_secret.size(), crypto::KEM_SHARED_SECRET_SIZE);

    auto decap = crypto::kem_decapsulate(encap.ciphertext, kp.secret_key);
    ASSERT_TRUE(decap.has_value());
    EXPECT_EQ(decap->size(), crypto::KEM_SHARED_SECRET_SIZE);
    EXPECT_EQ(*decap, encap.shared_secret);
}

TEST(KemTest, DifferentKeypairsProduceDifferentSecrets) {
    auto kp1 = crypto::generate_kem_keypair();
    auto kp2 = crypto::generate_kem_keypair();

    auto encap1 = crypto::kem_encapsulate(kp1.public_key);
    auto encap2 = crypto::kem_encapsulate(kp2.public_key);

    EXPECT_NE(encap1.shared_secret, encap2.shared_secret);
}

TEST(KemTest, WrongSecretKeyImplicitReject) {
    auto kp1 = crypto::generate_kem_keypair();
    auto kp2 = crypto::generate_kem_keypair();

    auto encap = crypto::kem_encapsulate(kp1.public_key);

    // ML-KEM implicit reject: decaps always returns a value,
    // but with wrong key it's a different (random) secret
    auto decap = crypto::kem_decapsulate(encap.ciphertext, kp2.secret_key);
    ASSERT_TRUE(decap.has_value());
    EXPECT_NE(*decap, encap.shared_secret);
}

TEST(KemTest, InvalidCiphertextSize) {
    auto kp = crypto::generate_kem_keypair();
    std::vector<uint8_t> bad_ct = {0x01, 0x02, 0x03};
    auto result = crypto::kem_decapsulate(bad_ct, kp.secret_key);
    EXPECT_FALSE(result.has_value());
}

TEST(KemTest, InvalidSecretKeySize) {
    auto kp = crypto::generate_kem_keypair();
    auto encap = crypto::kem_encapsulate(kp.public_key);
    std::vector<uint8_t> bad_sk = {0x01, 0x02, 0x03};
    auto result = crypto::kem_decapsulate(encap.ciphertext, bad_sk);
    EXPECT_FALSE(result.has_value());
}
