#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "db/util/endian.h"

namespace chromatindb::util {

/// Extract the namespace (first 32 bytes) from a payload.
/// Throws std::out_of_range if payload has fewer than 32 bytes.
inline std::array<uint8_t, 32> extract_namespace(std::span<const uint8_t> payload) {
    if (payload.size() < 32) {
        throw std::out_of_range("extract_namespace: need 32 bytes, got " +
                                std::to_string(payload.size()));
    }
    std::array<uint8_t, 32> ns{};
    std::memcpy(ns.data(), payload.data(), 32);
    return ns;
}

/// Extract the namespace (first 32 bytes) and hash (next 32 bytes) from a payload.
/// Throws std::out_of_range if payload has fewer than 64 bytes.
inline std::pair<std::array<uint8_t, 32>, std::array<uint8_t, 32>>
extract_namespace_hash(std::span<const uint8_t> payload) {
    if (payload.size() < 64) {
        throw std::out_of_range("extract_namespace_hash: need 64 bytes, got " +
                                std::to_string(payload.size()));
    }
    std::array<uint8_t, 32> ns{};
    std::array<uint8_t, 32> hash{};
    std::memcpy(ns.data(), payload.data(), 32);
    std::memcpy(hash.data(), payload.data() + 32, 32);
    return {ns, hash};
}

/// Encode a namespace + hash pair into a 64-byte vector.
inline std::vector<uint8_t> encode_namespace_hash(
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> hash) {
    std::vector<uint8_t> result;
    result.reserve(64);
    result.insert(result.end(), ns.begin(), ns.end());
    result.insert(result.end(), hash.begin(), hash.end());
    return result;
}

/// Encode a blob reference in BlobNotify format (77 bytes):
/// namespace:32 + hash:32 + seq_num_be:8 + size_be:4 + tombstone:1
inline std::vector<uint8_t> encode_blob_ref(
    std::span<const uint8_t, 32> ns,
    std::span<const uint8_t, 32> hash,
    uint64_t seq_num,
    uint32_t size,
    bool tombstone) {
    std::vector<uint8_t> result;
    result.reserve(77);
    result.insert(result.end(), ns.begin(), ns.end());
    result.insert(result.end(), hash.begin(), hash.end());
    write_u64_be(result, seq_num);
    write_u32_be(result, size);
    result.push_back(tombstone ? 0x01 : 0x00);
    return result;
}

} // namespace chromatindb::util
