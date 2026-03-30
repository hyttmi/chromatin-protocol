"""Unit tests for chromatindb._codec binary payload encode/decode."""

from __future__ import annotations

import struct

import pytest

from chromatindb._codec import (
    TOMBSTONE_MAGIC,
    decode_batch_exists_response,
    decode_batch_read_response,
    decode_delegation_list_response,
    decode_delete_ack,
    decode_exists_response,
    decode_list_response,
    decode_metadata_response,
    decode_namespace_list_response,
    decode_namespace_stats_response,
    decode_node_info_response,
    decode_notification,
    decode_peer_info_response,
    decode_read_response,
    decode_storage_status_response,
    decode_time_range_response,
    decode_write_ack,
    encode_batch_exists_request,
    encode_batch_read_request,
    encode_blob_payload,
    encode_delegation_list_request,
    encode_exists_request,
    encode_list_request,
    encode_metadata_request,
    encode_namespace_list_request,
    encode_namespace_stats_request,
    encode_read_request,
    encode_subscribe,
    encode_time_range_request,
    encode_unsubscribe,
    make_tombstone_data,
)
from chromatindb.exceptions import ProtocolError
from chromatindb.generated.blob_generated import Blob
from chromatindb.types import (
    BatchReadResult,
    DelegationEntry,
    DelegationList,
    MetadataResult,
    NamespaceEntry,
    NamespaceListResult,
    NamespaceStats,
    NodeInfo,
    Notification,
    PeerDetail,
    PeerInfo,
    StorageStatus,
    TimeRangeEntry,
    TimeRangeResult,
)


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


# --- Phase 73: New encode/decode tests ---


class TestEncodeMetadataRequest:
    """Tests for MetadataRequest binary payload encoding."""

    def test_encode_metadata_request(self) -> None:
        ns = b"\x01" * 32
        bh = b"\x02" * 32
        result = encode_metadata_request(ns, bh)
        assert result == ns + bh
        assert len(result) == 64

    def test_encode_metadata_request_validates_namespace(self) -> None:
        with pytest.raises(ValueError, match="namespace.*32 bytes"):
            encode_metadata_request(b"\x00" * 16, b"\x00" * 32)

    def test_encode_metadata_request_validates_blob_hash(self) -> None:
        with pytest.raises(ValueError, match="blob_hash.*32 bytes"):
            encode_metadata_request(b"\x00" * 32, b"\x00" * 16)


class TestDecodeMetadataResponse:
    """Tests for MetadataResponse binary payload decoding."""

    def test_decode_metadata_response_found(self) -> None:
        blob_hash = b"\xab" * 32
        timestamp = 1700000000
        ttl = 3600
        data_size = 1024
        seq_num = 42
        pubkey = b"\xcc" * 2592

        payload = (
            bytes([0x01])
            + blob_hash
            + struct.pack(">Q", timestamp)
            + struct.pack(">I", ttl)
            + struct.pack(">Q", data_size)
            + struct.pack(">Q", seq_num)
            + struct.pack(">H", len(pubkey))
            + pubkey
        )

        result = decode_metadata_response(payload)
        assert result is not None
        assert isinstance(result, MetadataResult)
        assert result.blob_hash == blob_hash
        assert result.timestamp == timestamp
        assert result.ttl == ttl
        assert result.data_size == data_size
        assert result.seq_num == seq_num
        assert result.pubkey == pubkey

    def test_decode_metadata_response_not_found(self) -> None:
        result = decode_metadata_response(bytes([0x00]))
        assert result is None

    def test_decode_metadata_response_empty(self) -> None:
        with pytest.raises(ProtocolError, match="empty"):
            decode_metadata_response(b"")

    def test_decode_metadata_response_bad_flag(self) -> None:
        with pytest.raises(ProtocolError, match="unexpected.*flag"):
            decode_metadata_response(bytes([0x02]))

    def test_decode_metadata_response_too_short(self) -> None:
        with pytest.raises(ProtocolError):
            decode_metadata_response(bytes([0x01]) + b"\x00" * 10)

    def test_decode_metadata_response_size_mismatch(self) -> None:
        """pubkey_len says 100 but not enough data follows."""
        blob_hash = b"\xab" * 32
        payload = (
            bytes([0x01])
            + blob_hash
            + struct.pack(">Q", 100)   # timestamp
            + struct.pack(">I", 60)    # ttl
            + struct.pack(">Q", 10)    # data_size
            + struct.pack(">Q", 1)     # seq_num
            + struct.pack(">H", 100)   # pubkey_len=100
            + b"\x00" * 50            # only 50 bytes, not 100
        )
        with pytest.raises(ProtocolError, match="size mismatch"):
            decode_metadata_response(payload)

    def test_decode_metadata_response_small_pubkey(self) -> None:
        """Test with a smaller pubkey to verify variable-length handling."""
        blob_hash = b"\x01" * 32
        pubkey = b"\x02" * 64  # small test pubkey

        payload = (
            bytes([0x01])
            + blob_hash
            + struct.pack(">Q", 200)
            + struct.pack(">I", 120)
            + struct.pack(">Q", 500)
            + struct.pack(">Q", 7)
            + struct.pack(">H", len(pubkey))
            + pubkey
        )

        result = decode_metadata_response(payload)
        assert result is not None
        assert result.pubkey == pubkey
        assert result.timestamp == 200
        assert result.ttl == 120


