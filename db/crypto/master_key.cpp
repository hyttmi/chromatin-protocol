#include "db/crypto/master_key.h"
#include "db/crypto/aead.h"
#include "db/crypto/kdf.h"

#include <sodium.h>
#include <spdlog/spdlog.h>

#include <fstream>
#include <stdexcept>

namespace chromatindb::crypto {

SecureBytes load_or_generate_master_key(const std::filesystem::path& data_dir) {
    namespace fs = std::filesystem;

    ensure_sodium_init();

    auto key_path = data_dir / "master.key";

    if (fs::exists(key_path)) {
        // Load existing master key
        std::ifstream f(key_path, std::ios::binary);
        if (!f) {
            throw std::runtime_error("Cannot open master.key: " + key_path.string());
        }

        SecureBytes key(AEAD::KEY_SIZE);
        f.read(reinterpret_cast<char*>(key.data()), AEAD::KEY_SIZE);
        auto bytes_read = f.gcount();

        if (static_cast<size_t>(bytes_read) != AEAD::KEY_SIZE) {
            throw std::runtime_error(
                "Invalid master.key size: expected " +
                std::to_string(AEAD::KEY_SIZE) + ", got " +
                std::to_string(bytes_read));
        }

        spdlog::info("Loaded master key from {}", key_path.string());
        return key;
    }

    // Generate new master key
    fs::create_directories(data_dir);

    SecureBytes key(AEAD::KEY_SIZE);
    randombytes_buf(key.data(), key.size());

    // Write with binary mode
    {
        std::ofstream f(key_path, std::ios::binary);
        if (!f) {
            throw std::runtime_error("Cannot write master.key: " + key_path.string());
        }
        f.write(reinterpret_cast<const char*>(key.data()), key.size());
    }

    // Set restricted permissions: owner read + write only (0600)
    fs::permissions(key_path,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);

    spdlog::info("Generated master key at {}", key_path.string());
    return key;
}

SecureBytes derive_blob_key(const SecureBytes& master_key) {
    return KDF::derive(
        {},                   // empty salt
        master_key.span(),    // input key material
        "chromatindb-dare-v1", // context/info label
        AEAD::KEY_SIZE);      // 32 bytes output
}

} // namespace chromatindb::crypto
