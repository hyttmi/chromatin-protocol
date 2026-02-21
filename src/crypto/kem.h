#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatin::crypto {

// ML-KEM-1024 (FIPS 203 Level 5) sizes
inline constexpr size_t KEM_PUBLIC_KEY_SIZE = 1568;
inline constexpr size_t KEM_SECRET_KEY_SIZE = 3168;
inline constexpr size_t KEM_CIPHERTEXT_SIZE = 1568;
inline constexpr size_t KEM_SHARED_SECRET_SIZE = 32;

struct KemKeyPair {
    std::vector<uint8_t> public_key;   // 1568 bytes
    std::vector<uint8_t> secret_key;   // 3168 bytes

    KemKeyPair() = default;
    ~KemKeyPair();

    KemKeyPair(KemKeyPair&&) noexcept;
    KemKeyPair& operator=(KemKeyPair&&) noexcept;
    KemKeyPair(const KemKeyPair&) = delete;
    KemKeyPair& operator=(const KemKeyPair&) = delete;
};

struct KemEncapResult {
    std::vector<uint8_t> ciphertext;      // 1568 bytes
    std::vector<uint8_t> shared_secret;   // 32 bytes

    ~KemEncapResult();
};

KemKeyPair generate_kem_keypair();
KemEncapResult kem_encapsulate(std::span<const uint8_t> public_key);
std::optional<std::vector<uint8_t>> kem_decapsulate(
    std::span<const uint8_t> ciphertext, std::span<const uint8_t> secret_key);

} // namespace chromatin::crypto
