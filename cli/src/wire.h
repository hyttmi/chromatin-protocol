#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace chromatindb::cli {

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
    Data                  = 8,
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
    std::array<uint8_t, 32> namespace_id{};
    std::vector<uint8_t> pubkey;
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

/// SHA3-256(namespace_id || data || ttl_be32 || timestamp_be64).
/// Returns 32-byte digest.
std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t, 32> namespace_id,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp);

// =============================================================================
// Tombstone
// =============================================================================

// =============================================================================
// SHA3-256
// =============================================================================

/// SHA3-256 hash.
std::array<uint8_t, 32> sha3_256(std::span<const uint8_t> data);

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