class TestEncodeBatchExistsRequest:
    """Tests for BatchExistsRequest binary payload encoding."""

    def test_encode_batch_exists_request(self) -> None:
        ns = b"\x01" * 32
        hashes = [b"\xaa" * 32, b"\xbb" * 32, b"\xcc" * 32]
        result = encode_batch_exists_request(ns, hashes)
        assert result[:32] == ns
        count = struct.unpack(">I", result[32:36])[0]
        assert count == 3
        assert result[36:68] == hashes[0]
        assert result[68:100] == hashes[1]
        assert result[100:132] == hashes[2]
        assert len(result) == 36 + 32 * 3

    def test_encode_batch_exists_request_validates_namespace(self) -> None:
        with pytest.raises(ValueError, match="namespace.*32 bytes"):
            encode_batch_exists_request(b"\x00" * 16, [b"\x00" * 32])

    def test_encode_batch_exists_request_validates_hash_length(self) -> None:
        with pytest.raises(ValueError, match="hash.*32 bytes"):
            encode_batch_exists_request(b"\x00" * 32, [b"\x00" * 16])

    def test_encode_batch_exists_single_hash(self) -> None:
        ns = b"\x01" * 32
        hashes = [b"\xaa" * 32]
        result = encode_batch_exists_request(ns, hashes)
        assert len(result) == 36 + 32


class TestDecodeBatchExistsResponse:
    """Tests for BatchExistsResponse binary payload decoding."""

    def test_decode_batch_exists_response(self) -> None:
        hashes = [b"\xaa" * 32, b"\xbb" * 32, b"\xcc" * 32]
        payload = bytes([0x01, 0x00, 0x01])  # exists, not-exists, exists

        result = decode_batch_exists_response(payload, hashes)
        assert isinstance(result, dict)
        assert result[hashes[0]] is True
        assert result[hashes[1]] is False
        assert result[hashes[2]] is True

    def test_decode_batch_exists_response_length_mismatch(self) -> None:
        hashes = [b"\xaa" * 32, b"\xbb" * 32]
        payload = bytes([0x01])  # only 1 byte, 2 hashes
        with pytest.raises(ProtocolError, match="length mismatch"):
            decode_batch_exists_response(payload, hashes)

    def test_decode_batch_exists_response_all_found(self) -> None:
        hashes = [b"\x01" * 32, b"\x02" * 32]
        payload = bytes([0x01, 0x01])
        result = decode_batch_exists_response(payload, hashes)
        assert all(result.values())

    def test_decode_batch_exists_response_none_found(self) -> None:
        hashes = [b"\x01" * 32, b"\x02" * 32]
        payload = bytes([0x00, 0x00])
        result = decode_batch_exists_response(payload, hashes)
        assert not any(result.values())


