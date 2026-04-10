#include "relay/translate/translator.h"
#include "relay/translate/json_schema.h"
#include "relay/translate/type_registry.h"
#include "relay/util/base64.h"
#include "relay/util/endian.h"
#include "relay/util/hex.h"
#include "relay/wire/blob_codec.h"

#include <cstring>

namespace chromatindb::relay::translate {

// Stub implementations -- Task 2 provides the full table-driven translator.

std::optional<TranslateResult> json_to_binary(const nlohmann::json& /*msg*/) {
    return std::nullopt;
}

std::optional<nlohmann::json> binary_to_json(uint8_t /*type*/,
                                              std::span<const uint8_t> /*payload*/) {
    return std::nullopt;
}

} // namespace chromatindb::relay::translate
