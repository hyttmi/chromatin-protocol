#include "db/wire/codec.h"
#include "db/wire/blob_generated.h"
#include "db/wire/transport_generated.h"
#include "db/crypto/hash.h"
#include "db/util/endian.h"
#include <flatbuffers/flatbuffers.h>
#include <oqs/sha3.h>
#include <cstring>
#include <stdexcept>

namespace chromatindb::wire {

std::vector<uint8_t> encode_blob(const BlobData& blob) {
    // Size builder to avoid reallocations: blob data + signer_hint(32) + sig(4627) + overhead(1024)
    size_t estimated_size = blob.data.size() + 8192;
    flatbuffers::FlatBufferBuilder builder(estimated_size);
    builder.ForceDefaults(true);  // Deterministic: include zero-value fields

    auto sh = builder.CreateVector(blob.signer_hint.data(), blob.signer_hint.size());
    auto dt = builder.CreateVector(blob.data.data(), blob.data.size());
    auto sg = builder.CreateVector(blob.signature.data(), blob.signature.size());

    auto fb_blob = chromatindb::wire::CreateBlob(builder, sh, dt, blob.ttl, blob.timestamp, sg);
    builder.Finish(fb_blob);

    auto* ptr = builder.GetBufferPointer();
    auto size = builder.GetSize();
    return {ptr, ptr + size};
}

BlobData decode_blob_from_fb(const chromatindb::wire::Blob* fb_blob) {
    if (!fb_blob) {
        throw std::runtime_error("Null FlatBuffer Blob accessor");
    }

    BlobData result;

    if (fb_blob->signer_hint() && fb_blob->signer_hint()->size() == 32) {
        std::memcpy(result.signer_hint.data(), fb_blob->signer_hint()->data(), 32);
    } else {
        throw std::runtime_error("Invalid signer_hint size in FlatBuffer blob (expected 32 bytes)");
    }

    if (fb_blob->data()) {
        result.data.assign(fb_blob->data()->begin(), fb_blob->data()->end());
    }

    result.ttl = fb_blob->ttl();
    result.timestamp = fb_blob->timestamp();

    if (fb_blob->signature()) {
        result.signature.assign(fb_blob->signature()->begin(), fb_blob->signature()->end());
    }

    return result;
}

BlobData decode_blob(std::span<const uint8_t> buffer) {
    auto verifier = flatbuffers::Verifier(buffer.data(), buffer.size());
    if (!chromatindb::wire::VerifyBlobBuffer(verifier)) {
        throw std::runtime_error("Invalid FlatBuffer blob data");
    }

    // Share field-extraction with decode_blob_from_fb (feedback_no_duplicate_code.md).
    return decode_blob_from_fb(chromatindb::wire::GetBlob(buffer.data()));
}

std::vector<uint8_t> encode_blob_write_envelope(
    std::span<const uint8_t, 32> target_namespace,
    const BlobData& blob) {
    // Estimate: outer envelope overhead (~64B) + inner blob body.
    size_t estimated_size = blob.data.size() + 8192;
    flatbuffers::FlatBufferBuilder builder(estimated_size);
    builder.ForceDefaults(true);  // Deterministic: include zero-value fields

    // Inner Blob table
    auto sh = builder.CreateVector(blob.signer_hint.data(), blob.signer_hint.size());
    auto dt = builder.CreateVector(blob.data.data(), blob.data.size());
    auto sg = builder.CreateVector(blob.signature.data(), blob.signature.size());
    auto fb_blob = chromatindb::wire::CreateBlob(builder, sh, dt, blob.ttl, blob.timestamp, sg);

    // Outer BlobWriteBody envelope
    auto ns_vec = builder.CreateVector(target_namespace.data(), 32);
    auto body = chromatindb::wire::CreateBlobWriteBody(builder, ns_vec, fb_blob);
    builder.Finish(body);

    auto* ptr = builder.GetBufferPointer();
    auto size = builder.GetSize();
    return {ptr, ptr + size};
}

std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t> target_namespace,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp) {

    // Incremental SHA3-256: hash target_namespace || data || ttl_be32 || timestamp_be64
    // directly into the sponge -- zero intermediate allocation.
    // D-01: byte output IDENTICAL to pre-122 for the same input bytes;
    // only the parameter name changes. Do NOT reorder absorption -- sponge order
    // is part of the wire-protocol contract (Pitfall #8).
    OQS_SHA3_sha3_256_inc_ctx ctx;
    OQS_SHA3_sha3_256_inc_init(&ctx);

    // Target namespace
    OQS_SHA3_sha3_256_inc_absorb(&ctx, target_namespace.data(), target_namespace.size());

    // Data (may be up to 100 MiB -- fed directly, no copy)
    OQS_SHA3_sha3_256_inc_absorb(&ctx, data.data(), data.size());

    // TTL as big-endian uint32
    uint8_t ttl_be[4];
    chromatindb::util::store_u32_be(ttl_be, ttl);
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ttl_be, 4);

    // Timestamp as big-endian uint64
    uint8_t ts_be[8];
    chromatindb::util::store_u64_be(ts_be, timestamp);
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ts_be, 8);

    std::array<uint8_t, 32> hash{};
    OQS_SHA3_sha3_256_inc_finalize(hash.data(), &ctx);
    OQS_SHA3_sha3_256_inc_ctx_release(&ctx);

    return hash;
}

