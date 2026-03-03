#include "crypto/aead.h"
#include <sodium.h>
#include <stdexcept>

namespace chromatin::crypto::AEAD {

SecureBytes keygen() {
    ensure_sodium_init();
    SecureBytes key(KEY_SIZE);
    crypto_aead_chacha20poly1305_ietf_keygen(key.data());
    return key;
}

std::vector<uint8_t> encrypt(
    std::span<const uint8_t> plaintext,
    std::span<const uint8_t> ad,
    std::span<const uint8_t> nonce,
    std::span<const uint8_t> key) {

    ensure_sodium_init();

    if (nonce.size() != NONCE_SIZE) {
        throw std::runtime_error("AEAD nonce must be 12 bytes");
    }
    if (key.size() != KEY_SIZE) {
        throw std::runtime_error("AEAD key must be 32 bytes");
    }

    std::vector<uint8_t> ciphertext(plaintext.size() + TAG_SIZE);
    unsigned long long ciphertext_len = 0;

    int rc = crypto_aead_chacha20poly1305_ietf_encrypt(
        ciphertext.data(), &ciphertext_len,
        plaintext.data(), plaintext.size(),
        ad.empty() ? nullptr : ad.data(), ad.size(),
        nullptr,  // nsec (unused)
        nonce.data(), key.data());

    if (rc != 0) {
        throw std::runtime_error("ChaCha20-Poly1305 encryption failed");
    }

    ciphertext.resize(static_cast<size_t>(ciphertext_len));
    return ciphertext;
}

std::optional<std::vector<uint8_t>> decrypt(
    std::span<const uint8_t> ciphertext,
    std::span<const uint8_t> ad,
    std::span<const uint8_t> nonce,
    std::span<const uint8_t> key) {

    ensure_sodium_init();

    if (nonce.size() != NONCE_SIZE) {
        throw std::runtime_error("AEAD nonce must be 12 bytes");
    }
    if (key.size() != KEY_SIZE) {
        throw std::runtime_error("AEAD key must be 32 bytes");
    }
    if (ciphertext.size() < TAG_SIZE) {
        return std::nullopt;  // Too short to contain tag
    }

    std::vector<uint8_t> plaintext(ciphertext.size() - TAG_SIZE);
    unsigned long long plaintext_len = 0;

    int rc = crypto_aead_chacha20poly1305_ietf_decrypt(
        plaintext.data(), &plaintext_len,
        nullptr,  // nsec (unused)
        ciphertext.data(), ciphertext.size(),
        ad.empty() ? nullptr : ad.data(), ad.size(),
        nonce.data(), key.data());

    if (rc != 0) {
        return std::nullopt;  // Authentication failed
    }

    plaintext.resize(static_cast<size_t>(plaintext_len));
    return plaintext;
}

} // namespace chromatin::crypto::AEAD
