"""Directory data types, UserEntry binary codec, and Directory class for chromatindb SDK.

Provides the full directory layer:
- UserEntry encode/decode for self-registration blobs
- DirectoryEntry frozen dataclass for lookup results
- Delegation data construction helper
- Constants for magic bytes and size validation
- Directory class for admin/non-admin directory operations with cached lookups

UserEntry binary format (D-06):
  [magic:4][version:1][signing_pk:2592][kem_pk:1568][name_len:2 BE][name:N][kem_sig:variable]
"""

from __future__ import annotations

import asyncio
import logging
import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING

from chromatindb._codec import decode_notification
from chromatindb.crypto import sha3_256
from chromatindb.exceptions import DelegationNotFoundError, DirectoryError, ProtocolError
from chromatindb.identity import (
    Identity,
    KEM_PUBLIC_KEY_SIZE,
    PUBLIC_KEY_SIZE,
)
from chromatindb.wire import TransportMsgType

if TYPE_CHECKING:
    from chromatindb.client import ChromatinClient
    from chromatindb.types import DelegationEntry, DeleteResult, WriteResult

# UserEntry magic bytes and version (D-07)
USERENTRY_MAGIC: bytes = b"UENT"
USERENTRY_VERSION: int = 0x01

# Delegation magic bytes matching C++ db/wire/codec.h (Pitfall 1)
DELEGATION_MAGIC: bytes = bytes([0xDE, 0x1E, 0x6A, 0x7E])

# Minimum UserEntry size: magic(4) + ver(1) + signing_pk(2592) + kem_pk(1568) + name_len(2) + kem_sig(>=1)
USERENTRY_MIN_SIZE: int = 4 + 1 + PUBLIC_KEY_SIZE + KEM_PUBLIC_KEY_SIZE + 2 + 1


@dataclass(frozen=True)
class DirectoryEntry:
    """A verified user entry from the directory (D-19).

    Attributes:
        identity: Verify + encrypt-capable identity reconstructed from pubkeys.
        display_name: User's display name.
        blob_hash: 32-byte hash of the blob containing this entry.
    """

    identity: Identity
    display_name: str
    blob_hash: bytes


# GroupEntry magic bytes and version (D-01, D-02)
GROUPENTRY_MAGIC: bytes = b"GRPE"
GROUPENTRY_VERSION: int = 0x01

# Minimum GroupEntry size: magic(4) + ver(1) + name_len(2) + member_count(2)
GROUPENTRY_MIN_SIZE: int = 9


@dataclass(frozen=True)
class GroupEntry:
    """A group entry from the directory (D-13).

    Groups are named member lists stored as blobs in the admin namespace.
    Members are identified by SHA3-256(signing_pk) hashes.

    Attributes:
        name: Group name (UTF-8).
        members: List of 32-byte SHA3-256(signing_pk) hashes.
        blob_hash: 32-byte hash of the blob containing this entry.
        timestamp: Blob timestamp for latest-timestamp-wins resolution.
    """

    name: str
    members: list[bytes]  # 32-byte SHA3-256(signing_pk) hashes
    blob_hash: bytes  # 32-byte hash of the blob
    timestamp: int  # blob timestamp for latest-timestamp-wins


def encode_group_entry(name: str, members: list[bytes]) -> bytes:
    """Encode a GroupEntry blob for directory storage.

    Binary format (D-03):
      [GRPE:4][version:1][name_len:2 BE][name:N][member_count:2 BE][N x member_hash:32]

    Args:
        name: Group name (UTF-8, max 65535 bytes encoded).
        members: List of 32-byte SHA3-256(signing_pk) hashes.

    Returns:
        GroupEntry blob bytes.

    Raises:
        ValueError: If name too long or member hash wrong size.
    """
    name_bytes = name.encode("utf-8")
    if len(name_bytes) > 65535:
        raise ValueError(f"Group name too long: {len(name_bytes)} bytes")
    for m in members:
        if len(m) != 32:
            raise ValueError(f"Member hash must be 32 bytes, got {len(m)}")
    return (
        GROUPENTRY_MAGIC
        + struct.pack("B", GROUPENTRY_VERSION)
        + struct.pack(">H", len(name_bytes))
        + name_bytes
        + struct.pack(">H", len(members))
        + b"".join(members)
    )


