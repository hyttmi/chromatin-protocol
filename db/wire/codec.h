#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace chromatindb::wire {

/// Structured blob data for codec operations (Phase 122 shape).
/// signer_hint = SHA3-256(signing pubkey); resolves to a 2592-byte ML-DSA-87
/// pubkey via Storage::get_owner_pubkey or delegation_map.
struct BlobData {
    std::array<uint8_t, 32> signer_hint{};
    std::vector<uint8_t> data;
    uint32_t ttl = 0;
    uint64_t timestamp = 0;
    std::vector<uint8_t> signature;
};

/// Serialize BlobData to FlatBuffer bytes.
/// Uses ForceDefaults for deterministic encoding.
std::vector<uint8_t> encode_blob(const BlobData& blob);

/// Deserialize FlatBuffer bytes to BlobData.
/// @throws std::runtime_error if buffer is invalid.
BlobData decode_blob(std::span<const uint8_t> buffer);

/// Build canonical signing input: SHA3-256(target_namespace || data || ttl_be32 || timestamp_be64).
/// Returns a 32-byte digest that is then signed -- independent of FlatBuffer format.
/// Phase 122 D-01: byte output IDENTICAL to pre-122 for the same input bytes; only
/// the parameter name changes. The signer commits to target_namespace (not signer_hint),
/// preventing cross-namespace replay by delegates with multi-namespace authority.
/// Uses incremental SHA3-256 hashing internally (zero intermediate allocation).
std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t> target_namespace,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp);

/// Compute SHA3-256 hash of the full encoded FlatBuffer blob.
/// Used for content-addressed deduplication.
std::array<uint8_t, 32> blob_hash(std::span<const uint8_t> encoded_blob);

// =============================================================================
// TTL / Expiry utilities
// =============================================================================

/// Compute expiry timestamp with saturating arithmetic (TTL-03).
/// Returns 0 for permanent blobs (ttl == 0).
/// Clamps to UINT64_MAX on overflow (effectively permanent).
inline uint64_t saturating_expiry(uint64_t timestamp, uint32_t ttl) {
    if (ttl == 0) return 0;  // Permanent
    uint64_t ttl64 = static_cast<uint64_t>(ttl);
    if (timestamp > UINT64_MAX - ttl64) return UINT64_MAX;  // Overflow -> clamp
    return timestamp + ttl64;
}

/// Check if a blob has expired (TTL-01, TTL-02).
/// Permanent blobs (ttl == 0) never expire.
/// Uses saturating arithmetic for overflow safety (TTL-03).
inline bool is_blob_expired(const BlobData& blob, uint64_t now) {
    if (blob.ttl == 0) return false;  // Permanent
    return saturating_expiry(blob.timestamp, blob.ttl) <= now;
}

// =============================================================================
// Tombstone utilities
// =============================================================================

/// 4-byte magic prefix identifying tombstone data.
inline constexpr std::array<uint8_t, 4> TOMBSTONE_MAGIC = {0xDE, 0xAD, 0xBE, 0xEF};

/// Tombstone data size: 4-byte magic + 32-byte target hash.
inline constexpr size_t TOMBSTONE_DATA_SIZE = 36;

/// Check if blob data is a tombstone (magic prefix + 32-byte target hash).
bool is_tombstone(std::span<const uint8_t> data);

/// Extract the 32-byte target blob hash from tombstone data.
/// @pre is_tombstone(data) must be true.
std::array<uint8_t, 32> extract_tombstone_target(std::span<const uint8_t> data);

/// Create 36-byte tombstone data: magic prefix + target hash.
std::vector<uint8_t> make_tombstone_data(std::span<const uint8_t, 32> target_hash);

// =============================================================================
// Delegation utilities
// =============================================================================

/// 4-byte magic prefix identifying delegation data.
inline constexpr std::array<uint8_t, 4> DELEGATION_MAGIC = {0xDE, 0x1E, 0x6A, 0x7E};

/// ML-DSA-87 delegate public key size in bytes.
inline constexpr size_t DELEGATION_PUBKEY_SIZE = 2592;

/// Delegation data size: 4-byte magic + 2592-byte delegate pubkey.
inline constexpr size_t DELEGATION_DATA_SIZE = 4 + DELEGATION_PUBKEY_SIZE;

/// Check if blob data is a delegation (magic prefix + delegate pubkey).
bool is_delegation(std::span<const uint8_t> data);

/// Extract the delegate public key from delegation data.
/// @pre is_delegation(data) must be true.
std::vector<uint8_t> extract_delegate_pubkey(std::span<const uint8_t> data);

/// Create delegation data: magic prefix + delegate public key.
std::vector<uint8_t> make_delegation_data(std::span<const uint8_t> delegate_pubkey);

// =============================================================================
// Public key blob utilities
// =============================================================================

/// 4-byte magic prefix identifying a published public key blob.
inline constexpr std::array<uint8_t, 4> PUBKEY_MAGIC = {0x50, 0x55, 0x42, 0x4B}; // "PUBK"

/// Published pubkey data size: 4-byte magic + 2592-byte signing pk + 1568-byte KEM pk.
inline constexpr size_t PUBKEY_DATA_SIZE = 4 + 2592 + 1568;

/// Check if blob data is a published public key.
inline bool is_pubkey_blob(std::span<const uint8_t> data) {
    return data.size() == PUBKEY_DATA_SIZE &&
           data[0] == PUBKEY_MAGIC[0] && data[1] == PUBKEY_MAGIC[1] &&
           data[2] == PUBKEY_MAGIC[2] && data[3] == PUBKEY_MAGIC[3];
}

/// Extract the 2592-byte signing pubkey from a verified PUBK blob's data.
/// @pre is_pubkey_blob(data) must be true. Caller is responsible.
/// PUBK body: [magic:4][signing_pk:2592][kem_pk:1568] = 4164 bytes.
inline std::span<const uint8_t, 2592> extract_pubk_signing_pk(std::span<const uint8_t> data) {
    return std::span<const uint8_t, 2592>(data.data() + 4, 2592);
}

// =============================================================================
// Generic blob type extraction
// =============================================================================

/// Extract the first 4 bytes of blob data as a type prefix.
/// Returns {0,0,0,0} for data shorter than 4 bytes.
inline std::array<uint8_t, 4> extract_blob_type(std::span<const uint8_t> data) {
    std::array<uint8_t, 4> type{};
    if (data.size() >= 4) {
        std::memcpy(type.data(), data.data(), 4);
    }
    return type;
}

} // namespace chromatindb::wire
