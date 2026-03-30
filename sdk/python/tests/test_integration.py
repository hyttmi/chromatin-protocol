"""Integration tests for chromatindb SDK against live KVM relay.

These tests require a running chromatindb relay.
Configure via environment variables (per D-13):
  CHROMATINDB_RELAY_HOST (default: 192.168.1.200)
  CHROMATINDB_RELAY_PORT (default: 4201)

Run: pytest tests/test_integration.py -v -m integration
Skip: automatically skipped if relay is unreachable
"""

from __future__ import annotations

import os
import socket

import pytest

from chromatindb import ChromatinClient
from chromatindb.exceptions import HandshakeError
from chromatindb.identity import Identity
from chromatindb.types import WriteResult, ReadResult, DeleteResult, BlobRef, ListPage

RELAY_HOST = os.environ.get("CHROMATINDB_RELAY_HOST", "192.168.1.200")
RELAY_PORT = int(os.environ.get("CHROMATINDB_RELAY_PORT", "4201"))


def _relay_reachable() -> bool:
    """Check if relay is reachable via TCP connect (1s timeout)."""
    try:
        sock = socket.create_connection((RELAY_HOST, RELAY_PORT), timeout=1.0)
        sock.close()
        return True
    except (OSError, socket.timeout):
        return False


pytestmark = [
    pytest.mark.integration,
    pytest.mark.skipif(
        not _relay_reachable(),
        reason=f"relay at {RELAY_HOST}:{RELAY_PORT} unreachable",
    ),
]


async def test_handshake_connect_disconnect() -> None:
    """PQ handshake + Goodbye (XPORT-02, XPORT-03, XPORT-07)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        assert not conn._transport.closed
    # After context manager exit, transport should be closed


async def test_ping_pong() -> None:
    """Send Ping, receive Pong over AEAD-encrypted channel (XPORT-04, XPORT-05)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        await conn.ping()  # Should not raise
        await conn.ping()  # Second ping to verify nonce counters advance correctly
        await conn.ping()  # Third for good measure


async def test_multiple_connections() -> None:
    """Two sequential connections work with different identities."""
    id1 = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, id1) as conn:
        await conn.ping()

    id2 = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, id2) as conn:
        await conn.ping()


async def test_handshake_timeout() -> None:
    """Very short timeout raises HandshakeError (XPORT-07)."""
    identity = Identity.generate()
    # Use a non-routable IP to guarantee timeout (not the relay)
    with pytest.raises((HandshakeError, OSError)):
        async with ChromatinClient.connect(
            "192.0.2.1", 4433, identity, timeout=0.5
        ) as conn:
            pass


# ------------------------------------------------------------------
# Data operation integration tests (Phase 72, Plan 03)
# ------------------------------------------------------------------


async def test_write_blob() -> None:
    """Write a blob and verify WriteResult fields (DATA-01)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        result = await conn.write_blob(data=b"hello integration", ttl=3600)
        assert isinstance(result, WriteResult)
        assert len(result.blob_hash) == 32
        assert result.seq_num >= 1
        assert result.duplicate is False


async def test_write_blob_duplicate() -> None:
    """Writing same data twice produces distinct blobs -- ML-DSA-87 signatures are
    non-deterministic, so each write generates a unique FlatBuffer and thus a unique
    blob_hash. Both writes succeed with duplicate=False (DATA-01)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        r1 = await conn.write_blob(data=b"dup test", ttl=3600)
        r2 = await conn.write_blob(data=b"dup test", ttl=3600)
        # Non-deterministic ML-DSA-87 signatures mean different blob_hash each time
        assert r1.blob_hash != r2.blob_hash
        assert r1.duplicate is False
        assert r2.duplicate is False


async def test_read_blob_found() -> None:
    """Read back a written blob, payload matches (DATA-02)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        original_data = b"read me back"
        wr = await conn.write_blob(data=original_data, ttl=3600)

        result = await conn.read_blob(identity.namespace, wr.blob_hash)
        assert result is not None
        assert isinstance(result, ReadResult)
        assert result.data == original_data
        assert result.ttl == 3600
        assert result.timestamp > 0
        assert len(result.signature) > 0


async def test_read_blob_not_found() -> None:
    """Reading non-existent blob returns None (DATA-02, D-14)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        fake_hash = b"\x00" * 32
        result = await conn.read_blob(identity.namespace, fake_hash)
        assert result is None


async def test_delete_blob() -> None:
    """Delete a blob via tombstone, then read returns None (DATA-03)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        wr = await conn.write_blob(data=b"delete me", ttl=3600)

        dr = await conn.delete_blob(wr.blob_hash)
        assert isinstance(dr, DeleteResult)
        assert len(dr.tombstone_hash) == 32
        assert dr.seq_num >= 1
        assert dr.duplicate is False

        # Verify blob is gone (tombstone marks it deleted)
        result = await conn.read_blob(identity.namespace, wr.blob_hash)
        assert result is None


async def test_list_blobs() -> None:
    """List blobs in namespace after writing (DATA-04)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        wr = await conn.write_blob(data=b"list me", ttl=3600)

        page = await conn.list_blobs(identity.namespace)
        assert isinstance(page, ListPage)
        assert len(page.blobs) >= 1
        hashes = [b.blob_hash for b in page.blobs]
        assert wr.blob_hash in hashes
        for blob_ref in page.blobs:
            assert isinstance(blob_ref, BlobRef)
            assert len(blob_ref.blob_hash) == 32
            assert blob_ref.seq_num >= 1


async def test_list_blobs_pagination() -> None:
    """List with limit and cursor-based pagination (DATA-04, D-06)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        # Write 3 blobs
        for i in range(3):
            await conn.write_blob(data=f"page test {i}".encode(), ttl=3600)

        # List with limit=2
        page1 = await conn.list_blobs(identity.namespace, limit=2)
        assert len(page1.blobs) == 2
        assert page1.cursor is not None

        # Get next page using cursor
        page2 = await conn.list_blobs(identity.namespace, after=page1.cursor, limit=2)
        assert len(page2.blobs) >= 1

        # No overlap between pages
        hashes1 = {b.blob_hash for b in page1.blobs}
        hashes2 = {b.blob_hash for b in page2.blobs}
        assert hashes1.isdisjoint(hashes2)


async def test_exists_true_and_false() -> None:
    """Exists returns True for written blob, False for random hash (DATA-05)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        wr = await conn.write_blob(data=b"exists test", ttl=3600)

        assert await conn.exists(identity.namespace, wr.blob_hash) is True

        fake_hash = b"\xff" * 32
        assert await conn.exists(identity.namespace, fake_hash) is False


async def test_full_blob_lifecycle() -> None:
    """Complete lifecycle: write, read, exists, list, delete, verify gone."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        # Write
        wr = await conn.write_blob(data=b"lifecycle test", ttl=7200)
        assert not wr.duplicate

        # Read
        rr = await conn.read_blob(identity.namespace, wr.blob_hash)
        assert rr is not None
        assert rr.data == b"lifecycle test"
        assert rr.ttl == 7200

        # Exists
        assert await conn.exists(identity.namespace, wr.blob_hash) is True

        # List
        page = await conn.list_blobs(identity.namespace)
        assert wr.blob_hash in [b.blob_hash for b in page.blobs]

        # Delete
        dr = await conn.delete_blob(wr.blob_hash)
        assert not dr.duplicate

        # Verify gone
        assert await conn.read_blob(identity.namespace, wr.blob_hash) is None
