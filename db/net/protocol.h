#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "db/wire/transport_generated.h"

namespace chromatindb::net {

/// Decoded transport message.
struct DecodedMessage {
    chromatindb::wire::TransportMsgType type;
    std::vector<uint8_t> payload;
};

/// Transport message codec using FlatBuffers.
/// Encodes/decodes TransportMessage for wire transmission.
struct TransportCodec {
    /// Encode a transport message to FlatBuffer bytes.
    static std::vector<uint8_t> encode(chromatindb::wire::TransportMsgType type,
                                        std::span<const uint8_t> payload);

    /// Decode FlatBuffer bytes to a transport message.
    /// Returns nullopt if the buffer is invalid.
    static std::optional<DecodedMessage> decode(std::span<const uint8_t> data);
};

} // namespace chromatindb::net
