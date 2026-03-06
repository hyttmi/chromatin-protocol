#include "db/crypto/signing.h"
#include <oqs/oqs.h>
#include <cstring>

namespace chromatindb::crypto {

Signer::Signer() {
    sig_ = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig_) {
        throw std::runtime_error("Failed to create ML-DSA-87 signature context");
    }
}

Signer::~Signer() {
    if (sig_) {
        OQS_SIG_free(sig_);
    }
    // SecureBytes destructor handles secret_key_ zeroing
}

Signer::Signer(Signer&& other) noexcept
    : sig_(other.sig_),
      public_key_(std::move(other.public_key_)),
      secret_key_(std::move(other.secret_key_)) {
    other.sig_ = nullptr;
}

Signer& Signer::operator=(Signer&& other) noexcept {
    if (this != &other) {
        if (sig_) OQS_SIG_free(sig_);
        sig_ = other.sig_;
        public_key_ = std::move(other.public_key_);
        secret_key_ = std::move(other.secret_key_);
        other.sig_ = nullptr;
    }
    return *this;
}

void Signer::generate_keypair() {
    if (!sig_) {
        throw std::runtime_error("Signer not initialized");
    }

    public_key_.resize(sig_->length_public_key);
    secret_key_ = SecureBytes(sig_->length_secret_key);

    OQS_STATUS rc = OQS_SIG_keypair(sig_, public_key_.data(), secret_key_.data());
    if (rc != OQS_SUCCESS) {
        public_key_.clear();
        secret_key_ = SecureBytes{};
        throw std::runtime_error("ML-DSA-87 keypair generation failed");
    }
}

std::vector<uint8_t> Signer::sign(std::span<const uint8_t> message) const {
    if (!sig_ || secret_key_.empty()) {
        throw std::runtime_error("No keypair available for signing");
    }

    std::vector<uint8_t> signature(sig_->length_signature);
    size_t signature_len = 0;

    OQS_STATUS rc = OQS_SIG_sign(sig_, signature.data(), &signature_len,
                                  message.data(), message.size(),
                                  secret_key_.data());
    if (rc != OQS_SUCCESS) {
        throw std::runtime_error("ML-DSA-87 signing failed");
    }

    signature.resize(signature_len);
    return signature;
}

bool Signer::verify(std::span<const uint8_t> message,
                    std::span<const uint8_t> signature,
                    std::span<const uint8_t> public_key) {
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) {
        throw std::runtime_error("Failed to create ML-DSA-87 context for verification");
    }

    OQS_STATUS rc = OQS_SIG_verify(sig, message.data(), message.size(),
                                    signature.data(), signature.size(),
                                    public_key.data());
    OQS_SIG_free(sig);
    return rc == OQS_SUCCESS;
}

std::span<const uint8_t> Signer::export_public_key() const {
    return {public_key_.data(), public_key_.size()};
}

std::span<const uint8_t> Signer::export_secret_key() const {
    return secret_key_.span();
}

Signer Signer::from_keypair(std::span<const uint8_t> pubkey,
                             std::span<const uint8_t> secret_key) {
    Signer s;
    if (pubkey.size() != s.sig_->length_public_key) {
        throw std::runtime_error("Invalid public key size for ML-DSA-87");
    }
    if (secret_key.size() != s.sig_->length_secret_key) {
        throw std::runtime_error("Invalid secret key size for ML-DSA-87");
    }

    s.public_key_.assign(pubkey.begin(), pubkey.end());
    s.secret_key_ = SecureBytes(secret_key.data(), secret_key.size());
    return s;
}

} // namespace chromatindb::crypto