std::array<uint8_t, 32> blob_hash(std::span<const uint8_t> encoded_blob) {
    return crypto::sha3_256(encoded_blob);
}

// =============================================================================
// Tombstone utilities
// =============================================================================

bool is_tombstone(std::span<const uint8_t> data) {
    if (data.size() != TOMBSTONE_DATA_SIZE) return false;
    return std::memcmp(data.data(), TOMBSTONE_MAGIC.data(), TOMBSTONE_MAGIC.size()) == 0;
}

std::array<uint8_t, 32> extract_tombstone_target(std::span<const uint8_t> data) {
    std::array<uint8_t, 32> target{};
    std::memcpy(target.data(), data.data() + TOMBSTONE_MAGIC.size(), 32);
    return target;
}

std::vector<uint8_t> make_tombstone_data(std::span<const uint8_t, 32> target_hash) {
    std::vector<uint8_t> result;
    result.reserve(TOMBSTONE_DATA_SIZE);
    result.insert(result.end(), TOMBSTONE_MAGIC.begin(), TOMBSTONE_MAGIC.end());
    result.insert(result.end(), target_hash.begin(), target_hash.end());
    return result;
}

// =============================================================================
// Delegation utilities
// =============================================================================

bool is_delegation(std::span<const uint8_t> data) {
    if (data.size() != DELEGATION_DATA_SIZE) return false;
    return std::memcmp(data.data(), DELEGATION_MAGIC.data(), DELEGATION_MAGIC.size()) == 0;
}

std::vector<uint8_t> extract_delegate_pubkey(std::span<const uint8_t> data) {
    return {data.begin() + DELEGATION_MAGIC.size(),
            data.begin() + DELEGATION_MAGIC.size() + DELEGATION_PUBKEY_SIZE};
}

std::vector<uint8_t> make_delegation_data(std::span<const uint8_t> delegate_pubkey) {
    std::vector<uint8_t> result;
    result.reserve(DELEGATION_DATA_SIZE);
    result.insert(result.end(), DELEGATION_MAGIC.begin(), DELEGATION_MAGIC.end());
    result.insert(result.end(), delegate_pubkey.begin(), delegate_pubkey.end());
    return result;
}

// =============================================================================
// NAME (Phase 123 D-03)
// =============================================================================

