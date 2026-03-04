#include "net/protocol.h"

#include <flatbuffers/flatbuffers.h>

namespace chromatin::net {

std::vector<uint8_t> TransportCodec::encode(chromatin::wire::TransportMsgType type,
                                             std::span<const uint8_t> payload) {
    flatbuffers::FlatBufferBuilder builder(payload.size() + 64);
    builder.ForceDefaults(true);

    auto payload_vec = builder.CreateVector(payload.data(), payload.size());
    auto msg = chromatin::wire::CreateTransportMessage(builder, type, payload_vec);
    builder.Finish(msg);

    auto* buf = builder.GetBufferPointer();
    auto size = builder.GetSize();
    return std::vector<uint8_t>(buf, buf + size);
}

std::optional<DecodedMessage> TransportCodec::decode(std::span<const uint8_t> data) {
    // Verify the buffer
    flatbuffers::Verifier verifier(data.data(), data.size());
    if (!chromatin::wire::VerifyTransportMessageBuffer(verifier)) {
        return std::nullopt;
    }

    auto msg = chromatin::wire::GetTransportMessage(data.data());
    if (!msg) {
        return std::nullopt;
    }

    DecodedMessage result;
    result.type = msg->type();

    if (msg->payload()) {
        result.payload.assign(msg->payload()->begin(), msg->payload()->end());
    }

    return result;
}

} // namespace chromatin::net
