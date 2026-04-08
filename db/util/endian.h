#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace chromatindb::util {

// =============================================================================
// Vector-append writers (append bytes in big-endian order)
// =============================================================================

/// Append a 16-bit value in big-endian order to a byte vector.
inline void write_u16_be(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

/// Append a 32-bit value in big-endian order to a byte vector.
inline void write_u32_be(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

/// Append a 64-bit value in big-endian order to a byte vector.
inline void write_u64_be(std::vector<uint8_t>& buf, uint64_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 56) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 48) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 40) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 32) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

// =============================================================================
// Fixed-buffer writers (write to pre-allocated buffer)
// =============================================================================

/// Write a 32-bit value in big-endian order to a pre-allocated buffer.
inline void store_u32_be(uint8_t* out, uint32_t val) {
    out[0] = static_cast<uint8_t>(val >> 24);
    out[1] = static_cast<uint8_t>(val >> 16);
    out[2] = static_cast<uint8_t>(val >> 8);
    out[3] = static_cast<uint8_t>(val);
}

/// Write a 64-bit value in big-endian order to a pre-allocated buffer.
inline void store_u64_be(uint8_t* out, uint64_t val) {
    for (int i = 7; i >= 0; --i) {
        out[7 - i] = static_cast<uint8_t>(val >> (i * 8));
    }
}

// =============================================================================
// Span readers (bounds-checked)
// =============================================================================

/// Read a 16-bit big-endian value from a span.
/// Throws std::out_of_range if span has fewer than 2 bytes.
inline uint16_t read_u16_be(std::span<const uint8_t> data) {
    if (data.size() < 2) {
        throw std::out_of_range("read_u16_be: need 2 bytes, got " +
                                std::to_string(data.size()));
    }
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) |
        static_cast<uint16_t>(data[1]));
}

/// Read a 32-bit big-endian value from a span.
/// Throws std::out_of_range if span has fewer than 4 bytes.
inline uint32_t read_u32_be(std::span<const uint8_t> data) {
    if (data.size() < 4) {
        throw std::out_of_range("read_u32_be: need 4 bytes, got " +
                                std::to_string(data.size()));
    }
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

/// Read a 64-bit big-endian value from a span.
/// Throws std::out_of_range if span has fewer than 8 bytes.
inline uint64_t read_u64_be(std::span<const uint8_t> data) {
    if (data.size() < 8) {
        throw std::out_of_range("read_u64_be: need 8 bytes, got " +
                                std::to_string(data.size()));
    }
    return (static_cast<uint64_t>(data[0]) << 56) |
           (static_cast<uint64_t>(data[1]) << 48) |
           (static_cast<uint64_t>(data[2]) << 40) |
           (static_cast<uint64_t>(data[3]) << 32) |
           (static_cast<uint64_t>(data[4]) << 24) |
           (static_cast<uint64_t>(data[5]) << 16) |
           (static_cast<uint64_t>(data[6]) << 8) |
           static_cast<uint64_t>(data[7]);
}

// =============================================================================
// Raw-pointer readers (unchecked, for pre-validated internal paths)
// =============================================================================

/// Read a 32-bit big-endian value from a raw pointer.
/// No bounds checking -- caller must ensure at least 4 valid bytes.
inline uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

/// Read a 64-bit big-endian value from a raw pointer.
/// No bounds checking -- caller must ensure at least 8 valid bytes.
inline uint64_t read_u64_be(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8) |
           static_cast<uint64_t>(p[7]);
}

// =============================================================================
// Overflow-checked arithmetic (for protocol parsing)
// =============================================================================

/// Checked multiplication: returns nullopt on overflow.
inline std::optional<size_t> checked_mul(size_t a, size_t b) {
    if (a == 0 || b == 0) return size_t{0};
    size_t result = a * b;
    if (result / a != b) return std::nullopt;
    return result;
}

/// Checked addition: returns nullopt on overflow.
inline std::optional<size_t> checked_add(size_t a, size_t b) {
    size_t result = a + b;
    if (result < a) return std::nullopt;
    return result;
}

} // namespace chromatindb::util
