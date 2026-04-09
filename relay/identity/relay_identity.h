#pragma once

#include <oqs/oqs.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace chromatindb::relay::identity {

/// Derive .pub path from .key path by replacing extension.
/// "/etc/chromatindb/relay.key" -> "/etc/chromatindb/relay.pub"
std::filesystem::path pub_path_from_key_path(const std::filesystem::path& key_path);

/// Relay identity: owns an ML-DSA-87 keypair.
/// Uses liboqs directly -- no db/ dependencies.
class RelayIdentity {
public:
    // ML-DSA-87 key and signature sizes
    static constexpr size_t SECRET_KEY_SIZE = 4896;
    static constexpr size_t PUBLIC_KEY_SIZE = 2592;
    static constexpr size_t SIGNATURE_SIZE = 4627;

    /// Generate a fresh identity (new ML-DSA-87 keypair).
    static RelayIdentity generate();

    /// Load identity from key_path (.key) and its .pub sibling.
    /// @throws std::runtime_error if files missing or corrupt.
    static RelayIdentity load_from(const std::filesystem::path& key_path);

    /// Load from key_path if exists, otherwise generate and save.
    static RelayIdentity load_or_generate(const std::filesystem::path& key_path);

    /// Save keypair: key_path gets secret key, .pub sibling gets public key.
    void save_to(const std::filesystem::path& key_path) const;

    /// SHA3-256 hash of public key (32 bytes).
    std::span<const uint8_t, 32> public_key_hash() const;

    /// Raw public key bytes.
    std::span<const uint8_t> public_key() const;

    /// Sign a message.
    std::vector<uint8_t> sign(std::span<const uint8_t> message) const;

private:
    RelayIdentity() = default;
    std::vector<uint8_t> secret_key_;
    std::vector<uint8_t> public_key_;
    std::array<uint8_t, 32> pk_hash_{};
};

} // namespace chromatindb::relay::identity