class TestEncodeBatchReadRequest:
    """Tests for BatchReadRequest binary payload encoding."""

    def test_encode_batch_read_request(self) -> None:
        ns = b"\x01" * 32
        hashes = [b"\xaa" * 32, b"\xbb" * 32]
        result = encode_batch_read_request(ns, hashes, cap_bytes=4_194_304)
        assert result[:32] == ns
        cap = struct.unpack(">I", result[32:36])[0]
        assert cap == 4_194_304
        count = struct.unpack(">I", result[36:40])[0]
        assert count == 2
        assert result[40:72] == hashes[0]
        assert result[72:104] == hashes[1]
        assert len(result) == 40 + 32 * 2

    def test_encode_batch_read_request_default_cap(self) -> None:
        ns = b"\x01" * 32
        hashes = [b"\xaa" * 32]
        result = encode_batch_read_request(ns, hashes)
        cap = struct.unpack(">I", result[32:36])[0]
        assert cap == 0  # 0 means server default (4MiB)

    def test_encode_batch_read_request_validates_namespace(self) -> None:
        with pytest.raises(ValueError, match="namespace.*32 bytes"):
            encode_batch_read_request(b"\x00" * 16, [b"\x00" * 32])

    def test_encode_batch_read_request_validates_hash_length(self) -> None:
        with pytest.raises(ValueError, match="hash.*32 bytes"):
            encode_batch_read_request(b"\x00" * 32, [b"\x00" * 16])


class TestDecodeBatchReadResponse:
    """Tests for BatchReadResponse binary payload decoding."""

    def test_decode_batch_read_response_found_and_not_found(self) -> None:
        """Test with one found entry (FlatBuffer) and one not-found entry."""
        # Build a FlatBuffer blob for the found entry
        ns = b"\x01" * 32
        pk = b"\x02" * 32
        data = b"test data"
        ttl = 3600
        ts = 1700000000
        sig = b"\x03" * 64
        fb_bytes = encode_blob_payload(ns, pk, data, ttl, ts, sig)

        found_hash = b"\xaa" * 32
        missing_hash = b"\xbb" * 32

        # Build response: [truncated:1][count:4BE][entries...]
        entries = b""
        # Found entry: [0x01][hash:32][size:8BE][fb_data:N]
        entries += bytes([0x01]) + found_hash + struct.pack(">Q", len(fb_bytes)) + fb_bytes
        # Not-found entry: [0x00][hash:32]
        entries += bytes([0x00]) + missing_hash

        payload = bytes([0x00]) + struct.pack(">I", 2) + entries

        result = decode_batch_read_response(payload)
        assert isinstance(result, BatchReadResult)
        assert result.truncated is False
        assert len(result.blobs) == 2
        assert result.blobs[found_hash] is not None
        assert result.blobs[found_hash].data == data
        assert result.blobs[found_hash].ttl == ttl
        assert result.blobs[found_hash].timestamp == ts
        assert result.blobs[missing_hash] is None

    def test_decode_batch_read_response_truncated(self) -> None:
        """Empty result with truncation flag."""
        payload = bytes([0x01]) + struct.pack(">I", 0)
        result = decode_batch_read_response(payload)
        assert result.truncated is True
        assert len(result.blobs) == 0

    def test_decode_batch_read_response_too_short(self) -> None:
        with pytest.raises(ProtocolError):
            decode_batch_read_response(b"\x00\x00")

    def test_decode_batch_read_response_all_not_found(self) -> None:
        hash1 = b"\xaa" * 32
        hash2 = b"\xbb" * 32
        entries = bytes([0x00]) + hash1 + bytes([0x00]) + hash2
        payload = bytes([0x00]) + struct.pack(">I", 2) + entries

        result = decode_batch_read_response(payload)
        assert result.blobs[hash1] is None
        assert result.blobs[hash2] is None


class TestEncodeTimeRangeRequest:
    """Tests for TimeRangeRequest binary payload encoding."""

    def test_encode_time_range_request(self) -> None:
        ns = b"\x01" * 32
        result = encode_time_range_request(ns, 1700000000, 1700003600, 50)
        assert len(result) == 52
        assert result[:32] == ns
        start = struct.unpack(">Q", result[32:40])[0]
        end = struct.unpack(">Q", result[40:48])[0]
        limit = struct.unpack(">I", result[48:52])[0]
        assert start == 1700000000
        assert end == 1700003600
        assert limit == 50

    def test_encode_time_range_request_default_limit(self) -> None:
        ns = b"\x01" * 32
        result = encode_time_range_request(ns, 100, 200)
        limit = struct.unpack(">I", result[48:52])[0]
        assert limit == 100  # default

    def test_encode_time_range_request_validates_namespace(self) -> None:
        with pytest.raises(ValueError, match="namespace.*32 bytes"):
            encode_time_range_request(b"\x00" * 16, 0, 100)


