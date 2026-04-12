#include "db/engine/chunking.h"

#include <algorithm>
#include <cstring>

#include "db/util/endian.h"

namespace chromatindb::engine {

bool is_manifest(std::span<const uint8_t> data) {
    // Minimum: 4 (magic) + 4 (count) = 8 bytes
    if (data.size() < 8) return false;
    return std::equal(data.begin(), data.begin() + 4, MANIFEST_MAGIC.begin());
}

std::optional<std::vector<std::array<uint8_t, 32>>> parse_manifest(
    std::span<const uint8_t> data) {
    // Minimum: 4 (magic) + 4 (count) = 8 bytes
    if (data.size() < 8) return std::nullopt;

    // Check magic prefix
    if (!std::equal(data.begin(), data.begin() + 4, MANIFEST_MAGIC.begin())) {
        return std::nullopt;
    }

    // Read chunk count (big-endian)
    uint32_t count = chromatindb::util::read_u32_be(data.subspan(4, 4));

    // Reject zero count (empty manifest)
    if (count == 0) return std::nullopt;

    // Reject count exceeding MAX_CHUNK_COUNT
    if (count > MAX_CHUNK_COUNT) return std::nullopt;

    // Validate exact size: 8 + count * 32
    if (data.size() != 8 + static_cast<size_t>(count) * 32) {
        return std::nullopt;
    }

    // Extract hashes in order
    std::vector<std::array<uint8_t, 32>> hashes;
    hashes.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::array<uint8_t, 32> hash;
        std::memcpy(hash.data(), data.data() + 8 + i * 32, 32);
        hashes.push_back(hash);
    }

    return hashes;
}

std::vector<uint8_t> make_manifest_data(
    const std::vector<std::array<uint8_t, 32>>& chunk_hashes) {
    // Format: [CHNK:4][chunk_count:4 BE][hash_1:32][hash_2:32]...
    std::vector<uint8_t> data;
    data.reserve(8 + chunk_hashes.size() * 32);

    // Magic prefix
    data.insert(data.end(), MANIFEST_MAGIC.begin(), MANIFEST_MAGIC.end());

    // Chunk count (big-endian u32)
    chromatindb::util::write_u32_be(data, static_cast<uint32_t>(chunk_hashes.size()));

    // Chunk hashes in order
    for (const auto& hash : chunk_hashes) {
        data.insert(data.end(), hash.begin(), hash.end());
    }

    return data;
}

} // namespace chromatindb::engine
