"""Tests for chromatindb directory data types and UserEntry codec."""

from __future__ import annotations

import struct

import pytest

from chromatindb._directory import (
    DELEGATION_MAGIC,
    USERENTRY_MAGIC,
    USERENTRY_MIN_SIZE,
    USERENTRY_VERSION,
    DirectoryEntry,
    decode_user_entry,
    encode_user_entry,
    make_delegation_data,
    verify_user_entry,
)
from chromatindb.exceptions import ChromatinError, DirectoryError
from chromatindb.identity import Identity, KEM_PUBLIC_KEY_SIZE, PUBLIC_KEY_SIZE


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
