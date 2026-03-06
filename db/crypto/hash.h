#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace chromatindb::crypto {

/// SHA3-256 hash size in bytes.
constexpr size_t SHA3_256_SIZE = 32;

/// Compute SHA3-256 hash of input data.
/// @return 32-byte hash digest.
std::array<uint8_t, SHA3_256_SIZE> sha3_256(std::span<const uint8_t> input);

/// Convenience overload accepting raw pointer + length.
std::array<uint8_t, SHA3_256_SIZE> sha3_256(const void* data, size_t len);

} // namespace chromatindb::crypto
