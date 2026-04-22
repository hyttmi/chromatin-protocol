#include "cli/src/wire.h"
#include "cli/src/identity.h"
#include <flatbuffers/flatbuffers.h>
#include <oqs/sha3.h>
#include <sodium.h>
#include <cstring>
#include <stdexcept>

namespace chromatindb::cli {

// =============================================================================
// FlatBuffer VTable offsets (hand-coded, matching the schemas)
// =============================================================================

// TransportMessage: table { type:uint8; payload:[ubyte]; request_id:uint32; }
// VTable field offsets: type=4, payload=6, request_id=8
namespace transport_vt {
    constexpr flatbuffers::voffset_t TYPE       = 4;
    constexpr flatbuffers::voffset_t PAYLOAD    = 6;
    constexpr flatbuffers::voffset_t REQUEST_ID = 8;
}

// Blob: table { signer_hint:[ubyte]; data:[ubyte]; ttl:uint32; timestamp:uint64; signature:[ubyte]; }
// VTable field offsets: 4, 6, 8, 10, 12 (byte-identical to db/schemas/blob.fbs)
namespace blob_vt {
    constexpr flatbuffers::voffset_t SIGNER_HINT = 4;
    constexpr flatbuffers::voffset_t DATA        = 6;
    constexpr flatbuffers::voffset_t TTL         = 8;
    constexpr flatbuffers::voffset_t TIMESTAMP   = 10;
    constexpr flatbuffers::voffset_t SIGNATURE   = 12;
}

// BlobWriteBody: table { target_namespace:[ubyte]; blob:Blob; }
// VTable field offsets (byte-identical to db/schemas/transport.fbs:83-87):
//   blob_write_body_vt::TARGET_NAMESPACE = 4
//   blob_write_body_vt::BLOB             = 6
namespace blob_write_body_vt {
    constexpr flatbuffers::voffset_t TARGET_NAMESPACE = 4;
    constexpr flatbuffers::voffset_t BLOB             = 6;
}

// Manifest: table {
//   version:uint32;  chunk_size_bytes:uint32;  segment_count:uint32;
//   total_plaintext_bytes:uint64;
//   plaintext_sha3:[ubyte];  chunk_hashes:[ubyte];  filename:string;
// }
// VTable field offsets: 4, 6, 8, 10, 12, 14, 16 (one more slot than blob_vt
// to carry filename). Hand-coded, mirroring blob_vt — no flatc dependency.
namespace manifest_vt {
    constexpr flatbuffers::voffset_t VERSION               = 4;
    constexpr flatbuffers::voffset_t CHUNK_SIZE_BYTES      = 6;
    constexpr flatbuffers::voffset_t SEGMENT_COUNT         = 8;
    constexpr flatbuffers::voffset_t TOTAL_PLAINTEXT_BYTES = 10;
    constexpr flatbuffers::voffset_t PLAINTEXT_SHA3        = 12;
    constexpr flatbuffers::voffset_t CHUNK_HASHES          = 14;
    constexpr flatbuffers::voffset_t FILENAME              = 16;
}

// =============================================================================
// TransportMessage encode/decode
// =============================================================================

std::vector<uint8_t> encode_transport(uint8_t type,
                                       std::span<const uint8_t> payload,
                                       uint32_t request_id) {
    flatbuffers::FlatBufferBuilder builder(payload.size() + 64);
    builder.ForceDefaults(true);

    auto payload_vec = builder.CreateVector(payload.data(), payload.size());

    // Build table manually: fields in reverse order before EndTable
    auto start = builder.StartTable();
    builder.AddElement<uint8_t>(transport_vt::TYPE, type, 0);
    builder.AddOffset(transport_vt::PAYLOAD, payload_vec);
    builder.AddElement<uint32_t>(transport_vt::REQUEST_ID, request_id, 0);
    auto root = builder.EndTable(start);
    builder.Finish(flatbuffers::Offset<flatbuffers::Table>(root));

    auto* buf = builder.GetBufferPointer();
    auto size = builder.GetSize();
    return {buf, buf + size};
}

std::optional<DecodedTransport> decode_transport(std::span<const uint8_t> data) {
    // Minimum FlatBuffer size: root offset (4) + vtable
    if (data.size() < sizeof(flatbuffers::uoffset_t) + sizeof(flatbuffers::voffset_t) * 2) {
        return std::nullopt;
    }

    // Verify the root offset points within the buffer
    auto root_offset = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(data.data());
    if (root_offset >= data.size()) {
        return std::nullopt;
    }

    auto* table = flatbuffers::GetRoot<flatbuffers::Table>(data.data());
    if (!table) return std::nullopt;

    DecodedTransport result;
    result.type = table->GetField<uint8_t>(transport_vt::TYPE, 0);
    result.request_id = table->GetField<uint32_t>(transport_vt::REQUEST_ID, 0);

    auto* payload = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(transport_vt::PAYLOAD);
    if (payload) {
        result.payload.assign(payload->begin(), payload->end());
    }

    return result;
}

// =============================================================================
// AEAD frame encrypt/decrypt
// =============================================================================

std::array<uint8_t, 12> make_aead_nonce(uint64_t counter) {
    std::array<uint8_t, 12> nonce{};
    // First 4 bytes: zeros (already zeroed)
    // Last 8 bytes: big-endian counter
    store_u64_be(nonce.data() + 4, counter);
    return nonce;
}

std::vector<uint8_t> encrypt_frame(std::span<const uint8_t> plaintext,
                                    std::span<const uint8_t, 32> key,
                                    uint64_t counter) {
    auto nonce = make_aead_nonce(counter);

    std::vector<uint8_t> ciphertext(plaintext.size() + crypto_aead_chacha20poly1305_ietf_ABYTES);
    unsigned long long ciphertext_len = 0;

    int rc = crypto_aead_chacha20poly1305_ietf_encrypt(
        ciphertext.data(), &ciphertext_len,
        plaintext.data(), plaintext.size(),
        nullptr, 0,  // no associated data
        nullptr,      // nsec (unused)
        nonce.data(), key.data());

    if (rc != 0) {
        throw std::runtime_error("ChaCha20-Poly1305 encryption failed");
    }

    ciphertext.resize(static_cast<size_t>(ciphertext_len));
    return ciphertext;
}

std::optional<std::vector<uint8_t>> decrypt_frame(
    std::span<const uint8_t> ciphertext_with_tag,
    std::span<const uint8_t, 32> key,
    uint64_t counter) {

    if (ciphertext_with_tag.size() < crypto_aead_chacha20poly1305_ietf_ABYTES) {
        return std::nullopt;
    }

    auto nonce = make_aead_nonce(counter);

    std::vector<uint8_t> plaintext(ciphertext_with_tag.size() - crypto_aead_chacha20poly1305_ietf_ABYTES);
    unsigned long long plaintext_len = 0;

    int rc = crypto_aead_chacha20poly1305_ietf_decrypt(
        plaintext.data(), &plaintext_len,
        nullptr,  // nsec (unused)
        ciphertext_with_tag.data(), ciphertext_with_tag.size(),
        nullptr, 0,  // no associated data
        nonce.data(), key.data());

    if (rc != 0) {
        return std::nullopt;
    }

    plaintext.resize(static_cast<size_t>(plaintext_len));
    return plaintext;
}

// =============================================================================
// Blob FlatBuffer encode/decode
// =============================================================================

std::vector<uint8_t> encode_blob(const BlobData& blob) {
    size_t estimated = blob.data.size() + blob.signer_hint.size() + blob.signature.size() + 256;
    flatbuffers::FlatBufferBuilder builder(estimated);
    builder.ForceDefaults(true);

    auto sh  = builder.CreateVector(blob.signer_hint.data(), blob.signer_hint.size());
    auto dt  = builder.CreateVector(blob.data.data(), blob.data.size());
    auto sig = builder.CreateVector(blob.signature.data(), blob.signature.size());

    auto start = builder.StartTable();
    builder.AddOffset(blob_vt::SIGNER_HINT, sh);
    builder.AddOffset(blob_vt::DATA, dt);
    builder.AddElement<uint32_t>(blob_vt::TTL, blob.ttl, 0);
    builder.AddElement<uint64_t>(blob_vt::TIMESTAMP, blob.timestamp, 0);
    builder.AddOffset(blob_vt::SIGNATURE, sig);
    auto root = builder.EndTable(start);
    builder.Finish(flatbuffers::Offset<flatbuffers::Table>(root));

    auto* buf = builder.GetBufferPointer();
    auto size = builder.GetSize();
    return {buf, buf + size};
}

std::optional<BlobData> decode_blob(std::span<const uint8_t> buffer) {
    if (buffer.size() < sizeof(flatbuffers::uoffset_t) + sizeof(flatbuffers::voffset_t) * 2) {
        return std::nullopt;
    }

    auto root_offset = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(buffer.data());
    if (root_offset >= buffer.size()) {
        return std::nullopt;
    }

    auto* table = flatbuffers::GetRoot<flatbuffers::Table>(buffer.data());
    if (!table) return std::nullopt;

    BlobData result;

    auto* sh = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(blob_vt::SIGNER_HINT);
    if (sh && sh->size() == 32) {
        std::memcpy(result.signer_hint.data(), sh->data(), 32);
    }

    auto* dt = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(blob_vt::DATA);
    if (dt) {
        result.data.assign(dt->begin(), dt->end());
    }

    result.ttl = table->GetField<uint32_t>(blob_vt::TTL, 0);
    result.timestamp = table->GetField<uint64_t>(blob_vt::TIMESTAMP, 0);

    auto* sig = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(blob_vt::SIGNATURE);
    if (sig) {
        result.signature.assign(sig->begin(), sig->end());
    }

    return result;
}

// =============================================================================
// Canonical signing input
// =============================================================================

std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t, 32> target_namespace,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp) {

    OQS_SHA3_sha3_256_inc_ctx ctx;
    OQS_SHA3_sha3_256_inc_init(&ctx);

    OQS_SHA3_sha3_256_inc_absorb(&ctx, target_namespace.data(), target_namespace.size());
    OQS_SHA3_sha3_256_inc_absorb(&ctx, data.data(), data.size());

    uint8_t ttl_be[4];
    store_u32_be(ttl_be, ttl);
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ttl_be, 4);

    uint8_t ts_be[8];
    store_u64_be(ts_be, timestamp);
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ts_be, 8);

    std::array<uint8_t, 32> hash{};
    OQS_SHA3_sha3_256_inc_finalize(hash.data(), &ctx);
    OQS_SHA3_sha3_256_inc_ctx_release(&ctx);

    return hash;
}

