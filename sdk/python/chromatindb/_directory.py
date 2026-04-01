"""Directory data types and UserEntry binary codec for chromatindb SDK.

Provides the foundational data layer for the Directory class (Plan 02):
- UserEntry encode/decode for self-registration blobs
- DirectoryEntry frozen dataclass for lookup results
- Delegation data construction helper
- Constants for magic bytes and size validation

UserEntry binary format (D-06):
  [magic:4][version:1][signing_pk:2592][kem_pk:1568][name_len:2 BE][name:N][kem_sig:variable]
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from chromatindb.identity import (
    Identity,
    KEM_PUBLIC_KEY_SIZE,
    PUBLIC_KEY_SIZE,
)

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
