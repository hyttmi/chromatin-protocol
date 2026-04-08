#pragma once

#include "db/crypto/secure_bytes.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::crypto {

/// ChaCha20-Poly1305 AEAD encryption/decryption.
/// Uses libsodium's IETF construction.
namespace AEAD {

constexpr size_t KEY_SIZE = 32;   // crypto_aead_chacha20poly1305_IETF_KEYBYTES
constexpr size_t NONCE_SIZE = 12; // crypto_aead_chacha20poly1305_IETF_NPUBBYTES
constexpr size_t TAG_SIZE = 16;   // crypto_aead_chacha20poly1305_IETF_ABYTES
constexpr size_t MAX_AD_LENGTH = 65536; // 64 KiB defense-in-depth bound

/// Generate a random AEAD key.
SecureBytes keygen();

/// Encrypt plaintext with associated data.
/// @return ciphertext (plaintext_len + TAG_SIZE bytes).
std::vector<uint8_t> encrypt(
    std::span<const uint8_t> plaintext,
    std::span<const uint8_t> ad,
    std::span<const uint8_t> nonce,  // NONCE_SIZE bytes
    std::span<const uint8_t> key);   // KEY_SIZE bytes

/// Decrypt ciphertext with associated data.
/// @return plaintext on success, nullopt if authentication fails.
std::optional<std::vector<uint8_t>> decrypt(
    std::span<const uint8_t> ciphertext,
    std::span<const uint8_t> ad,
    std::span<const uint8_t> nonce,  // NONCE_SIZE bytes
    std::span<const uint8_t> key);   // KEY_SIZE bytes

} // namespace AEAD

} // namespace chromatindb::crypto