class TestDecodeTimeRangeResponse:
    """Tests for TimeRangeResponse binary payload decoding."""

    def test_decode_time_range_response(self) -> None:
        hash1 = b"\xaa" * 32
        hash2 = b"\xbb" * 32

        entries = (
            hash1 + struct.pack(">Q", 10) + struct.pack(">Q", 1700000000)
            + hash2 + struct.pack(">Q", 20) + struct.pack(">Q", 1700000100)
        )
        payload = bytes([0x00]) + struct.pack(">I", 2) + entries

        result = decode_time_range_response(payload)
        assert isinstance(result, TimeRangeResult)
        assert result.truncated is False
        assert len(result.entries) == 2
        assert result.entries[0].blob_hash == hash1
        assert result.entries[0].seq_num == 10
        assert result.entries[0].timestamp == 1700000000
        assert result.entries[1].blob_hash == hash2
        assert result.entries[1].seq_num == 20
        assert result.entries[1].timestamp == 1700000100

    def test_decode_time_range_response_truncated(self) -> None:
        payload = bytes([0x01]) + struct.pack(">I", 0)
        result = decode_time_range_response(payload)
        assert result.truncated is True
        assert len(result.entries) == 0

    def test_decode_time_range_response_too_short(self) -> None:
        with pytest.raises(ProtocolError):
            decode_time_range_response(b"\x00\x00")


class TestEncodeNamespaceListRequest:
    """Tests for NamespaceListRequest binary payload encoding."""

    def test_encode_namespace_list_request(self) -> None:
        after_ns = b"\x00" * 32  # first page
        result = encode_namespace_list_request(after_ns, 100)
        assert len(result) == 36
        assert result[:32] == after_ns
        limit = struct.unpack(">I", result[32:36])[0]
        assert limit == 100

    def test_encode_namespace_list_request_default_limit(self) -> None:
        after_ns = b"\x00" * 32
        result = encode_namespace_list_request(after_ns)
        limit = struct.unpack(">I", result[32:36])[0]
        assert limit == 100

    def test_encode_namespace_list_request_validates_namespace(self) -> None:
        with pytest.raises(ValueError, match="after_ns.*32 bytes"):
            encode_namespace_list_request(b"\x00" * 16)


class TestDecodeNamespaceListResponse:
    """Tests for NamespaceListResponse binary payload decoding."""

    def test_decode_namespace_list_response(self) -> None:
        ns1 = b"\xaa" * 32
        ns2 = b"\xbb" * 32
        entries = (
            ns1 + struct.pack(">Q", 100)
            + ns2 + struct.pack(">Q", 200)
        )
        payload = struct.pack(">I", 2) + bytes([0x01]) + entries  # has_more=True

        result = decode_namespace_list_response(payload)
        assert isinstance(result, NamespaceListResult)
        assert len(result.namespaces) == 2
        assert result.namespaces[0].namespace_id == ns1
        assert result.namespaces[0].blob_count == 100
        assert result.namespaces[1].namespace_id == ns2
        assert result.namespaces[1].blob_count == 200
        assert result.cursor == ns2  # last ns is cursor

    def test_decode_namespace_list_response_no_more(self) -> None:
        payload = struct.pack(">I", 0) + bytes([0x00])  # empty, no more
        result = decode_namespace_list_response(payload)
        assert len(result.namespaces) == 0
        assert result.cursor is None

    def test_decode_namespace_list_response_too_short(self) -> None:
        with pytest.raises(ProtocolError):
            decode_namespace_list_response(b"\x00\x00")


class TestEncodeNamespaceStatsRequest:
    """Tests for NamespaceStatsRequest binary payload encoding."""

    def test_encode_namespace_stats_request(self) -> None:
        ns = b"\x01" * 32
        result = encode_namespace_stats_request(ns)
        assert result == ns
        assert len(result) == 32

    def test_encode_namespace_stats_request_validates_namespace(self) -> None:
        with pytest.raises(ValueError, match="namespace.*32 bytes"):
            encode_namespace_stats_request(b"\x00" * 16)


