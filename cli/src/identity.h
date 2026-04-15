#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace chromatindb::cli {

/// Client identity: ML-DSA-87 signing keypair + ML-KEM-1024 encryption keypair.
/// Namespace = SHA3-256(signing_pubkey).
class Identity {
public:
    static constexpr size_t SIGNING_PK_SIZE = 2592;   // ML-DSA-87
    static constexpr size_t SIGNING_SK_SIZE = 4896;
    static constexpr size_t KEM_PK_SIZE     = 1568;    // ML-KEM-1024
    static constexpr size_t KEM_SK_SIZE     = 3168;
    static constexpr size_t NAMESPACE_SIZE  = 32;
    static constexpr size_t EXPORT_SIZE     = SIGNING_PK_SIZE + KEM_PK_SIZE;  // 4160

    /// Generate a fresh identity (signing + KEM keypairs).
    static Identity generate();

    /// Load identity from key files in directory.
    /// @throws std::runtime_error if files are missing or corrupt.
    static Identity load_from(const std::filesystem::path& dir);

    /// Save key files to directory.
    void save_to(const std::filesystem::path& dir) const;

    std::span<const uint8_t> signing_pubkey() const { return signing_pk_; }
    std::span<const uint8_t> signing_seckey() const { return signing_sk_; }
    std::span<const uint8_t> kem_pubkey() const { return kem_pk_; }
    std::span<const uint8_t> kem_seckey() const { return kem_sk_; }
    std::span<const uint8_t, 32> namespace_id() const {
        return std::span<const uint8_t, 32>(namespace_id_.data(), 32);
    }

    /// Sign a message with the ML-DSA-87 secret key.
    std::vector<uint8_t> sign(std::span<const uint8_t> message) const;

    /// Export public keys: [signing_pk:2592][kem_pk:1568] = 4160 bytes.
    std::vector<uint8_t> export_public_keys() const;

    /// Parse exported public keys back into (signing_pk, kem_pk).
    /// @throws std::runtime_error if data size != 4160.
    static std::pair<std::vector<uint8_t>, std::vector<uint8_t>>
    load_public_keys(std::span<const uint8_t> data);

private:
    std::vector<uint8_t> signing_pk_, signing_sk_;
    std::vector<uint8_t> kem_pk_, kem_sk_;
    std::array<uint8_t, 32> namespace_id_{};
};

} // namespace chromatindb::cli
