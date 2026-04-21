#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace chromatindb::cli {

// Forward declaration so wire.h does not need to pull in identity.h.
// build_owned_blob composes Identity::sign + Identity::signing_pubkey; the
// full header is included only in wire.cpp.
class Identity;

// =============================================================================
// Big-endian helpers
// =============================================================================

inline void store_u16_be(uint8_t* out, uint16_t val) {
    out[0] = static_cast<uint8_t>(val >> 8);
    out[1] = static_cast<uint8_t>(val);
}

inline void store_u32_be(uint8_t* out, uint32_t val) {
    out[0] = static_cast<uint8_t>(val >> 24);
    out[1] = static_cast<uint8_t>(val >> 16);
    out[2] = static_cast<uint8_t>(val >> 8);
    out[3] = static_cast<uint8_t>(val);
}

inline void store_u64_be(uint8_t* out, uint64_t val) {
    out[0] = static_cast<uint8_t>(val >> 56);
    out[1] = static_cast<uint8_t>(val >> 48);
    out[2] = static_cast<uint8_t>(val >> 40);
    out[3] = static_cast<uint8_t>(val >> 32);
    out[4] = static_cast<uint8_t>(val >> 24);
    out[5] = static_cast<uint8_t>(val >> 16);
    out[6] = static_cast<uint8_t>(val >> 8);
    out[7] = static_cast<uint8_t>(val);
}

inline uint16_t load_u16_be(const uint8_t* p) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) |
        static_cast<uint16_t>(p[1]));
}

inline uint32_t load_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint64_t load_u64_be(const uint8_t* p) {
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
// Message types (CLI subset)
// =============================================================================

enum class MsgType : uint8_t {
    // KEEP — node still emits DeleteAck=18 for this TransportMsgType; payload is now BlobWriteBody-shaped (post-D-04b).
    Delete                = 17,
    DeleteAck             = 18,
    WriteAck              = 30,
    ReadRequest           = 31,
    ReadResponse          = 32,
    ListRequest           = 33,
    ListResponse          = 34,
    StatsRequest          = 35,
    StatsResponse         = 36,
    ExistsRequest         = 37,
    ExistsResponse        = 38,
    NodeInfoRequest       = 39,
    NodeInfoResponse      = 40,
    DelegationListRequest = 51,
    DelegationListResponse = 52,
    SyncNamespaceAnnounce = 62,
    ErrorResponse         = 63,
    // D-04: all blob writes go through BlobWrite + BlobWriteBody envelope; node emits WriteAck=30.
    BlobWrite             = 64,
};

// =============================================================================
// TransportMessage encode/decode (FlatBuffer wire format)
// =============================================================================

struct DecodedTransport {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    uint32_t request_id = 0;
};

/// Encode a transport message to FlatBuffer bytes.
std::vector<uint8_t> encode_transport(uint8_t type,
                                       std::span<const uint8_t> payload,
                                       uint32_t request_id);

/// Decode FlatBuffer bytes to a transport message.
/// Returns nullopt if the buffer is invalid.
std::optional<DecodedTransport> decode_transport(std::span<const uint8_t> data);

// =============================================================================
// AEAD frame encrypt/decrypt (ChaCha20-Poly1305)
// =============================================================================

/// Build a 12-byte nonce: [4 zero bytes][8-byte BE counter].
std::array<uint8_t, 12> make_aead_nonce(uint64_t counter);

/// Encrypt plaintext with ChaCha20-Poly1305. Returns ciphertext + 16-byte tag.
std::vector<uint8_t> encrypt_frame(std::span<const uint8_t> plaintext,
                                    std::span<const uint8_t, 32> key,
                                    uint64_t counter);

/// Decrypt ciphertext (with appended 16-byte tag).
/// Returns plaintext or nullopt on auth failure.
std::optional<std::vector<uint8_t>> decrypt_frame(
    std::span<const uint8_t> ciphertext_with_tag,
    std::span<const uint8_t, 32> key,
    uint64_t counter);

// =============================================================================
// Blob FlatBuffer encode/decode
// =============================================================================

struct BlobData {
    std::array<uint8_t, 32> signer_hint{};  // D-03a: SHA3(signing_pk). 32 bytes.
    std::vector<uint8_t> data;
    uint32_t ttl = 0;
    uint64_t timestamp = 0;
    std::vector<uint8_t> signature;
};

/// Encode BlobData to FlatBuffer bytes.
std::vector<uint8_t> encode_blob(const BlobData& blob);

/// Decode FlatBuffer bytes to BlobData.
/// Returns nullopt if the buffer is invalid.
std::optional<BlobData> decode_blob(std::span<const uint8_t> buffer);

// =============================================================================
// Canonical signing input
// =============================================================================

/// SHA3-256(target_namespace || data || ttl_be32 || timestamp_be64).
/// Returns 32-byte digest. Byte output IDENTICAL to pre-rename (Phase 122 D-01 invariant):
/// the parameter rename is semantic-only; the input to SHA3 is the raw span bytes.
std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t, 32> target_namespace,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp);