// =============================================================================
// D-03 central blob builder
// =============================================================================

NamespacedBlob build_owned_blob(
    const Identity& id,
    std::span<const uint8_t, 32> target_namespace,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp) {

    // signer_hint = SHA3-256(signing_pubkey). For owner writes this equals
    // id.namespace_id(); for delegate writes (target_namespace != own ns)
    // the signer_hint is still SHA3(own_sp) — the node uses signer_hint to
    // fetch the signer's PUBK for signature verification (T-124-02 mitigation).
    auto signer_hint = sha3_256(id.signing_pubkey());
    auto signing_input = build_signing_input(target_namespace, data, ttl, timestamp);
    auto signature = id.sign(std::span<const uint8_t>(signing_input.data(), signing_input.size()));

    BlobData blob{};
    blob.signer_hint = signer_hint;
    blob.data.assign(data.begin(), data.end());
    blob.ttl = ttl;
    blob.timestamp = timestamp;
    blob.signature = std::move(signature);

    std::array<uint8_t, 32> ns_arr{};
    std::memcpy(ns_arr.data(), target_namespace.data(), 32);
    return NamespacedBlob{ ns_arr, std::move(blob) };
}

// =============================================================================
// D-04 BlobWriteBody envelope encoder
// =============================================================================

std::vector<uint8_t> encode_blob_write_body(
    std::span<const uint8_t, 32> target_namespace,
    const BlobData& blob) {

    flatbuffers::FlatBufferBuilder builder(256 + blob.data.size() + blob.signature.size());
    builder.ForceDefaults(true);

    // Build the inner Blob table first so its offset can be referenced by
    // the outer BlobWriteBody table.
    auto sh  = builder.CreateVector(blob.signer_hint.data(), blob.signer_hint.size());
    auto dt  = builder.CreateVector(blob.data.data(), blob.data.size());
    auto sig = builder.CreateVector(blob.signature.data(), blob.signature.size());

    auto inner_start = builder.StartTable();
    builder.AddOffset(blob_vt::SIGNER_HINT, sh);
    builder.AddOffset(blob_vt::DATA, dt);
    builder.AddElement<uint32_t>(blob_vt::TTL, blob.ttl, 0);
    builder.AddElement<uint64_t>(blob_vt::TIMESTAMP, blob.timestamp, 0);
    builder.AddOffset(blob_vt::SIGNATURE, sig);
    auto inner_raw = builder.EndTable(inner_start);
    flatbuffers::Offset<void> inner_offset(inner_raw);

    auto ns = builder.CreateVector(target_namespace.data(), target_namespace.size());

    auto outer_start = builder.StartTable();
    builder.AddOffset(blob_write_body_vt::TARGET_NAMESPACE, ns);
    builder.AddOffset(blob_write_body_vt::BLOB, inner_offset);
    auto outer_raw = builder.EndTable(outer_start);
    builder.Finish(flatbuffers::Offset<flatbuffers::Table>(outer_raw));

    const auto* buf = builder.GetBufferPointer();
    const auto  size = builder.GetSize();
    return {buf, buf + size};
}

