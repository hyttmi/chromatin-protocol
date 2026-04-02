"""Tests for chromatindb directory data types, UserEntry codec, and Directory class."""

from __future__ import annotations

import asyncio
import struct
from unittest.mock import AsyncMock, MagicMock, call

import pytest

from chromatindb._directory import (
    DELEGATION_MAGIC,
    GROUPENTRY_MAGIC,
    GROUPENTRY_MIN_SIZE,
    GROUPENTRY_VERSION,
    GroupEntry,
    USERENTRY_MAGIC,
    USERENTRY_MIN_SIZE,
    USERENTRY_VERSION,
    Directory,
    DirectoryEntry,
    decode_group_entry,
    decode_user_entry,
    encode_group_entry,
    encode_user_entry,
    make_delegation_data,
    verify_user_entry,
)
from chromatindb.crypto import sha3_256
from chromatindb.exceptions import ChromatinError, DirectoryError, ProtocolError
from chromatindb.identity import Identity, KEM_PUBLIC_KEY_SIZE, PUBLIC_KEY_SIZE
from chromatindb.types import BlobRef, ListPage, ReadResult, WriteResult
from chromatindb.wire import TransportMsgType


# ---------------------------------------------------------------------------
# UserEntry codec
# ---------------------------------------------------------------------------


class TestUserEntryCodec:
    """Tests for encode_user_entry and decode_user_entry."""

    def test_encode_decode_roundtrip(self, identity: Identity) -> None:
        """Encode then decode: signing_pk, kem_pk, display_name match."""
        encoded = encode_user_entry(identity, "alice")
        result = decode_user_entry(encoded)
        assert result is not None
        signing_pk, kem_pk, display_name, kem_sig = result
        assert signing_pk == identity.public_key
        assert kem_pk == identity.kem_public_key
        assert display_name == "alice"
        assert len(kem_sig) > 0

    def test_encode_validates_no_kem(self, identity: Identity) -> None:
        """Identity without KEM raises ValueError."""
        no_kem = Identity.from_public_key(identity.public_key)
        with pytest.raises(ValueError, match="KEM keypair"):
            encode_user_entry(no_kem, "alice")

    def test_encode_validates_no_sign(self, identity: Identity) -> None:
        """Verify-only identity (has KEM but can't sign) raises ValueError."""
        no_sign = Identity.from_public_keys(
            identity.public_key, identity.kem_public_key
        )
        with pytest.raises(ValueError, match="able to sign"):
            encode_user_entry(no_sign, "alice")

    def test_encode_validates_name_too_long(self, identity: Identity) -> None:
        """257-byte UTF-8 name raises ValueError."""
        with pytest.raises(ValueError, match="too long"):
            encode_user_entry(identity, "x" * 257)

    def test_encode_magic_and_version(self, identity: Identity) -> None:
        """First 5 bytes are b'UENT' + 0x01."""
        encoded = encode_user_entry(identity, "alice")
        assert encoded[:4] == b"UENT"
        assert encoded[4] == 0x01

    def test_encode_field_offsets(self, identity: Identity) -> None:
        """signing_pk at [5:2597], kem_pk at [2597:4165], name_len at [4165:4167]."""
        encoded = encode_user_entry(identity, "alice")
        assert encoded[5 : 5 + PUBLIC_KEY_SIZE] == identity.public_key
        assert (
            encoded[5 + PUBLIC_KEY_SIZE : 5 + PUBLIC_KEY_SIZE + KEM_PUBLIC_KEY_SIZE]
            == identity.kem_public_key
        )
        name_len = struct.unpack(
            ">H", encoded[4165:4167]
        )[0]
        assert name_len == len("alice".encode("utf-8"))

    def test_decode_wrong_magic(self) -> None:
        """Wrong magic returns None."""
        data = b"XXXX" + b"\x01" + b"\x00" * (USERENTRY_MIN_SIZE - 5)
        assert decode_user_entry(data) is None

    def test_decode_too_short(self) -> None:
        """Too-short data returns None."""
        data = b"UENT\x01" + b"\x00" * 10
        assert decode_user_entry(data) is None

    def test_decode_wrong_version(self) -> None:
        """Wrong version returns None."""
        data = b"UENT\x02" + b"\x00" * (USERENTRY_MIN_SIZE - 5)
        assert decode_user_entry(data) is None

    def test_decode_name_extends_past_data(self) -> None:
        """Name length exceeds remaining data returns None."""
        # Build data with name_len=100 but only 10 bytes after header
        header = b"UENT\x01"
        signing_pk = b"\x00" * PUBLIC_KEY_SIZE
        kem_pk = b"\x00" * KEM_PUBLIC_KEY_SIZE
        name_len = struct.pack(">H", 100)
        name_and_sig = b"\x00" * 10  # way too short for name_len=100
        data = header + signing_pk + kem_pk + name_len + name_and_sig
        assert decode_user_entry(data) is None

    def test_decode_empty_kem_sig(self) -> None:
        """Data ending right after display_name (no kem_sig) returns None."""
        header = b"UENT\x01"
        signing_pk = b"\x00" * PUBLIC_KEY_SIZE
        kem_pk = b"\x00" * KEM_PUBLIC_KEY_SIZE
        name = b"alice"
        name_len = struct.pack(">H", len(name))
        # No kem_sig bytes after name
        data = header + signing_pk + kem_pk + name_len + name
        assert decode_user_entry(data) is None

    def test_encode_empty_display_name(self, identity: Identity) -> None:
        """Empty string encodes and decodes correctly."""
        encoded = encode_user_entry(identity, "")
        result = decode_user_entry(encoded)
        assert result is not None
        _, _, display_name, _ = result
        assert display_name == ""

    def test_encode_unicode_display_name(self, identity: Identity) -> None:
        """Multi-byte UTF-8 name roundtrips correctly."""
        name = "\u00e9\u00e8\u00ea"  # French accented characters
        encoded = encode_user_entry(identity, name)
        result = decode_user_entry(encoded)
        assert result is not None
        _, _, display_name, _ = result
        assert display_name == name

    def test_verify_valid_kem_sig(self, identity: Identity) -> None:
        """verify_user_entry returns True for encode output."""
        encoded = encode_user_entry(identity, "alice")
        result = decode_user_entry(encoded)
        assert result is not None
        signing_pk, kem_pk, _, kem_sig = result
        assert verify_user_entry(signing_pk, kem_pk, kem_sig) is True

    def test_verify_tampered_kem_pk(self, identity: Identity) -> None:
        """verify_user_entry returns False when kem_pk is tampered."""
        encoded = encode_user_entry(identity, "alice")
        result = decode_user_entry(encoded)
        assert result is not None
        signing_pk, kem_pk, _, kem_sig = result
        # Flip first byte of kem_pk
        tampered = bytes([kem_pk[0] ^ 0xFF]) + kem_pk[1:]
        assert verify_user_entry(signing_pk, tampered, kem_sig) is False