def decode_group_entry(data: bytes) -> tuple[str, list[bytes]] | None:
    """Decode a GroupEntry blob.

    Returns None for invalid data (wrong magic, too short, version mismatch,
    truncated name, truncated members).

    Args:
        data: Raw blob bytes.

    Returns:
        (name, members) tuple, or None if invalid.
    """
    if len(data) < GROUPENTRY_MIN_SIZE:
        return None
    if data[:4] != GROUPENTRY_MAGIC:
        return None
    if data[4] != GROUPENTRY_VERSION:
        return None
    offset = 5
    name_len = struct.unpack(">H", data[offset : offset + 2])[0]
    offset += 2
    if offset + name_len > len(data):
        return None
    name = data[offset : offset + name_len].decode("utf-8")
    offset += name_len
    if offset + 2 > len(data):
        return None
    member_count = struct.unpack(">H", data[offset : offset + 2])[0]
    offset += 2
    if offset + member_count * 32 > len(data):
        return None
    members = []
    for _ in range(member_count):
        members.append(data[offset : offset + 32])
        offset += 32
    return name, members


def encode_user_entry(identity: Identity, display_name: str) -> bytes:
    """Encode a UserEntry blob for directory registration.

    Signs the KEM public key with the identity's signing key (D-08)
    to prevent MITM key substitution.

    Args:
        identity: Full identity with signing and KEM keypairs.
        display_name: UTF-8 display name, max 256 bytes encoded.

    Returns:
        UserEntry blob bytes.

    Raises:
        ValueError: If identity lacks KEM or signing capability,
            or display name exceeds 256 bytes.
    """
    if not identity.has_kem:
        raise ValueError("Identity must have KEM keypair")
    if not identity.can_sign:
        raise ValueError("Identity must be able to sign")

    name_bytes = display_name.encode("utf-8")
    if len(name_bytes) > 256:
        raise ValueError(
            f"Display name too long: {len(name_bytes)} > 256 bytes"
        )

    # Sign KEM pubkey with signing key (D-08: prevents MITM key substitution)
    kem_sig = identity.sign(identity.kem_public_key)

    return (
        USERENTRY_MAGIC
        + struct.pack("B", USERENTRY_VERSION)
        + identity.public_key  # 2592 bytes
        + identity.kem_public_key  # 1568 bytes
        + struct.pack(">H", len(name_bytes))
        + name_bytes
        + kem_sig  # variable length, up to 4627 bytes (Pitfall 4: no length prefix)
    )


def decode_user_entry(data: bytes) -> tuple[bytes, bytes, str, bytes] | None:
    """Decode a UserEntry blob.

    Returns None for invalid data (wrong magic, too short, version mismatch,
    truncated fields, empty kem_sig). Per Pitfall 3 and Pitfall 4.

    Args:
        data: Raw blob bytes.

    Returns:
        (signing_pk, kem_pk, display_name, kem_sig) tuple, or None if invalid.
    """
    if len(data) < USERENTRY_MIN_SIZE:
        return None
    if data[:4] != USERENTRY_MAGIC:
        return None
    if data[4] != USERENTRY_VERSION:
        return None

    offset = 5
    signing_pk = data[offset : offset + PUBLIC_KEY_SIZE]
    offset += PUBLIC_KEY_SIZE

    kem_pk = data[offset : offset + KEM_PUBLIC_KEY_SIZE]
    offset += KEM_PUBLIC_KEY_SIZE

    name_len = struct.unpack(">H", data[offset : offset + 2])[0]
    offset += 2

    if offset + name_len > len(data):
        return None

    display_name = data[offset : offset + name_len].decode("utf-8")
    offset += name_len

    # Remainder is kem_sig (Pitfall 4: no length prefix for kem_sig)
    kem_sig = data[offset:]
    if len(kem_sig) == 0:
        return None

    return signing_pk, kem_pk, display_name, kem_sig


