#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatin::crypto {

// ChaCha20-Poly1305 AEAD constants
inline constexpr size_t AEAD_KEY_SIZE = 32;
inline constexpr size_t AEAD_NONCE_SIZE = 12;
inline constexpr size_t AEAD_TAG_SIZE = 16;

// Encrypt with ChaCha20-Poly1305.
// Nonce is constructed from a counter: 4 zero bytes + 8 bytes big-endian counter.
// Returns ciphertext || tag (plaintext.size() + 16 bytes).
std::vector<uint8_t> aead_encrypt(
    std::span<const uint8_t> key,
    uint64_t nonce_counter,
    std::span<const uint8_t> plaintext,
    std::span<const uint8_t> aad = {});

// Decrypt with ChaCha20-Poly1305.
// Returns plaintext on success, nullopt if authentication fails.
std::optional<std::vector<uint8_t>> aead_decrypt(
    std::span<const uint8_t> key,
    uint64_t nonce_counter,
    std::span<const uint8_t> ciphertext,
    std::span<const uint8_t> aad = {});

} // namespace chromatin::crypto