# ---------------------------------------------------------------------------
# Delegation data
# ---------------------------------------------------------------------------


class TestDelegationData:
    """Tests for make_delegation_data."""

    def test_make_delegation_data(self, identity: Identity) -> None:
        """Output is DELEGATION_MAGIC + pubkey, 2596 bytes total."""
        result = make_delegation_data(identity.public_key)
        assert result[:4] == DELEGATION_MAGIC
        assert result[4:] == identity.public_key
        assert len(result) == 2596

    def test_make_delegation_data_wrong_size(self) -> None:
        """Wrong pubkey size raises ValueError."""
        with pytest.raises(ValueError):
            make_delegation_data(b"\x00" * 100)


# ---------------------------------------------------------------------------
# DirectoryEntry dataclass
# ---------------------------------------------------------------------------


class TestDirectoryEntry:
    """Tests for DirectoryEntry frozen dataclass."""

    def test_frozen_dataclass(self, identity: Identity) -> None:
        """DirectoryEntry fields are accessible and instance is frozen."""
        pub_identity = Identity.from_public_keys(
            identity.public_key, identity.kem_public_key
        )
        entry = DirectoryEntry(
            identity=pub_identity,
            display_name="alice",
            blob_hash=b"\xaa" * 32,
        )
        assert entry.identity is pub_identity
        assert entry.display_name == "alice"
        assert entry.blob_hash == b"\xaa" * 32
        with pytest.raises(AttributeError):
            entry.display_name = "bob"  # type: ignore[misc]


# ---------------------------------------------------------------------------
# DirectoryError exception
# ---------------------------------------------------------------------------


class TestDirectoryError:
    """Tests for DirectoryError exception hierarchy."""

    def test_inherits_chromatin_error(self) -> None:
        """DirectoryError is a ChromatinError."""
        assert issubclass(DirectoryError, ChromatinError)

    def test_instantiation(self) -> None:
        """DirectoryError can be instantiated with a message."""
        err = DirectoryError("test error")
        assert str(err) == "test error"
        assert isinstance(err, ChromatinError)


# ---------------------------------------------------------------------------
# Helper functions for Directory class tests
# ---------------------------------------------------------------------------


def make_mock_client(identity: Identity) -> MagicMock:
    """Create a mock ChromatinClient with async methods and notification queue."""
    client = MagicMock()
    client._identity = identity
    client.write_blob = AsyncMock()
    client.read_blob = AsyncMock()
    client.list_blobs = AsyncMock()
    client.subscribe = AsyncMock()
    client._transport = MagicMock()
    client._transport.notifications = asyncio.Queue(maxsize=1000)
    client._transport.closed = False
    return client


def make_user_entry_read_result(
    identity: Identity, display_name: str
) -> ReadResult:
    """Build a ReadResult containing an encoded UserEntry."""
    entry_data = encode_user_entry(identity, display_name)
    return ReadResult(data=entry_data, ttl=0, timestamp=1000000, signature=b"\x00" * 100)


def make_notification_payload(namespace: bytes, blob_hash: bytes) -> bytes:
    """Build a 77-byte notification payload matching decode_notification format."""
    return (
        namespace
        + blob_hash
        + struct.pack(">Q", 1)
        + struct.pack(">I", 100)
        + b"\x00"
    )


# ---------------------------------------------------------------------------
# Directory class init
# ---------------------------------------------------------------------------


