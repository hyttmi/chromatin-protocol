#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::relay::wire {

struct DecodedBlob {
    std::vector<uint8_t> namespace_id;   // 32 bytes
    std::vector<uint8_t> pubkey;         // 2592 bytes
    std::vector<uint8_t> data;           // variable
    uint32_t ttl;
    uint64_t timestamp;
    std::vector<uint8_t> signature;      // 4627 bytes
};

/// Encode a blob as FlatBuffer bytes (for Data message type 8).
std::vector<uint8_t> encode_blob(const DecodedBlob& blob);

/// Decode FlatBuffer blob bytes. Returns nullopt if invalid.
std::optional<DecodedBlob> decode_blob(std::span<const uint8_t> data);

} // namespace chromatindb::relay::wire
