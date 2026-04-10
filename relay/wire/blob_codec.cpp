#include "relay/wire/blob_codec.h"
#include "relay/wire/blob_generated.h"

#include <flatbuffers/flatbuffers.h>

namespace chromatindb::relay::wire {

std::vector<uint8_t> encode_blob(const DecodedBlob& blob) {
    flatbuffers::FlatBufferBuilder builder(
        blob.data.size() + blob.pubkey.size() + blob.signature.size() + 256);

    auto ns_vec = builder.CreateVector(blob.namespace_id.data(), blob.namespace_id.size());
    auto pk_vec = builder.CreateVector(blob.pubkey.data(), blob.pubkey.size());
    auto data_vec = builder.CreateVector(blob.data.data(), blob.data.size());
    auto sig_vec = builder.CreateVector(blob.signature.data(), blob.signature.size());

    auto fb_blob = chromatindb::wire::CreateBlob(
        builder, ns_vec, pk_vec, data_vec, blob.ttl, blob.timestamp, sig_vec);
    builder.Finish(fb_blob);

    auto* buf = builder.GetBufferPointer();
    auto size = builder.GetSize();
    return std::vector<uint8_t>(buf, buf + size);
}

std::optional<DecodedBlob> decode_blob(std::span<const uint8_t> data) {
    flatbuffers::Verifier verifier(data.data(), data.size());
    if (!chromatindb::wire::VerifyBlobBuffer(verifier)) {
        return std::nullopt;
    }

    auto fb = chromatindb::wire::GetBlob(data.data());
    if (!fb) {
        return std::nullopt;
    }

    // Require namespace_id and pubkey at minimum.
    if (!fb->namespace_id() || !fb->pubkey()) {
        return std::nullopt;
    }

    DecodedBlob result;
    result.namespace_id.assign(fb->namespace_id()->begin(), fb->namespace_id()->end());
    result.pubkey.assign(fb->pubkey()->begin(), fb->pubkey()->end());

    if (fb->data()) {
        result.data.assign(fb->data()->begin(), fb->data()->end());
    }

    result.ttl = fb->ttl();
    result.timestamp = fb->timestamp();

    if (fb->signature()) {
        result.signature.assign(fb->signature()->begin(), fb->signature()->end());
    }

    return result;
}

} // namespace chromatindb::relay::wire