class TestDirectoryInit:
    """Tests for Directory constructor modes."""

    def test_admin_mode(self, identity: Identity) -> None:
        """Directory(client, identity) sets admin mode with identity's namespace."""
        client = make_mock_client(identity)
        d = Directory(client, identity)
        assert d._directory_namespace == identity.namespace
        assert d._is_admin is True

    def test_user_mode(self, identity: Identity) -> None:
        """Directory(client, identity, directory_namespace=ns) sets user mode."""
        client = make_mock_client(identity)
        admin_ns = b"\xaa" * 32
        d = Directory(client, identity, directory_namespace=admin_ns)
        assert d._directory_namespace == admin_ns
        assert d._is_admin is False


# ---------------------------------------------------------------------------
# Delegation
# ---------------------------------------------------------------------------


class TestDelegate:
    """Tests for Directory.delegate()."""

    async def test_delegate_success(self, identity: Identity) -> None:
        """Admin can delegate write access; write_blob called with DELEGATION_MAGIC + pubkey."""
        other = Identity.generate()
        client = make_mock_client(identity)
        wr = WriteResult(blob_hash=b"\xbb" * 32, seq_num=1, duplicate=False)
        client.write_blob.return_value = wr

        d = Directory(client, identity)
        result = await d.delegate(other)

        assert result is wr
        client.write_blob.assert_called_once()
        args, kwargs = client.write_blob.call_args
        data_arg = args[0]
        assert data_arg[:4] == DELEGATION_MAGIC
        assert data_arg[4:] == other.public_key
        assert len(data_arg) == 2596
        # ttl=0 as positional or keyword
        ttl_arg = args[1] if len(args) > 1 else kwargs.get("ttl")
        assert ttl_arg == 0

    async def test_delegate_non_admin_raises(self, identity: Identity) -> None:
        """Non-admin Directory.delegate raises DirectoryError."""
        client = make_mock_client(identity)
        d = Directory(client, identity, directory_namespace=b"\xaa" * 32)
        with pytest.raises(DirectoryError, match="admin"):
            await d.delegate(Identity.generate())


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------


class TestRegister:
    """Tests for Directory.register()."""

    async def test_register_success(self, identity: Identity) -> None:
        """register() encodes UserEntry, writes blob, returns blob_hash."""
        client = make_mock_client(identity)
        wr = WriteResult(blob_hash=b"\xcc" * 32, seq_num=1, duplicate=False)
        client.write_blob.return_value = wr

        d = Directory(client, identity)
        result = await d.register("alice")

        assert result == b"\xcc" * 32
        client.write_blob.assert_called_once()
        data_arg = client.write_blob.call_args[0][0]
        assert data_arg[:4] == USERENTRY_MAGIC

    async def test_register_no_kem(self, identity: Identity) -> None:
        """register raises ValueError for identity without KEM."""
        no_kem = Identity.from_public_key(identity.public_key)
        client = make_mock_client(no_kem)
        d = Directory(client, no_kem)
        with pytest.raises(ValueError, match="KEM"):
            await d.register("alice")

    async def test_register_no_sign(self, identity: Identity) -> None:
        """register raises ValueError for identity that cannot sign."""
        no_sign = Identity.from_public_keys(
            identity.public_key, identity.kem_public_key
        )
        client = make_mock_client(no_sign)
        d = Directory(client, no_sign)
        with pytest.raises(ValueError, match="sign"):
            await d.register("alice")

    async def test_register_wraps_protocol_error(
        self, identity: Identity
    ) -> None:
        """write_blob raises ProtocolError -> register raises DirectoryError."""
        client = make_mock_client(identity)
        client.write_blob.side_effect = ProtocolError("rejected")
        d = Directory(client, identity)
        with pytest.raises(DirectoryError, match="Registration failed"):
            await d.register("alice")

    async def test_register_invalidates_cache(
        self, identity: Identity
    ) -> None:
        """After register, cache is dirty so next read triggers rebuild."""
        client = make_mock_client(identity)
        wr = WriteResult(blob_hash=b"\xdd" * 32, seq_num=1, duplicate=False)
        client.write_blob.return_value = wr
        d = Directory(client, identity)
        await d.register("alice")
        assert d._dirty is True


# ---------------------------------------------------------------------------
# List users
# ---------------------------------------------------------------------------