// =============================================================================
// Tombstone
// =============================================================================

std::array<uint8_t, 32> sha3_256(std::span<const uint8_t> data) {
    std::array<uint8_t, 32> out{};
    OQS_SHA3_sha3_256(out.data(), data.data(), data.size());
    return out;
}

std::vector<uint8_t> make_tombstone_data(std::span<const uint8_t, 32> target_hash) {
    std::vector<uint8_t> result;
    result.reserve(36);
    result.push_back(0xDE);
    result.push_back(0xAD);
    result.push_back(0xBE);
    result.push_back(0xEF);
    result.insert(result.end(), target_hash.begin(), target_hash.end());
    return result;
}

std::vector<uint8_t> make_delegation_data(std::span<const uint8_t> delegate_signing_pubkey) {
    std::vector<uint8_t> result;
    result.reserve(2596);
    result.push_back(0xDE);
    result.push_back(0x1E);
    result.push_back(0x6A);
    result.push_back(0x7E);
    result.insert(result.end(), delegate_signing_pubkey.begin(), delegate_signing_pubkey.end());
    return result;
}

std::vector<uint8_t> make_pubkey_data(std::span<const uint8_t> signing_pk,
                                       std::span<const uint8_t> kem_pk) {
    std::vector<uint8_t> result;
    result.reserve(PUBKEY_DATA_SIZE);
    result.insert(result.end(), PUBKEY_MAGIC.begin(), PUBKEY_MAGIC.end());
    result.insert(result.end(), signing_pk.begin(), signing_pk.end());
    result.insert(result.end(), kem_pk.begin(), kem_pk.end());
    return result;
}

