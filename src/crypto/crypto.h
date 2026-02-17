#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace helix::crypto {

using Hash = std::array<uint8_t, 32>;

// SHA3-256
Hash sha3_256(std::span<const uint8_t> data);
Hash sha3_256_prefixed(std::string_view prefix, std::span<const uint8_t> data);

// ML-DSA-87 (FIPS 204 Level 5, formerly Dilithium5)
inline constexpr size_t PUBLIC_KEY_SIZE = 2592;
inline constexpr size_t SECRET_KEY_SIZE = 4896;
inline constexpr size_t SIGNATURE_SIZE = 4627;

struct KeyPair {
    std::vector<uint8_t> public_key;
    std::vector<uint8_t> secret_key;
};

KeyPair generate_keypair();
std::vector<uint8_t> sign(std::span<const uint8_t> message, std::span<const uint8_t> secret_key);
bool verify(std::span<const uint8_t> message, std::span<const uint8_t> signature, std::span<const uint8_t> public_key);

// PoW verification
bool verify_pow(std::span<const uint8_t> preimage, uint64_t nonce, int required_zero_bits);
int leading_zero_bits(const Hash& hash);

} // namespace helix::crypto
