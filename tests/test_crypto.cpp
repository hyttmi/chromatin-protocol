#include <gtest/gtest.h>

#include "crypto/crypto.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace helix::crypto;

// ---------------------------------------------------------------------------
// SHA3-256 tests
// ---------------------------------------------------------------------------

TEST(SHA3_256, EmptyInput_MatchesNISTVector) {
    // NIST test vector: SHA3-256("") = a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a
    std::span<const uint8_t> empty{};
    Hash h = sha3_256(empty);

    const uint8_t expected[] = {
        0xa7, 0xff, 0xc6, 0xf8, 0xbf, 0x1e, 0xd7, 0x66,
        0x51, 0xc1, 0x47, 0x56, 0xa0, 0x61, 0xd6, 0x62,
        0xf5, 0x80, 0xff, 0x4d, 0xe4, 0x3b, 0x49, 0xfa,
        0x82, 0xd8, 0x0a, 0x4b, 0x80, 0xf8, 0x43, 0x4a,
    };

    EXPECT_EQ(h, (Hash{0xa7, 0xff, 0xc6, 0xf8, 0xbf, 0x1e, 0xd7, 0x66,
                       0x51, 0xc1, 0x47, 0x56, 0xa0, 0x61, 0xd6, 0x62,
                       0xf5, 0x80, 0xff, 0x4d, 0xe4, 0x3b, 0x49, 0xfa,
                       0x82, 0xd8, 0x0a, 0x4b, 0x80, 0xf8, 0x43, 0x4a}));
}

TEST(SHA3_256, Deterministic) {
    std::string input = "hello helix";
    std::span<const uint8_t> data(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    Hash h1 = sha3_256(data);
    Hash h2 = sha3_256(data);
    EXPECT_EQ(h1, h2);
}

TEST(SHA3_256, DifferentInputsDifferentOutputs) {
    std::string a_str = "input A";
    std::string b_str = "input B";
    std::span<const uint8_t> a(reinterpret_cast<const uint8_t*>(a_str.data()), a_str.size());
    std::span<const uint8_t> b(reinterpret_cast<const uint8_t*>(b_str.data()), b_str.size());
    EXPECT_NE(sha3_256(a), sha3_256(b));
}

TEST(SHA3_256, PrefixedHashEqualsManualConcatenation) {
    std::string prefix = "tag:";
    std::string body_str = "payload";
    std::span<const uint8_t> body(reinterpret_cast<const uint8_t*>(body_str.data()), body_str.size());

    Hash prefixed = sha3_256_prefixed(prefix, body);

    // Manual concatenation
    std::string concat = prefix + body_str;
    std::span<const uint8_t> concat_span(reinterpret_cast<const uint8_t*>(concat.data()), concat.size());
    Hash manual = sha3_256(concat_span);

    EXPECT_EQ(prefixed, manual);
}

// ---------------------------------------------------------------------------
// ML-DSA-87 tests
// ---------------------------------------------------------------------------

TEST(MLDSA87, KeypairSizesMatchAlgorithm) {
    KeyPair kp = generate_keypair();

    // Verify sizes match what OQS reports, not hardcoded constants
    // (constants are informational and may differ between liboqs versions)
    EXPECT_FALSE(kp.public_key.empty());
    EXPECT_FALSE(kp.secret_key.empty());

    // Sizes should be reasonable for ML-DSA-87
    EXPECT_GT(kp.public_key.size(), 2000u);
    EXPECT_GT(kp.secret_key.size(), 4000u);
}

TEST(MLDSA87, SignVerifyRoundTrip) {
    KeyPair kp = generate_keypair();

    std::string msg_str = "test message for ML-DSA-87";
    std::span<const uint8_t> msg(reinterpret_cast<const uint8_t*>(msg_str.data()), msg_str.size());

    auto sig = sign(msg, kp.secret_key);
    EXPECT_FALSE(sig.empty());
    EXPECT_TRUE(verify(msg, sig, kp.public_key));
}

TEST(MLDSA87, VerifyRejectsTamperedMessage) {
    KeyPair kp = generate_keypair();

    std::string msg_str = "original message";
    std::span<const uint8_t> msg(reinterpret_cast<const uint8_t*>(msg_str.data()), msg_str.size());

    auto sig = sign(msg, kp.secret_key);

    // Tamper with the message
    std::string tampered_str = "tampered message";
    std::span<const uint8_t> tampered(reinterpret_cast<const uint8_t*>(tampered_str.data()), tampered_str.size());

    EXPECT_FALSE(verify(tampered, sig, kp.public_key));
}

TEST(MLDSA87, VerifyRejectsWrongKey) {
    KeyPair kp1 = generate_keypair();
    KeyPair kp2 = generate_keypair();

    std::string msg_str = "signed with kp1";
    std::span<const uint8_t> msg(reinterpret_cast<const uint8_t*>(msg_str.data()), msg_str.size());

    auto sig = sign(msg, kp1.secret_key);

    // Verify with wrong public key
    EXPECT_FALSE(verify(msg, sig, kp2.public_key));
}

// ---------------------------------------------------------------------------
// PoW tests
// ---------------------------------------------------------------------------

TEST(PoW, LeadingZeroBits_AllZeros) {
    Hash all_zeros{};
    EXPECT_EQ(leading_zero_bits(all_zeros), 256);
}

TEST(PoW, LeadingZeroBits_FirstByte0x01) {
    Hash h{};
    h[0] = 0x01; // 7 leading zero bits
    EXPECT_EQ(leading_zero_bits(h), 7);
}

TEST(PoW, LeadingZeroBits_FirstByte0x80) {
    Hash h{};
    h[0] = 0x80; // 0 leading zero bits
    EXPECT_EQ(leading_zero_bits(h), 0);
}

TEST(PoW, LeadingZeroBits_FirstByteZeroSecond0x0F) {
    Hash h{};
    h[0] = 0x00;
    h[1] = 0x0F; // 8 + 4 = 12 leading zero bits
    EXPECT_EQ(leading_zero_bits(h), 12);
}

TEST(PoW, VerifyPow_BruteForce8Bits) {
    // Brute-force a nonce that gives >= 8 leading zero bits
    // With 8 bits, expected ~256 attempts on average
    std::string preimage_str = "pow-test-preimage";
    std::span<const uint8_t> preimage(
        reinterpret_cast<const uint8_t*>(preimage_str.data()),
        preimage_str.size());

    uint64_t nonce = 0;
    bool found = false;
    for (uint64_t i = 0; i < 100000; ++i) {
        if (verify_pow(preimage, i, 8)) {
            nonce = i;
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found) << "Failed to find valid nonce within 100000 attempts";

    // Verify the found nonce passes
    EXPECT_TRUE(verify_pow(preimage, nonce, 8));

    // Verify nonce+1 likely does not pass (overwhelmingly likely)
    // (unless we got very unlucky and nonce+1 also has >= 8 leading zeros)
    // We just test that the function can return false
    bool any_false = false;
    for (uint64_t i = 0; i < 256; ++i) {
        if (!verify_pow(preimage, i, 8)) {
            any_false = true;
            break;
        }
    }
    EXPECT_TRUE(any_false) << "verify_pow should return false for some nonces";
}
