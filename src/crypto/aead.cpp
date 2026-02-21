#include "crypto/aead.h"

#include <cstring>
#include <stdexcept>

#include <sodium.h>

namespace chromatin::crypto {

namespace {

// Build 12-byte nonce: 4 zero bytes + 8-byte big-endian counter
void build_nonce(uint64_t counter, uint8_t nonce[12]) {
    std::memset(nonce, 0, 4);
    for (int i = 7; i >= 0; --i) {
        nonce[4 + (7 - i)] = static_cast<uint8_t>((counter >> (i * 8)) & 0xFF);
    }
}

} // anonymous namespace

std::vector<uint8_t> aead_encrypt(
    std::span<const uint8_t> key,
    uint64_t nonce_counter,
    std::span<const uint8_t> plaintext,
    std::span<const uint8_t> aad) {

    if (key.size() != AEAD_KEY_SIZE) {
        throw std::runtime_error("aead_encrypt: key must be 32 bytes");
    }

    uint8_t nonce[AEAD_NONCE_SIZE];
    build_nonce(nonce_counter, nonce);

    std::vector<uint8_t> ciphertext(plaintext.size() + AEAD_TAG_SIZE);
    unsigned long long ciphertext_len = 0;

    crypto_aead_chacha20poly1305_ietf_encrypt(
        ciphertext.data(), &ciphertext_len,
        plaintext.data(), plaintext.size(),
        aad.empty() ? nullptr : aad.data(), aad.size(),
        nullptr,  // nsec (unused)
        nonce,
        key.data());

    ciphertext.resize(static_cast<size_t>(ciphertext_len));
    return ciphertext;
}

std::optional<std::vector<uint8_t>> aead_decrypt(
    std::span<const uint8_t> key,
    uint64_t nonce_counter,
    std::span<const uint8_t> ciphertext,
    std::span<const uint8_t> aad) {

    if (key.size() != AEAD_KEY_SIZE) {
        return std::nullopt;
    }
    if (ciphertext.size() < AEAD_TAG_SIZE) {
        return std::nullopt;
    }

    uint8_t nonce[AEAD_NONCE_SIZE];
    build_nonce(nonce_counter, nonce);

    std::vector<uint8_t> plaintext(ciphertext.size() - AEAD_TAG_SIZE);
    unsigned long long plaintext_len = 0;

    int rc = crypto_aead_chacha20poly1305_ietf_decrypt(
        plaintext.data(), &plaintext_len,
        nullptr,  // nsec (unused)
        ciphertext.data(), ciphertext.size(),
        aad.empty() ? nullptr : aad.data(), aad.size(),
        nonce,
        key.data());

    if (rc != 0) {
        return std::nullopt;
    }

    plaintext.resize(static_cast<size_t>(plaintext_len));
    return plaintext;
}

} // namespace chromatin::crypto