def verify_user_entry(
    signing_pk: bytes, kem_pk: bytes, kem_sig: bytes
) -> bool:
    """Verify that kem_sig is a valid ML-DSA-87 signature of kem_pk by signing_pk.

    Per D-08 and D-25: the signing key owner signed their own KEM pubkey.

    Args:
        signing_pk: 2592-byte ML-DSA-87 public key.
        kem_pk: 1568-byte ML-KEM-1024 public key (the signed message).
        kem_sig: ML-DSA-87 signature bytes.

    Returns:
        True if signature is valid, False otherwise.
    """
    return Identity.verify(kem_pk, kem_sig, signing_pk)


def make_delegation_data(delegate_signing_pk: bytes) -> bytes:
    """Build delegation blob data: [magic:4][delegate_pubkey:2592].

    Per PROTOCOL.md and db/wire/codec.h line 66. Total 2596 bytes.

    Args:
        delegate_signing_pk: 2592-byte ML-DSA-87 public key of the delegate.

    Returns:
        Delegation blob data bytes.

    Raises:
        ValueError: If pubkey size is wrong.
    """
    if len(delegate_signing_pk) != PUBLIC_KEY_SIZE:
        raise ValueError(
            f"delegate pubkey must be {PUBLIC_KEY_SIZE} bytes, "
            f"got {len(delegate_signing_pk)}"
        )
    return DELEGATION_MAGIC + delegate_signing_pk


# ---------------------------------------------------------------------------
# Directory class (D-01 through D-04, D-11 through D-27)
# ---------------------------------------------------------------------------

logger = logging.getLogger(__name__)


