#pragma once

#include "db/crypto/aead.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::net {

/// Maximum frame payload size (16 MB).
constexpr uint32_t MAX_FRAME_SIZE = 16 * 1024 * 1024;

/// Frame header size (4-byte big-endian uint32_t length prefix).
constexpr size_t FRAME_HEADER_SIZE = 4;

/// Build a counter-based nonce for AEAD.
/// Format: 4 zero bytes + 8-byte big-endian counter.
/// Each direction has its own counter starting at 0.
std::array<uint8_t, crypto::AEAD::NONCE_SIZE> make_nonce(uint64_t counter);

/// Encrypt plaintext and wrap in a length-prefixed frame.
/// Returns: [4-byte BE ciphertext_length][ciphertext with AEAD tag]
/// Uses empty associated data.
std::vector<uint8_t> write_frame(
    std::span<const uint8_t> plaintext,
    std::span<const uint8_t> key,
    uint64_t counter);

/// Result of reading a frame from a buffer.
struct FrameResult {
    std::vector<uint8_t> plaintext;
    size_t bytes_consumed;  // Total bytes consumed from input (header + ciphertext)
};

/// Parse length prefix from buffer, validate size, decrypt ciphertext.
/// Returns: plaintext on success, nullopt on decrypt failure.
/// Throws: runtime_error if frame exceeds MAX_FRAME_SIZE.
/// Throws: runtime_error if buffer is too short for the declared frame.
std::optional<FrameResult> read_frame(
    std::span<const uint8_t> buffer,
    std::span<const uint8_t> key,
    uint64_t counter);

} // namespace chromatindb::net