// =============================================================================
// NAME + BOMB — mirrors db/wire/codec.cpp byte-for-byte.
// Separate implementation because the CLI wire module is compiled independently
// of chromatindb_lib; logic is identical, helpers come from cli/src/wire.h
// (store_u16_be / store_u32_be).
// =============================================================================

std::vector<uint8_t> make_name_data(std::span<const uint8_t> name,
                                     std::span<const uint8_t, 32> target_hash) {
    if (name.size() > 65535) {
        throw std::invalid_argument("make_name_data: name.size() exceeds 65535");
    }
    std::vector<uint8_t> result;
    result.reserve(4 + 2 + name.size() + 32);
    // Magic "NAME"
    result.insert(result.end(), NAME_MAGIC_CLI.begin(), NAME_MAGIC_CLI.end());
    // name_len as 2-byte big-endian
    uint8_t len_be[2];
    store_u16_be(len_be, static_cast<uint16_t>(name.size()));
    result.push_back(len_be[0]);
    result.push_back(len_be[1]);
    // Name bytes (opaque — D-04)
    result.insert(result.end(), name.begin(), name.end());
    // Target content hash
    result.insert(result.end(), target_hash.begin(), target_hash.end());
    return result;
}

std::vector<uint8_t> make_bomb_data(std::span<const std::array<uint8_t, 32>> targets) {
    if (targets.size() > static_cast<size_t>(UINT32_MAX)) {
        throw std::invalid_argument("make_bomb_data: target count exceeds UINT32_MAX");
    }
    std::vector<uint8_t> result;
    result.reserve(8 + targets.size() * 32);
    // Magic "BOMB"
    result.insert(result.end(), BOMB_MAGIC_CLI.begin(), BOMB_MAGIC_CLI.end());
    // count as 4-byte big-endian
    uint8_t count_be[4];
    store_u32_be(count_be, static_cast<uint32_t>(targets.size()));
    result.push_back(count_be[0]);
    result.push_back(count_be[1]);
    result.push_back(count_be[2]);
    result.push_back(count_be[3]);
    // Target hashes
    for (const auto& t : targets) {
        result.insert(result.end(), t.begin(), t.end());
    }
    return result;
}

