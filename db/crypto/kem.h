#pragma once

#include "db/crypto/secure_bytes.h"
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

struct OQS_KEM;

namespace chromatin::crypto {

/// ML-KEM-1024 key encapsulation mechanism RAII wrapper.
/// Provides key exchange via encapsulation/decapsulation.
/// Move-only.
class KEM {
public:
    /// Key and ciphertext sizes for ML-KEM-1024.
    static constexpr size_t PUBLIC_KEY_SIZE = 1568;
    static constexpr size_t SECRET_KEY_SIZE = 3168;
    static constexpr size_t CIPHERTEXT_SIZE = 1568;
    static constexpr size_t SHARED_SECRET_SIZE = 32;

    /// Create a new KEM (allocates OQS_KEM context).
    KEM();
    ~KEM();

    // Move only
    KEM(KEM&& other) noexcept;
    KEM& operator=(KEM&& other) noexcept;
    KEM(const KEM&) = delete;
    KEM& operator=(const KEM&) = delete;

    /// Generate a fresh ML-KEM-1024 keypair.
    void generate_keypair();

    /// Encapsulate: given a public key, produce (ciphertext, shared_secret).
    /// The ciphertext is sent to the keypair owner, who decapsulates to get the same shared secret.
    std::pair<std::vector<uint8_t>, SecureBytes> encaps(
        std::span<const uint8_t> public_key) const;

    /// Decapsulate: given ciphertext and secret key, recover shared secret.
    SecureBytes decaps(std::span<const uint8_t> ciphertext,
                       std::span<const uint8_t> secret_key) const;

    /// Access the public key.
    std::span<const uint8_t> export_public_key() const;

    /// Access the secret key.
    std::span<const uint8_t> export_secret_key() const;

    /// Whether a keypair has been generated.
    bool has_keypair() const { return !public_key_.empty(); }

private:
    OQS_KEM* kem_ = nullptr;
    std::vector<uint8_t> public_key_;
    SecureBytes secret_key_;
};

} // namespace chromatin::crypto
