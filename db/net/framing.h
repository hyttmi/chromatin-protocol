#pragma once

#include "db/crypto/aead.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chromatindb::net {

/// Maximum frame payload size (2 MiB — per-frame limit, not per-logical-message).
/// In chunked mode, each sub-frame is <= 1 MiB + AEAD overhead, well within this.
/// Shrunk from 110 MiB in Phase 128 per FRAME-01; invariant pinned by the paired
/// static_asserts below.
constexpr uint32_t MAX_FRAME_SIZE = 2 * 1024 * 1024;

/// Build-time upper ceiling on the operator-configurable blob cap (64 MiB).
/// This is NOT the current blob cap at runtime — that lives in Config::blob_max_bytes.
/// This constant survives only as (a) the upper bound in validate_config's bounds
/// check on blob_max_bytes, and (b) the static_assert below pinning the relationship
/// between the operator cap and the build-time ceiling. Renamed in Phase 128
/// (D-03/D-05) from the previous ambiguous "data size" name to make
/// "build-time invariant" vs "operational cap" unambiguous.
constexpr uint64_t MAX_BLOB_DATA_HARD_CEILING = 64ULL * 1024 * 1024;

/// Streaming threshold: payloads >= this use chunked sub-frames (1 MiB per D-02).
/// Matches the node's internal chunk granularity.
constexpr uint64_t STREAMING_THRESHOLD = 1048576;

/// Worst-case overhead of the TransportMessage FlatBuffer envelope around a
/// plaintext payload. Root table + vtable + vector prefix typically sum to
/// ~20-32 bytes; 64 is a conservative upper bound used by the non-chunked
/// send assertion in Connection::enqueue_send to allow the encoded
/// TransportMessage envelope around a near-threshold plaintext payload.
constexpr size_t TRANSPORT_ENVELOPE_MARGIN = 64;

/// Worst-case wire-layer overhead wrapping a blob's data. Covers:
///   - ML-DSA-87 signature (up to 4627 bytes)
///   - signer_hint (32 bytes)
///   - BlobWriteBody + Blob FlatBuffer table overhead (~100 bytes)
///   - CENV envelope header + wrapped DEKs for typical group sizes (up to ~512 KiB
///     for many-recipient groups with per-recipient ML-KEM-1024 ciphertexts)
/// Used as the wire-layer defensive allocation bound in chunked reassembly.
/// NOT the semantic blob-size cap — that is Config::blob_max_bytes, checked at
/// Engine::ingest on the decoded blob.data.size(). This constant is a PROTOCOL
/// wire invariant; the semantic cap is an OPERATOR runtime knob.
constexpr uint64_t MAX_BLOB_ENVELOPE_OVERHEAD = 1ULL * 1024 * 1024;  // 1 MiB

/// Wire-layer upper bound for a legitimate chunked-reassembly total_payload_size:
/// the protocol hard ceiling on blob.data plus worst-case envelope overhead.
/// Anything larger is rejected at the transport layer as a defensive allocation
/// bound. Used by Connection::recv_chunked.
constexpr uint64_t MAX_CHUNKED_WIRE_PAYLOAD =
    MAX_BLOB_DATA_HARD_CEILING + MAX_BLOB_ENVELOPE_OVERHEAD;

static_assert(MAX_BLOB_ENVELOPE_OVERHEAD >= 8192,
    "MAX_BLOB_ENVELOPE_OVERHEAD must cover ML-DSA-87 signature (4627 bytes) + "
    "signer_hint (32 bytes) + CENV header + FlatBuffer table overhead (~4 KiB). "
    "Shrinking below 8 KiB risks rejecting legitimate chunked blobs at the "
    "protocol hard ceiling.");

static_assert(MAX_FRAME_SIZE >= 2 * STREAMING_THRESHOLD,
    "MAX_FRAME_SIZE must admit one full streaming sub-frame plus headroom "
    "for AEAD tag (16B), length prefix (4B), and transport envelope. "
    "Shrinking either constant without re-checking the other breaks the "
    "invariant.");

static_assert(MAX_FRAME_SIZE <= 2 * STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN,
    "MAX_FRAME_SIZE must stay close to one streaming sub-frame + AEAD margin; "
    "raising it decouples framing from streaming and breaks Phase 126's audit.");

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
