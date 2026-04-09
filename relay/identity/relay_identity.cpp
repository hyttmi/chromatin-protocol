#include "relay/identity/relay_identity.h"

#include <oqs/oqs.h>
#include <oqs/sha3.h>
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

    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) {
        throw std::runtime_error("Failed to initialize ML-DSA-87");
    }

    id.public_key_.resize(sig->length_public_key);
    id.secret_key_.resize(sig->length_secret_key);

    if (OQS_SIG_keypair(sig, id.public_key_.data(), id.secret_key_.data()) != OQS_SUCCESS) {
        OQS_SIG_free(sig);
        throw std::runtime_error("ML-DSA-87 keypair generation failed");
    }

    OQS_SIG_free(sig);

    // SHA3-256 hash of public key
    OQS_SHA3_sha3_256(id.pk_hash_.data(), id.public_key_.data(), id.public_key_.size());

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

    if (seckey.size() != SECRET_KEY_SIZE) {
        throw std::runtime_error("Invalid secret key size: expected " +
            std::to_string(SECRET_KEY_SIZE) +
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

    if (pubkey.size() != PUBLIC_KEY_SIZE) {
        throw std::runtime_error("Invalid public key size: expected " +
            std::to_string(PUBLIC_KEY_SIZE) +
            ", got " + std::to_string(pubkey.size()));
    }

    RelayIdentity id;
    id.secret_key_ = std::move(seckey);
    id.public_key_ = std::move(pubkey);
    OQS_SHA3_sha3_256(id.pk_hash_.data(), id.public_key_.data(), id.public_key_.size());

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
    key_file.write(reinterpret_cast<const char*>(secret_key_.data()),
                   static_cast<std::streamsize>(secret_key_.size()));

    // Write public key
    std::ofstream pub_file(pub_path, std::ios::binary);
    if (!pub_file) {
        throw std::runtime_error("Cannot write public key file: " + pub_path.string());
    }
    pub_file.write(reinterpret_cast<const char*>(public_key_.data()),
                   static_cast<std::streamsize>(public_key_.size()));
}

std::span<const uint8_t, 32> RelayIdentity::public_key_hash() const {
    return std::span<const uint8_t, 32>(pk_hash_.data(), 32);
}

std::span<const uint8_t> RelayIdentity::public_key() const {
    return std::span<const uint8_t>(public_key_.data(), public_key_.size());
}

std::vector<uint8_t> RelayIdentity::sign(std::span<const uint8_t> message) const {
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) {
        throw std::runtime_error("Failed to initialize ML-DSA-87 for signing");
    }

    std::vector<uint8_t> sig_out(SIGNATURE_SIZE);
    size_t sig_len = 0;

    OQS_STATUS rc = OQS_SIG_sign(sig, sig_out.data(), &sig_len,
                                  message.data(), message.size(),
                                  secret_key_.data());

    OQS_SIG_free(sig);

    if (rc != OQS_SUCCESS) {
        throw std::runtime_error("ML-DSA-87 signing failed");
    }

    sig_out.resize(sig_len);
    return sig_out;
}

} // namespace chromatindb::relay::identity