class TestListUsers:
    """Tests for Directory.list_users()."""

    async def test_list_users_populates_cache(
        self, identity: Identity
    ) -> None:
        """First call triggers subscribe + list_blobs + read_blob."""
        user = Identity.generate()
        client = make_mock_client(identity)
        blob_hash = b"\x01" * 32
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=blob_hash, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_user_entry_read_result(user, "alice")

        d = Directory(client, identity)
        users = await d.list_users()

        assert len(users) == 1
        assert users[0].display_name == "alice"
        assert users[0].identity.has_kem is True
        client.subscribe.assert_called_once_with(identity.namespace)
        client.list_blobs.assert_called_once()

    async def test_list_users_cached_second_call(
        self, identity: Identity
    ) -> None:
        """Second call returns from cache without re-scanning."""
        user = Identity.generate()
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_user_entry_read_result(user, "alice")

        d = Directory(client, identity)
        await d.list_users()
        await d.list_users()  # second call

        assert client.list_blobs.call_count == 1  # Only called once

    async def test_list_users_skips_non_userentry(
        self, identity: Identity
    ) -> None:
        """Non-UserEntry blobs (delegation, tombstone) are silently skipped."""
        user = Identity.generate()
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[
                BlobRef(blob_hash=b"\x01" * 32, seq_num=1),
                BlobRef(blob_hash=b"\x02" * 32, seq_num=2),
            ],
            cursor=None,
        )
        # First blob is not a UserEntry, second is valid
        client.read_blob.side_effect = [
            ReadResult(data=b"not-a-user-entry", ttl=0, timestamp=1000, signature=b"\x00" * 100),
            make_user_entry_read_result(user, "bob"),
        ]

        d = Directory(client, identity)
        users = await d.list_users()

        assert len(users) == 1
        assert users[0].display_name == "bob"

    async def test_list_users_skips_invalid_sig(
        self, identity: Identity
    ) -> None:
        """Entry with bad kem_sig is skipped (logged as warning)."""
        user = Identity.generate()
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        # Build valid entry then tamper with kem_pk to break kem_sig
        entry_data = encode_user_entry(user, "alice")
        parsed = decode_user_entry(entry_data)
        assert parsed is not None
        signing_pk, kem_pk, name, kem_sig = parsed
        # Flip a byte in kem_pk so kem_sig won't verify
        tampered_kem_pk = bytes([kem_pk[0] ^ 0xFF]) + kem_pk[1:]
        # Rebuild the blob with tampered kem_pk
        name_bytes = name.encode("utf-8")
        tampered_data = (
            USERENTRY_MAGIC
            + struct.pack("B", USERENTRY_VERSION)
            + signing_pk
            + tampered_kem_pk
            + struct.pack(">H", len(name_bytes))
            + name_bytes
            + kem_sig
        )
        client.read_blob.return_value = ReadResult(
            data=tampered_data, ttl=0, timestamp=1000, signature=b"\x00" * 100
        )

        d = Directory(client, identity)
        users = await d.list_users()
        assert len(users) == 0

    async def test_list_users_pagination(self, identity: Identity) -> None:
        """Handles multi-page results correctly."""
        user1 = Identity.generate()
        user2 = Identity.generate()
        client = make_mock_client(identity)
        # Page 1 with cursor, page 2 without
        client.list_blobs.side_effect = [
            ListPage(
                blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)],
                cursor=1,
            ),
            ListPage(
                blobs=[BlobRef(blob_hash=b"\x02" * 32, seq_num=2)],
                cursor=None,
            ),
        ]
        client.read_blob.side_effect = [
            make_user_entry_read_result(user1, "alice"),
            make_user_entry_read_result(user2, "bob"),
        ]

        d = Directory(client, identity)
        users = await d.list_users()

        assert len(users) == 2
        names = {u.display_name for u in users}
        assert names == {"alice", "bob"}
        assert client.list_blobs.call_count == 2

    async def test_list_users_skips_deleted_blob(
        self, identity: Identity
    ) -> None:
        """read_blob returning None (deleted/expired) is skipped."""
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = None

        d = Directory(client, identity)
        users = await d.list_users()
        assert len(users) == 0


# ---------------------------------------------------------------------------
# Get user lookups
# ---------------------------------------------------------------------------


class TestGetUser:
    """Tests for Directory.get_user() and get_user_by_pubkey()."""

    async def test_get_user_by_name(self, identity: Identity) -> None:
        """get_user returns DirectoryEntry with matching display_name."""
        user = Identity.generate()
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_user_entry_read_result(user, "alice")

        d = Directory(client, identity)
        entry = await d.get_user("alice")

        assert entry is not None
        assert entry.display_name == "alice"

    async def test_get_user_not_found(self, identity: Identity) -> None:
        """get_user returns None for non-existent name."""
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(blobs=[], cursor=None)

        d = Directory(client, identity)
        result = await d.get_user("nonexistent")
        assert result is None

    async def test_get_user_by_pubkey(self, identity: Identity) -> None:
        """get_user_by_pubkey returns DirectoryEntry by SHA3-256(signing_pk) hash."""
        user = Identity.generate()
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_user_entry_read_result(user, "alice")

        d = Directory(client, identity)
        pk_hash = sha3_256(user.public_key)
        entry = await d.get_user_by_pubkey(pk_hash)

        assert entry is not None
        assert entry.display_name == "alice"

    async def test_get_user_by_pubkey_not_found(
        self, identity: Identity
    ) -> None:
        """get_user_by_pubkey returns None for unknown hash."""
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(blobs=[], cursor=None)

        d = Directory(client, identity)
        result = await d.get_user_by_pubkey(b"\x00" * 32)
        assert result is None

    async def test_get_user_returns_encrypt_capable_identity(
        self, identity: Identity
    ) -> None:
        """Returned DirectoryEntry has identity.has_kem == True."""
        user = Identity.generate()
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_user_entry_read_result(user, "alice")

        d = Directory(client, identity)
        entry = await d.get_user("alice")
        assert entry is not None
        assert entry.identity.has_kem is True


# ---------------------------------------------------------------------------
# Cache behavior
# ---------------------------------------------------------------------------


