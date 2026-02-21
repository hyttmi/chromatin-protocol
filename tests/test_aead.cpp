#include <gtest/gtest.h>
#include "crypto/aead.h"

namespace crypto = chromatin::crypto;

TEST(AeadTest, EncryptDecryptRoundtrip) {
    std::vector<uint8_t> key(32, 0xAA);
    std::vector<uint8_t> plaintext = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"

    auto ct = crypto::aead_encrypt(key, 0, plaintext);
    EXPECT_EQ(ct.size(), plaintext.size() + crypto::AEAD_TAG_SIZE);

    auto pt = crypto::aead_decrypt(key, 0, ct);
    ASSERT_TRUE(pt.has_value());
    EXPECT_EQ(*pt, plaintext);
}

TEST(AeadTest, DifferentNoncesProduceDifferentCiphertext) {
    std::vector<uint8_t> key(32, 0xBB);
    std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03, 0x04};

    auto ct0 = crypto::aead_encrypt(key, 0, plaintext);
    auto ct1 = crypto::aead_encrypt(key, 1, plaintext);

    EXPECT_NE(ct0, ct1);
}

TEST(AeadTest, WrongKeyFails) {
    std::vector<uint8_t> key1(32, 0xCC);
    std::vector<uint8_t> key2(32, 0xDD);
    std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03};

    auto ct = crypto::aead_encrypt(key1, 0, plaintext);
    auto pt = crypto::aead_decrypt(key2, 0, ct);
    EXPECT_FALSE(pt.has_value());
}

TEST(AeadTest, WrongNonceFails) {
    std::vector<uint8_t> key(32, 0xEE);
    std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03};

    auto ct = crypto::aead_encrypt(key, 42, plaintext);
    auto pt = crypto::aead_decrypt(key, 43, ct);
    EXPECT_FALSE(pt.has_value());
}

TEST(AeadTest, TamperedCiphertextFails) {
    std::vector<uint8_t> key(32, 0xFF);
    std::vector<uint8_t> plaintext = {0x48, 0x65, 0x6C, 0x6C, 0x6F};

    auto ct = crypto::aead_encrypt(key, 0, plaintext);
    ct[0] ^= 0x01;

    auto pt = crypto::aead_decrypt(key, 0, ct);
    EXPECT_FALSE(pt.has_value());
}

TEST(AeadTest, WithAAD) {
    std::vector<uint8_t> key(32, 0x11);
    std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03};
    std::vector<uint8_t> aad = {0xAA, 0xBB, 0xCC, 0xDD};

    auto ct = crypto::aead_encrypt(key, 0, plaintext, aad);

    // Correct AAD
    auto pt = crypto::aead_decrypt(key, 0, ct, aad);
    ASSERT_TRUE(pt.has_value());
    EXPECT_EQ(*pt, plaintext);

    // Wrong AAD
    std::vector<uint8_t> wrong_aad = {0x00, 0x00, 0x00, 0x00};
    EXPECT_FALSE(crypto::aead_decrypt(key, 0, ct, wrong_aad).has_value());

    // Missing AAD
    EXPECT_FALSE(crypto::aead_decrypt(key, 0, ct).has_value());
}

TEST(AeadTest, EmptyPlaintext) {
    std::vector<uint8_t> key(32, 0x22);
    std::vector<uint8_t> plaintext;

    auto ct = crypto::aead_encrypt(key, 0, plaintext);
    EXPECT_EQ(ct.size(), crypto::AEAD_TAG_SIZE);

    auto pt = crypto::aead_decrypt(key, 0, ct);
    ASSERT_TRUE(pt.has_value());
    EXPECT_TRUE(pt->empty());
}

TEST(AeadTest, LargePlaintext) {
    std::vector<uint8_t> key(32, 0x33);
    std::vector<uint8_t> plaintext(100000, 0x42);  // 100 KB

    auto ct = crypto::aead_encrypt(key, 0, plaintext);
    EXPECT_EQ(ct.size(), plaintext.size() + crypto::AEAD_TAG_SIZE);

    auto pt = crypto::aead_decrypt(key, 0, ct);
    ASSERT_TRUE(pt.has_value());
    EXPECT_EQ(*pt, plaintext);
}

TEST(AeadTest, TruncatedCiphertextFails) {
    std::vector<uint8_t> key(32, 0x44);
    std::vector<uint8_t> bad_ct = {0x01, 0x02, 0x03};
    auto pt = crypto::aead_decrypt(key, 0, bad_ct);
    EXPECT_FALSE(pt.has_value());
}
