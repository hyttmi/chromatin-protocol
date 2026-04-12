#include "db/engine/chunking.h"

#include <algorithm>
#include <cstring>

#include "db/util/endian.h"

namespace chromatindb::engine {

bool is_manifest(std::span<const uint8_t> data) {
    // STUB: will be implemented in GREEN phase
    return false;
}

std::optional<std::vector<std::array<uint8_t, 32>>> parse_manifest(
    std::span<const uint8_t> data) {
    // STUB: will be implemented in GREEN phase
    return std::nullopt;
}

std::vector<uint8_t> make_manifest_data(
    const std::vector<std::array<uint8_t, 32>>& chunk_hashes) {
    // STUB: will be implemented in GREEN phase
    return {};
}

} // namespace chromatindb::engine