std::optional<ParsedNamePayload> parse_name_payload(std::span<const uint8_t> data) {
    // Layout (D-03): [NAME:4][name_len:2 BE][name:N][target_hash:32]
    // MIN = 4 + 2 + 1 + 32 = 39 (names must be ≥1 byte per D-04).
    constexpr size_t kMin = 4 + 2 + 1 + 32;
    if (data.size() < kMin) return std::nullopt;
    if (std::memcmp(data.data(), NAME_MAGIC_CLI.data(), 4) != 0) return std::nullopt;

    uint16_t name_len = load_u16_be(data.data() + 4);
    if (name_len == 0) return std::nullopt;  // D-04: names are 1..65535 bytes

    const size_t required = 4 + 2 + static_cast<size_t>(name_len) + 32;
    if (data.size() != required) return std::nullopt;

    ParsedNamePayload out;
    out.name = data.subspan(6, name_len);
    std::memcpy(out.target_hash.data(), data.data() + 6 + name_len, 32);
    return out;
}

// =============================================================================
// Sha3Hasher (Phase 119 — incremental SHA3-256 for streaming plaintext_sha3)
// =============================================================================

struct Sha3Hasher::Impl {
    OQS_SHA3_sha3_256_inc_ctx ctx;
    bool finalized = false;
    Impl() { OQS_SHA3_sha3_256_inc_init(&ctx); }
    ~Impl() { if (!finalized) OQS_SHA3_sha3_256_inc_ctx_release(&ctx); }
};

Sha3Hasher::Sha3Hasher() : impl_(std::make_unique<Impl>()) {}
Sha3Hasher::~Sha3Hasher() = default;

void Sha3Hasher::absorb(std::span<const uint8_t> data) {
    OQS_SHA3_sha3_256_inc_absorb(&impl_->ctx, data.data(), data.size());
}

std::array<uint8_t, 32> Sha3Hasher::finalize() {
    std::array<uint8_t, 32> out{};
    OQS_SHA3_sha3_256_inc_finalize(out.data(), &impl_->ctx);
    OQS_SHA3_sha3_256_inc_ctx_release(&impl_->ctx);
    impl_->finalized = true;
    return out;
}

// =============================================================================
// Manifest FlatBuffer encode/decode (Phase 119 — hand-coded, matching blob_vt)
// =============================================================================

