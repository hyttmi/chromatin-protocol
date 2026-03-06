#pragma once

#include "db/crypto/secure_bytes.h"
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

struct OQS_SIG;

namespace chromatin::crypto {

/// ML-DSA-87 digital signature RAII wrapper.
/// Owns a keypair and provides sign/verify operations.
/// Move-only: copying secret keys is a security anti-pattern.
class Signer {
public:
    /// Key and signature sizes for ML-DSA-87 (from liboqs 0.15.0).
    static constexpr size_t PUBLIC_KEY_SIZE = 2592;
    static constexpr size_t SECRET_KEY_SIZE = 4896;
    static constexpr size_t MAX_SIGNATURE_SIZE = 4627;

    /// Create a new Signer (allocates OQS_SIG context).
    Signer();
    ~Signer();

    // Move only
    Signer(Signer&& other) noexcept;
    Signer& operator=(Signer&& other) noexcept;
    Signer(const Signer&) = delete;
    Signer& operator=(const Signer&) = delete;

    /// Generate a fresh ML-DSA-87 keypair.
    void generate_keypair();

    /// Sign a message using the secret key.
    /// @return Signature bytes (up to MAX_SIGNATURE_SIZE).
    std::vector<uint8_t> sign(std::span<const uint8_t> message) const;

    /// Verify a signature against a message and public key.
    /// @return true if signature is valid.
    static bool verify(std::span<const uint8_t> message,
                       std::span<const uint8_t> signature,
                       std::span<const uint8_t> public_key);

    /// Access the public key.
    std::span<const uint8_t> export_public_key() const;

    /// Access the secret key (use carefully).
    std::span<const uint8_t> export_secret_key() const;

    /// Reconstruct a Signer from previously saved key material.
    static Signer from_keypair(std::span<const uint8_t> pubkey,
                               std::span<const uint8_t> secret_key);

    /// Whether a keypair has been generated/loaded.
    bool has_keypair() const { return !public_key_.empty(); }

private:
    OQS_SIG* sig_ = nullptr;
    std::vector<uint8_t> public_key_;
    SecureBytes secret_key_;
};

} // namespace chromatin::crypto