class TestCache:
    """Tests for Directory cache population, invalidation, and refresh."""

    async def test_subscribe_before_scan(self, identity: Identity) -> None:
        """Subscribe is called before list_blobs (Pitfall 6)."""
        user = Identity.generate()
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_user_entry_read_result(user, "alice")

        # Track call order
        call_order: list[str] = []
        original_subscribe = client.subscribe
        original_list_blobs = client.list_blobs

        async def track_subscribe(*args, **kwargs):
            call_order.append("subscribe")
            return await original_subscribe(*args, **kwargs)

        async def track_list_blobs(*args, **kwargs):
            call_order.append("list_blobs")
            return await original_list_blobs(*args, **kwargs)

        client.subscribe = AsyncMock(side_effect=track_subscribe)
        client.list_blobs = AsyncMock(side_effect=track_list_blobs)

        d = Directory(client, identity)
        await d.list_users()

        assert call_order.index("subscribe") < call_order.index("list_blobs")

    async def test_notification_invalidation(
        self, identity: Identity
    ) -> None:
        """Notification for directory namespace sets dirty; next call rebuilds."""
        user = Identity.generate()
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_user_entry_read_result(user, "alice")

        d = Directory(client, identity)
        await d.list_users()  # First call populates cache
        assert client.list_blobs.call_count == 1

        # Inject a notification for the directory namespace
        notif_payload = make_notification_payload(
            identity.namespace, b"\x02" * 32
        )
        client._transport.notifications.put_nowait(
            (TransportMsgType.Notification, notif_payload, 0)
        )

        # Next call should detect dirty and rebuild
        await d.list_users()
        assert client.list_blobs.call_count == 2

    async def test_notification_requeued(self, identity: Identity) -> None:
        """Notifications are put back in queue after drain."""
        user = Identity.generate()
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_user_entry_read_result(user, "alice")

        d = Directory(client, identity)
        await d.list_users()  # Subscribe happens here

        # Put notification in queue
        notif_payload = make_notification_payload(
            identity.namespace, b"\x02" * 32
        )
        client._transport.notifications.put_nowait(
            (TransportMsgType.Notification, notif_payload, 0)
        )

        # Call list_users which drains and requeues
        await d.list_users()

        # Queue should not be empty (notification was put back)
        assert not client._transport.notifications.empty()

    async def test_notification_other_namespace_ignored(
        self, identity: Identity
    ) -> None:
        """Notification for different namespace does not invalidate cache."""
        user = Identity.generate()
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_user_entry_read_result(user, "alice")

        d = Directory(client, identity)
        await d.list_users()  # First call
        assert client.list_blobs.call_count == 1

        # Inject notification for DIFFERENT namespace
        other_ns = b"\xff" * 32
        notif_payload = make_notification_payload(other_ns, b"\x02" * 32)
        client._transport.notifications.put_nowait(
            (TransportMsgType.Notification, notif_payload, 0)
        )

        await d.list_users()  # Should NOT trigger rebuild
        assert client.list_blobs.call_count == 1  # Still just 1

    async def test_refresh_clears_cache(self, identity: Identity) -> None:
        """refresh() then list_users re-populates from scratch."""
        user = Identity.generate()
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_user_entry_read_result(user, "alice")

        d = Directory(client, identity)
        await d.list_users()
        assert client.list_blobs.call_count == 1

        d.refresh()
        assert d._cache is None

        await d.list_users()
        assert client.list_blobs.call_count == 2

    async def test_check_invalidation_no_subscription(
        self, identity: Identity
    ) -> None:
        """_check_invalidation is a no-op if not yet subscribed."""
        client = make_mock_client(identity)
        d = Directory(client, identity)
        assert d._subscribed is False

        # Put something in the queue -- should NOT be drained
        notif_payload = make_notification_payload(
            identity.namespace, b"\x02" * 32
        )
        client._transport.notifications.put_nowait(
            (TransportMsgType.Notification, notif_payload, 0)
        )

        await d._check_invalidation()
        assert d._dirty is False
        # Notification still in queue (wasn't drained)
        assert not client._transport.notifications.empty()


# ---------------------------------------------------------------------------
# GroupEntry codec
# ---------------------------------------------------------------------------


