"""Integration tests for chromatindb SDK against live KVM relay.

These tests require a running chromatindb relay.
Configure via environment variables (per D-13):
  CHROMATINDB_RELAY_HOST (default: 192.168.1.200)
  CHROMATINDB_RELAY_PORT (default: 4201)

Run: pytest tests/test_integration.py -v -m integration
Skip: automatically skipped if relay is unreachable
"""

from __future__ import annotations

import asyncio
import os
import socket
import time

import pytest

from chromatindb import ChromatinClient
from chromatindb._directory import Directory
from chromatindb._envelope import envelope_decrypt
from chromatindb.exceptions import HandshakeError, NotARecipientError, ProtocolError
from chromatindb.identity import Identity
from chromatindb.types import (
    BatchReadResult,
    BlobRef,
    DelegationList,
    DeleteResult,
    ListPage,
    MetadataResult,
    NamespaceListResult,
    NamespaceStats,
    NodeInfo,
    Notification,
    PeerInfo,
    ReadResult,
    StorageStatus,
    TimeRangeResult,
    WriteResult,
)

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
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        assert not conn._transport.closed
    # After context manager exit, transport should be closed


async def test_ping_pong() -> None:
    """Send Ping, receive Pong over AEAD-encrypted channel (XPORT-04, XPORT-05)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        await conn.ping()  # Should not raise
        await conn.ping()  # Second ping to verify nonce counters advance correctly
        await conn.ping()  # Third for good measure


async def test_multiple_connections() -> None:
    """Two sequential connections work with different identities."""
    id1 = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], id1) as conn:
        await conn.ping()

    id2 = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], id2) as conn:
        await conn.ping()


async def test_handshake_timeout() -> None:
    """Very short timeout raises HandshakeError (XPORT-07)."""
    identity = Identity.generate()
    # Use a non-routable IP to guarantee timeout (not the relay)
    with pytest.raises((HandshakeError, OSError)):
        async with ChromatinClient.connect(
            [("192.0.2.1", 4433)], identity, timeout=0.5
        ) as conn:
            pass


# ------------------------------------------------------------------
# Data operation integration tests (Phase 72, Plan 03)
# ------------------------------------------------------------------


async def test_write_blob() -> None:
    """Write a blob and verify WriteResult fields (DATA-01)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
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
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        r1 = await conn.write_blob(data=b"dup test", ttl=3600)
        r2 = await conn.write_blob(data=b"dup test", ttl=3600)
        # Non-deterministic ML-DSA-87 signatures mean different blob_hash each time
        assert r1.blob_hash != r2.blob_hash
        assert r1.duplicate is False
        assert r2.duplicate is False


async def test_read_blob_found() -> None:
    """Read back a written blob, payload matches (DATA-02)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
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
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        fake_hash = b"\x00" * 32
        result = await conn.read_blob(identity.namespace, fake_hash)
        assert result is None


async def test_delete_blob() -> None:
    """Delete a blob via tombstone, then read returns None (DATA-03)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
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
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
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
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
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
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        wr = await conn.write_blob(data=b"exists test", ttl=3600)

        assert await conn.exists(identity.namespace, wr.blob_hash) is True

        fake_hash = b"\xff" * 32
        assert await conn.exists(identity.namespace, fake_hash) is False


async def test_full_blob_lifecycle() -> None:
    """Complete lifecycle: write, read, exists, list, delete, verify gone."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
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


# ------------------------------------------------------------------
# Query integration tests (Phase 73, Plan 03)
# ------------------------------------------------------------------


async def test_metadata_query() -> None:
    """Query blob metadata without payload (QUERY-01)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        original_data = b"metadata test payload"
        wr = await conn.write_blob(data=original_data, ttl=3600)

        result = await conn.metadata(identity.namespace, wr.blob_hash)
        assert result is not None
        assert isinstance(result, MetadataResult)
        assert result.blob_hash == wr.blob_hash
        assert result.ttl == 3600
        assert result.timestamp > 0
        assert result.data_size == len(original_data)
        assert result.pubkey == identity.public_key
        assert result.seq_num >= 1

        # Not-found case: random hash returns None
        fake_hash = b"\xab" * 32
        not_found = await conn.metadata(identity.namespace, fake_hash)
        assert not_found is None


