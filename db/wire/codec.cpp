#include "db/wire/codec.h"
#include "db/wire/blob_generated.h"
#include "db/crypto/hash.h"
#include "db/crypto/signing.h"
#include "db/util/endian.h"
#include <flatbuffers/flatbuffers.h>
#include <oqs/sha3.h>
#include <cstring>
#include <stdexcept>

namespace chromatindb::wire {

std::vector<uint8_t> encode_blob(const BlobData& blob) {
    // Size builder to avoid reallocations: blob data + pubkey(2592) + sig(4627) + overhead(1024)
    size_t estimated_size = blob.data.size() + 8192;
    flatbuffers::FlatBufferBuilder builder(estimated_size);
    builder.ForceDefaults(true);  // Deterministic: include zero-value fields

    auto ns = builder.CreateVector(blob.namespace_id.data(), blob.namespace_id.size());
    auto pk = builder.CreateVector(blob.pubkey.data(), blob.pubkey.size());
    auto dt = builder.CreateVector(blob.data.data(), blob.data.size());
    auto sg = builder.CreateVector(blob.signature.data(), blob.signature.size());

    auto fb_blob = chromatindb::wire::CreateBlob(builder, ns, pk, dt, blob.ttl, blob.timestamp, sg);
    builder.Finish(fb_blob);

    auto* ptr = builder.GetBufferPointer();
    auto size = builder.GetSize();
    return {ptr, ptr + size};
}

BlobData decode_blob(std::span<const uint8_t> buffer) {
    auto verifier = flatbuffers::Verifier(buffer.data(), buffer.size());
    if (!chromatindb::wire::VerifyBlobBuffer(verifier)) {
        throw std::runtime_error("Invalid FlatBuffer blob data");
    }

    auto fb_blob = chromatindb::wire::GetBlob(buffer.data());
    BlobData result;

    if (fb_blob->namespace_id() && fb_blob->namespace_id()->size() == 32) {
        std::memcpy(result.namespace_id.data(), fb_blob->namespace_id()->data(), 32);
    }

    if (fb_blob->pubkey()) {
        if (fb_blob->pubkey()->size() != chromatindb::crypto::Signer::PUBLIC_KEY_SIZE) {
            throw std::runtime_error("Invalid pubkey size in FlatBuffer blob");
        }
        result.pubkey.assign(fb_blob->pubkey()->begin(), fb_blob->pubkey()->end());
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

std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t> namespace_id,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp) {

    // Incremental SHA3-256: hash namespace || data || ttl_le32 || timestamp_le64
    // directly into the sponge -- zero intermediate allocation.
    OQS_SHA3_sha3_256_inc_ctx ctx;
    OQS_SHA3_sha3_256_inc_init(&ctx);

    // Namespace
    OQS_SHA3_sha3_256_inc_absorb(&ctx, namespace_id.data(), namespace_id.size());

    // Data (may be up to 100 MiB -- fed directly, no copy)
    OQS_SHA3_sha3_256_inc_absorb(&ctx, data.data(), data.size());

    // TTL as little-endian uint32
    uint8_t ttl_le[4] = {
        static_cast<uint8_t>(ttl),
        static_cast<uint8_t>(ttl >> 8),
        static_cast<uint8_t>(ttl >> 16),
        static_cast<uint8_t>(ttl >> 24),
    };
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ttl_le, 4);

    // Timestamp as little-endian uint64
    uint8_t ts_le[8] = {
        static_cast<uint8_t>(timestamp),
        static_cast<uint8_t>(timestamp >> 8),
        static_cast<uint8_t>(timestamp >> 16),
        static_cast<uint8_t>(timestamp >> 24),
        static_cast<uint8_t>(timestamp >> 32),
        static_cast<uint8_t>(timestamp >> 40),
        static_cast<uint8_t>(timestamp >> 48),
        static_cast<uint8_t>(timestamp >> 56),
    };
    OQS_SHA3_sha3_256_inc_absorb(&ctx, ts_le, 8);

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

} // namespace chromatindb::wire