class TestGroupEntryCodec:
    """Tests for encode_group_entry, decode_group_entry, and GroupEntry."""

    def test_encode_decode_roundtrip(self) -> None:
        """Encode then decode: name and member hashes match."""
        hash_a = b"\xaa" * 32
        hash_b = b"\xbb" * 32
        encoded = encode_group_entry("team", [hash_a, hash_b])
        result = decode_group_entry(encoded)
        assert result is not None
        name, members = result
        assert name == "team"
        assert members == [hash_a, hash_b]

    def test_magic_and_version_bytes(self) -> None:
        """First 4 bytes are b'GRPE', byte 5 is 0x01."""
        encoded = encode_group_entry("test", [b"\x00" * 32])
        assert encoded[:4] == b"GRPE"
        assert encoded[4] == 0x01

    def test_field_offsets(self) -> None:
        """name_len at [5:7] BE, name follows, member_count after name."""
        name = "abc"
        encoded = encode_group_entry(name, [b"\x11" * 32])
        # name_len at offset 5
        name_len = struct.unpack(">H", encoded[5:7])[0]
        assert name_len == 3
        # name at offset 7
        assert encoded[7:10] == b"abc"
        # member_count at offset 10
        member_count = struct.unpack(">H", encoded[10:12])[0]
        assert member_count == 1
        # member hash at offset 12
        assert encoded[12:44] == b"\x11" * 32

    def test_empty_members(self) -> None:
        """Empty member list: member_count=0, decode returns empty list."""
        encoded = encode_group_entry("empty", [])
        result = decode_group_entry(encoded)
        assert result is not None
        name, members = result
        assert name == "empty"
        assert members == []
        # Check member_count is 0 in wire format
        name_len = struct.unpack(">H", encoded[5:7])[0]
        mc_offset = 7 + name_len
        member_count = struct.unpack(">H", encoded[mc_offset : mc_offset + 2])[0]
        assert member_count == 0

    def test_encode_validates_member_hash_size(self) -> None:
        """Wrong member hash size raises ValueError."""
        with pytest.raises(ValueError, match="32 bytes"):
            encode_group_entry("test", [b"\x00" * 16])

    def test_encode_validates_name_too_long(self) -> None:
        """Name exceeding 65535 bytes raises ValueError."""
        with pytest.raises(ValueError, match="too long"):
            encode_group_entry("x" * 70000, [])

    def test_decode_too_short(self) -> None:
        """Data shorter than GROUPENTRY_MIN_SIZE returns None."""
        assert decode_group_entry(b"GRPE\x01\x00") is None
        assert decode_group_entry(b"") is None
        assert decode_group_entry(b"GR") is None

    def test_decode_wrong_magic(self) -> None:
        """Wrong magic bytes returns None."""
        data = b"XXXX\x01\x00\x04test\x00\x00"
        assert decode_group_entry(data) is None

    def test_decode_wrong_version(self) -> None:
        """Wrong version byte returns None."""
        data = b"GRPE\x02\x00\x04test\x00\x00"
        assert decode_group_entry(data) is None

    def test_decode_truncated_name(self) -> None:
        """Name length exceeds remaining data returns None."""
        # name_len=100 but only 4 bytes follow
        data = GROUPENTRY_MAGIC + b"\x01" + struct.pack(">H", 100) + b"test"
        assert decode_group_entry(data) is None

    def test_decode_truncated_member_count(self) -> None:
        """Data ends after name without member_count returns None."""
        name = b"test"
        data = GROUPENTRY_MAGIC + b"\x01" + struct.pack(">H", len(name)) + name
        assert decode_group_entry(data) is None

    def test_decode_truncated_members(self) -> None:
        """member_count exceeds remaining data returns None."""
        name = b"test"
        data = (
            GROUPENTRY_MAGIC
            + b"\x01"
            + struct.pack(">H", len(name))
            + name
            + struct.pack(">H", 3)  # 3 members claimed
            + b"\x00" * 32  # only 1 member provided
        )
        assert decode_group_entry(data) is None

    def test_multi_member_roundtrip(self) -> None:
        """Multiple members roundtrip with order preserved."""
        hashes = [bytes([i]) * 32 for i in range(5)]
        encoded = encode_group_entry("multi", hashes)
        result = decode_group_entry(encoded)
        assert result is not None
        name, members = result
        assert name == "multi"
        assert len(members) == 5
        for i, m in enumerate(members):
            assert m == bytes([i]) * 32

    def test_utf8_name_encoding(self) -> None:
        """Multi-byte UTF-8 group name roundtrips correctly."""
        name = "\u00e9quipe"  # French for "team"
        encoded = encode_group_entry(name, [b"\x00" * 32])
        result = decode_group_entry(encoded)
        assert result is not None
        assert result[0] == name

    def test_group_entry_frozen(self) -> None:
        """GroupEntry is a frozen dataclass with correct fields."""
        ge = GroupEntry(
            name="g", members=[b"\xaa" * 32], blob_hash=b"\xbb" * 32, timestamp=100
        )
        assert ge.name == "g"
        assert ge.members == [b"\xaa" * 32]
        assert ge.blob_hash == b"\xbb" * 32
        assert ge.timestamp == 100
        with pytest.raises(AttributeError):
            ge.name = "x"  # type: ignore[misc]

    def test_group_entry_fields(self) -> None:
        """GroupEntry has the 4 expected fields."""
        ge = GroupEntry(name="g", members=[], blob_hash=b"\x00" * 32, timestamp=0)
        assert hasattr(ge, "name")
        assert hasattr(ge, "members")
        assert hasattr(ge, "blob_hash")
        assert hasattr(ge, "timestamp")

    def test_constants_values(self) -> None:
        """GROUPENTRY constants have correct values."""
        assert GROUPENTRY_MAGIC == b"GRPE"
        assert GROUPENTRY_VERSION == 0x01
        assert GROUPENTRY_MIN_SIZE == 9


# ---------------------------------------------------------------------------
# Helper for group tests
# ---------------------------------------------------------------------------


