#pragma once

#include "db/crypto/secure_bytes.h"
#include <filesystem>

namespace chromatindb::crypto {

/// Load master key from data_dir/master.key, or generate a new one.
/// Generated keys are 32 random bytes written with 0600 permissions.
/// @throws std::runtime_error if file exists but is wrong size, or cannot be read/written.
SecureBytes load_or_generate_master_key(const std::filesystem::path& data_dir);

/// Derive a blob encryption key from a master key using HKDF-SHA256.
/// Context label: "chromatindb-dare-v1". Output: 32 bytes (AEAD key size).
SecureBytes derive_blob_key(const SecureBytes& master_key);

} // namespace chromatindb::crypto
