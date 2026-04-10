#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace chromatindb::relay::util {

/// Encode binary data to base64 string using OpenSSL EVP.
std::string base64_encode(std::span<const uint8_t> data);

/// Decode a base64 string to binary data using OpenSSL EVP.
/// Returns nullopt on invalid base64 input.
std::optional<std::vector<uint8_t>> base64_decode(std::string_view encoded);

} // namespace chromatindb::relay::util
