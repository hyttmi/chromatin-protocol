"""Unit tests for chromatindb.types frozen result dataclasses."""

from __future__ import annotations

import dataclasses

import pytest

from chromatindb.types import BlobRef, DeleteResult, ListPage, ReadResult, WriteResult


class TestWriteResult:
    """Tests for WriteResult dataclass."""

    def test_construction(self) -> None:
        blob_hash = b"\x01" * 32
        result = WriteResult(blob_hash=blob_hash, seq_num=42, duplicate=False)
        assert result.blob_hash == blob_hash
        assert result.seq_num == 42
        assert result.duplicate is False

    def test_duplicate_true(self) -> None:
        result = WriteResult(blob_hash=b"\x02" * 32, seq_num=7, duplicate=True)
        assert result.duplicate is True

    def test_frozen(self) -> None:
        result = WriteResult(blob_hash=b"\x03" * 32, seq_num=1, duplicate=False)
        with pytest.raises(dataclasses.FrozenInstanceError):
            result.blob_hash = b"\x04" * 32  # type: ignore[misc]
        with pytest.raises(dataclasses.FrozenInstanceError):
            result.seq_num = 99  # type: ignore[misc]
        with pytest.raises(dataclasses.FrozenInstanceError):
            result.duplicate = True  # type: ignore[misc]

    def test_equality(self) -> None:
        a = WriteResult(blob_hash=b"\xaa" * 32, seq_num=5, duplicate=False)
        b = WriteResult(blob_hash=b"\xaa" * 32, seq_num=5, duplicate=False)
        assert a == b


class TestReadResult:
    """Tests for ReadResult dataclass."""

    def test_construction(self) -> None:
        data = b"hello world"
        sig = b"\xff" * 4627  # ML-DSA-87 signature size
        result = ReadResult(data=data, ttl=3600, timestamp=1700000000, signature=sig)
        assert result.data == data
        assert result.ttl == 3600
        assert result.timestamp == 1700000000
        assert result.signature == sig

    def test_frozen(self) -> None:
        result = ReadResult(
            data=b"test", ttl=60, timestamp=100, signature=b"\x00" * 16
        )
        with pytest.raises(dataclasses.FrozenInstanceError):
            result.data = b"other"  # type: ignore[misc]

    def test_zero_ttl(self) -> None:
        """TTL=0 is valid (permanent blobs / tombstones)."""
        result = ReadResult(data=b"", ttl=0, timestamp=1, signature=b"\x01")
        assert result.ttl == 0


class TestDeleteResult:
    """Tests for DeleteResult dataclass."""

    def test_construction(self) -> None:
        result = DeleteResult(
            tombstone_hash=b"\xab" * 32, seq_num=10, duplicate=False
        )
        assert result.tombstone_hash == b"\xab" * 32
        assert result.seq_num == 10
        assert result.duplicate is False

    def test_duplicate_true(self) -> None:
        result = DeleteResult(
            tombstone_hash=b"\xcd" * 32, seq_num=3, duplicate=True
        )
        assert result.duplicate is True

    def test_frozen(self) -> None:
        result = DeleteResult(
            tombstone_hash=b"\x00" * 32, seq_num=1, duplicate=False
        )
        with pytest.raises(dataclasses.FrozenInstanceError):
            result.tombstone_hash = b"\x01" * 32  # type: ignore[misc]


class TestBlobRef:
    """Tests for BlobRef dataclass."""

    def test_construction(self) -> None:
        ref = BlobRef(blob_hash=b"\xde" * 32, seq_num=99)
        assert ref.blob_hash == b"\xde" * 32
        assert ref.seq_num == 99

    def test_frozen(self) -> None:
        ref = BlobRef(blob_hash=b"\x00" * 32, seq_num=0)
        with pytest.raises(dataclasses.FrozenInstanceError):
            ref.blob_hash = b"\x01" * 32  # type: ignore[misc]


class TestListPage:
    """Tests for ListPage dataclass."""

    def test_empty_list_no_more(self) -> None:
        page = ListPage(blobs=[], cursor=None)
        assert page.blobs == []
        assert page.cursor is None

    def test_with_entries_and_cursor(self) -> None:
        refs = [
            BlobRef(blob_hash=b"\x01" * 32, seq_num=1),
            BlobRef(blob_hash=b"\x02" * 32, seq_num=2),
        ]
        page = ListPage(blobs=refs, cursor=42)
        assert len(page.blobs) == 2
        assert page.blobs[0].seq_num == 1
        assert page.blobs[1].seq_num == 2
        assert page.cursor == 42

    def test_frozen(self) -> None:
        page = ListPage(blobs=[], cursor=None)
        with pytest.raises(dataclasses.FrozenInstanceError):
            page.cursor = 5  # type: ignore[misc]

    def test_single_entry_last_page(self) -> None:
        """Single blob, cursor=None means no more pages."""
        ref = BlobRef(blob_hash=b"\xaa" * 32, seq_num=100)
        page = ListPage(blobs=[ref], cursor=None)
        assert len(page.blobs) == 1
        assert page.cursor is None
