#include "cli/src/identity.h"
#include <oqs/oqs.h>
#include <oqs/sha3.h>
#include <fstream>
#include <stdexcept>

namespace chromatindb::cli {

namespace {

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, std::span<const uint8_t> data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("Cannot write file: " + path.string());
    }
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

void derive_namespace(std::span<const uint8_t> signing_pk, std::array<uint8_t, 32>& out) {
    OQS_SHA3_sha3_256(out.data(), signing_pk.data(), signing_pk.size());
}

} // anonymous namespace

Identity Identity::generate() {
    Identity id;

    // ML-DSA-87 signing keypair
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) {
        throw std::runtime_error("Failed to create ML-DSA-87 context");
    }
    id.signing_pk_.resize(sig->length_public_key);
    id.signing_sk_.resize(sig->length_secret_key);
    if (OQS_SIG_keypair(sig, id.signing_pk_.data(), id.signing_sk_.data()) != OQS_SUCCESS) {
        OQS_SIG_free(sig);
        throw std::runtime_error("ML-DSA-87 keypair generation failed");
    }
    OQS_SIG_free(sig);

    // ML-KEM-1024 encryption keypair
    OQS_KEM* kem = OQS_KEM_new(OQS_KEM_alg_ml_kem_1024);
    if (!kem) {
        throw std::runtime_error("Failed to create ML-KEM-1024 context");
    }
    id.kem_pk_.resize(kem->length_public_key);
    id.kem_sk_.resize(kem->length_secret_key);
    if (OQS_KEM_keypair(kem, id.kem_pk_.data(), id.kem_sk_.data()) != OQS_SUCCESS) {
        OQS_KEM_free(kem);
        throw std::runtime_error("ML-KEM-1024 keypair generation failed");
    }
    OQS_KEM_free(kem);

    derive_namespace(id.signing_pk_, id.namespace_id_);
    return id;
}

Identity Identity::load_from(const std::filesystem::path& dir) {
    auto pub_path  = dir / "identity.pub";
    auto key_path  = dir / "identity.key";
    auto kpub_path = dir / "identity.kpub";
    auto kem_path  = dir / "identity.kem";

    if (!std::filesystem::exists(pub_path))
        throw std::runtime_error("Signing public key not found: " + pub_path.string());
    if (!std::filesystem::exists(key_path))
        throw std::runtime_error("Signing secret key not found: " + key_path.string());
    if (!std::filesystem::exists(kpub_path))
        throw std::runtime_error("KEM public key not found: " + kpub_path.string());
    if (!std::filesystem::exists(kem_path))
        throw std::runtime_error("KEM secret key not found: " + kem_path.string());

    Identity id;
    id.signing_pk_ = read_file(pub_path);
    id.signing_sk_ = read_file(key_path);
    id.kem_pk_     = read_file(kpub_path);
    id.kem_sk_     = read_file(kem_path);

    if (id.signing_pk_.size() != SIGNING_PK_SIZE)
        throw std::runtime_error("Invalid signing pubkey size: " + std::to_string(id.signing_pk_.size()));
    if (id.signing_sk_.size() != SIGNING_SK_SIZE)
        throw std::runtime_error("Invalid signing seckey size: " + std::to_string(id.signing_sk_.size()));
    if (id.kem_pk_.size() != KEM_PK_SIZE)
        throw std::runtime_error("Invalid KEM pubkey size: " + std::to_string(id.kem_pk_.size()));
    if (id.kem_sk_.size() != KEM_SK_SIZE)
        throw std::runtime_error("Invalid KEM seckey size: " + std::to_string(id.kem_sk_.size()));

    derive_namespace(id.signing_pk_, id.namespace_id_);
    return id;
}

void Identity::save_to(const std::filesystem::path& dir) const {
    std::filesystem::create_directories(dir);

    write_file(dir / "identity.pub",  signing_pk_);
    write_file(dir / "identity.key",  signing_sk_);
    write_file(dir / "identity.kpub", kem_pk_);
    write_file(dir / "identity.kem",  kem_sk_);

    // Restrict secret keys to owner-only
    namespace fs = std::filesystem;
    auto perms = fs::perms::owner_read | fs::perms::owner_write;
    fs::permissions(dir / "identity.key",  perms, fs::perm_options::replace);
    fs::permissions(dir / "identity.kem",  perms, fs::perm_options::replace);
}

std::vector<uint8_t> Identity::sign(std::span<const uint8_t> message) const {
    OQS_SIG* sig = OQS_SIG_new(OQS_SIG_alg_ml_dsa_87);
    if (!sig) {
        throw std::runtime_error("Failed to create ML-DSA-87 context");
    }

    std::vector<uint8_t> signature(sig->length_signature);
    size_t sig_len = 0;

    OQS_STATUS rc = OQS_SIG_sign(sig, signature.data(), &sig_len,
                                  message.data(), message.size(),
                                  signing_sk_.data());
    OQS_SIG_free(sig);

    if (rc != OQS_SUCCESS) {
        throw std::runtime_error("ML-DSA-87 signing failed");
    }

    signature.resize(sig_len);
    return signature;
}

std::vector<uint8_t> Identity::export_public_keys() const {
    std::vector<uint8_t> out;
    out.reserve(EXPORT_SIZE);
    out.insert(out.end(), signing_pk_.begin(), signing_pk_.end());
    out.insert(out.end(), kem_pk_.begin(), kem_pk_.end());
    return out;
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>>
Identity::load_public_keys(std::span<const uint8_t> data) {
    if (data.size() != EXPORT_SIZE) {
        throw std::runtime_error("Invalid public key export size: expected " +
            std::to_string(EXPORT_SIZE) + ", got " + std::to_string(data.size()));
    }
    return {
        {data.begin(), data.begin() + SIGNING_PK_SIZE},
        {data.begin() + SIGNING_PK_SIZE, data.end()}
    };
}

} // namespace chromatindb::cli