bool is_name(std::span<const uint8_t> data) {
    if (data.size() < NAME_MIN_DATA_SIZE) return false;
    if (std::memcmp(data.data(), NAME_MAGIC.data(), NAME_MAGIC.size()) != 0) return false;
    // name_len is 2-byte big-endian at offset 4.
    uint16_t name_len = chromatindb::util::read_u16_be(data.subspan(4, 2));
    // Total size must be exactly magic(4) + len_field(2) + name(name_len) + target_hash(32).
    return data.size() == static_cast<size_t>(4) + 2 + name_len + 32;
}

std::optional<NamePayload> parse_name_payload(std::span<const uint8_t> data) {
    if (!is_name(data)) return std::nullopt;

    uint16_t name_len = chromatindb::util::read_u16_be(data.subspan(4, 2));
    NamePayload out;
    out.name = data.subspan(6, name_len);
    std::memcpy(out.target_hash.data(), data.data() + 6 + name_len, 32);
    return out;
}

std::vector<uint8_t> make_name_data(std::span<const uint8_t> name,
                                     std::span<const uint8_t, 32> target_hash) {
    if (name.size() > 65535) {
        throw std::invalid_argument("make_name_data: name.size() exceeds 65535");
    }
    std::vector<uint8_t> result;
    result.reserve(4 + 2 + name.size() + 32);
    // Magic
    result.insert(result.end(), NAME_MAGIC.begin(), NAME_MAGIC.end());
    // name_len as 2-byte big-endian
    chromatindb::util::write_u16_be(result, static_cast<uint16_t>(name.size()));
    // Name bytes (opaque — D-04)
    result.insert(result.end(), name.begin(), name.end());
    // Target content hash
    result.insert(result.end(), target_hash.begin(), target_hash.end());
    return result;
}

// =============================================================================
// BOMB (Phase 123 D-05)
// =============================================================================

bool is_bomb(std::span<const uint8_t> data) {
    if (data.size() < BOMB_MIN_DATA_SIZE) return false;
    if (std::memcmp(data.data(), BOMB_MAGIC.data(), BOMB_MAGIC.size()) != 0) return false;
    // count is 4-byte big-endian at offset 4.
    uint32_t count = chromatindb::util::read_u32_be(data.data() + 4);
    // Compute expected size in size_t to avoid uint32 overflow for pathological counts.
    size_t expected = size_t{8} + static_cast<size_t>(count) * size_t{32};
    return data.size() == expected;
}

bool validate_bomb_structure(std::span<const uint8_t> data) {
    return is_bomb(data);
}

bool has_bomb_magic(std::span<const uint8_t> data) {
    if (data.size() < BOMB_MIN_DATA_SIZE) return false;
    return std::memcmp(data.data(), BOMB_MAGIC.data(), BOMB_MAGIC.size()) == 0;
}

std::vector<std::array<uint8_t, 32>> extract_bomb_targets(std::span<const uint8_t> data) {
    // Precondition: is_bomb(data). Structural invariants already confirmed.
    uint32_t count = chromatindb::util::read_u32_be(data.data() + 4);
    std::vector<std::array<uint8_t, 32>> targets;
    targets.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::array<uint8_t, 32> t{};
        std::memcpy(t.data(), data.data() + 8 + static_cast<size_t>(i) * 32, 32);
        targets.push_back(t);
    }
    return targets;
}

std::vector<uint8_t> make_bomb_data(std::span<const std::array<uint8_t, 32>> targets) {
    if (targets.size() > static_cast<size_t>(UINT32_MAX)) {
        throw std::invalid_argument("make_bomb_data: target count exceeds UINT32_MAX");
    }
    std::vector<uint8_t> result;
    result.reserve(8 + targets.size() * 32);
    // Magic
    result.insert(result.end(), BOMB_MAGIC.begin(), BOMB_MAGIC.end());
    // count as 4-byte big-endian
    chromatindb::util::write_u32_be(result, static_cast<uint32_t>(targets.size()));
    // Target hashes
    for (const auto& t : targets) {
        result.insert(result.end(), t.begin(), t.end());
    }
    return result;
}

} // namespace chromatindb::wire
