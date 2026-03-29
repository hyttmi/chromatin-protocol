"""FlatBuffers wire format helpers for chromatindb SDK.

Encode/decode TransportMessage envelope used for all communication
with the relay. The underlying generated code is in chromatindb.generated.
"""

from __future__ import annotations

import flatbuffers

from chromatindb.exceptions import DecodeError

# Re-export TransportMsgType for convenience
from chromatindb.generated.transport_generated import TransportMsgType

__all__ = [
    "TransportMsgType",
    "decode_transport_message",
    "encode_transport_message",
]


def encode_transport_message(
    msg_type: int, payload: bytes, request_id: int = 0
) -> bytes:
    """Encode a TransportMessage FlatBuffer.

    Args:
        msg_type: TransportMsgType enum value.
        payload: Message payload bytes.
        request_id: Client-assigned request correlation ID.

    Returns:
        Serialized FlatBuffer bytes.
    """
    builder = flatbuffers.Builder(len(payload) + 64)
    payload_vec = builder.CreateByteVector(payload)

    # Import generated builder functions
    from chromatindb.generated.transport_generated import (
        TransportMessageAddPayload,
        TransportMessageAddRequestId,
        TransportMessageAddType,
        TransportMessageEnd,
        TransportMessageStart,
    )

    TransportMessageStart(builder)
    TransportMessageAddType(builder, msg_type)
    TransportMessageAddPayload(builder, payload_vec)
    TransportMessageAddRequestId(builder, request_id)
    msg = TransportMessageEnd(builder)
    builder.Finish(msg)
    return bytes(builder.Output())


def decode_transport_message(data: bytes) -> tuple[int, bytes, int]:
    """Decode a TransportMessage FlatBuffer.

    Args:
        data: Serialized FlatBuffer bytes.

    Returns:
        Tuple of (msg_type, payload_bytes, request_id).

    Raises:
        DecodeError: If the buffer is invalid.
    """
    from chromatindb.generated.transport_generated import TransportMessage

    try:
        msg = TransportMessage.GetRootAs(data, 0)
        msg_type: int = msg.Type()
        if msg.PayloadIsNone():
            payload = b""
        else:
            length: int = msg.PayloadLength()
            payload = bytes([msg.Payload(j) for j in range(length)])
        request_id = msg.RequestId()
    except DecodeError:
        raise
    except Exception as e:
        raise DecodeError(f"Invalid TransportMessage FlatBuffer: {e}") from e

    return msg_type, payload, request_id
