"""Typed result dataclasses for chromatindb SDK operations."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class WriteResult:
    """Result of a successful write_blob operation (per D-08)."""

    blob_hash: bytes  # 32-byte SHA3-256 hash (server-computed)
    seq_num: int  # Sequence number in namespace
    duplicate: bool  # True if blob already existed


@dataclass(frozen=True)
class ReadResult:
    """Result of a successful read_blob operation (per D-09)."""

    data: bytes  # Blob payload
    ttl: int  # Time-to-live in seconds
    timestamp: int  # Unix timestamp in seconds
    signature: bytes  # ML-DSA-87 signature


@dataclass(frozen=True)
class DeleteResult:
    """Result of a successful delete_blob operation (per D-13)."""

    tombstone_hash: bytes  # 32-byte hash of the tombstone blob
    seq_num: int  # Sequence number of tombstone
    duplicate: bool  # True if tombstone already existed


@dataclass(frozen=True)
class BlobRef:
    """Reference to a blob in a namespace listing (per D-10)."""

    blob_hash: bytes  # 32-byte SHA3-256 hash
    seq_num: int  # Sequence number


@dataclass(frozen=True)
class ListPage:
    """Paginated listing result (per D-11)."""

    blobs: list[BlobRef]  # Blob references in this page
    cursor: int | None  # Pass to after= for next page; None = no more pages


@dataclass(frozen=True)
class MetadataResult:
    """Blob metadata without payload (QUERY-01)."""

    blob_hash: bytes  # 32-byte hash
    timestamp: int  # seconds
    ttl: int  # seconds
    data_size: int  # raw data size in bytes
    seq_num: int  # sequence number
    pubkey: bytes  # ML-DSA-87 public key (2592 bytes)


@dataclass(frozen=True)
class BatchReadResult:
    """Batch read result with truncation flag (D-09, QUERY-03)."""

    blobs: dict[bytes, ReadResult | None]  # hash -> result or None
    truncated: bool


@dataclass(frozen=True)
class TimeRangeEntry:
    """Single entry in a time range query result (QUERY-04)."""

    blob_hash: bytes  # 32-byte hash
    seq_num: int
    timestamp: int  # seconds


@dataclass(frozen=True)
class TimeRangeResult:
    """Time range query result with truncation flag (QUERY-04)."""

    entries: list[TimeRangeEntry]
    truncated: bool


@dataclass(frozen=True)
class NamespaceEntry:
    """Single namespace in a namespace listing (QUERY-05)."""

    namespace_id: bytes  # 32-byte namespace
    blob_count: int


@dataclass(frozen=True)
class NamespaceListResult:
    """Paginated namespace listing (QUERY-05)."""

    namespaces: list[NamespaceEntry]
    cursor: bytes | None  # 32-byte cursor for next page, None if no more


@dataclass(frozen=True)
class NamespaceStats:
    """Per-namespace statistics (QUERY-06)."""

    found: bool
    blob_count: int
    total_bytes: int
    delegation_count: int
    quota_bytes_limit: int
    quota_count_limit: int


@dataclass(frozen=True)
class StorageStatus:
    """Node storage status (QUERY-07)."""

    used_data_bytes: int
    max_storage_bytes: int  # 0 = unlimited
    tombstone_count: int
    namespace_count: int
    total_blobs: int
    mmap_bytes: int


@dataclass(frozen=True)
class NodeInfo:
    """Node info and capabilities (QUERY-08, per D-12 Python-native types)."""

    version: str
    git_hash: str
    uptime_seconds: int
    peer_count: int
    namespace_count: int
    total_blobs: int
    storage_used_bytes: int
    storage_max_bytes: int  # 0 = unlimited
    supported_types: list[int]


@dataclass(frozen=True)
class PeerDetail:
    """Single peer entry in trusted PeerInfo response (QUERY-09)."""

    address: str
    is_bootstrap: bool
    syncing: bool
    peer_is_full: bool
    connected_duration_ms: int


@dataclass(frozen=True)
class PeerInfo:
    """Peer info with trust-gated detail (QUERY-09). Empty peers list if untrusted."""

    peer_count: int
    bootstrap_count: int
    peers: list[PeerDetail]  # empty list if untrusted response


@dataclass(frozen=True)
class DelegationEntry:
    """Single delegation entry (QUERY-10)."""

    delegate_pk_hash: bytes  # 32-byte hash
    delegation_blob_hash: bytes  # 32-byte hash


@dataclass(frozen=True)
class DelegationList:
    """Delegation list for a namespace (QUERY-10)."""

    entries: list[DelegationEntry]


@dataclass(frozen=True)
class Notification:
    """Real-time namespace notification (PUBSUB-03, expanded beyond D-03).

    Includes blob_size and is_tombstone from wire format.
    """

    namespace: bytes  # 32-byte namespace
    blob_hash: bytes  # 32-byte hash
    seq_num: int
    blob_size: int  # uint32 data size
    is_tombstone: bool  # True if notification is for a tombstone
