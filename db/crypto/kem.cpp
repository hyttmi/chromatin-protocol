#include "db/crypto/kem.h"
#include <oqs/oqs.h>

namespace chromatin::crypto {

KEM::KEM() {
    kem_ = OQS_KEM_new(OQS_KEM_alg_ml_kem_1024);
    if (!kem_) {
        throw std::runtime_error("Failed to create ML-KEM-1024 context");
    }
}

KEM::~KEM() {
    if (kem_) {
        OQS_KEM_free(kem_);
    }
}

KEM::KEM(KEM&& other) noexcept
    : kem_(other.kem_),
      public_key_(std::move(other.public_key_)),
      secret_key_(std::move(other.secret_key_)) {
    other.kem_ = nullptr;
}

KEM& KEM::operator=(KEM&& other) noexcept {
    if (this != &other) {
        if (kem_) OQS_KEM_free(kem_);
        kem_ = other.kem_;
        public_key_ = std::move(other.public_key_);
        secret_key_ = std::move(other.secret_key_);
        other.kem_ = nullptr;
    }
    return *this;
}

void KEM::generate_keypair() {
    if (!kem_) {
        throw std::runtime_error("KEM not initialized");
    }

    public_key_.resize(kem_->length_public_key);
    secret_key_ = SecureBytes(kem_->length_secret_key);

    OQS_STATUS rc = OQS_KEM_keypair(kem_, public_key_.data(), secret_key_.data());
    if (rc != OQS_SUCCESS) {
        public_key_.clear();
        secret_key_ = SecureBytes{};
        throw std::runtime_error("ML-KEM-1024 keypair generation failed");
    }
}

std::pair<std::vector<uint8_t>, SecureBytes> KEM::encaps(
    std::span<const uint8_t> public_key) const {
    if (!kem_) {
        throw std::runtime_error("KEM not initialized");
    }

    std::vector<uint8_t> ciphertext(kem_->length_ciphertext);
    SecureBytes shared_secret(kem_->length_shared_secret);

    OQS_STATUS rc = OQS_KEM_encaps(kem_, ciphertext.data(),
                                    shared_secret.data(), public_key.data());
    if (rc != OQS_SUCCESS) {
        throw std::runtime_error("ML-KEM-1024 encapsulation failed");
    }

    return {std::move(ciphertext), std::move(shared_secret)};
}

SecureBytes KEM::decaps(std::span<const uint8_t> ciphertext,
                        std::span<const uint8_t> secret_key) const {
    if (!kem_) {
        throw std::runtime_error("KEM not initialized");
    }

    SecureBytes shared_secret(kem_->length_shared_secret);

    OQS_STATUS rc = OQS_KEM_decaps(kem_, shared_secret.data(),
                                    ciphertext.data(), secret_key.data());
    if (rc != OQS_SUCCESS) {
        throw std::runtime_error("ML-KEM-1024 decapsulation failed");
    }

    return shared_secret;
}

std::span<const uint8_t> KEM::export_public_key() const {
    return {public_key_.data(), public_key_.size()};
}

std::span<const uint8_t> KEM::export_secret_key() const {
    return secret_key_.span();
}

} // namespace chromatin::crypto
