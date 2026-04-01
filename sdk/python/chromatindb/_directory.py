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
from chromatindb.exceptions import DirectoryError, ProtocolError
from chromatindb.identity import (
    Identity,
    KEM_PUBLIC_KEY_SIZE,
    PUBLIC_KEY_SIZE,
)
from chromatindb.wire import TransportMsgType

if TYPE_CHECKING:
    from chromatindb.client import ChromatinClient
    from chromatindb.types import WriteResult

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

    def refresh(self) -> None:
        """Explicitly invalidate the cache (D-23).

        Next call to list_users/get_user/get_user_by_pubkey will
        trigger a full rebuild from the directory namespace.
        """
        self._cache = None
        self._by_name.clear()
        self._by_pubkey_hash.clear()
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

                parsed = decode_user_entry(result.data)
                if parsed is None:
                    continue  # Not a UserEntry (delegation blob, tombstone, etc.)

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

            if page.cursor is None:
                break
            cursor = page.cursor

        self._cache = cache
        self._by_name = by_name
        self._by_pubkey_hash = by_pubkey_hash
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
