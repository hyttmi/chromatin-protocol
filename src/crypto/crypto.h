#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace chromatin::crypto {

using Hash = std::array<uint8_t, 32>;

// Hash functor for use in unordered containers.
struct HashHash {
    size_t operator()(const Hash& h) const noexcept {
        size_t result = 0;
        // Use first 8 bytes as the hash value (the data is already well-distributed).
        for (int i = 0; i < 8 && i < static_cast<int>(h.size()); ++i) {
            result = (result << 8) | h[i];
        }
        return result;
    }
};

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

    KeyPair() = default;
    ~KeyPair();

    // Move-only: prevent accidental copies of secret key material
    KeyPair(KeyPair&& other) noexcept;
    KeyPair& operator=(KeyPair&& other) noexcept;
    KeyPair(const KeyPair&) = delete;
    KeyPair& operator=(const KeyPair&) = delete;
};

KeyPair generate_keypair();
std::vector<uint8_t> sign(std::span<const uint8_t> message, std::span<const uint8_t> secret_key);
bool verify(std::span<const uint8_t> message, std::span<const uint8_t> signature, std::span<const uint8_t> public_key);

// PoW verification
bool verify_pow(std::span<const uint8_t> preimage, uint64_t nonce, int required_zero_bits);
int leading_zero_bits(const Hash& hash);

} // namespace chromatin::crypto