// =============================================================================
// D-03 central blob builder + D-04 envelope encoder
// =============================================================================

/// D-03: (target_namespace, BlobData) pair returned by build_owned_blob.
/// Name mirrors db/sync/sync_protocol.h's NamespacedBlob for CLI<->node
/// vocabulary symmetry.
struct NamespacedBlob {
    std::array<uint8_t, 32> target_namespace{};
    BlobData blob;
};

/// D-03: compose signer_hint = SHA3(id.signing_pubkey()), build the canonical
/// signing input, sign with ML-DSA-87, and return the (target_namespace,
/// signed-blob) pair. For owner writes, signer_hint == id.namespace_id();
/// for delegate writes (target_namespace != id.namespace_id()) the signer_hint
/// is structurally forced to SHA3(own_signing_pk), so delegates cannot
/// impersonate the namespace owner (T-124-02 mitigation).
NamespacedBlob build_owned_blob(
    const Identity& id,
    std::span<const uint8_t, 32> target_namespace,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp);

/// D-04: encode BlobWriteBody { target_namespace:[ubyte]; blob:Blob; }
/// byte-compatible with db/schemas/transport.fbs:83-87. Send under
/// MsgType::BlobWrite=64; node responds with WriteAck=30. Also accepted
/// by node's Delete dispatcher for tombstones (TransportMsgType=17).
std::vector<uint8_t> encode_blob_write_body(
    std::span<const uint8_t, 32> target_namespace,
    const BlobData& blob);

// =============================================================================
// Tombstone
// =============================================================================

// =============================================================================
// SHA3-256
// =============================================================================

/// SHA3-256 hash.
std::array<uint8_t, 32> sha3_256(std::span<const uint8_t> data);