class TestDecodeNamespaceStatsResponse:
    """Tests for NamespaceStatsResponse binary payload decoding."""

    def test_decode_namespace_stats_response_found(self) -> None:
        payload = (
            bytes([0x01])
            + struct.pack(">Q", 100)       # blob_count
            + struct.pack(">Q", 500_000)   # total_bytes
            + struct.pack(">Q", 3)         # delegation_count
            + struct.pack(">Q", 10_000_000)  # quota_bytes_limit
            + struct.pack(">Q", 5000)      # quota_count_limit
        )
        assert len(payload) == 41

        result = decode_namespace_stats_response(payload)
        assert isinstance(result, NamespaceStats)
        assert result.found is True
        assert result.blob_count == 100
        assert result.total_bytes == 500_000
        assert result.delegation_count == 3
        assert result.quota_bytes_limit == 10_000_000
        assert result.quota_count_limit == 5000

    def test_decode_namespace_stats_response_not_found(self) -> None:
        payload = bytes([0x00]) + b"\x00" * 40
        result = decode_namespace_stats_response(payload)
        assert result.found is False
        assert result.blob_count == 0

    def test_decode_namespace_stats_response_wrong_size(self) -> None:
        with pytest.raises(ProtocolError, match="41 bytes"):
            decode_namespace_stats_response(b"\x00" * 40)
        with pytest.raises(ProtocolError, match="41 bytes"):
            decode_namespace_stats_response(b"\x00" * 42)


class TestDecodeStorageStatusResponse:
    """Tests for StorageStatusResponse binary payload decoding."""

    def test_decode_storage_status_response(self) -> None:
        payload = (
            struct.pack(">Q", 500_000)     # used_data_bytes
            + struct.pack(">Q", 10_000_000)  # max_storage_bytes
            + struct.pack(">Q", 5)          # tombstone_count
            + struct.pack(">I", 3)          # namespace_count (uint32)
            + struct.pack(">Q", 1000)       # total_blobs
            + struct.pack(">Q", 2_000_000)  # mmap_bytes
        )
        assert len(payload) == 44

        result = decode_storage_status_response(payload)
        assert isinstance(result, StorageStatus)
        assert result.used_data_bytes == 500_000
        assert result.max_storage_bytes == 10_000_000
        assert result.tombstone_count == 5
        assert result.namespace_count == 3
        assert result.total_blobs == 1000
        assert result.mmap_bytes == 2_000_000

    def test_decode_storage_status_response_unlimited(self) -> None:
        """max_storage_bytes=0 means unlimited."""
        payload = (
            struct.pack(">Q", 100)
            + struct.pack(">Q", 0)  # unlimited
            + struct.pack(">Q", 0)
            + struct.pack(">I", 1)
            + struct.pack(">Q", 10)
            + struct.pack(">Q", 1024)
        )
        result = decode_storage_status_response(payload)
        assert result.max_storage_bytes == 0

    def test_decode_storage_status_response_wrong_size(self) -> None:
        with pytest.raises(ProtocolError, match="44 bytes"):
            decode_storage_status_response(b"\x00" * 43)
        with pytest.raises(ProtocolError, match="44 bytes"):
            decode_storage_status_response(b"\x00" * 45)