async def test_batch_exists() -> None:
    """Batch-check blob existence (QUERY-02)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        wr1 = await conn.write_blob(data=b"batch exists 1", ttl=3600)
        wr2 = await conn.write_blob(data=b"batch exists 2", ttl=3600)
        fake_hash = b"\xcd" * 32

        result = await conn.batch_exists(
            identity.namespace, [wr1.blob_hash, wr2.blob_hash, fake_hash]
        )
        assert isinstance(result, dict)
        assert result[wr1.blob_hash] is True
        assert result[wr2.blob_hash] is True
        assert result[fake_hash] is False


async def test_batch_read() -> None:
    """Batch-read multiple blobs (QUERY-03)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        data1 = b"batch read blob one"
        data2 = b"batch read blob two"
        wr1 = await conn.write_blob(data=data1, ttl=3600)
        wr2 = await conn.write_blob(data=data2, ttl=3600)
        fake_hash = b"\xef" * 32

        result = await conn.batch_read(
            identity.namespace, [wr1.blob_hash, wr2.blob_hash, fake_hash]
        )
        assert isinstance(result, BatchReadResult)
        assert result.blobs[wr1.blob_hash] is not None
        assert result.blobs[wr1.blob_hash].data == data1
        assert result.blobs[wr2.blob_hash] is not None
        assert result.blobs[wr2.blob_hash].data == data2
        assert result.truncated is False


async def test_time_range() -> None:
    """Query blobs by time range (QUERY-04)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        now = int(time.time())
        wr = await conn.write_blob(data=b"time range test", ttl=3600)

        result = await conn.time_range(
            identity.namespace, start_ts=now - 60, end_ts=now + 60
        )
        assert isinstance(result, TimeRangeResult)
        assert len(result.entries) >= 1
        found_hashes = [e.blob_hash for e in result.entries]
        assert wr.blob_hash in found_hashes
        for entry in result.entries:
            assert len(entry.blob_hash) == 32
            assert entry.seq_num > 0
            assert now - 60 <= entry.timestamp <= now + 60


async def test_namespace_list() -> None:
    """List namespaces with at least one blob (QUERY-05)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        # Ensure at least one blob in this namespace
        await conn.write_blob(data=b"namespace list test", ttl=3600)

        result = await conn.namespace_list()
        assert isinstance(result, NamespaceListResult)
        assert len(result.namespaces) >= 1
        # Our namespace should appear in the listing
        ns_ids = [ns.namespace_id for ns in result.namespaces]
        assert identity.namespace in ns_ids
        for ns in result.namespaces:
            assert len(ns.namespace_id) == 32
            assert ns.blob_count >= 0
        # Our namespace specifically must have blobs
        our_ns = next(
            ns for ns in result.namespaces
            if ns.namespace_id == identity.namespace
        )
        assert our_ns.blob_count >= 1


async def test_namespace_stats() -> None:
    """Query per-namespace statistics (QUERY-06)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        await conn.write_blob(data=b"namespace stats test", ttl=3600)

        result = await conn.namespace_stats(identity.namespace)
        assert isinstance(result, NamespaceStats)
        assert result.found is True
        assert result.blob_count >= 1
        assert result.total_bytes > 0

        # Not-found namespace
        random_ns = b"\xaa" * 32
        not_found = await conn.namespace_stats(random_ns)
        assert isinstance(not_found, NamespaceStats)
        assert not_found.found is False


async def test_storage_status() -> None:
    """Query node storage status (QUERY-07)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        result = await conn.storage_status()
        assert isinstance(result, StorageStatus)
        assert result.total_blobs >= 0
        assert result.namespace_count >= 0
        assert result.used_data_bytes >= 0
        assert result.mmap_bytes >= 0


