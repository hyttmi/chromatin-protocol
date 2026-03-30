"""Unit tests for chromatindb.types frozen result dataclasses."""

from __future__ import annotations

import dataclasses

import pytest

from chromatindb.types import (
    BatchReadResult,
    BlobRef,
    DelegationEntry,
    DelegationList,
    DeleteResult,
    ListPage,
    MetadataResult,
    NamespaceEntry,
    NamespaceListResult,
    NamespaceStats,
    NodeInfo,
    Notification,
    PeerDetail,
    PeerInfo,
    ReadResult,
    StorageStatus,
    TimeRangeEntry,
    TimeRangeResult,
    WriteResult,
)


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


class TestMetadataResult:
    """Tests for MetadataResult dataclass."""

    def test_construction(self) -> None:
        result = MetadataResult(
            blob_hash=b"\x01" * 32,
            timestamp=1700000000,
            ttl=3600,
            data_size=1024,
            seq_num=42,
            pubkey=b"\x02" * 2592,
        )
        assert result.blob_hash == b"\x01" * 32
        assert result.timestamp == 1700000000
        assert result.ttl == 3600
        assert result.data_size == 1024
        assert result.seq_num == 42
        assert result.pubkey == b"\x02" * 2592

    def test_frozen(self) -> None:
        result = MetadataResult(
            blob_hash=b"\x01" * 32,
            timestamp=100,
            ttl=60,
            data_size=10,
            seq_num=1,
            pubkey=b"\x02" * 32,
        )
        with pytest.raises(dataclasses.FrozenInstanceError):
            result.blob_hash = b"\x03" * 32  # type: ignore[misc]

    def test_equality(self) -> None:
        a = MetadataResult(
            blob_hash=b"\xaa" * 32, timestamp=100, ttl=60,
            data_size=10, seq_num=1, pubkey=b"\xbb" * 32,
        )
        b = MetadataResult(
            blob_hash=b"\xaa" * 32, timestamp=100, ttl=60,
            data_size=10, seq_num=1, pubkey=b"\xbb" * 32,
        )
        assert a == b


class TestBatchReadResult:
    """Tests for BatchReadResult dataclass."""

    def test_construction(self) -> None:
        blobs = {
            b"\x01" * 32: ReadResult(
                data=b"hello", ttl=3600, timestamp=100, signature=b"\xff" * 64
            ),
            b"\x02" * 32: None,
        }
        result = BatchReadResult(blobs=blobs, truncated=False)
        assert result.blobs[b"\x01" * 32] is not None
        assert result.blobs[b"\x02" * 32] is None
        assert result.truncated is False

    def test_truncated_true(self) -> None:
        result = BatchReadResult(blobs={}, truncated=True)
        assert result.truncated is True

    def test_frozen(self) -> None:
        result = BatchReadResult(blobs={}, truncated=False)
        with pytest.raises(dataclasses.FrozenInstanceError):
            result.truncated = True  # type: ignore[misc]

    def test_equality(self) -> None:
        a = BatchReadResult(blobs={}, truncated=False)
        b = BatchReadResult(blobs={}, truncated=False)
        assert a == b


class TestTimeRangeEntry:
    """Tests for TimeRangeEntry dataclass."""

    def test_construction(self) -> None:
        entry = TimeRangeEntry(
            blob_hash=b"\xab" * 32, seq_num=10, timestamp=1700000000
        )
        assert entry.blob_hash == b"\xab" * 32
        assert entry.seq_num == 10
        assert entry.timestamp == 1700000000

    def test_frozen(self) -> None:
        entry = TimeRangeEntry(blob_hash=b"\x00" * 32, seq_num=0, timestamp=0)
        with pytest.raises(dataclasses.FrozenInstanceError):
            entry.seq_num = 99  # type: ignore[misc]

    def test_equality(self) -> None:
        a = TimeRangeEntry(blob_hash=b"\x01" * 32, seq_num=5, timestamp=100)
        b = TimeRangeEntry(blob_hash=b"\x01" * 32, seq_num=5, timestamp=100)
        assert a == b


