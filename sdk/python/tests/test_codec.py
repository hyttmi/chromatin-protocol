"""Unit tests for chromatindb._codec binary payload encode/decode."""

from __future__ import annotations

import struct

import pytest

from chromatindb._codec import (
    TOMBSTONE_MAGIC,
    decode_delete_ack,
    decode_exists_response,
    decode_list_response,
    decode_read_response,
    decode_write_ack,
    encode_blob_payload,
    encode_exists_request,
    encode_list_request,
    encode_read_request,
    make_tombstone_data,
)
from chromatindb.exceptions import ProtocolError
from chromatindb.generated.blob_generated import Blob


class TestTombstone:
    """Tests for tombstone data construction."""

    def test_make_tombstone_data(self) -> None:
        target_hash = b"\xaa" * 32
        result = make_tombstone_data(target_hash)
        assert result == b"\xDE\xAD\xBE\xEF" + target_hash
        assert len(result) == 36

    def test_make_tombstone_data_validates_hash_length(self) -> None:
        with pytest.raises(ValueError, match="32 bytes"):
            make_tombstone_data(b"\x00" * 16)
        with pytest.raises(ValueError, match="32 bytes"):
            make_tombstone_data(b"\x00" * 64)

    def test_tombstone_magic_constant(self) -> None:
        assert TOMBSTONE_MAGIC == b"\xDE\xAD\xBE\xEF"
        assert len(TOMBSTONE_MAGIC) == 4


class TestEncodeBlobPayload:
    """Tests for FlatBuffer blob encoding."""

    def test_encode_blob_payload_roundtrip(self) -> None:
        ns = b"\x01" * 32
        pk = b"\x02" * 2592  # ML-DSA-87 public key size
        data = b"hello world"
        ttl = 3600
        ts = 1700000000
        sig = b"\x03" * 4627  # ML-DSA-87 signature size

        encoded = encode_blob_payload(ns, pk, data, ttl, ts, sig)
        assert isinstance(encoded, bytes)
        assert len(encoded) > 0

        # Decode and verify round-trip
        blob = Blob.GetRootAs(encoded, 0)
        decoded_ns = bytes(blob.NamespaceId(j) for j in range(blob.NamespaceIdLength()))
        assert decoded_ns == ns
        decoded_pk = bytes(blob.Pubkey(j) for j in range(blob.PubkeyLength()))
        assert decoded_pk == pk
        decoded_data = bytes(blob.Data(j) for j in range(blob.DataLength()))
        assert decoded_data == data
        assert blob.Ttl() == ttl
        assert blob.Timestamp() == ts
        decoded_sig = bytes(blob.Signature(j) for j in range(blob.SignatureLength()))
        assert decoded_sig == sig

    def test_encode_blob_payload_force_defaults(self) -> None:
        """ttl=0 (tombstone) must still encode the ttl field."""
        ns = b"\x01" * 32
        pk = b"\x02" * 32
        data = b"\xDE\xAD\xBE\xEF" + b"\x00" * 32  # tombstone data
        ttl = 0  # permanent
        ts = 1700000000
        sig = b"\x03" * 64

        encoded = encode_blob_payload(ns, pk, data, ttl, ts, sig)
        blob = Blob.GetRootAs(encoded, 0)
        # Without ForceDefaults, Ttl() would return 0 but the field wouldn't
        # be present in the buffer. With ForceDefaults it IS present.
        assert blob.Ttl() == 0
        assert blob.Timestamp() == ts

    def test_encode_blob_payload_empty_data(self) -> None:
        """Empty data blob is valid."""
        encoded = encode_blob_payload(
            b"\x01" * 32, b"\x02" * 32, b"", 60, 100, b"\x03" * 16
        )
        blob = Blob.GetRootAs(encoded, 0)
        assert blob.DataLength() == 0


class TestDecodeWriteAck:
    """Tests for WriteAck binary payload decoding."""

    def test_decode_write_ack(self) -> None:
        blob_hash = b"\xab" * 32
        seq_num = 42
        status = 0  # stored
        payload = blob_hash + struct.pack(">Q", seq_num) + bytes([status])
        assert len(payload) == 41

        h, s, d = decode_write_ack(payload)
        assert h == blob_hash
        assert s == 42
        assert d is False

    def test_decode_write_ack_duplicate(self) -> None:
        blob_hash = b"\xcd" * 32
        seq_num = 7
        status = 1  # duplicate
        payload = blob_hash + struct.pack(">Q", seq_num) + bytes([status])

        h, s, d = decode_write_ack(payload)
        assert h == blob_hash
        assert s == 7
        assert d is True

    def test_decode_write_ack_wrong_size(self) -> None:
        with pytest.raises(ProtocolError, match="41 bytes"):
            decode_write_ack(b"\x00" * 40)
        with pytest.raises(ProtocolError, match="41 bytes"):
            decode_write_ack(b"\x00" * 42)

    def test_decode_write_ack_large_seq(self) -> None:
        """Verify big-endian uint64 for large sequence numbers."""
        blob_hash = b"\x00" * 32
        seq_num = 2**48 + 123456
        payload = blob_hash + struct.pack(">Q", seq_num) + bytes([0])

        _, s, _ = decode_write_ack(payload)
        assert s == seq_num


class TestDecodeDeleteAck:
    """Tests for DeleteAck binary payload decoding."""

    def test_decode_delete_ack(self) -> None:
        blob_hash = b"\xef" * 32
        seq_num = 100
        status = 0  # stored
        payload = blob_hash + struct.pack(">Q", seq_num) + bytes([status])

        h, s, d = decode_delete_ack(payload)
        assert h == blob_hash
        assert s == 100
        assert d is False

    def test_decode_delete_ack_wrong_size(self) -> None:
        with pytest.raises(ProtocolError, match="41 bytes"):
            decode_delete_ack(b"\x00" * 10)


