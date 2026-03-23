#include "db/identity/identity.h"
#include <fstream>
#include <stdexcept>

namespace chromatindb::identity {

NodeIdentity NodeIdentity::generate() {
    NodeIdentity id;
    id.signer_.generate_keypair();
    id.namespace_id_ = crypto::sha3_256(id.signer_.export_public_key());
    return id;
}

NodeIdentity NodeIdentity::from_keys(std::span<const uint8_t> pubkey,
                                      std::span<const uint8_t> seckey) {
    if (pubkey.size() != crypto::Signer::PUBLIC_KEY_SIZE)
        throw std::runtime_error("Invalid public key size");
    if (seckey.size() != crypto::Signer::SECRET_KEY_SIZE)
        throw std::runtime_error("Invalid secret key size");
    NodeIdentity id;
    id.signer_ = crypto::Signer::from_keypair(pubkey, seckey);
    id.namespace_id_ = crypto::sha3_256(id.signer_.export_public_key());
    return id;
}

NodeIdentity NodeIdentity::load_from(const std::filesystem::path& data_dir) {
    auto pub_path = data_dir / "node.pub";
    auto key_path = data_dir / "node.key";

    if (!std::filesystem::exists(pub_path)) {
        throw std::runtime_error("Public key file not found: " + pub_path.string());
    }
    if (!std::filesystem::exists(key_path)) {
        throw std::runtime_error("Secret key file not found: " + key_path.string());
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

    NodeIdentity id;
    id.signer_ = crypto::Signer::from_keypair(pubkey, seckey);
    id.namespace_id_ = crypto::sha3_256(id.signer_.export_public_key());
    return id;
}

NodeIdentity NodeIdentity::load_or_generate(const std::filesystem::path& data_dir) {
    auto pub_path = data_dir / "node.pub";
    auto key_path = data_dir / "node.key";

    if (std::filesystem::exists(pub_path) && std::filesystem::exists(key_path)) {
        return load_from(data_dir);
    }

    auto id = generate();
    std::filesystem::create_directories(data_dir);
    id.save_to(data_dir);
    return id;
}

void NodeIdentity::save_to(const std::filesystem::path& data_dir) const {
    std::filesystem::create_directories(data_dir);

    // Write public key
    auto pub_path = data_dir / "node.pub";
    std::ofstream pub_file(pub_path, std::ios::binary);
    if (!pub_file) {
        throw std::runtime_error("Cannot write public key file: " + pub_path.string());
    }
    auto pk = signer_.export_public_key();
    pub_file.write(reinterpret_cast<const char*>(pk.data()), pk.size());

    // Write secret key
    auto key_path = data_dir / "node.key";
    std::ofstream key_file(key_path, std::ios::binary);
    if (!key_file) {
        throw std::runtime_error("Cannot write secret key file: " + key_path.string());
    }
    auto sk = signer_.export_secret_key();
    key_file.write(reinterpret_cast<const char*>(sk.data()), sk.size());
}

} // namespace chromatindb::identity
