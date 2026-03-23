#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace chromatindb::util {

/// Convert bytes to lowercase hex string.
inline std::string to_hex(std::span<const uint8_t> bytes) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        result += hex_chars[(b >> 4) & 0xF];
        result += hex_chars[b & 0xF];
    }
    return result;
}

/// Convert bytes to lowercase hex string, truncating to max_bytes.
/// Used for abbreviated key logging.
inline std::string to_hex(std::span<const uint8_t> bytes, size_t max_bytes) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    std::string result;
    size_t len = std::min(bytes.size(), max_bytes);
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result += hex_chars[(bytes[i] >> 4) & 0xF];
        result += hex_chars[bytes[i] & 0xF];
    }
    return result;
}

/// Decode a hex string to a byte vector.
/// Throws std::invalid_argument on odd-length or invalid hex chars.
inline std::vector<uint8_t> from_hex(const std::string& hex_str) {
    if (hex_str.size() % 2 != 0) {
        throw std::invalid_argument("from_hex: odd-length hex string (" +
                                    std::to_string(hex_str.size()) + " chars)");
    }
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        throw std::invalid_argument(std::string("from_hex: invalid hex char '") + c + "'");
    };
    std::vector<uint8_t> result(hex_str.size() / 2);
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<uint8_t>(
            (nibble(hex_str[i * 2]) << 4) | nibble(hex_str[i * 2 + 1]));
    }
    return result;
}

/// Decode a hex string to a fixed-size byte array.
/// Throws std::invalid_argument if length != N*2 or on invalid hex chars.
template <size_t N>
inline std::array<uint8_t, N> from_hex_fixed(const std::string& hex_str) {
    if (hex_str.size() != N * 2) {
        throw std::invalid_argument("from_hex_fixed: expected " +
                                    std::to_string(N * 2) + " hex chars, got " +
                                    std::to_string(hex_str.size()));
    }
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        throw std::invalid_argument(std::string("from_hex_fixed: invalid hex char '") + c + "'");
    };
    std::array<uint8_t, N> result{};
    for (size_t i = 0; i < N; ++i) {
        result[i] = static_cast<uint8_t>(
            (nibble(hex_str[i * 2]) << 4) | nibble(hex_str[i * 2 + 1]));
    }
    return result;
}

} // namespace chromatindb::util
