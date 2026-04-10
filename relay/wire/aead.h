#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace chromatindb::relay::wire {

constexpr size_t AEAD_KEY_SIZE = 32;
constexpr size_t AEAD_NONCE_SIZE = 12;
constexpr size_t AEAD_TAG_SIZE = 16;

/// Build a counter-based nonce for AEAD.
/// Format: [4 zero bytes][8-byte big-endian counter].
std::array<uint8_t, AEAD_NONCE_SIZE> make_nonce(uint64_t counter);

/// ChaCha20-Poly1305 encrypt with counter-based nonce.
/// Returns ciphertext || tag (plaintext.size() + AEAD_TAG_SIZE bytes).
/// Empty associated data.
std::vector<uint8_t> aead_encrypt(std::span<const uint8_t> plaintext,
                                   std::span<const uint8_t> key,
                                   uint64_t counter);

/// ChaCha20-Poly1305 decrypt with counter-based nonce.
/// Input: ciphertext || tag. Returns plaintext or nullopt on auth failure.
std::optional<std::vector<uint8_t>> aead_decrypt(std::span<const uint8_t> ciphertext_and_tag,
                                                  std::span<const uint8_t> key,
                                                  uint64_t counter);

/// HKDF-SHA256 extract: salt + IKM -> 32-byte PRK.
std::vector<uint8_t> hkdf_extract(std::span<const uint8_t> salt,
                                   std::span<const uint8_t> ikm);

/// HKDF-SHA256 expand: PRK + info -> 32-byte output key material.
std::array<uint8_t, 32> hkdf_expand(std::span<const uint8_t> prk,
                                     std::string_view info,
                                     size_t length = 32);

} // namespace chromatindb::relay::wire
