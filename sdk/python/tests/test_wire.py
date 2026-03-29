"""Tests for chromatindb wire format (FlatBuffers TransportMessage).

Tests validate encode/decode roundtrip, enum values, and error handling.
"""

from __future__ import annotations

import pytest

from chromatindb.exceptions import DecodeError
from chromatindb.wire import (
    TransportMsgType,
    decode_transport_message,
    encode_transport_message,
)


def test_encode_decode_roundtrip():
    """Encode Data type with payload and request_id, decode gets same values."""
    payload = b"hello world"
    msg_type = TransportMsgType.Data
    request_id = 12345

    encoded = encode_transport_message(msg_type, payload, request_id)
    decoded_type, decoded_payload, decoded_rid = decode_transport_message(encoded)

    assert decoded_type == msg_type
    assert decoded_payload == payload
    assert decoded_rid == request_id


def test_encode_decode_empty_payload():
    """Encode with empty payload and zero request_id."""
    encoded = encode_transport_message(TransportMsgType.Ping, b"", 0)
    decoded_type, decoded_payload, decoded_rid = decode_transport_message(encoded)

    assert decoded_type == TransportMsgType.Ping
    assert decoded_payload == b""
    assert decoded_rid == 0


def test_msg_type_values():
    """TransportMsgType enum values match C++ FlatBuffers definition."""
    assert TransportMsgType.Data == 8
    assert TransportMsgType.Ping == 5
    assert TransportMsgType.Goodbye == 7
    assert TransportMsgType.WriteAck == 30
    assert TransportMsgType.ReadRequest == 31


def test_decode_invalid_buffer():
    """Decoding garbage bytes raises DecodeError."""
    with pytest.raises(DecodeError, match="Invalid TransportMessage"):
        decode_transport_message(b"\x00\x01\x02\x03")


def test_various_request_ids():
    """Request IDs roundtrip correctly."""
    for rid in [0, 1, 42, 0xFFFFFFFF]:
        encoded = encode_transport_message(TransportMsgType.ReadRequest, b"x", rid)
        _, _, decoded_rid = decode_transport_message(encoded)
        assert decoded_rid == rid


def test_large_payload():
    """Large payload encodes and decodes correctly."""
    payload = b"\xab" * 10000
    encoded = encode_transport_message(TransportMsgType.Data, payload, 99)
    _, decoded_payload, _ = decode_transport_message(encoded)
    assert decoded_payload == payload
