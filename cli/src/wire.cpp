#include "cli/src/wire.h"
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

// Blob: table { namespace_id:[ubyte]; pubkey:[ubyte]; data:[ubyte]; ttl:uint32; timestamp:uint64; signature:[ubyte]; }
// VTable field offsets: 4, 6, 8, 10, 12, 14
namespace blob_vt {
    constexpr flatbuffers::voffset_t NAMESPACE_ID = 4;
    constexpr flatbuffers::voffset_t PUBKEY       = 6;
    constexpr flatbuffers::voffset_t DATA         = 8;
    constexpr flatbuffers::voffset_t TTL          = 10;
    constexpr flatbuffers::voffset_t TIMESTAMP    = 12;
    constexpr flatbuffers::voffset_t SIGNATURE    = 14;
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
    size_t estimated = blob.data.size() + blob.pubkey.size() + blob.signature.size() + 1024;
    flatbuffers::FlatBufferBuilder builder(estimated);
    builder.ForceDefaults(true);

    auto ns  = builder.CreateVector(blob.namespace_id.data(), blob.namespace_id.size());
    auto pk  = builder.CreateVector(blob.pubkey.data(), blob.pubkey.size());
    auto dt  = builder.CreateVector(blob.data.data(), blob.data.size());
    auto sig = builder.CreateVector(blob.signature.data(), blob.signature.size());

    auto start = builder.StartTable();
    builder.AddOffset(blob_vt::NAMESPACE_ID, ns);
    builder.AddOffset(blob_vt::PUBKEY, pk);
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

    auto* ns = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(blob_vt::NAMESPACE_ID);
    if (ns && ns->size() == 32) {
        std::memcpy(result.namespace_id.data(), ns->data(), 32);
    }

    auto* pk = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(blob_vt::PUBKEY);
    if (pk) {
        result.pubkey.assign(pk->begin(), pk->end());
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
    std::span<const uint8_t, 32> namespace_id,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp) {

    OQS_SHA3_sha3_256_inc_ctx ctx;
    OQS_SHA3_sha3_256_inc_init(&ctx);

    OQS_SHA3_sha3_256_inc_absorb(&ctx, namespace_id.data(), namespace_id.size());
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

} // namespace chromatindb::cli
