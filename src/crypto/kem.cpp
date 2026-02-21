#include "crypto/kem.h"

#include <memory>
#include <stdexcept>

#include <oqs/oqs.h>

namespace chromatin::crypto {

// ---------------------------------------------------------------------------
// KemKeyPair: secure zeroing + move semantics
// ---------------------------------------------------------------------------

KemKeyPair::~KemKeyPair() {
    if (!secret_key.empty()) {
        OQS_MEM_cleanse(secret_key.data(), secret_key.size());
    }
}

KemKeyPair::KemKeyPair(KemKeyPair&& other) noexcept
    : public_key(std::move(other.public_key))
    , secret_key(std::move(other.secret_key)) {}

KemKeyPair& KemKeyPair::operator=(KemKeyPair&& other) noexcept {
    if (this != &other) {
        if (!secret_key.empty()) {
            OQS_MEM_cleanse(secret_key.data(), secret_key.size());
        }
        public_key = std::move(other.public_key);
        secret_key = std::move(other.secret_key);
    }
    return *this;
}

KemEncapResult::~KemEncapResult() {
    if (!shared_secret.empty()) {
        OQS_MEM_cleanse(shared_secret.data(), shared_secret.size());
    }
}

// ---------------------------------------------------------------------------
// ML-KEM-1024 helpers
// ---------------------------------------------------------------------------

namespace {

struct KemDeleter {
    void operator()(OQS_KEM* k) const noexcept {
        if (k) OQS_KEM_free(k);
    }
};
using KemPtr = std::unique_ptr<OQS_KEM, KemDeleter>;

KemPtr make_kem() {
    KemPtr kem(OQS_KEM_new(OQS_KEM_alg_ml_kem_1024));
    if (!kem) {
        throw std::runtime_error("OQS_KEM_new(ML-KEM-1024) failed — algorithm not available");
    }
    return kem;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ML-KEM-1024 public API
// ---------------------------------------------------------------------------

KemKeyPair generate_kem_keypair() {
    auto kem = make_kem();

    KemKeyPair kp;
    kp.public_key.resize(kem->length_public_key);
    kp.secret_key.resize(kem->length_secret_key);

    if (OQS_KEM_keypair(kem.get(), kp.public_key.data(), kp.secret_key.data()) != OQS_SUCCESS) {
        throw std::runtime_error("OQS_KEM_keypair failed");
    }
    return kp;
}

KemEncapResult kem_encapsulate(std::span<const uint8_t> public_key) {
    auto kem = make_kem();

    if (public_key.size() != kem->length_public_key) {
        throw std::runtime_error("kem_encapsulate: invalid public key size");
    }

    KemEncapResult result;
    result.ciphertext.resize(kem->length_ciphertext);
    result.shared_secret.resize(kem->length_shared_secret);

    if (OQS_KEM_encaps(kem.get(), result.ciphertext.data(),
                        result.shared_secret.data(), public_key.data()) != OQS_SUCCESS) {
        throw std::runtime_error("OQS_KEM_encaps failed");
    }
    return result;
}

std::optional<std::vector<uint8_t>> kem_decapsulate(
    std::span<const uint8_t> ciphertext, std::span<const uint8_t> secret_key) {
    auto kem = make_kem();

    if (ciphertext.size() != kem->length_ciphertext) {
        return std::nullopt;
    }
    if (secret_key.size() != kem->length_secret_key) {
        return std::nullopt;
    }

    std::vector<uint8_t> shared_secret(kem->length_shared_secret);

    if (OQS_KEM_decaps(kem.get(), shared_secret.data(),
                        ciphertext.data(), secret_key.data()) != OQS_SUCCESS) {
        return std::nullopt;
    }
    return shared_secret;
}

} // namespace chromatin::crypto