class TestEncodeReadRequest:
    """Tests for ReadRequest binary payload encoding."""

    def test_encode_read_request(self) -> None:
        ns = b"\x01" * 32
        bh = b"\x02" * 32
        result = encode_read_request(ns, bh)
        assert result == ns + bh
        assert len(result) == 64

    def test_encode_read_request_validates_lengths(self) -> None:
        with pytest.raises(ValueError, match="namespace.*32 bytes"):
            encode_read_request(b"\x00" * 16, b"\x00" * 32)
        with pytest.raises(ValueError, match="blob_hash.*32 bytes"):
            encode_read_request(b"\x00" * 32, b"\x00" * 16)


class TestDecodeReadResponse:
    """Tests for ReadResponse binary payload decoding."""

    def test_decode_read_response_found(self) -> None:
        # Build a FlatBuffer blob payload
        ns = b"\x01" * 32
        pk = b"\x02" * 32
        data = b"test data"
        ttl = 3600
        ts = 1700000000
        sig = b"\x03" * 64

        fb_bytes = encode_blob_payload(ns, pk, data, ttl, ts, sig)
        payload = bytes([0x01]) + fb_bytes

        result = decode_read_response(payload)
        assert result is not None
        d, t, stamp, s = result
        assert d == data
        assert t == ttl
        assert stamp == ts
        assert s == sig

    def test_decode_read_response_not_found(self) -> None:
        result = decode_read_response(bytes([0x00]))
        assert result is None

    def test_decode_read_response_empty(self) -> None:
        with pytest.raises(ProtocolError, match="empty"):
            decode_read_response(b"")


class TestEncodeListRequest:
    """Tests for ListRequest binary payload encoding."""

    def test_encode_list_request(self) -> None:
        ns = b"\x01" * 32
        since_seq = 42
        limit = 100
        result = encode_list_request(ns, since_seq, limit)
        assert len(result) == 44
        assert result[:32] == ns
        assert struct.unpack(">Q", result[32:40])[0] == 42
        assert struct.unpack(">I", result[40:44])[0] == 100

    def test_encode_list_request_validates_namespace(self) -> None:
        with pytest.raises(ValueError, match="namespace.*32 bytes"):
            encode_list_request(b"\x00" * 16, 0, 100)


class TestDecodeListResponse:
    """Tests for ListResponse binary payload decoding."""

    def test_decode_list_response(self) -> None:
        """Two entries, has_more=True."""
        hash1 = b"\xaa" * 32
        seq1 = 10
        hash2 = b"\xbb" * 32
        seq2 = 20
        count = 2
        payload = (
            struct.pack(">I", count)
            + hash1 + struct.pack(">Q", seq1)
            + hash2 + struct.pack(">Q", seq2)
            + bytes([1])  # has_more
        )

        entries, has_more = decode_list_response(payload)
        assert len(entries) == 2
        assert entries[0] == (hash1, 10)
        assert entries[1] == (hash2, 20)
        assert has_more is True

    def test_decode_list_response_empty(self) -> None:
        """Zero entries, no more pages."""
        payload = struct.pack(">I", 0) + bytes([0])
        entries, has_more = decode_list_response(payload)
        assert len(entries) == 0
        assert has_more is False

    def test_decode_list_response_wrong_size(self) -> None:
        """Payload too short."""
        with pytest.raises(ProtocolError):
            decode_list_response(b"\x00\x00\x00")  # < 5 bytes

    def test_decode_list_response_size_mismatch(self) -> None:
        """Count says 1 entry but payload has 0 entry data."""
        payload = struct.pack(">I", 1) + bytes([0])  # Missing 40 bytes of entry data
        with pytest.raises(ProtocolError, match="size mismatch"):
            decode_list_response(payload)


class TestEncodeExistsRequest:
    """Tests for ExistsRequest binary payload encoding."""

    def test_encode_exists_request(self) -> None:
        ns = b"\x01" * 32
        bh = b"\x02" * 32
        result = encode_exists_request(ns, bh)
        assert result == ns + bh
        assert len(result) == 64

    def test_encode_exists_request_validates_lengths(self) -> None:
        with pytest.raises(ValueError, match="namespace.*32 bytes"):
            encode_exists_request(b"\x00" * 31, b"\x00" * 32)
        with pytest.raises(ValueError, match="blob_hash.*32 bytes"):
            encode_exists_request(b"\x00" * 32, b"\x00" * 31)


class TestDecodeExistsResponse:
    """Tests for ExistsResponse binary payload decoding."""

    def test_decode_exists_response_found(self) -> None:
        blob_hash = b"\xab" * 32
        payload = bytes([1]) + blob_hash
        exists, h = decode_exists_response(payload)
        assert exists is True
        assert h == blob_hash

    def test_decode_exists_response_not_found(self) -> None:
        blob_hash = b"\xcd" * 32
        payload = bytes([0]) + blob_hash
        exists, h = decode_exists_response(payload)
        assert exists is False
        assert h == blob_hash

    def test_decode_exists_response_wrong_size(self) -> None:
        with pytest.raises(ProtocolError, match="33 bytes"):
            decode_exists_response(b"\x00" * 32)
        with pytest.raises(ProtocolError, match="33 bytes"):
            decode_exists_response(b"\x00" * 34)
