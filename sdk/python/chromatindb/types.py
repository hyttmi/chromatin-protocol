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