class TestTimeRangeResult:
    """Tests for TimeRangeResult dataclass."""

    def test_construction(self) -> None:
        entries = [
            TimeRangeEntry(blob_hash=b"\x01" * 32, seq_num=1, timestamp=100),
            TimeRangeEntry(blob_hash=b"\x02" * 32, seq_num=2, timestamp=200),
        ]
        result = TimeRangeResult(entries=entries, truncated=False)
        assert len(result.entries) == 2
        assert result.truncated is False

    def test_empty_entries(self) -> None:
        result = TimeRangeResult(entries=[], truncated=False)
        assert len(result.entries) == 0

    def test_frozen(self) -> None:
        result = TimeRangeResult(entries=[], truncated=False)
        with pytest.raises(dataclasses.FrozenInstanceError):
            result.truncated = True  # type: ignore[misc]


class TestNamespaceEntry:
    """Tests for NamespaceEntry dataclass."""

    def test_construction(self) -> None:
        entry = NamespaceEntry(namespace_id=b"\xaa" * 32, blob_count=42)
        assert entry.namespace_id == b"\xaa" * 32
        assert entry.blob_count == 42

    def test_frozen(self) -> None:
        entry = NamespaceEntry(namespace_id=b"\x00" * 32, blob_count=0)
        with pytest.raises(dataclasses.FrozenInstanceError):
            entry.blob_count = 99  # type: ignore[misc]

    def test_equality(self) -> None:
        a = NamespaceEntry(namespace_id=b"\x01" * 32, blob_count=5)
        b = NamespaceEntry(namespace_id=b"\x01" * 32, blob_count=5)
        assert a == b


class TestNamespaceListResult:
    """Tests for NamespaceListResult dataclass."""

    def test_construction_with_cursor(self) -> None:
        ns = [NamespaceEntry(namespace_id=b"\x01" * 32, blob_count=10)]
        result = NamespaceListResult(namespaces=ns, cursor=b"\x01" * 32)
        assert len(result.namespaces) == 1
        assert result.cursor == b"\x01" * 32

    def test_no_more_pages(self) -> None:
        result = NamespaceListResult(namespaces=[], cursor=None)
        assert result.cursor is None

    def test_frozen(self) -> None:
        result = NamespaceListResult(namespaces=[], cursor=None)
        with pytest.raises(dataclasses.FrozenInstanceError):
            result.cursor = b"\x01" * 32  # type: ignore[misc]


class TestNamespaceStats:
    """Tests for NamespaceStats dataclass."""

    def test_construction(self) -> None:
        stats = NamespaceStats(
            found=True, blob_count=100, total_bytes=1_000_000,
            delegation_count=3, quota_bytes_limit=10_000_000,
            quota_count_limit=5000,
        )
        assert stats.found is True
        assert stats.blob_count == 100
        assert stats.total_bytes == 1_000_000
        assert stats.delegation_count == 3
        assert stats.quota_bytes_limit == 10_000_000
        assert stats.quota_count_limit == 5000

    def test_not_found(self) -> None:
        stats = NamespaceStats(
            found=False, blob_count=0, total_bytes=0,
            delegation_count=0, quota_bytes_limit=0, quota_count_limit=0,
        )
        assert stats.found is False

    def test_frozen(self) -> None:
        stats = NamespaceStats(
            found=True, blob_count=1, total_bytes=1,
            delegation_count=0, quota_bytes_limit=0, quota_count_limit=0,
        )
        with pytest.raises(dataclasses.FrozenInstanceError):
            stats.blob_count = 99  # type: ignore[misc]

    def test_equality(self) -> None:
        a = NamespaceStats(
            found=True, blob_count=10, total_bytes=100,
            delegation_count=1, quota_bytes_limit=0, quota_count_limit=0,
        )
        b = NamespaceStats(
            found=True, blob_count=10, total_bytes=100,
            delegation_count=1, quota_bytes_limit=0, quota_count_limit=0,
        )
        assert a == b