std::vector<uint8_t> encode_manifest_payload(const ManifestData& m) {
    size_t estimated = 64 + m.chunk_hashes.size() + m.filename.size();
    flatbuffers::FlatBufferBuilder builder(estimated);
    builder.ForceDefaults(true);

    auto sha3   = builder.CreateVector(m.plaintext_sha3.data(), m.plaintext_sha3.size());
    auto hashes = builder.CreateVector(m.chunk_hashes.data(),   m.chunk_hashes.size());
    auto fname  = builder.CreateString(m.filename);

    auto start = builder.StartTable();
    builder.AddElement<uint32_t>(manifest_vt::VERSION,               m.version,               0);
    builder.AddElement<uint32_t>(manifest_vt::CHUNK_SIZE_BYTES,      m.chunk_size_bytes,      0);
    builder.AddElement<uint32_t>(manifest_vt::SEGMENT_COUNT,         m.segment_count,         0);
    builder.AddElement<uint64_t>(manifest_vt::TOTAL_PLAINTEXT_BYTES, m.total_plaintext_bytes, 0);
    builder.AddOffset(manifest_vt::PLAINTEXT_SHA3,  sha3);
    builder.AddOffset(manifest_vt::CHUNK_HASHES,    hashes);
    builder.AddOffset(manifest_vt::FILENAME,        fname);
    auto root = builder.EndTable(start);
    builder.Finish(flatbuffers::Offset<flatbuffers::Table>(root));

    const auto* buf = builder.GetBufferPointer();
    const auto  size = builder.GetSize();

    std::vector<uint8_t> out;
    out.reserve(4 + size);
    out.insert(out.end(), CPAR_MAGIC.begin(), CPAR_MAGIC.end());
    out.insert(out.end(), buf, buf + size);
    return out;
}

std::optional<ManifestData> decode_manifest_payload(std::span<const uint8_t> data) {
    if (data.size() < 4 ||
        std::memcmp(data.data(), CPAR_MAGIC.data(), 4) != 0) {
        return std::nullopt;
    }
    auto fb = data.subspan(4);
    if (fb.size() < sizeof(flatbuffers::uoffset_t) + sizeof(flatbuffers::voffset_t) * 2) {
        return std::nullopt;
    }
    auto root_offset = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(fb.data());
    if (root_offset >= fb.size()) return std::nullopt;

    const auto* table = flatbuffers::GetRoot<flatbuffers::Table>(fb.data());
    if (!table) return std::nullopt;

    ManifestData m;
    m.version               = table->GetField<uint32_t>(manifest_vt::VERSION, 0);
    m.chunk_size_bytes      = table->GetField<uint32_t>(manifest_vt::CHUNK_SIZE_BYTES, 0);
    m.segment_count         = table->GetField<uint32_t>(manifest_vt::SEGMENT_COUNT, 0);
    m.total_plaintext_bytes = table->GetField<uint64_t>(manifest_vt::TOTAL_PLAINTEXT_BYTES, 0);

    if (m.version != MANIFEST_VERSION_V1)                                     return std::nullopt;
    if (m.segment_count == 0 || m.segment_count > MAX_CHUNKS)                 return std::nullopt;
    if (m.chunk_size_bytes < CHUNK_SIZE_BYTES_MIN ||
        m.chunk_size_bytes > CHUNK_SIZE_BYTES_MAX)                            return std::nullopt;
    const uint64_t max_plain = static_cast<uint64_t>(m.segment_count) *
                               static_cast<uint64_t>(m.chunk_size_bytes);
    if (m.total_plaintext_bytes == 0 || m.total_plaintext_bytes > max_plain)  return std::nullopt;

    const auto* sha3   = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(
        manifest_vt::PLAINTEXT_SHA3);
    const auto* hashes = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(
        manifest_vt::CHUNK_HASHES);
    const auto* fname  = table->GetPointer<const flatbuffers::String*>(
        manifest_vt::FILENAME);
    if (!sha3 || sha3->size() != 32)                                          return std::nullopt;
    if (!hashes ||
        hashes->size() != static_cast<uint64_t>(m.segment_count) * 32u)       return std::nullopt;

    std::memcpy(m.plaintext_sha3.data(), sha3->data(), 32);
    m.chunk_hashes.assign(hashes->begin(), hashes->end());
    if (fname) m.filename.assign(fname->c_str(), fname->size());
    return m;
}

} // namespace chromatindb::cli
