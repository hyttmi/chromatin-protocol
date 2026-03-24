#include "db/net/protocol.h"

#include <flatbuffers/flatbuffers.h>

namespace chromatindb::net {

std::vector<uint8_t> TransportCodec::encode(chromatindb::wire::TransportMsgType type,
                                             std::span<const uint8_t> payload,
                                             uint32_t request_id) {
    flatbuffers::FlatBufferBuilder builder(payload.size() + 64);
    builder.ForceDefaults(true);

    auto payload_vec = builder.CreateVector(payload.data(), payload.size());
    auto msg = chromatindb::wire::CreateTransportMessage(builder, type, payload_vec, request_id);
    builder.Finish(msg);

    auto* buf = builder.GetBufferPointer();
    auto size = builder.GetSize();
    return std::vector<uint8_t>(buf, buf + size);
}

std::optional<DecodedMessage> TransportCodec::decode(std::span<const uint8_t> data) {
    // Verify the buffer
    flatbuffers::Verifier verifier(data.data(), data.size());
    if (!chromatindb::wire::VerifyTransportMessageBuffer(verifier)) {
        return std::nullopt;
    }

    auto msg = chromatindb::wire::GetTransportMessage(data.data());
    if (!msg) {
        return std::nullopt;
    }

    DecodedMessage result;
    result.type = msg->type();
    result.request_id = msg->request_id();

    if (msg->payload()) {
        result.payload.assign(msg->payload()->begin(), msg->payload()->end());
    }

    return result;
}

} // namespace chromatindb::net
