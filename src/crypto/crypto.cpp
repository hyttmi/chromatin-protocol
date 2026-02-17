#include "crypto/crypto.h"

#include <cstring>
#include <memory>
#include <stdexcept>

#include <oqs/oqs.h>
#include <oqs/sha3.h>

namespace helix::crypto {

// ---------------------------------------------------------------------------
// SHA3-256
// ---------------------------------------------------------------------------

Hash sha3_256(std::span<const uint8_t> data) {
    Hash out{};
    OQS_SHA3_sha3_256(out.data(), data.data(), data.size());
    return out;
}

Hash sha3_256_prefixed(std::string_view prefix, std::span<const uint8_t> data) {
    OQS_SHA3_sha3_256_inc_ctx ctx;
    OQS_SHA3_sha3_256_inc_init(&ctx);

    OQS_SHA3_sha3_256_inc_absorb(
        &ctx,
        reinterpret_cast<const uint8_t*>(prefix.data()),
        prefix.size());

    OQS_SHA3_sha3_256_inc_absorb(&ctx, data.data(), data.size());

    Hash out{};
    OQS_SHA3_sha3_256_inc_finalize(out.data(), &ctx);
    OQS_SHA3_sha3_256_inc_ctx_release(&ctx);
    return out;
}

// ---------------------------------------------------------------------------
// ML-DSA-87 helpers
// ---------------------------------------------------------------------------

namespace {

struct SigDeleter {
    void operator()(OQS_SIG* s) const noexcept {
        if (s) {
            OQS_SIG_free(s);
        }
    }
};
using SigPtr = std::unique_ptr<OQS_SIG, SigDeleter>;

SigPtr make_sig() {
    SigPtr sig(OQS_SIG_new(OQS_SIG_alg_ml_dsa_87));
    if (!sig) {
        throw std::runtime_error("OQS_SIG_new(ML-DSA-87) failed — algorithm not available");
    }
    return sig;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ML-DSA-87 public API
// ---------------------------------------------------------------------------

KeyPair generate_keypair() {
    auto sig = make_sig();

    KeyPair kp;
    kp.public_key.resize(sig->length_public_key);
    kp.secret_key.resize(sig->length_secret_key);

    if (OQS_SIG_keypair(sig.get(), kp.public_key.data(), kp.secret_key.data()) != OQS_SUCCESS) {
        throw std::runtime_error("OQS_SIG_keypair failed");
    }
    return kp;
}

std::vector<uint8_t> sign(std::span<const uint8_t> message,
                          std::span<const uint8_t> secret_key) {
    auto sig = make_sig();

    std::vector<uint8_t> signature(sig->length_signature);
    size_t sig_len = 0;

    if (OQS_SIG_sign(sig.get(),
                     signature.data(), &sig_len,
                     message.data(), message.size(),
                     secret_key.data()) != OQS_SUCCESS) {
        throw std::runtime_error("OQS_SIG_sign failed");
    }

    signature.resize(sig_len);
    return signature;
}

bool verify(std::span<const uint8_t> message,
            std::span<const uint8_t> signature,
            std::span<const uint8_t> public_key) {
    auto sig = make_sig();

    return OQS_SIG_verify(sig.get(),
                          message.data(), message.size(),
                          signature.data(), signature.size(),
                          public_key.data()) == OQS_SUCCESS;
}

// ---------------------------------------------------------------------------
// PoW
// ---------------------------------------------------------------------------

int leading_zero_bits(const Hash& hash) {
    int bits = 0;
    for (uint8_t byte : hash) {
        if (byte == 0) {
            bits += 8;
        } else {
            bits += __builtin_clz(byte) - 24;
            break;
        }
    }
    return bits;
}

bool verify_pow(std::span<const uint8_t> preimage, uint64_t nonce, int required_zero_bits) {
    // Concatenate preimage + nonce (little-endian 8 bytes)
    std::vector<uint8_t> buf(preimage.size() + 8);
    std::memcpy(buf.data(), preimage.data(), preimage.size());

    // Write nonce as little-endian
    for (int i = 0; i < 8; ++i) {
        buf[preimage.size() + i] = static_cast<uint8_t>(nonce >> (i * 8));
    }

    Hash h = sha3_256(buf);
    return leading_zero_bits(h) >= required_zero_bits;
}

} // namespace helix::crypto