class TestStorageStatus:
    """Tests for StorageStatus dataclass."""

    def test_construction(self) -> None:
        status = StorageStatus(
            used_data_bytes=500_000, max_storage_bytes=10_000_000,
            tombstone_count=5, namespace_count=3,
            total_blobs=1000, mmap_bytes=2_000_000,
        )
        assert status.used_data_bytes == 500_000
        assert status.max_storage_bytes == 10_000_000
        assert status.tombstone_count == 5
        assert status.namespace_count == 3
        assert status.total_blobs == 1000
        assert status.mmap_bytes == 2_000_000

    def test_unlimited_storage(self) -> None:
        """max_storage_bytes=0 means unlimited."""
        status = StorageStatus(
            used_data_bytes=100, max_storage_bytes=0,
            tombstone_count=0, namespace_count=1,
            total_blobs=10, mmap_bytes=1024,
        )
        assert status.max_storage_bytes == 0

    def test_frozen(self) -> None:
        status = StorageStatus(
            used_data_bytes=0, max_storage_bytes=0,
            tombstone_count=0, namespace_count=0,
            total_blobs=0, mmap_bytes=0,
        )
        with pytest.raises(dataclasses.FrozenInstanceError):
            status.used_data_bytes = 99  # type: ignore[misc]


class TestNodeInfo:
    """Tests for NodeInfo dataclass."""

    def test_construction(self) -> None:
        info = NodeInfo(
            version="1.5.0", git_hash="abc1234",
            uptime_seconds=86400, peer_count=5,
            namespace_count=10, total_blobs=1000,
            storage_used_bytes=500_000, storage_max_bytes=10_000_000,
            supported_types=[1, 2, 3, 39, 40],
        )
        assert info.version == "1.5.0"
        assert info.git_hash == "abc1234"
        assert info.uptime_seconds == 86400
        assert info.peer_count == 5
        assert info.namespace_count == 10
        assert info.total_blobs == 1000
        assert info.storage_used_bytes == 500_000
        assert info.storage_max_bytes == 10_000_000
        assert info.supported_types == [1, 2, 3, 39, 40]

    def test_frozen(self) -> None:
        info = NodeInfo(
            version="1.0.0", git_hash="x",
            uptime_seconds=0, peer_count=0,
            namespace_count=0, total_blobs=0,
            storage_used_bytes=0, storage_max_bytes=0,
            supported_types=[],
        )
        with pytest.raises(dataclasses.FrozenInstanceError):
            info.version = "2.0.0"  # type: ignore[misc]

    def test_equality(self) -> None:
        a = NodeInfo(
            version="1.0.0", git_hash="abc",
            uptime_seconds=100, peer_count=2,
            namespace_count=1, total_blobs=50,
            storage_used_bytes=1000, storage_max_bytes=0,
            supported_types=[1, 2],
        )
        b = NodeInfo(
            version="1.0.0", git_hash="abc",
            uptime_seconds=100, peer_count=2,
            namespace_count=1, total_blobs=50,
            storage_used_bytes=1000, storage_max_bytes=0,
            supported_types=[1, 2],
        )
        assert a == b


class TestPeerDetail:
    """Tests for PeerDetail dataclass."""

    def test_construction(self) -> None:
        detail = PeerDetail(
            address="192.168.1.100:4200",
            is_bootstrap=True, syncing=False,
            peer_is_full=False, connected_duration_ms=60000,
        )
        assert detail.address == "192.168.1.100:4200"
        assert detail.is_bootstrap is True
        assert detail.syncing is False
        assert detail.peer_is_full is False
        assert detail.connected_duration_ms == 60000

    def test_frozen(self) -> None:
        detail = PeerDetail(
            address="127.0.0.1:4200",
            is_bootstrap=False, syncing=False,
            peer_is_full=False, connected_duration_ms=0,
        )
        with pytest.raises(dataclasses.FrozenInstanceError):
            detail.address = "other"  # type: ignore[misc]

    def test_equality(self) -> None:
        a = PeerDetail(
            address="10.0.0.1:4200", is_bootstrap=False,
            syncing=True, peer_is_full=False, connected_duration_ms=1000,
        )
        b = PeerDetail(
            address="10.0.0.1:4200", is_bootstrap=False,
            syncing=True, peer_is_full=False, connected_duration_ms=1000,
        )
        assert a == b