class TestDecodeNodeInfoResponse:
    """Tests for NodeInfoResponse binary payload decoding."""

    def test_decode_node_info_response(self) -> None:
        version = b"1.5.0"
        git_hash = b"abc1234"
        uptime = 86400
        peer_count = 5
        ns_count = 10
        total_blobs = 1000
        storage_used = 500_000
        storage_max = 10_000_000
        supported_types = [1, 2, 3, 39, 40, 47, 48]

        payload = (
            bytes([len(version)]) + version
            + bytes([len(git_hash)]) + git_hash
            + struct.pack(">Q", uptime)
            + struct.pack(">I", peer_count)
            + struct.pack(">I", ns_count)
            + struct.pack(">Q", total_blobs)
            + struct.pack(">Q", storage_used)
            + struct.pack(">Q", storage_max)
            + bytes([len(supported_types)]) + bytes(supported_types)
        )

        result = decode_node_info_response(payload)
        assert isinstance(result, NodeInfo)
        assert result.version == "1.5.0"
        assert result.git_hash == "abc1234"
        assert result.uptime_seconds == uptime
        assert result.peer_count == peer_count
        assert result.namespace_count == ns_count
        assert result.total_blobs == total_blobs
        assert result.storage_used_bytes == storage_used
        assert result.storage_max_bytes == storage_max
        assert result.supported_types == supported_types

    def test_decode_node_info_response_empty_strings(self) -> None:
        """Test with empty version and git hash."""
        payload = (
            bytes([0])  # empty version
            + bytes([0])  # empty git hash
            + struct.pack(">Q", 0)
            + struct.pack(">I", 0)
            + struct.pack(">I", 0)
            + struct.pack(">Q", 0)
            + struct.pack(">Q", 0)
            + struct.pack(">Q", 0)
            + bytes([0])  # no supported types
        )
        result = decode_node_info_response(payload)
        assert result.version == ""
        assert result.git_hash == ""
        assert result.supported_types == []

    def test_decode_node_info_response_too_short(self) -> None:
        with pytest.raises(ProtocolError):
            decode_node_info_response(b"\x05ab")  # version says 5 but only 2 bytes


class TestDecodePeerInfoResponse:
    """Tests for PeerInfoResponse binary payload decoding."""

    def test_decode_peer_info_response_untrusted(self) -> None:
        """8-byte untrusted response: just counts, no peer details."""
        payload = struct.pack(">I", 5) + struct.pack(">I", 2)
        assert len(payload) == 8

        result = decode_peer_info_response(payload)
        assert isinstance(result, PeerInfo)
        assert result.peer_count == 5
        assert result.bootstrap_count == 2
        assert len(result.peers) == 0

    def test_decode_peer_info_response_trusted(self) -> None:
        """Trusted response with per-peer detail."""
        addr1 = b"192.168.1.100:4200"
        addr2 = b"10.0.0.1:4200"

        entries = (
            struct.pack(">H", len(addr1)) + addr1
            + bytes([0x01])  # is_bootstrap
            + bytes([0x00])  # syncing
            + bytes([0x00])  # peer_is_full
            + struct.pack(">Q", 60000)  # connected_duration_ms
            + struct.pack(">H", len(addr2)) + addr2
            + bytes([0x00])  # is_bootstrap
            + bytes([0x01])  # syncing
            + bytes([0x01])  # peer_is_full
            + struct.pack(">Q", 120000)  # connected_duration_ms
        )
        payload = struct.pack(">I", 2) + struct.pack(">I", 1) + entries

        result = decode_peer_info_response(payload)
        assert result.peer_count == 2
        assert result.bootstrap_count == 1
        assert len(result.peers) == 2
        assert result.peers[0].address == "192.168.1.100:4200"
        assert result.peers[0].is_bootstrap is True
        assert result.peers[0].syncing is False
        assert result.peers[0].peer_is_full is False
        assert result.peers[0].connected_duration_ms == 60000
        assert result.peers[1].address == "10.0.0.1:4200"
        assert result.peers[1].is_bootstrap is False
        assert result.peers[1].syncing is True
        assert result.peers[1].peer_is_full is True
        assert result.peers[1].connected_duration_ms == 120000

    def test_decode_peer_info_response_too_short(self) -> None:
        with pytest.raises(ProtocolError):
            decode_peer_info_response(b"\x00" * 4)  # < 8 bytes


class TestEncodeDelegationListRequest:
    """Tests for DelegationListRequest binary payload encoding."""

    def test_encode_delegation_list_request(self) -> None:
        ns = b"\x01" * 32
        result = encode_delegation_list_request(ns)
        assert result == ns
        assert len(result) == 32

    def test_encode_delegation_list_request_validates_namespace(self) -> None:
        with pytest.raises(ValueError, match="namespace.*32 bytes"):
            encode_delegation_list_request(b"\x00" * 16)


