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
// Chunked large files (CHUNK-01..CHUNK-05)
// =============================================================================
//
// Contracts:
//  - put_chunked and rm_chunked are free functions. All per-invocation state
//    (Connection ref, Identity ref, recipient spans, rid counter,
//    rid_to_chunk_index map, retry counters, Sha3Hasher) lives in the
//    function body. No static/global state on the helper.
//  - Both functions ride the request-pipelining primitives (Connection::send_async +
//    Connection::recv) — one shared Connection, no second handshake, no
//    second thread. Single-sender / single-reader (PIPE-02) preserved.
//  - Outer-magic placement per D-13: chunk blob.data =
//        [CDAT magic:4][CENV envelope(raw chunk plaintext)]
//    manifest blob.data =
//        [CPAR magic:4][CENV envelope([CPAR magic:4][Manifest FlatBuffer])]
//    so type indexing works on the outer bytes and on-path
//    observers see only the role of the blob, not its content.
//  - Memory envelope per D-11: the plaintext read buffer (one chunk =
//    conn.session_blob_cap()) is reused; plaintext is passed to
//    envelope::encrypt by span and the caller releases the buffer eagerly
//    before the next read. Peak working memory stays bounded to
//    ~kPipelineDepth × one-encoded-chunk + one reusable read buffer.
//  - Retry policy per D-15: up to RETRY_ATTEMPTS per chunk with exponential
//    backoff. Each retry re-signs (ML-DSA-87 is non-deterministic, so the
//    hash differs), and the chunk_index -> final_hash binding happens at
//    drain time (post-WriteAck), never at send time.

// Phase 130 CLI-02/03 / CONTEXT.md D-04: the former hardcoded chunking
// threshold constant is deleted. The chunking boundary is now the live
// session cap (conn.session_blob_cap()): files larger than one cap-sized
// blob take the chunked path; files ≤ cap go as a single blob.
inline constexpr uint64_t MAX_CHUNKED_FILE_SIZE = 1024ULL * 1024 * 1024 * 1024; // 1 TiB (D-14)
inline constexpr int      RETRY_ATTEMPTS        = 3;                         // D-15
inline constexpr int      RETRY_BACKOFF_MS[RETRY_ATTEMPTS] = { 250, 1000, 4000 };

/// Pipelined chunked upload. Streams `path` as N × conn.session_blob_cap()
/// CDAT chunks + 1 CPAR manifest over `conn`, all envelope-encrypted to
/// `recipient_spans`. Returns 0 on success (prints manifest hash to stdout
/// unless opts.quiet); returns 1 on any failure.
///
/// Caller requirements:
///  - `conn` is already connected and has a non-zero session_blob_cap().
///  - `recipient_spans` contains at least the caller's own KEM pubkey.
///  - `path` exists; file size is in (conn.session_blob_cap(),
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

// =============================================================================
// Read side (CHUNK-03 + CHUNK-05)
// =============================================================================

/// Pipelined chunked download. Reads N chunk_hashes from `manifest` over
/// `conn`, envelope-decrypts each, pwrite()s at
/// `chunk_index * manifest.chunk_size_bytes` into `out_path`, and after all
/// chunks land verifies the re-read file SHA3-256 matches
/// manifest.plaintext_sha3 (CHUNK-05 defense-in-depth). Returns 0 on
/// success, non-zero on any failure; on failure the partial output file is
/// unlink()'d (D-12).
///
/// Caller requirements:
///  - `conn` is already connected and has just delivered the manifest blob
///    (so ONE PQ handshake serves the whole get).
///  - `out_path` is the fully-resolved output path. If `force_overwrite` is
///    false and `out_path` exists, this function errors before sending
///    any ReadRequest.
int get_chunked(
    const Identity& id,
    std::span<const uint8_t, 32> ns,
    const ManifestData& manifest,
    const std::string& out_path,
    bool force_overwrite,
    Connection& conn,
    const cmd::ConnectOpts& opts);

// =============================================================================
// Pure helpers exposed for unit tests (no socket, no disk beyond verify)
// =============================================================================

/// Refuse to overwrite an existing output file unless `force_overwrite` is set.
/// Writes a single stderr "Error: <path> already exists (use --force to
/// overwrite)" line when refusing. Returns true if the caller should proceed
/// with open+write; false if the caller should abort with a non-zero return
/// code. Factored out so tests can exercise the overwrite-guard invariant
/// without constructing a real Connection. get_chunked calls this as its
/// FIRST statement.
bool refuse_if_exists(const std::string& out_path, bool force_overwrite);

/// Ordered list of chunk hashes get_chunked will issue ReadRequests for.
/// Pure derivation from manifest.chunk_hashes (split every 32 bytes, ordered
/// by chunk_index). Exposed so tests can assert ordering.
std::vector<std::array<uint8_t, 32>> plan_chunk_read_targets(
    const ManifestData& manifest);

/// Re-read `path` in `read_buf_bytes` increments, streaming into a
/// Sha3Hasher, and compare to `expected`. Returns true on match, false on
/// mismatch or any I/O error. Does not modify the file.
///
/// Phase 130 CLI-02/03 / CONTEXT.md D-04: the read buffer size is now
/// derived from the session cap (callers pass conn.session_blob_cap() when
/// driving the production flow). The default fallback covers unit tests
/// that exercise the hasher directly without a live Connection.
bool verify_plaintext_sha3(const std::string& path,
                           std::span<const uint8_t, 32> expected,
                           std::size_t read_buf_bytes = 4ULL * 1024 * 1024);

/// 4-byte prefix classification for cdb get dispatch. Used by cmd::get to
/// decide whether to invoke chunked::get_chunked (CPAR), reject (CDAT), or
/// fall through to the existing single-blob path (Other, which covers CENV,
/// PUBK, TOMB, DLGT, or any raw/unrecognized type).
enum class GetDispatch { CPAR, CDAT, Other };
GetDispatch classify_blob_data(std::span<const uint8_t> blob_data);

} // namespace chromatindb::cli::chunked
