"""Tests for chromatindb identity management (ML-DSA-87).

Tests validate key generation, file I/O, signing, verification,
and cross-language interoperability against C++ test vectors.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from tests.conftest import load_vectors

from chromatindb.crypto import sha3_256
from chromatindb.exceptions import KeyFileError, SignatureError
from chromatindb.identity import (
    PUBLIC_KEY_SIZE,
    SECRET_KEY_SIZE,
    Identity,
)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def vectors() -> dict:
    """Load C++ crypto test vectors."""
    return load_vectors("crypto_vectors.json")


# ---------------------------------------------------------------------------
# Key generation
# ---------------------------------------------------------------------------

def test_generate_key_sizes():
    """Identity.generate() produces pubkey of 2592 bytes, namespace of 32 bytes."""
    identity = Identity.generate()
    assert len(identity.public_key) == PUBLIC_KEY_SIZE
    assert len(identity.namespace) == 32


def test_generate_namespace_derivation():
    """Generated namespace == sha3_256(pubkey)."""
    identity = Identity.generate()
    assert identity.namespace == sha3_256(identity.public_key)


def test_generate_can_sign():
    """Generated identity can sign."""
    identity = Identity.generate()
    assert identity.can_sign is True


# ---------------------------------------------------------------------------
# File I/O
# ---------------------------------------------------------------------------

def test_save_and_load(tmp_dir: Path):
    """generate_and_save then load recovers same identity."""
    key_path = tmp_dir / "test.key"
    original = Identity.generate_and_save(key_path)
    loaded = Identity.load(key_path)
    assert loaded.public_key == original.public_key
    assert loaded.namespace == original.namespace


def test_save_file_sizes(tmp_dir: Path):
    """Key files have correct sizes (raw binary, no headers)."""
    key_path = tmp_dir / "test.key"
    Identity.generate_and_save(key_path)
    assert key_path.stat().st_size == SECRET_KEY_SIZE
    assert key_path.with_suffix(".pub").stat().st_size == PUBLIC_KEY_SIZE


def test_load_wrong_secret_key_size(tmp_dir: Path):
    """Loading a .key file with wrong size raises KeyFileError."""
    key_path = tmp_dir / "bad.key"
    pub_path = tmp_dir / "bad.pub"
    key_path.write_bytes(b"\x00" * 100)
    pub_path.write_bytes(b"\x00" * PUBLIC_KEY_SIZE)
    with pytest.raises(KeyFileError, match="Invalid secret key size"):
        Identity.load(key_path)


def test_load_wrong_public_key_size(tmp_dir: Path):
    """Loading a .pub file with wrong size raises KeyFileError."""
    key_path = tmp_dir / "bad.key"
    pub_path = tmp_dir / "bad.pub"
    key_path.write_bytes(b"\x00" * SECRET_KEY_SIZE)
    pub_path.write_bytes(b"\x00" * 100)
    with pytest.raises(KeyFileError, match="Invalid public key size"):
        Identity.load(key_path)


def test_load_missing_pub_file(tmp_dir: Path):
    """Loading identity with missing .pub raises KeyFileError."""
    key_path = tmp_dir / "nopub.key"
    key_path.write_bytes(b"\x00" * SECRET_KEY_SIZE)
    # Deliberately do NOT create .pub
    with pytest.raises(KeyFileError, match="not found"):
        Identity.load(key_path)


# ---------------------------------------------------------------------------
# Signing and verification
# ---------------------------------------------------------------------------

def test_sign_and_verify():
    """sign + verify roundtrip succeeds."""
    identity = Identity.generate()
    message = b"test message to sign"
    signature = identity.sign(message)
    assert Identity.verify(message, signature, identity.public_key) is True


def test_verify_wrong_message():
    """Verify with different message returns False."""
    identity = Identity.generate()
    signature = identity.sign(b"message A")
    assert Identity.verify(b"message B", signature, identity.public_key) is False


def test_verify_only_cannot_sign():
    """Verify-only identity (from_public_key) cannot sign."""
    identity = Identity.generate()
    verify_only = Identity.from_public_key(identity.public_key)
    assert verify_only.can_sign is False
    with pytest.raises(SignatureError, match="Cannot sign"):
        verify_only.sign(b"anything")


def test_from_public_key_wrong_size():
    """from_public_key with wrong size raises KeyFileError."""
    with pytest.raises(KeyFileError, match="Invalid public key size"):
        Identity.from_public_key(b"\x00" * 100)


# ---------------------------------------------------------------------------
# C++ vector cross-language verification
# ---------------------------------------------------------------------------

def test_cpp_vector_namespace(vectors: dict):
    """Namespace derivation matches C++ vector: sha3_256(pubkey) == expected."""
    ns_vec = vectors["namespace_derivation"]
    pubkey = bytes.fromhex(ns_vec["public_key_hex"])
    expected_ns = bytes.fromhex(ns_vec["namespace_hex"])
    assert sha3_256(pubkey) == expected_ns


def test_cpp_vector_verify(vectors: dict):
    """ML-DSA-87 signature from C++ vector verifies correctly."""
    ml_vec = vectors["ml_dsa_87"]
    pubkey = bytes.fromhex(ml_vec["public_key_hex"])
    message = bytes.fromhex(ml_vec["message_hex"])
    signature = bytes.fromhex(ml_vec["signature_hex"])
    assert Identity.verify(message, signature, pubkey) is True


def test_cpp_vector_verify_wrong_message(vectors: dict):
    """C++ vector signature does not verify with different message."""
    ml_vec = vectors["ml_dsa_87"]
    pubkey = bytes.fromhex(ml_vec["public_key_hex"])
    signature = bytes.fromhex(ml_vec["signature_hex"])
    assert Identity.verify(b"wrong message", signature, pubkey) is False
