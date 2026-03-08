#include "db/wire/codec.h"
#include "db/wire/blob_generated.h"
#include "db/crypto/hash.h"
#include <flatbuffers/flatbuffers.h>
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

std::vector<uint8_t> build_signing_input(
    std::span<const uint8_t> namespace_id,
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp) {

    // Fixed layout: namespace(32) || data(var) || ttl_le(4) || timestamp_le(8)
    std::vector<uint8_t> buf;
    buf.reserve(namespace_id.size() + data.size() + 4 + 8);

    // Namespace
    buf.insert(buf.end(), namespace_id.begin(), namespace_id.end());

    // Data
    buf.insert(buf.end(), data.begin(), data.end());

    // TTL as little-endian uint32
    buf.push_back(static_cast<uint8_t>(ttl));
    buf.push_back(static_cast<uint8_t>(ttl >> 8));
    buf.push_back(static_cast<uint8_t>(ttl >> 16));
    buf.push_back(static_cast<uint8_t>(ttl >> 24));

    // Timestamp as little-endian uint64
    buf.push_back(static_cast<uint8_t>(timestamp));
    buf.push_back(static_cast<uint8_t>(timestamp >> 8));
    buf.push_back(static_cast<uint8_t>(timestamp >> 16));
    buf.push_back(static_cast<uint8_t>(timestamp >> 24));
    buf.push_back(static_cast<uint8_t>(timestamp >> 32));
    buf.push_back(static_cast<uint8_t>(timestamp >> 40));
    buf.push_back(static_cast<uint8_t>(timestamp >> 48));
    buf.push_back(static_cast<uint8_t>(timestamp >> 56));

    return buf;
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
