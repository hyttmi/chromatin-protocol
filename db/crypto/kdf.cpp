#include "db/crypto/kdf.h"
#include <sodium.h>
#include <stdexcept>

namespace chromatin::crypto::KDF {

SecureBytes extract(std::span<const uint8_t> salt,
                    std::span<const uint8_t> ikm) {
    ensure_sodium_init();

    SecureBytes prk(crypto_kdf_hkdf_sha256_KEYBYTES);

    int rc = crypto_kdf_hkdf_sha256_extract(
        prk.data(),
        salt.empty() ? nullptr : salt.data(), salt.size(),
        ikm.data(), ikm.size());

    if (rc != 0) {
        throw std::runtime_error("HKDF-SHA256 extract failed");
    }

    return prk;
}

SecureBytes expand(std::span<const uint8_t> prk,
                   std::string_view context,
                   size_t output_len) {
    ensure_sodium_init();

    if (output_len == 0 || output_len > 255 * crypto_kdf_hkdf_sha256_KEYBYTES) {
        throw std::runtime_error("HKDF-SHA256 expand: invalid output length");
    }

    SecureBytes out(output_len);

    int rc = crypto_kdf_hkdf_sha256_expand(
        out.data(), output_len,
        context.data(), context.size(),
        prk.data());

    if (rc != 0) {
        throw std::runtime_error("HKDF-SHA256 expand failed");
    }

    return out;
}

SecureBytes derive(std::span<const uint8_t> salt,
                   std::span<const uint8_t> ikm,
                   std::string_view context,
                   size_t output_len) {
    auto prk = extract(salt, ikm);
    return expand(prk.span(), context, output_len);
}

} // namespace chromatin::crypto::KDF