class TestDecodeDelegationListResponse:
    """Tests for DelegationListResponse binary payload decoding."""

    def test_decode_delegation_list_response(self) -> None:
        entries = (
            b"\xaa" * 32 + b"\xbb" * 32  # entry 1
            + b"\xcc" * 32 + b"\xdd" * 32  # entry 2
        )
        payload = struct.pack(">I", 2) + entries

        result = decode_delegation_list_response(payload)
        assert isinstance(result, DelegationList)
        assert len(result.entries) == 2
        assert result.entries[0].delegate_pk_hash == b"\xaa" * 32
        assert result.entries[0].delegation_blob_hash == b"\xbb" * 32
        assert result.entries[1].delegate_pk_hash == b"\xcc" * 32
        assert result.entries[1].delegation_blob_hash == b"\xdd" * 32

    def test_decode_delegation_list_response_empty(self) -> None:
        payload = struct.pack(">I", 0)
        result = decode_delegation_list_response(payload)
        assert len(result.entries) == 0

    def test_decode_delegation_list_response_size_mismatch(self) -> None:
        # count=2 but only 64 bytes of entries (need 128)
        payload = struct.pack(">I", 2) + b"\x00" * 64
        with pytest.raises(ProtocolError, match="size mismatch"):
            decode_delegation_list_response(payload)

    def test_decode_delegation_list_response_too_short(self) -> None:
        with pytest.raises(ProtocolError):
            decode_delegation_list_response(b"\x00\x00")


class TestEncodeSubscribe:
    """Tests for Subscribe binary payload encoding."""

    def test_encode_subscribe_single(self) -> None:
        ns = b"\x01" * 32
        result = encode_subscribe([ns])
        assert len(result) == 34
        count = struct.unpack(">H", result[:2])[0]
        assert count == 1
        assert result[2:34] == ns

    def test_encode_subscribe_multiple(self) -> None:
        namespaces = [b"\x01" * 32, b"\x02" * 32, b"\x03" * 32]
        result = encode_subscribe(namespaces)
        assert len(result) == 2 + 32 * 3
        count = struct.unpack(">H", result[:2])[0]
        assert count == 3

    def test_encode_subscribe_validates_namespace_length(self) -> None:
        with pytest.raises(ValueError, match="namespace.*32 bytes"):
            encode_subscribe([b"\x00" * 16])

    def test_encode_subscribe_empty(self) -> None:
        result = encode_subscribe([])
        assert len(result) == 2
        count = struct.unpack(">H", result[:2])[0]
        assert count == 0


class TestEncodeUnsubscribe:
    """Tests for Unsubscribe binary payload encoding."""

    def test_encode_unsubscribe_single(self) -> None:
        ns = b"\x01" * 32
        result = encode_unsubscribe([ns])
        assert len(result) == 34
        count = struct.unpack(">H", result[:2])[0]
        assert count == 1
        assert result[2:34] == ns

    def test_encode_unsubscribe_validates_namespace_length(self) -> None:
        with pytest.raises(ValueError, match="namespace.*32 bytes"):
            encode_unsubscribe([b"\x00" * 31])


class TestDecodeNotification:
    """Tests for Notification binary payload decoding."""

    def test_decode_notification(self) -> None:
        ns = b"\x01" * 32
        blob_hash = b"\x02" * 32
        seq_num = 42
        blob_size = 1024
        is_tombstone = False

        payload = (
            ns + blob_hash
            + struct.pack(">Q", seq_num)
            + struct.pack(">I", blob_size)
            + bytes([0x00])
        )
        assert len(payload) == 77

        result = decode_notification(payload)
        assert isinstance(result, Notification)
        assert result.namespace == ns
        assert result.blob_hash == blob_hash
        assert result.seq_num == seq_num
        assert result.blob_size == blob_size
        assert result.is_tombstone is False

    def test_decode_notification_tombstone(self) -> None:
        payload = (
            b"\xaa" * 32 + b"\xbb" * 32
            + struct.pack(">Q", 10)
            + struct.pack(">I", 36)
            + bytes([0x01])
        )
        result = decode_notification(payload)
        assert result.is_tombstone is True
        assert result.blob_size == 36

    def test_decode_notification_wrong_size(self) -> None:
        with pytest.raises(ProtocolError, match="77 bytes"):
            decode_notification(b"\x00" * 76)
        with pytest.raises(ProtocolError, match="77 bytes"):
            decode_notification(b"\x00" * 78)
