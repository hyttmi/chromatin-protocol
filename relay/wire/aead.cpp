#include "relay/wire/aead.h"
#include "relay/util/endian.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>

#include <cstring>
#include <stdexcept>

namespace chromatindb::relay::wire {

namespace {

/// RAII wrapper for EVP_CIPHER_CTX.
struct CipherCtx {
    EVP_CIPHER_CTX* ctx;
    CipherCtx() : ctx(EVP_CIPHER_CTX_new()) {
        if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    }
    ~CipherCtx() { EVP_CIPHER_CTX_free(ctx); }
    CipherCtx(const CipherCtx&) = delete;
    CipherCtx& operator=(const CipherCtx&) = delete;
};

/// RAII wrapper for EVP_PKEY_CTX.
struct PKeyCtx {
    EVP_PKEY_CTX* ctx;
    explicit PKeyCtx(int id) : ctx(EVP_PKEY_CTX_new_id(id, nullptr)) {
        if (!ctx) throw std::runtime_error("EVP_PKEY_CTX_new_id failed");
    }
    ~PKeyCtx() { EVP_PKEY_CTX_free(ctx); }
    PKeyCtx(const PKeyCtx&) = delete;
    PKeyCtx& operator=(const PKeyCtx&) = delete;
};

} // anonymous namespace

std::array<uint8_t, AEAD_NONCE_SIZE> make_nonce(uint64_t counter) {
    std::array<uint8_t, AEAD_NONCE_SIZE> nonce{};
    // First 4 bytes are zero (default initialized)
    util::store_u64_be(nonce.data() + 4, counter);
    return nonce;
}

std::vector<uint8_t> aead_encrypt(std::span<const uint8_t> plaintext,
                                   std::span<const uint8_t> key,
                                   uint64_t counter) {
    if (key.size() != AEAD_KEY_SIZE) {
        throw std::runtime_error("aead_encrypt: key must be 32 bytes");
    }

    CipherCtx c;
    auto nonce = make_nonce(counter);

    if (EVP_EncryptInit_ex(c.ctx, EVP_chacha20_poly1305(), nullptr,
                           key.data(), nonce.data()) != 1) {
        throw std::runtime_error("EVP_EncryptInit_ex failed");
    }

    std::vector<uint8_t> result(plaintext.size() + AEAD_TAG_SIZE);

    int outlen = 0;
    if (EVP_EncryptUpdate(c.ctx, result.data(), &outlen,
                          plaintext.data(), static_cast<int>(plaintext.size())) != 1) {
        throw std::runtime_error("EVP_EncryptUpdate failed");
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(c.ctx, result.data() + outlen, &final_len) != 1) {
        throw std::runtime_error("EVP_EncryptFinal_ex failed");
    }

    if (EVP_CIPHER_CTX_ctrl(c.ctx, EVP_CTRL_AEAD_GET_TAG,
                             static_cast<int>(AEAD_TAG_SIZE),
                             result.data() + outlen + final_len) != 1) {
        throw std::runtime_error("EVP_CTRL_AEAD_GET_TAG failed");
    }

    return result;
}

std::optional<std::vector<uint8_t>> aead_decrypt(std::span<const uint8_t> ciphertext_and_tag,
                                                  std::span<const uint8_t> key,
                                                  uint64_t counter) {
    if (key.size() != AEAD_KEY_SIZE) {
        return std::nullopt;
    }
    if (ciphertext_and_tag.size() < AEAD_TAG_SIZE) {
        return std::nullopt;
    }

    auto ct_len = ciphertext_and_tag.size() - AEAD_TAG_SIZE;
    CipherCtx c;
    auto nonce = make_nonce(counter);

    if (EVP_DecryptInit_ex(c.ctx, EVP_chacha20_poly1305(), nullptr,
                           key.data(), nonce.data()) != 1) {
        return std::nullopt;
    }

    std::vector<uint8_t> plaintext(ct_len);

    int outlen = 0;
    if (EVP_DecryptUpdate(c.ctx, plaintext.data(), &outlen,
                          ciphertext_and_tag.data(), static_cast<int>(ct_len)) != 1) {
        return std::nullopt;
    }

    // Set the expected tag
    if (EVP_CIPHER_CTX_ctrl(c.ctx, EVP_CTRL_AEAD_SET_TAG,
                             static_cast<int>(AEAD_TAG_SIZE),
                             const_cast<uint8_t*>(ciphertext_and_tag.data() + ct_len)) != 1) {
        return std::nullopt;
    }

    int final_len = 0;
    if (EVP_DecryptFinal_ex(c.ctx, plaintext.data() + outlen, &final_len) != 1) {
        return std::nullopt;  // Authentication failed
    }

    plaintext.resize(static_cast<size_t>(outlen + final_len));
    return plaintext;
}

std::vector<uint8_t> hkdf_extract(std::span<const uint8_t> salt,
                                   std::span<const uint8_t> ikm) {
    PKeyCtx pctx(EVP_PKEY_HKDF);

    if (EVP_PKEY_derive_init(pctx.ctx) != 1) {
        throw std::runtime_error("HKDF derive_init failed");
    }
    if (EVP_PKEY_CTX_set_hkdf_md(pctx.ctx, EVP_sha256()) != 1) {
        throw std::runtime_error("HKDF set_md failed");
    }
    if (EVP_PKEY_CTX_hkdf_mode(pctx.ctx, EVP_PKEY_HKDEF_MODE_EXTRACT_ONLY) != 1) {
        throw std::runtime_error("HKDF set_mode failed");
    }
    // OpenSSL requires a non-null salt pointer even for empty salt.
    // Use a dummy byte when salt is empty (HKDF spec treats empty salt
    // as a string of HashLen zero bytes).
    const uint8_t dummy = 0;
    const uint8_t* salt_ptr = salt.empty() ? &dummy : salt.data();
    int salt_len = salt.empty() ? 0 : static_cast<int>(salt.size());
    if (EVP_PKEY_CTX_set1_hkdf_salt(pctx.ctx, salt_ptr, salt_len) != 1) {
        throw std::runtime_error("HKDF set_salt failed");
    }
    if (EVP_PKEY_CTX_set1_hkdf_key(pctx.ctx, ikm.data(),
                                    static_cast<int>(ikm.size())) != 1) {
        throw std::runtime_error("HKDF set_key failed");
    }

    std::vector<uint8_t> prk(32);
    size_t outlen = 32;
    if (EVP_PKEY_derive(pctx.ctx, prk.data(), &outlen) != 1) {
        throw std::runtime_error("HKDF extract failed");
    }

    prk.resize(outlen);
    return prk;
}

std::array<uint8_t, 32> hkdf_expand(std::span<const uint8_t> prk,
                                     std::string_view info,
                                     size_t length) {
    if (length > 32) {
        throw std::runtime_error("hkdf_expand: max output 32 bytes");
    }

    PKeyCtx pctx(EVP_PKEY_HKDF);

    if (EVP_PKEY_derive_init(pctx.ctx) != 1) {
        throw std::runtime_error("HKDF derive_init failed");
    }
    if (EVP_PKEY_CTX_set_hkdf_md(pctx.ctx, EVP_sha256()) != 1) {
        throw std::runtime_error("HKDF set_md failed");
    }
    if (EVP_PKEY_CTX_hkdf_mode(pctx.ctx, EVP_PKEY_HKDEF_MODE_EXPAND_ONLY) != 1) {
        throw std::runtime_error("HKDF set_mode failed");
    }
    if (EVP_PKEY_CTX_set1_hkdf_key(pctx.ctx, prk.data(),
                                    static_cast<int>(prk.size())) != 1) {
        throw std::runtime_error("HKDF set_key failed");
    }
    if (EVP_PKEY_CTX_add1_hkdf_info(pctx.ctx,
                                     reinterpret_cast<const unsigned char*>(info.data()),
                                     static_cast<int>(info.size())) != 1) {
        throw std::runtime_error("HKDF add_info failed");
    }

    std::array<uint8_t, 32> okm{};
    size_t outlen = length;
    if (EVP_PKEY_derive(pctx.ctx, okm.data(), &outlen) != 1) {
        throw std::runtime_error("HKDF expand failed");
    }

    return okm;
}

} // namespace chromatindb::relay::wire
