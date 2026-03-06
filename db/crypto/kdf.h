#pragma once

#include "db/crypto/secure_bytes.h"
#include <cstdint>
#include <span>
#include <string_view>

namespace chromatindb::crypto {

/// HKDF-SHA256 key derivation functions.
/// Uses libsodium's crypto_kdf_hkdf_sha256_* API (RFC 5869).
namespace KDF {

/// PRK (pseudorandom key) size from HKDF-SHA256 extract.
constexpr size_t PRK_SIZE = 32;

/// Extract: absorb input key material with salt to produce PRK.
SecureBytes extract(std::span<const uint8_t> salt,
                    std::span<const uint8_t> ikm);

/// Expand: derive output key material from PRK + context.
/// @param output_len Must be between 1 and 255*32 bytes.
SecureBytes expand(std::span<const uint8_t> prk,
                   std::string_view context,
                   size_t output_len);

/// Convenience: extract then expand in one call.
SecureBytes derive(std::span<const uint8_t> salt,
                   std::span<const uint8_t> ikm,
                   std::string_view context,
                   size_t output_len);

} // namespace KDF

} // namespace chromatindb::crypto