def make_group_entry_read_result(
    name: str, member_hashes: list[bytes], *, timestamp: int = 1000
) -> ReadResult:
    """Build a ReadResult containing an encoded GroupEntry."""
    group_data = encode_group_entry(name, member_hashes)
    return ReadResult(data=group_data, ttl=0, timestamp=timestamp, signature=b"\x00" * 100)


# ---------------------------------------------------------------------------
# Directory group methods
# ---------------------------------------------------------------------------


class TestDirectoryGroups:
    """Tests for Directory group management methods."""

    async def test_create_group_admin(self, identity: Identity) -> None:
        """Admin creates group, write_blob called with GRPE-encoded data."""
        member = Identity.generate()
        client = make_mock_client(identity)
        wr = WriteResult(blob_hash=b"\xee" * 32, seq_num=1, duplicate=False)
        client.write_blob.return_value = wr

        d = Directory(client, identity)
        result = await d.create_group("devs", [member])

        assert result is wr
        client.write_blob.assert_called_once()
        data_arg = client.write_blob.call_args[0][0]
        assert data_arg[:4] == GROUPENTRY_MAGIC
        # Verify encoded data decodes correctly
        parsed = decode_group_entry(data_arg)
        assert parsed is not None
        gname, gmembers = parsed
        assert gname == "devs"
        assert len(gmembers) == 1
        assert gmembers[0] == sha3_256(member.public_key)
        # ttl=0
        ttl_arg = client.write_blob.call_args[0][1] if len(client.write_blob.call_args[0]) > 1 else client.write_blob.call_args[1].get("ttl")
        assert ttl_arg == 0
        # dirty set
        assert d._dirty is True

    async def test_create_group_non_admin(self, identity: Identity) -> None:
        """Non-admin raises DirectoryError."""
        client = make_mock_client(identity)
        d = Directory(client, identity, directory_namespace=b"\xaa" * 32)
        with pytest.raises(DirectoryError, match="admin"):
            await d.create_group("devs", [])

    async def test_create_group_empty_members(self, identity: Identity) -> None:
        """Creating group with empty member list is valid."""
        client = make_mock_client(identity)
        wr = WriteResult(blob_hash=b"\xee" * 32, seq_num=1, duplicate=False)
        client.write_blob.return_value = wr

        d = Directory(client, identity)
        result = await d.create_group("empty-group", [])

        assert result is wr
        data_arg = client.write_blob.call_args[0][0]
        parsed = decode_group_entry(data_arg)
        assert parsed is not None
        assert parsed[0] == "empty-group"
        assert parsed[1] == []

    async def test_add_member_admin(self, identity: Identity) -> None:
        """Admin adds member to existing group via read-modify-write."""
        existing_member = Identity.generate()
        new_member = Identity.generate()
        client = make_mock_client(identity)

        # Set up cache with existing group
        existing_hash = sha3_256(existing_member.public_key)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_group_entry_read_result(
            "devs", [existing_hash]
        )

        wr = WriteResult(blob_hash=b"\xee" * 32, seq_num=2, duplicate=False)
        client.write_blob.return_value = wr

        d = Directory(client, identity)
        result = await d.add_member("devs", new_member)

        assert result is wr
        data_arg = client.write_blob.call_args[0][0]
        parsed = decode_group_entry(data_arg)
        assert parsed is not None
        _, members = parsed
        assert len(members) == 2
        assert existing_hash in members
        assert sha3_256(new_member.public_key) in members

    async def test_add_member_non_admin(self, identity: Identity) -> None:
        """Non-admin add_member raises DirectoryError."""
        client = make_mock_client(identity)
        d = Directory(client, identity, directory_namespace=b"\xaa" * 32)
        with pytest.raises(DirectoryError, match="admin"):
            await d.add_member("devs", Identity.generate())

    async def test_add_member_group_not_found(self, identity: Identity) -> None:
        """add_member for non-existent group raises DirectoryError."""
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(blobs=[], cursor=None)

        d = Directory(client, identity)
        with pytest.raises(DirectoryError, match="not found"):
            await d.add_member("nonexistent", Identity.generate())

    async def test_add_member_already_present(self, identity: Identity) -> None:
        """add_member for member already in group raises DirectoryError."""
        member = Identity.generate()
        member_hash = sha3_256(member.public_key)
        client = make_mock_client(identity)

        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_group_entry_read_result(
            "devs", [member_hash]
        )

        d = Directory(client, identity)
        with pytest.raises(DirectoryError, match="already in group"):
            await d.add_member("devs", member)

    async def test_remove_member_admin(self, identity: Identity) -> None:
        """Admin removes member from group."""
        member_a = Identity.generate()
        member_b = Identity.generate()
        hash_a = sha3_256(member_a.public_key)
        hash_b = sha3_256(member_b.public_key)
        client = make_mock_client(identity)

        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_group_entry_read_result(
            "devs", [hash_a, hash_b]
        )

        wr = WriteResult(blob_hash=b"\xee" * 32, seq_num=2, duplicate=False)
        client.write_blob.return_value = wr

        d = Directory(client, identity)
        result = await d.remove_member("devs", member_a)

        assert result is wr
        data_arg = client.write_blob.call_args[0][0]
        parsed = decode_group_entry(data_arg)
        assert parsed is not None
        _, members = parsed
        assert len(members) == 1
        assert hash_b in members
        assert hash_a not in members

    async def test_remove_member_non_admin(self, identity: Identity) -> None:
        """Non-admin remove_member raises DirectoryError."""
        client = make_mock_client(identity)
        d = Directory(client, identity, directory_namespace=b"\xaa" * 32)
        with pytest.raises(DirectoryError, match="admin"):
            await d.remove_member("devs", Identity.generate())

    async def test_remove_member_group_not_found(self, identity: Identity) -> None:
        """remove_member for non-existent group raises DirectoryError."""
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(blobs=[], cursor=None)

        d = Directory(client, identity)
        with pytest.raises(DirectoryError, match="not found"):
            await d.remove_member("nonexistent", Identity.generate())

    async def test_remove_member_not_in_group(self, identity: Identity) -> None:
        """remove_member for member not in group raises DirectoryError."""
        member = Identity.generate()
        other = Identity.generate()
        client = make_mock_client(identity)

        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_group_entry_read_result(
            "devs", [sha3_256(member.public_key)]
        )

        d = Directory(client, identity)
        with pytest.raises(DirectoryError, match="not in group"):
            await d.remove_member("devs", other)

    async def test_list_groups(self, identity: Identity) -> None:
        """list_groups returns list of GroupEntry from cache."""
        client = make_mock_client(identity)
        hash_a = b"\xaa" * 32

        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_group_entry_read_result(
            "team", [hash_a]
        )

        d = Directory(client, identity)
        groups = await d.list_groups()

        assert len(groups) == 1
        assert groups[0].name == "team"
        assert groups[0].members == [hash_a]

    async def test_get_group_found(self, identity: Identity) -> None:
        """get_group returns GroupEntry by name."""
        client = make_mock_client(identity)

        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_group_entry_read_result(
            "team", [b"\xaa" * 32]
        )

        d = Directory(client, identity)
        group = await d.get_group("team")

        assert group is not None
        assert group.name == "team"

    async def test_get_group_not_found(self, identity: Identity) -> None:
        """get_group returns None for non-existent group."""
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(blobs=[], cursor=None)

        d = Directory(client, identity)
        result = await d.get_group("nonexistent")
        assert result is None

    async def test_populate_cache_grpe(self, identity: Identity) -> None:
        """_populate_cache decodes GRPE blobs and builds _groups."""
        client = make_mock_client(identity)
        hash_a = b"\xaa" * 32

        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_group_entry_read_result(
            "devs", [hash_a], timestamp=500
        )

        d = Directory(client, identity)
        await d._populate_cache()

        assert "devs" in d._groups
        assert d._groups["devs"].name == "devs"
        assert d._groups["devs"].members == [hash_a]
        assert d._groups["devs"].timestamp == 500
        assert d._groups["devs"].blob_hash == b"\x01" * 32

    async def test_populate_cache_latest_timestamp_wins(
        self, identity: Identity
    ) -> None:
        """Two GRPE blobs with same name: latest timestamp wins."""
        client = make_mock_client(identity)
        old_hash = b"\xaa" * 32
        new_hash = b"\xbb" * 32

        client.list_blobs.return_value = ListPage(
            blobs=[
                BlobRef(blob_hash=b"\x01" * 32, seq_num=1),
                BlobRef(blob_hash=b"\x02" * 32, seq_num=2),
            ],
            cursor=None,
        )
        client.read_blob.side_effect = [
            make_group_entry_read_result("devs", [old_hash], timestamp=100),
            make_group_entry_read_result("devs", [new_hash], timestamp=200),
        ]

        d = Directory(client, identity)
        await d._populate_cache()

        assert d._groups["devs"].members == [new_hash]
        assert d._groups["devs"].timestamp == 200

    async def test_populate_cache_mixed_uent_grpe(
        self, identity: Identity
    ) -> None:
        """Namespace with both UENT and GRPE blobs: both indexes populated."""
        user = Identity.generate()
        client = make_mock_client(identity)

        client.list_blobs.return_value = ListPage(
            blobs=[
                BlobRef(blob_hash=b"\x01" * 32, seq_num=1),
                BlobRef(blob_hash=b"\x02" * 32, seq_num=2),
            ],
            cursor=None,
        )
        client.read_blob.side_effect = [
            make_user_entry_read_result(user, "alice"),
            make_group_entry_read_result("devs", [b"\xaa" * 32]),
        ]

        d = Directory(client, identity)
        await d._populate_cache()

        # User index populated
        assert len(d._cache) == 1
        assert "alice" in d._by_name

        # Group index populated
        assert "devs" in d._groups
        assert d._groups["devs"].members == [b"\xaa" * 32]

    async def test_refresh_clears_groups(self, identity: Identity) -> None:
        """refresh() clears _groups index."""
        client = make_mock_client(identity)
        client.list_blobs.return_value = ListPage(
            blobs=[BlobRef(blob_hash=b"\x01" * 32, seq_num=1)], cursor=None
        )
        client.read_blob.return_value = make_group_entry_read_result(
            "devs", [b"\xaa" * 32]
        )

        d = Directory(client, identity)
        await d.list_groups()
        assert len(d._groups) == 1

        d.refresh()
        assert len(d._groups) == 0
        assert d._cache is None
