#pragma once

#include "db/crypto/signing.h"
#include "db/crypto/hash.h"
#include "db/identity/identity.h"
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
/// Unlike NodeIdentity (which uses data_dir/node.key), RelayIdentity
/// takes a direct path to the secret key file (per D-07).
/// Public key is the .pub sibling (per D-08).
class RelayIdentity {
public:
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

    /// Convert to NodeIdentity for use with Connection class.
    chromatindb::identity::NodeIdentity to_node_identity() const;

private:
    RelayIdentity() = default;
    crypto::Signer signer_;
    std::array<uint8_t, 32> pk_hash_{};
};

} // namespace chromatindb::relay::identity
