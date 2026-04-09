#include "relay/translate/type_registry.h"

#include <algorithm>

namespace chromatindb::relay::translate {

std::optional<uint8_t> type_from_string(std::string_view name) {
    // Binary search on sorted TYPE_REGISTRY (sorted by json_name).
    auto it = std::lower_bound(
        std::begin(TYPE_REGISTRY), std::end(TYPE_REGISTRY), name,
        [](const TypeEntry& e, std::string_view n) { return e.json_name < n; });
    if (it != std::end(TYPE_REGISTRY) && it->json_name == name) {
        return it->wire_type;
    }
    return std::nullopt;
}

std::optional<std::string_view> type_to_string(uint8_t wire_type) {
    // Linear scan (small array, infrequent calls for outbound messages).
    for (const auto& entry : TYPE_REGISTRY) {
        if (entry.wire_type == wire_type) {
            return entry.json_name;
        }
    }
    return std::nullopt;
}

} // namespace chromatindb::relay::translate