async def test_node_info() -> None:
    """Query node info and capabilities (QUERY-08)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        result = await conn.node_info()
        assert isinstance(result, NodeInfo)
        assert isinstance(result.version, str)
        assert len(result.version) > 0
        assert isinstance(result.git_hash, str)
        assert result.uptime_seconds >= 0
        assert result.peer_count >= 0
        assert isinstance(result.supported_types, list)
        assert len(result.supported_types) > 0


async def test_peer_info() -> None:
    """Query peer info (QUERY-09, trust-gated)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        result = await conn.peer_info()
        assert isinstance(result, PeerInfo)
        assert result.peer_count >= 0
        assert result.bootstrap_count >= 0
        assert isinstance(result.peers, list)


async def test_delegation_list() -> None:
    """List delegations for a namespace (QUERY-10)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        result = await conn.delegation_list(identity.namespace)
        assert isinstance(result, DelegationList)
        assert isinstance(result.entries, list)
        # May be empty if no delegations exist -- that is valid


# ------------------------------------------------------------------
# Pub/Sub integration tests (Phase 73, Plan 03)
# ------------------------------------------------------------------


async def test_subscribe_and_notification() -> None:
    """Subscribe, write, receive notification, unsubscribe (PUBSUB-01/02/03)."""
    identity = Identity.generate()
    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], identity) as conn:
        ns = identity.namespace

        # Subscribe
        await conn.subscribe(ns)
        assert ns in conn.subscriptions

        # Write a blob (should trigger notification)
        await conn.write_blob(data=b"pubsub test", ttl=3600)

        # Consume first notification with timeout
        async def _get_first():
            async for notif in conn.notifications():
                return notif

        notification = await asyncio.wait_for(_get_first(), timeout=10.0)
        assert isinstance(notification, Notification)
        assert notification.namespace == ns
        assert len(notification.blob_hash) == 32
        assert notification.seq_num > 0
        assert notification.is_tombstone is False

        # Unsubscribe
        await conn.unsubscribe(ns)
        assert ns not in conn.subscriptions


# ------------------------------------------------------------------
# Delegation revocation integration tests (Phase 91, Plan 02)
# ------------------------------------------------------------------


async def test_delegation_revocation_propagation() -> None:
    """REV-01 + REV-02: delegate, write, revoke, verify rejection.

    Full lifecycle:
    1. Owner generates identity, connects, creates Directory
    2. Owner delegates write access to delegate identity
    3. Delegate connects, writes a blob to owner's namespace (succeeds)
    4. Owner calls revoke_delegation(delegate)
    5. Wait for tombstone propagation (5s for LAN swarm)
    6. Delegate reconnects and attempts write (should be rejected with ProtocolError)
    7. Owner calls list_delegates() and delegate is absent
    """
    owner = Identity.generate()
    delegate = Identity.generate()

    async with ChromatinClient.connect([(RELAY_HOST, RELAY_PORT)], owner) as owner_conn:
        directory = Directory(owner_conn, owner)

        # Step 1: Delegate write access
        await directory.delegate(delegate)

        # Step 2: Delegate writes to owner's namespace (succeeds with delegation)
        async with ChromatinClient.connect(
            [(RELAY_HOST, RELAY_PORT)], delegate
        ) as del_conn:
            wr = await del_conn.write_blob(
                b"pre-revocation-data", ttl=300, namespace=owner.namespace
            )
            assert len(wr.blob_hash) == 32

        # Step 3: Owner revokes delegate
        result = await directory.revoke_delegation(delegate)
        assert isinstance(result, DeleteResult)
        assert len(result.tombstone_hash) == 32

        # Step 4: Wait for tombstone propagation across swarm
        await asyncio.sleep(5)

        # Step 5: Delegate write to owner's namespace should be rejected
        async with ChromatinClient.connect(
            [(RELAY_HOST, RELAY_PORT)], delegate
        ) as del_conn2:
            with pytest.raises(ProtocolError):
                await del_conn2.write_blob(
                    b"post-revocation-data", ttl=300, namespace=owner.namespace
                )

        # Step 6: Delegate should not appear in list
        delegates = await directory.list_delegates()
        delegate_pk_hashes = [e.delegate_pk_hash for e in delegates]
        from chromatindb.crypto import sha3_256
        assert sha3_256(delegate.public_key) not in delegate_pk_hashes


# ------------------------------------------------------------------
# Group membership revocation integration tests (Phase 93, Plan 02)
# ------------------------------------------------------------------


async def test_group_membership_revocation() -> None:
    """GRP-01 + GRP-02: admin removes member, future group writes exclude them.

    Full lifecycle:
    1. Admin creates group with admin + member_a + member_b
    2. Admin writes encrypted data to group -- all 3 can decrypt
    3. Admin removes member_b from group
    4. Wait for propagation (5s for LAN swarm)
    5. Admin writes encrypted data to group again -- only admin + member_a can decrypt
    6. member_b cannot decrypt new data (NotARecipientError)
    7. member_b CAN still decrypt old data (forward exclusion only, per D-04)
    """
    admin = Identity.generate()
    member_a = Identity.generate()
    member_b = Identity.generate()  # will be removed

    async with ChromatinClient.connect(
        [(RELAY_HOST, RELAY_PORT)], admin
    ) as admin_conn:
        directory = Directory(admin_conn, admin)

        # Delegate write access so members can self-register in admin's namespace
        await directory.delegate(member_a)
        await directory.delegate(member_b)

        # Admin registers itself in directory
        await directory.register("admin")

        # member_a self-registers via delegated access
        async with ChromatinClient.connect(
            [(RELAY_HOST, RELAY_PORT)], member_a
        ) as ma_conn:
            dir_a = Directory(
                ma_conn, member_a, directory_namespace=admin.namespace
            )
            await dir_a.register("member_a")

        # member_b self-registers via delegated access
        async with ChromatinClient.connect(
            [(RELAY_HOST, RELAY_PORT)], member_b
        ) as mb_conn:
            dir_b = Directory(
                mb_conn, member_b, directory_namespace=admin.namespace
            )
            await dir_b.register("member_b")

        # Step 1: Create group with all 3 members
        await directory.create_group("team", [admin, member_a, member_b])

        # Step 2: Write encrypted data to group -- all members should decrypt
        wr1 = await admin_conn.write_to_group(
            b"before-removal", "team", directory, 300
        )
        assert len(wr1.blob_hash) == 32

        # Verify all 3 can decrypt the pre-removal message
        for member in [admin, member_a, member_b]:
            async with ChromatinClient.connect(
                [(RELAY_HOST, RELAY_PORT)], member
            ) as conn:
                result = await conn.read_blob(admin.namespace, wr1.blob_hash)
                assert result is not None
                plaintext = envelope_decrypt(result.data, member)
                assert plaintext == b"before-removal"

        # Step 3: Admin removes member_b
        remove_result = await directory.remove_member("team", member_b)
        assert len(remove_result.blob_hash) == 32

        # Step 4: Wait for propagation across swarm
        await asyncio.sleep(5)

        # Step 5: Write new encrypted data to group (refresh ensures exclusion)
        wr2 = await admin_conn.write_to_group(
            b"after-removal", "team", directory, 300
        )
        assert len(wr2.blob_hash) == 32

        # Step 6: Remaining members can decrypt new message
        for member in [admin, member_a]:
            async with ChromatinClient.connect(
                [(RELAY_HOST, RELAY_PORT)], member
            ) as conn:
                result = await conn.read_blob(admin.namespace, wr2.blob_hash)
                assert result is not None
                plaintext = envelope_decrypt(result.data, member)
                assert plaintext == b"after-removal"

        # Step 7: Removed member CANNOT decrypt new message
        async with ChromatinClient.connect(
            [(RELAY_HOST, RELAY_PORT)], member_b
        ) as mb_conn:
            result = await mb_conn.read_blob(admin.namespace, wr2.blob_hash)
            assert result is not None
            with pytest.raises(NotARecipientError):
                envelope_decrypt(result.data, member_b)

        # Step 8: Removed member CAN still decrypt old message (D-04)
        async with ChromatinClient.connect(
            [(RELAY_HOST, RELAY_PORT)], member_b
        ) as mb_conn:
            result = await mb_conn.read_blob(admin.namespace, wr1.blob_hash)
            assert result is not None
            plaintext = envelope_decrypt(result.data, member_b)
            assert plaintext == b"before-removal"