/// Incremental SHA3-256 hasher. RAII wrapper around OQS_SHA3_sha3_256_inc_ctx,
/// so the Phase 119 streaming-upload path can absorb 16 MiB plaintext chunks
/// as they are read from disk (instead of concatenating and one-shot hashing,
/// which would defeat D-11 bounded memory).
///
/// Single-use: call absorb() any number of times, then finalize() exactly once.
/// After finalize() the object is done; subsequent absorb()/finalize() are
/// undefined behavior and must not be invoked.
class Sha3Hasher {
public:
    Sha3Hasher();
    ~Sha3Hasher();
    Sha3Hasher(const Sha3Hasher&) = delete;
    Sha3Hasher& operator=(const Sha3Hasher&) = delete;
    void absorb(std::span<const uint8_t> data);
    std::array<uint8_t, 32> finalize();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// =============================================================================
// Tombstone
// =============================================================================

/// Create tombstone data: [0xDE,0xAD,0xBE,0xEF][target_hash:32] = 36 bytes.
std::vector<uint8_t> make_tombstone_data(std::span<const uint8_t, 32> target_hash);

// =============================================================================
// Delegation
// =============================================================================

/// Create delegation data: [0xDE,0x1E,0x6A,0x7E][delegate_signing_pubkey:2592] = 2596 bytes.
std::vector<uint8_t> make_delegation_data(std::span<const uint8_t> delegate_signing_pubkey);

// =============================================================================
// NAME + BOMB (Phase 123)
// =============================================================================

/// Build NAME payload bytes: [NAME:4][name_len:2 BE][name][target_hash:32].
/// Name is opaque bytes (D-04). @throws std::invalid_argument if name.size() > 65535.
std::vector<uint8_t> make_name_data(std::span<const uint8_t> name,
                                     std::span<const uint8_t, 32> target_hash);

/// Build BOMB payload bytes: [BOMB:4][count:4 BE][target_hash:32]*count.
/// @throws std::invalid_argument if targets.size() > UINT32_MAX.
std::vector<uint8_t> make_bomb_data(std::span<const std::array<uint8_t, 32>> targets);

/// Decoded view of a NAME payload. `name` references bytes INSIDE the caller's
/// original buffer — the returned struct is valid only while that buffer is
/// kept alive (zero-copy, zero-alloc per Plan 01 design note).
struct ParsedNamePayload {
    std::span<const uint8_t> name;              // opaque bytes (D-04)
    std::array<uint8_t, 32> target_hash{};      // bound content blob
};

/// Parse a NAME payload blob.data. Returns nullopt on magic mismatch,
/// truncation, or declared name_len inconsistent with payload size.
/// Byte-identical to db/wire/codec.h parse_name_payload; the two modules are
/// separately compiled but logically paired.
std::optional<ParsedNamePayload> parse_name_payload(std::span<const uint8_t> data);

// =============================================================================
// Public key blob
// =============================================================================

/// PUBK magic: 0x50 0x55 0x42 0x4B
inline constexpr std::array<uint8_t, 4> PUBKEY_MAGIC = {0x50, 0x55, 0x42, 0x4B};

/// PUBK data size: 4 magic + 2592 signing pk + 1568 KEM pk = 4164 bytes.
inline constexpr size_t PUBKEY_DATA_SIZE = 4 + 2592 + 1568;

/// Create pubkey blob data: [PUBK magic][signing_pk][kem_pk]
std::vector<uint8_t> make_pubkey_data(std::span<const uint8_t> signing_pk,
                                       std::span<const uint8_t> kem_pk);

/// Check if blob data is a published pubkey.
inline bool is_pubkey_blob(std::span<const uint8_t> data) {
    return data.size() == PUBKEY_DATA_SIZE &&
           data[0] == 0x50 && data[1] == 0x55 && data[2] == 0x42 && data[3] == 0x4B;
}

// =============================================================================
// Chunked large files (Phase 119) — shared constants and manifest payload
// =============================================================================

/// Phase 119 limits (D-01, D-14). Public so chunked.cpp and wire.cpp share
/// one canonical copy and tests can reference them by name.
inline constexpr uint32_t MANIFEST_VERSION_V1      = 1;
inline constexpr uint32_t MAX_CHUNKS               = 65536;              // 1 TiB / 16 MiB
inline constexpr uint32_t CHUNK_SIZE_BYTES_DEFAULT = 16u * 1024u * 1024u;
inline constexpr uint32_t CHUNK_SIZE_BYTES_MIN     =  1u * 1024u * 1024u;
inline constexpr uint32_t CHUNK_SIZE_BYTES_MAX     = 256u * 1024u * 1024u;

/// CPAR manifest payload. Wire shape (per D-13):
///   [CPAR magic:4][FlatBuffer Manifest]
///
/// segment_count counts INTER-blob CDAT chunks (one CDAT = 16 MiB plaintext).
/// Intentionally distinct from envelope.cpp's internal SEGMENT_SIZE, which
/// counts INTRA-blob AEAD segments (1 MiB) inside one CENV envelope.
struct ManifestData {
    uint32_t version = MANIFEST_VERSION_V1;
    uint32_t chunk_size_bytes = 0;
    uint32_t segment_count = 0;                    // CHUNK-05 truncation guard
    uint64_t total_plaintext_bytes = 0;
    std::array<uint8_t, 32> plaintext_sha3{};      // CHUNK-05 defense-in-depth
    std::vector<uint8_t> chunk_hashes;             // segment_count * 32 bytes
    std::string filename;                           // may be empty (stdin case)
};

/// Encode a ManifestData as [CPAR magic:4][FlatBuffer Manifest]. Hand-coded
/// to match the blob_vt style; no flatc. Never fails on well-formed input.
std::vector<uint8_t> encode_manifest_payload(const ManifestData& m);

/// Decode [CPAR magic:4][FlatBuffer Manifest] with the validation invariants
/// required by P-119-04 / T-119-06. Returns nullopt on any failure. On
/// success every ManifestData field passes the public caps defined above.
std::optional<ManifestData> decode_manifest_payload(std::span<const uint8_t> data);

// =============================================================================
// Type label mapping (Phase 117)
// =============================================================================

/// CENV (client envelope) magic: "CENV" in ASCII
inline constexpr std::array<uint8_t, 4> CENV_MAGIC = {0x43, 0x45, 0x4E, 0x56};

/// Tombstone magic
inline constexpr std::array<uint8_t, 4> TOMBSTONE_MAGIC_CLI = {0xDE, 0xAD, 0xBE, 0xEF};

/// Delegation magic
inline constexpr std::array<uint8_t, 4> DELEGATION_MAGIC_CLI = {0xDE, 0x1E, 0x6A, 0x7E};

/// CDAT (chunk data) magic: "CDAT" in ASCII (Phase 119)
inline constexpr std::array<uint8_t, 4> CDAT_MAGIC = {0x43, 0x44, 0x41, 0x54};

/// CPAR (chunked manifest) magic: "CPAR" in ASCII (Phase 119, CHUNK-02).
/// Present on the OUTER blob.data (not inside the envelope) so Phase 117
/// type indexing sees it pre-decrypt (D-13).
inline constexpr std::array<uint8_t, 4> CPAR_MAGIC = {0x43, 0x50, 0x41, 0x52};

/// NAME (mutable name pointer — Phase 123 D-03). Byte-identical to
/// db/wire/codec.h NAME_MAGIC; the two modules are separately-compiled but
/// logically paired.
inline constexpr std::array<uint8_t, 4> NAME_MAGIC_CLI = {0x4E, 0x41, 0x4D, 0x45}; // "NAME"

/// BOMB (batched tombstone — Phase 123 D-05). Byte-identical to codec.h BOMB_MAGIC.
inline constexpr std::array<uint8_t, 4> BOMB_MAGIC_CLI = {0x42, 0x4F, 0x4D, 0x42}; // "BOMB"

/// Map 4-byte type prefix to human-readable label (per D-18).
/// Returns: "CENV", "PUBK", "TOMB", "DLGT", "CDAT", "CPAR", "NAME", "BOMB",
/// or "DATA" for unknown.
inline const char* type_label(const uint8_t* type) {
    if (std::memcmp(type, CENV_MAGIC.data(), 4) == 0) return "CENV";
    if (std::memcmp(type, PUBKEY_MAGIC.data(), 4) == 0) return "PUBK";
    if (std::memcmp(type, TOMBSTONE_MAGIC_CLI.data(), 4) == 0) return "TOMB";
    if (std::memcmp(type, DELEGATION_MAGIC_CLI.data(), 4) == 0) return "DLGT";
    if (std::memcmp(type, CDAT_MAGIC.data(), 4) == 0) return "CDAT";
    if (std::memcmp(type, CPAR_MAGIC.data(), 4) == 0) return "CPAR";
    if (std::memcmp(type, NAME_MAGIC_CLI.data(), 4) == 0) return "NAME";
    if (std::memcmp(type, BOMB_MAGIC_CLI.data(), 4) == 0) return "BOMB";
    return "DATA";
}

/// Check if a type prefix should be hidden in default cdb ls output (per D-13).
/// Hidden types: PUBK, CDAT, DLGT, NAME (defense-in-depth + infrastructure).
///
/// CPAR is deliberately NOT hidden: the manifest is the user-facing handle
/// for a chunked file — the user runs `cdb get <manifest_hash>` to
/// reassemble, so the manifest must appear in default `cdb ls` output. CDAT
/// chunks stay hidden because the user never addresses them directly.
///
/// NAME is hidden (Phase 123 A4): NAME pointer blobs are infrastructure —
/// users interact with `cdb get <name>` / `cdb put --name`, not with the
/// raw NAME blobs. BOMB is NOT hidden (mirrors TOMB's default visibility —
/// deletion records are auditable by default).
inline bool is_hidden_type(const uint8_t* type) {
    if (std::memcmp(type, PUBKEY_MAGIC.data(), 4) == 0) return true;
    if (std::memcmp(type, CDAT_MAGIC.data(), 4) == 0) return true;
    if (std::memcmp(type, DELEGATION_MAGIC_CLI.data(), 4) == 0) return true;
    if (std::memcmp(type, NAME_MAGIC_CLI.data(), 4) == 0) return true;
    return false;
}

// =============================================================================
// Hex utilities
// =============================================================================

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

/// Decode hex string to bytes. Returns nullopt on odd length or invalid chars.
inline std::optional<std::vector<uint8_t>> from_hex(const std::string& hex_str) {
    if (hex_str.size() % 2 != 0) {
        return std::nullopt;
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> result(hex_str.size() / 2);
    for (size_t i = 0; i < result.size(); ++i) {
        int hi = nibble(hex_str[i * 2]);
        int lo = nibble(hex_str[i * 2 + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        result[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return result;
}

} // namespace chromatindb::cli
