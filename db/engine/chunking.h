#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::engine {

/// 4-byte magic prefix identifying chunking manifest data: "CHNK"
inline constexpr std::array<uint8_t, 4> MANIFEST_MAGIC = {0x43, 0x48, 0x4E, 0x4B};

/// Fixed chunk size: 1 MiB (1,048,576 bytes). Not configurable (D-01).
inline constexpr size_t CHUNK_SIZE = 1048576;

/// Maximum number of chunks per manifest (~97.6 GiB cap).
/// Prevents memory exhaustion on read_chunked for corrupt manifests.
inline constexpr uint32_t MAX_CHUNK_COUNT = 100000;

/// Check if blob data is a chunking manifest (CHNK magic prefix, minimum 8 bytes).
bool is_manifest(std::span<const uint8_t> data);

/// Parse manifest data into ordered list of chunk hashes.
/// Returns nullopt if format is invalid (bad magic, wrong size, zero count,
/// count exceeding MAX_CHUNK_COUNT).
/// Format: [CHNK:4][chunk_count:4 BE][hash_1:32][hash_2:32]...
std::optional<std::vector<std::array<uint8_t, 32>>> parse_manifest(
    std::span<const uint8_t> data);

/// Build manifest data from ordered list of chunk hashes.
/// Format: [CHNK:4][chunk_count:4 BE][hash_1:32][hash_2:32]...
std::vector<uint8_t> make_manifest_data(
    const std::vector<std::array<uint8_t, 32>>& chunk_hashes);

} // namespace chromatindb::engine