class TestPeerInfo:
    """Tests for PeerInfo dataclass."""

    def test_construction_trusted(self) -> None:
        peers = [
            PeerDetail(
                address="192.168.1.100:4200",
                is_bootstrap=True, syncing=False,
                peer_is_full=False, connected_duration_ms=60000,
            ),
        ]
        info = PeerInfo(peer_count=3, bootstrap_count=1, peers=peers)
        assert info.peer_count == 3
        assert info.bootstrap_count == 1
        assert len(info.peers) == 1

    def test_untrusted_empty_peers(self) -> None:
        """Untrusted response has empty peers list."""
        info = PeerInfo(peer_count=5, bootstrap_count=2, peers=[])
        assert info.peer_count == 5
        assert info.bootstrap_count == 2
        assert len(info.peers) == 0

    def test_frozen(self) -> None:
        info = PeerInfo(peer_count=0, bootstrap_count=0, peers=[])
        with pytest.raises(dataclasses.FrozenInstanceError):
            info.peer_count = 99  # type: ignore[misc]


class TestDelegationEntry:
    """Tests for DelegationEntry dataclass."""

    def test_construction(self) -> None:
        entry = DelegationEntry(
            delegate_pk_hash=b"\xaa" * 32,
            delegation_blob_hash=b"\xbb" * 32,
        )
        assert entry.delegate_pk_hash == b"\xaa" * 32
        assert entry.delegation_blob_hash == b"\xbb" * 32

    def test_frozen(self) -> None:
        entry = DelegationEntry(
            delegate_pk_hash=b"\x00" * 32,
            delegation_blob_hash=b"\x01" * 32,
        )
        with pytest.raises(dataclasses.FrozenInstanceError):
            entry.delegate_pk_hash = b"\xff" * 32  # type: ignore[misc]

    def test_equality(self) -> None:
        a = DelegationEntry(
            delegate_pk_hash=b"\x01" * 32,
            delegation_blob_hash=b"\x02" * 32,
        )
        b = DelegationEntry(
            delegate_pk_hash=b"\x01" * 32,
            delegation_blob_hash=b"\x02" * 32,
        )
        assert a == b


class TestDelegationList:
    """Tests for DelegationList dataclass."""

    def test_construction(self) -> None:
        entries = [
            DelegationEntry(
                delegate_pk_hash=b"\xaa" * 32,
                delegation_blob_hash=b"\xbb" * 32,
            ),
        ]
        result = DelegationList(entries=entries)
        assert len(result.entries) == 1

    def test_empty(self) -> None:
        result = DelegationList(entries=[])
        assert len(result.entries) == 0

    def test_frozen(self) -> None:
        result = DelegationList(entries=[])
        with pytest.raises(dataclasses.FrozenInstanceError):
            result.entries = []  # type: ignore[misc]


class TestNotification:
    """Tests for Notification dataclass."""

    def test_construction(self) -> None:
        notif = Notification(
            namespace=b"\x01" * 32,
            blob_hash=b"\x02" * 32,
            seq_num=42,
            blob_size=1024,
            is_tombstone=False,
        )
        assert notif.namespace == b"\x01" * 32
        assert notif.blob_hash == b"\x02" * 32
        assert notif.seq_num == 42
        assert notif.blob_size == 1024
        assert notif.is_tombstone is False

    def test_tombstone_notification(self) -> None:
        notif = Notification(
            namespace=b"\xaa" * 32,
            blob_hash=b"\xbb" * 32,
            seq_num=10,
            blob_size=36,
            is_tombstone=True,
        )
        assert notif.is_tombstone is True
        assert notif.blob_size == 36

    def test_frozen(self) -> None:
        notif = Notification(
            namespace=b"\x00" * 32,
            blob_hash=b"\x00" * 32,
            seq_num=0,
            blob_size=0,
            is_tombstone=False,
        )
        with pytest.raises(dataclasses.FrozenInstanceError):
            notif.seq_num = 99  # type: ignore[misc]

    def test_equality(self) -> None:
        a = Notification(
            namespace=b"\x01" * 32, blob_hash=b"\x02" * 32,
            seq_num=5, blob_size=100, is_tombstone=False,
        )
        b = Notification(
            namespace=b"\x01" * 32, blob_hash=b"\x02" * 32,
            seq_num=5, blob_size=100, is_tombstone=False,
        )
        assert a == b
