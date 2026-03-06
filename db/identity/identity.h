#pragma once

#include "db/crypto/signing.h"
#include "db/crypto/hash.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace chromatin::identity {

/// Node identity: owns an ML-DSA-87 keypair and the derived namespace.
/// Namespace = SHA3-256(public_key).
class NodeIdentity {
public:
    /// Generate a fresh identity (new keypair + derived namespace).
    static NodeIdentity generate();

    /// Load identity from key files in data directory.
    /// @throws std::runtime_error if files are missing or corrupt.
    static NodeIdentity load_from(const std::filesystem::path& data_dir);

    /// Load from files if they exist, otherwise generate and save.
    static NodeIdentity load_or_generate(const std::filesystem::path& data_dir);

    /// Save key files to data directory (node.key + node.pub).
    void save_to(const std::filesystem::path& data_dir) const;

    /// Get the 32-byte namespace ID (SHA3-256 of public key).
    std::span<const uint8_t, 32> namespace_id() const {
        return std::span<const uint8_t, 32>(namespace_id_.data(), 32);
    }

    /// Get the public key.
    std::span<const uint8_t> public_key() const {
        return signer_.export_public_key();
    }

    /// Sign a message using the node's secret key.
    std::vector<uint8_t> sign(std::span<const uint8_t> message) const {
        return signer_.sign(message);
    }

private:
    NodeIdentity() = default;

    crypto::Signer signer_;
    std::array<uint8_t, 32> namespace_id_{};
};

} // namespace chromatin::identity
