#include "relay/identity/relay_identity.h"
#include <fstream>
#include <stdexcept>

namespace chromatindb::relay::identity {

std::filesystem::path pub_path_from_key_path(const std::filesystem::path& key_path) {
    auto p = key_path;
    p.replace_extension(".pub");
    return p;
}

RelayIdentity RelayIdentity::generate() {
    RelayIdentity id;
    id.signer_.generate_keypair();
    id.pk_hash_ = crypto::sha3_256(id.signer_.export_public_key());
    return id;
}

RelayIdentity RelayIdentity::load_from(const std::filesystem::path& key_path) {
    auto pub_path = pub_path_from_key_path(key_path);

    if (!std::filesystem::exists(key_path)) {
        throw std::runtime_error("Secret key file not found: " + key_path.string());
    }
    if (!std::filesystem::exists(pub_path)) {
        throw std::runtime_error("Public key file not found: " + pub_path.string());
    }

    // Read secret key
    std::ifstream key_file(key_path, std::ios::binary);
    if (!key_file) {
        throw std::runtime_error("Cannot open secret key file: " + key_path.string());
    }
    std::vector<uint8_t> seckey(
        (std::istreambuf_iterator<char>(key_file)),
        std::istreambuf_iterator<char>());

    if (seckey.size() != crypto::Signer::SECRET_KEY_SIZE) {
        throw std::runtime_error("Invalid secret key size: expected " +
            std::to_string(crypto::Signer::SECRET_KEY_SIZE) +
            ", got " + std::to_string(seckey.size()));
    }

    // Read public key
    std::ifstream pub_file(pub_path, std::ios::binary);
    if (!pub_file) {
        throw std::runtime_error("Cannot open public key file: " + pub_path.string());
    }
    std::vector<uint8_t> pubkey(
        (std::istreambuf_iterator<char>(pub_file)),
        std::istreambuf_iterator<char>());

    if (pubkey.size() != crypto::Signer::PUBLIC_KEY_SIZE) {
        throw std::runtime_error("Invalid public key size: expected " +
            std::to_string(crypto::Signer::PUBLIC_KEY_SIZE) +
            ", got " + std::to_string(pubkey.size()));
    }

    RelayIdentity id;
    id.signer_ = crypto::Signer::from_keypair(pubkey, seckey);
    id.pk_hash_ = crypto::sha3_256(id.signer_.export_public_key());
    return id;
}

RelayIdentity RelayIdentity::load_or_generate(const std::filesystem::path& key_path) {
    auto pub_path = pub_path_from_key_path(key_path);

    if (std::filesystem::exists(key_path) && std::filesystem::exists(pub_path)) {
        return load_from(key_path);
    }

    auto id = generate();
    std::filesystem::create_directories(key_path.parent_path());
    id.save_to(key_path);
    return id;
}

void RelayIdentity::save_to(const std::filesystem::path& key_path) const {
    auto pub_path = pub_path_from_key_path(key_path);

    std::filesystem::create_directories(key_path.parent_path());

    // Write secret key
    std::ofstream key_file(key_path, std::ios::binary);
    if (!key_file) {
        throw std::runtime_error("Cannot write secret key file: " + key_path.string());
    }
    auto sk = signer_.export_secret_key();
    key_file.write(reinterpret_cast<const char*>(sk.data()),
                   static_cast<std::streamsize>(sk.size()));

    // Write public key
    std::ofstream pub_file(pub_path, std::ios::binary);
    if (!pub_file) {
        throw std::runtime_error("Cannot write public key file: " + pub_path.string());
    }
    auto pk = signer_.export_public_key();
    pub_file.write(reinterpret_cast<const char*>(pk.data()),
                   static_cast<std::streamsize>(pk.size()));
}

std::span<const uint8_t, 32> RelayIdentity::public_key_hash() const {
    return std::span<const uint8_t, 32>(pk_hash_.data(), 32);
}

std::span<const uint8_t> RelayIdentity::public_key() const {
    return signer_.export_public_key();
}

std::vector<uint8_t> RelayIdentity::sign(std::span<const uint8_t> message) const {
    return signer_.sign(message);
}

chromatindb::identity::NodeIdentity RelayIdentity::to_node_identity() const {
    // Stub: throws (tests will fail)
    throw std::runtime_error("to_node_identity not implemented");
}

} // namespace chromatindb::relay::identity
