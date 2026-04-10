#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::relay::translate {

/// Result of json_to_binary translation.
struct TranslateResult {
    uint8_t wire_type;
    std::vector<uint8_t> payload;
};

/// JSON -> binary payload for sending to node.
/// Looks up MessageSchema by type name, iterates FieldSpec for encoding.
/// Special-cases FlatBuffer types (Data=8).
/// Returns nullopt on invalid input or unknown type.
std::optional<TranslateResult> json_to_binary(const nlohmann::json& msg);

/// Binary payload -> JSON for sending to client.
/// type: wire type from TransportMessage.
/// payload: raw payload bytes (after transport envelope decode).
/// Returns nullopt on decode failure.
std::optional<nlohmann::json> binary_to_json(uint8_t type,
                                              std::span<const uint8_t> payload);

/// Whether a response type should be sent as binary WS frame.
/// Per D-20: ReadResponse(32) and BatchReadResponse(54) -> true.
inline bool is_binary_response(uint8_t type) {
    return type == 32 || type == 54;
}

} // namespace chromatindb::relay::translate