class Directory:
    """PQ pubkey directory backed by an admin-owned namespace.

    Admin mode:  Directory(client, admin_identity)
    User mode:   Directory(client, user_identity, directory_namespace=admin_ns)

    The directory enables users to publish and discover encryption pubkeys
    through a shared namespace. Admin delegates write access; users
    self-register by writing UserEntry blobs.

    Cache is populated lazily on first read and invalidated via pub/sub
    notifications using a drain-and-requeue pattern (Pitfall 2, Pitfall 6).
    """

    def __init__(
        self,
        client: ChromatinClient,
        identity: Identity,
        *,
        directory_namespace: bytes | None = None,
    ) -> None:
        self._client = client
        self._identity = identity
        self._directory_namespace: bytes = (
            directory_namespace
            if directory_namespace is not None
            else identity.namespace
        )
        self._is_admin: bool = directory_namespace is None
        # Cache state (D-20, D-21, D-24)
        self._cache: dict[bytes, DirectoryEntry] | None = None
        self._by_name: dict[str, DirectoryEntry] = {}
        self._by_pubkey_hash: dict[bytes, DirectoryEntry] = {}
        self._dirty: bool = False
        self._subscribed: bool = False
        # Group index (D-12)
        self._groups: dict[str, GroupEntry] = {}

    async def delegate(self, delegate_identity: Identity) -> WriteResult:
        """Grant write access to a delegate's signing key in directory namespace.

        Per D-11, D-12: admin writes a delegation blob so the delegate
        can subsequently self-register.

        Args:
            delegate_identity: Identity of the user to delegate to.

        Returns:
            WriteResult from the node.

        Raises:
            DirectoryError: If not in admin mode.
        """
        if not self._is_admin:
            raise DirectoryError("only admin can delegate")
        delegation_data = make_delegation_data(delegate_identity.public_key)
        return await self._client.write_blob(delegation_data, ttl=0)

    async def revoke_delegation(self, delegate_identity: Identity) -> DeleteResult:
        """Revoke a delegate's write access by tombstoning their delegation blob.

        Per D-01, D-02: computes pk_hash from delegate_identity.public_key,
        looks up delegation_blob_hash via delegation_list(), tombstones via
        delete_blob().

        Args:
            delegate_identity: Identity of the delegate to revoke.

        Returns:
            DeleteResult from the tombstone write.

        Raises:
            DirectoryError: If not in admin mode.
            DelegationNotFoundError: If no active delegation exists for delegate.
            ProtocolError: If the node rejects the tombstone (per D-05).
            ConnectionError: If the connection is lost (per D-05).
        """
        if not self._is_admin:
            raise DirectoryError("only admin can revoke delegations")
        pk_hash = sha3_256(delegate_identity.public_key)
        delegation_result = await self._client.delegation_list(
            self._directory_namespace
        )
        for entry in delegation_result.entries:
            if entry.delegate_pk_hash == pk_hash:
                return await self._client.delete_blob(
                    entry.delegation_blob_hash
                )
        raise DelegationNotFoundError(
            "no active delegation found for delegate"
        )

    async def list_delegates(self) -> list[DelegationEntry]:
        """List active delegates in the directory namespace.

        Per D-03: wraps client.delegation_list(self._directory_namespace),
        returns list of DelegationEntry.

        Returns:
            List of DelegationEntry objects with delegate_pk_hash and
            delegation_blob_hash fields.

        Raises:
            DirectoryError: If not in admin mode.
            ProtocolError: If the node rejects the request (per D-05).
            ConnectionError: If the connection is lost (per D-05).
        """
        if not self._is_admin:
            raise DirectoryError("only admin can list delegates")
        result = await self._client.delegation_list(
            self._directory_namespace
        )
        return result.entries

    async def register(self, display_name: str) -> bytes:
        """Self-register in the directory by writing a UserEntry blob.

        Per D-13, D-14, D-26, D-29: encodes UserEntry with KEM pubkey
        cross-signed by signing key, then writes to directory namespace.

        Args:
            display_name: User-facing display name (max 256 UTF-8 bytes).

        Returns:
            32-byte blob_hash of the written UserEntry.

        Raises:
            ValueError: If identity lacks KEM keypair or signing capability.
            DirectoryError: If the node rejects the write (wraps ProtocolError).
        """
        if not self._identity.has_kem:
            raise ValueError(
                "Identity must have KEM keypair for registration"
            )
        if not self._identity.can_sign:
            raise ValueError(
                "Identity must be able to sign for registration"
            )
        entry_data = encode_user_entry(self._identity, display_name)
        try:
            result = await self._client.write_blob(entry_data, ttl=0)
        except ProtocolError as e:
            raise DirectoryError(f"Registration failed: {e}") from e
        self._dirty = True
        return result.blob_hash

    async def list_users(self) -> list[DirectoryEntry]:
        """List all registered users in the directory.

        Per D-15, D-21: populates cache on first call via full namespace
        scan. Subsequent calls return from cache unless invalidated.

        Returns:
            List of verified DirectoryEntry objects.
        """
        await self._check_invalidation()
        if self._cache is None or self._dirty:
            await self._populate_cache()
        return list(self._cache.values())

    async def get_user(self, display_name: str) -> DirectoryEntry | None:
        """Look up a user by display name.

        Per D-16, D-18: O(1) lookup from secondary index.

        Args:
            display_name: Exact display name to match.

        Returns:
            DirectoryEntry if found, None otherwise.
        """
        await self._check_invalidation()
        if self._cache is None or self._dirty:
            await self._populate_cache()
        return self._by_name.get(display_name)

    async def get_user_by_pubkey(
        self, pubkey_hash: bytes
    ) -> DirectoryEntry | None:
        """Look up a user by SHA3-256(signing_pk) hash.

        Per D-17, D-18: O(1) lookup from secondary index.

        Args:
            pubkey_hash: 32-byte SHA3-256 hash of the user's signing pubkey.

        Returns:
            DirectoryEntry if found, None otherwise.
        """
        await self._check_invalidation()
        if self._cache is None or self._dirty:
            await self._populate_cache()
        return self._by_pubkey_hash.get(pubkey_hash)

    # -----------------------------------------------------------------
    # Group management (GRP-01 through GRP-04)
    # -----------------------------------------------------------------

    async def create_group(
        self, name: str, members: list[Identity]
    ) -> WriteResult:
        """Create a named group with an initial member list.

        Per D-08, D-10, GRP-01: admin-only, writes GRPE blob to directory
        namespace with member hashes derived from signing pubkeys.

        Args:
            name: Group name (UTF-8).
            members: List of Identity objects to include in the group.

        Returns:
            WriteResult from the node.

        Raises:
            DirectoryError: If not in admin mode.
        """
        if not self._is_admin:
            raise DirectoryError("only admin can create groups")
        member_hashes = [sha3_256(m.public_key) for m in members]
        group_data = encode_group_entry(name, member_hashes)
        result = await self._client.write_blob(group_data, ttl=0)
        self._dirty = True
        return result

    async def add_member(
        self, group_name: str, member: Identity
    ) -> WriteResult:
        """Add a member to an existing group via read-modify-write.

        Per D-07, D-09, GRP-02: reads current group, appends member hash,
        writes new group blob.

        Args:
            group_name: Name of the group to modify.
            member: Identity to add.

        Returns:
            WriteResult from the node.

        Raises:
            DirectoryError: If not admin, group not found, or member already present.
        """
        if not self._is_admin:
            raise DirectoryError("only admin can modify groups")
        group = await self.get_group(group_name)
        if group is None:
            raise DirectoryError(f"group not found: {group_name}")
        member_hash = sha3_256(member.public_key)
        if member_hash in group.members:
            raise DirectoryError(
                f"member already in group: {group_name}"
            )
        new_members = list(group.members) + [member_hash]
        group_data = encode_group_entry(group_name, new_members)
        result = await self._client.write_blob(group_data, ttl=0)
        self._dirty = True
        return result

    async def remove_member(
        self, group_name: str, member: Identity
    ) -> WriteResult:
        """Remove a member from an existing group via read-modify-write.

        Per D-07, D-09, GRP-02: reads current group, filters out member hash,
        writes new group blob.

        Args:
            group_name: Name of the group to modify.
            member: Identity to remove.

        Returns:
            WriteResult from the node.

        Raises:
            DirectoryError: If not admin, group not found, or member not present.
        """
        if not self._is_admin:
            raise DirectoryError("only admin can modify groups")
        group = await self.get_group(group_name)
        if group is None:
            raise DirectoryError(f"group not found: {group_name}")
        member_hash = sha3_256(member.public_key)
        if member_hash not in group.members:
            raise DirectoryError(
                f"member not in group: {group_name}"
            )
        new_members = [m for m in group.members if m != member_hash]
        group_data = encode_group_entry(group_name, new_members)
        result = await self._client.write_blob(group_data, ttl=0)
        self._dirty = True
        return result

    async def list_groups(self) -> list[GroupEntry]:
        """List all groups in the directory.

        Per GRP-03: returns cached group entries, populating cache if needed.

        Returns:
            List of GroupEntry objects.
        """
        await self._check_invalidation()
        if self._cache is None or self._dirty:
            await self._populate_cache()
        return list(self._groups.values())

    async def get_group(self, group_name: str) -> GroupEntry | None:
        """Look up a group by name.

        Per GRP-03: O(1) lookup from group index.

        Args:
            group_name: Exact group name to match.

        Returns:
            GroupEntry if found, None otherwise.
        """
        await self._check_invalidation()
        if self._cache is None or self._dirty:
            await self._populate_cache()
        return self._groups.get(group_name)

    def refresh(self) -> None:
        """Explicitly invalidate the cache (D-23).

        Next call to list_users/get_user/get_user_by_pubkey/list_groups/get_group
        will trigger a full rebuild from the directory namespace.
        """
        self._cache = None
        self._by_name.clear()
        self._by_pubkey_hash.clear()
        self._groups.clear()
        self._dirty = False

    async def _populate_cache(self) -> None:
        """Full namespace scan to rebuild cache (D-21, D-22, D-24, D-25).

        Subscribe BEFORE scanning (Pitfall 6) to avoid missing notifications
        that arrive between scan completion and subscription.
        """
        # Subscribe first (Pitfall 6)
        if not self._subscribed:
            await self._client.subscribe(self._directory_namespace)
            self._subscribed = True

        cache: dict[bytes, DirectoryEntry] = {}
        by_name: dict[str, DirectoryEntry] = {}
        by_pubkey_hash: dict[bytes, DirectoryEntry] = {}
        groups_by_name: dict[str, GroupEntry] = {}

        cursor = 0
        while True:
            page = await self._client.list_blobs(
                self._directory_namespace, after=cursor
            )
            for blob_ref in page.blobs:
                result = await self._client.read_blob(
                    self._directory_namespace, blob_ref.blob_hash
                )
                if result is None:
                    continue  # Deleted or expired

                # Try UserEntry first
                parsed = decode_user_entry(result.data)
                if parsed is not None:
                    signing_pk, kem_pk, display_name, kem_sig = parsed
                    if not verify_user_entry(signing_pk, kem_pk, kem_sig):
                        logger.warning(
                            "Skipping UserEntry with invalid kem_sig: %s",
                            blob_ref.blob_hash.hex(),
                        )
                        continue

                    identity = Identity.from_public_keys(signing_pk, kem_pk)
                    entry = DirectoryEntry(
                        identity=identity,
                        display_name=display_name,
                        blob_hash=blob_ref.blob_hash,
                    )
                    cache[blob_ref.blob_hash] = entry
                    by_name[display_name] = entry
                    by_pubkey_hash[sha3_256(signing_pk)] = entry
                    continue

                # Try GroupEntry (D-11)
                group_parsed = decode_group_entry(result.data)
                if group_parsed is not None:
                    group_name, group_members = group_parsed
                    group_entry = GroupEntry(
                        name=group_name,
                        members=group_members,
                        blob_hash=blob_ref.blob_hash,
                        timestamp=result.timestamp,
                    )
                    # Latest-timestamp-wins (D-06)
                    existing = groups_by_name.get(group_name)
                    if existing is None or group_entry.timestamp > existing.timestamp:
                        groups_by_name[group_name] = group_entry
                    continue

                # Neither UserEntry nor GroupEntry -- skip (delegation, tombstone, etc.)

            if page.cursor is None:
                break
            cursor = page.cursor

        self._cache = cache
        self._by_name = by_name
        self._by_pubkey_hash = by_pubkey_hash
        self._groups = groups_by_name
        self._dirty = False

    async def _check_invalidation(self) -> None:
        """Drain notification queue and set dirty flag if directory namespace seen.

        Per D-22, Research Pattern 6: drain-and-requeue pattern. All
        notifications are put back so user code can still consume them.
        """
        if not self._subscribed:
            return  # No subscription yet, nothing to check

        requeue: list[tuple] = []
        while not self._client._transport.notifications.empty():
            try:
                item = self._client._transport.notifications.get_nowait()
            except asyncio.QueueEmpty:
                break
            msg_type, payload, request_id = item
            if msg_type == TransportMsgType.Notification:
                notif = decode_notification(payload)
                if notif.namespace == self._directory_namespace:
                    self._dirty = True
            requeue.append(item)

        for item in requeue:
            try:
                self._client._transport.notifications.put_nowait(item)
            except asyncio.QueueFull:
                break
