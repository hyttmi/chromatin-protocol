#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace chromatindb::wire {

/// Structured blob data for codec operations.
struct BlobData {
    std::array<uint8_t, 32> namespace_id{};
    std::vector<uint8_t> pubkey;
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

/// Build canonical signing input: namespace(32) || data(var) || ttl_le(4) || timestamp_le(8).
/// This is what gets hashed then signed -- independent of FlatBuffer format.
std::vector<uint8_t> build_signing_input(
    std::span<const uint8_t> namespace_id,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp);

/// Compute SHA3-256 hash of the full encoded FlatBuffer blob.
/// Used for content-addressed deduplication.
std::array<uint8_t, 32> blob_hash(std::span<const uint8_t> encoded_blob);

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

} // namespace chromatindb::wire
