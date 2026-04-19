#pragma once

#include "cli/src/wire.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace chromatindb::cli {
// Forward decls — keep include graph shallow.
class Connection;
class Identity;
namespace cmd { struct ConnectOpts; }
}

namespace chromatindb::cli::chunked {

// =============================================================================
// Chunked large files (Phase 119 — CHUNK-01..CHUNK-05)
// =============================================================================
//
// Contracts:
//  - put_chunked and rm_chunked are free functions. All per-invocation state
//    (Connection ref, Identity ref, recipient spans, rid counter,
//    rid_to_chunk_index map, retry counters, Sha3Hasher) lives in the
//    function body. No static/global state on the helper.
//  - Both functions ride Phase 120 primitives (Connection::send_async +
//    Connection::recv) — one shared Connection, no second handshake, no
//    second thread. Single-sender / single-reader (PIPE-02) preserved.
//  - Outer-magic placement per D-13: chunk blob.data =
//        [CDAT magic:4][CENV envelope(raw chunk plaintext)]
//    manifest blob.data =
//        [CPAR magic:4][CENV envelope([CPAR magic:4][Manifest FlatBuffer])]
//    so Phase 117 type indexing works on the outer bytes and on-path
//    observers see only the role of the blob, not its content.
//  - Memory envelope per D-11: the plaintext read buffer
//    (CHUNK_SIZE_BYTES_DEFAULT = 16 MiB) is reused; plaintext is passed to
//    envelope::encrypt by span and the caller releases the buffer eagerly
//    before the next read. Peak working memory stays bounded to
//    ~kPipelineDepth × one-encoded-chunk + one reusable read buffer.
//  - Retry policy per D-15: up to RETRY_ATTEMPTS per chunk with exponential
//    backoff. Each retry re-signs (ML-DSA-87 is non-deterministic, so the
//    hash differs), and the chunk_index -> final_hash binding happens at
//    drain time (post-WriteAck), never at send time.

inline constexpr uint64_t CHUNK_THRESHOLD_BYTES = 400ULL * 1024 * 1024;      // D-02
inline constexpr uint64_t MAX_CHUNKED_FILE_SIZE = 1024ULL * 1024 * 1024 * 1024; // 1 TiB (D-14)
inline constexpr int      RETRY_ATTEMPTS        = 3;                         // D-15
inline constexpr int      RETRY_BACKOFF_MS[RETRY_ATTEMPTS] = { 250, 1000, 4000 };

/// Pipelined chunked upload. Streams `path` as N × CHUNK_SIZE_BYTES_DEFAULT
/// CDAT chunks + 1 CPAR manifest over `conn`, all envelope-encrypted to
/// `recipient_spans`. Returns 0 on success (prints manifest hash to stdout
/// unless opts.quiet); returns 1 on any failure.
///
/// Caller requirements:
///  - `conn` is already connected.
///  - `recipient_spans` contains at least the caller's own KEM pubkey.
///  - `path` exists; file size is in [CHUNK_THRESHOLD_BYTES,
///    MAX_CHUNKED_FILE_SIZE]. The caller (cmd::put) enforces bounds.
int put_chunked(
    const Identity& id,
    std::span<const uint8_t, 32> ns,
    std::vector<std::span<const uint8_t>> recipient_spans,
    const std::string& path,
    const std::string& filename,
    uint32_t ttl,
    Connection& conn,
    const cmd::ConnectOpts& opts);

/// Cascade-delete a CPAR manifest: tombstone each chunk first (D-09), then
/// tombstone the manifest last. Idempotent on retry (D-10). `manifest` must
/// already be decoded by the caller (cmd::rm decrypts blob.data[4..] and
/// calls decode_manifest_payload). `manifest_hash` is the hash of the CPAR
/// blob itself, used for the final tombstone.
int rm_chunked(
    const Identity& id,
    std::span<const uint8_t, 32> ns,
    const ManifestData& manifest,
    std::span<const uint8_t, 32> manifest_hash,
    Connection& conn,
    const cmd::ConnectOpts& opts);

// =============================================================================
// Low-level helpers (exposed so cli_tests can drive them without a socket)
// =============================================================================

/// Read the next chunk from an already-open ifstream into `buf`. Resizes
/// `buf` to max_bytes on the first call. Returns the number of bytes read
/// (0 on EOF, max_bytes on a full chunk, or [1, max_bytes) on the last
/// short chunk). Throws std::runtime_error on I/O failure.
std::size_t read_next_chunk(std::ifstream& f, std::vector<uint8_t>& buf,
                            std::size_t max_bytes);

/// Return the tombstone targets in the order rm_chunked will send them:
/// every chunk_hash in index order first, then manifest_hash last (D-09).
/// Pure function — safe to test without a network.
std::vector<std::array<uint8_t, 32>> plan_tombstone_targets(
    const ManifestData& manifest,
    std::span<const uint8_t, 32> manifest_hash);

} // namespace chromatindb::cli::chunked
