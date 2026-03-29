"""Tests for chromatindb crypto primitives against C++ test vectors.

Every test validates that the Python implementation produces byte-identical
output to the C++ node for the same inputs.
"""

from __future__ import annotations

import pytest

from tests.conftest import load_vectors

from chromatindb.crypto import (
    AEAD_KEY_SIZE,
    AEAD_NONCE_SIZE,
    AEAD_TAG_SIZE,
    SHA3_256_SIZE,
    aead_decrypt,
    aead_encrypt,
    build_signing_input,
    sha3_256,
)
from chromatindb._hkdf import hkdf_derive, hkdf_expand, hkdf_extract
from chromatindb.exceptions import CryptoError


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def vectors() -> dict:
    """Load C++ crypto test vectors."""
    return load_vectors("crypto_vectors.json")


# ---------------------------------------------------------------------------
# SHA3-256
# ---------------------------------------------------------------------------

def test_sha3_256_empty():
    """SHA3-256 of empty input matches known value."""
    result = sha3_256(b"")
    expected = bytes.fromhex(
        "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"
    )
    assert result == expected
    assert len(result) == SHA3_256_SIZE


@pytest.mark.parametrize("idx", range(3))
def test_sha3_256_vectors(vectors: dict, idx: int):
    """SHA3-256 matches all C++ test vectors."""
    v = vectors["sha3_256"][idx]
    result = sha3_256(bytes.fromhex(v["input_hex"]))
    assert result == bytes.fromhex(v["expected_hex"])


# ---------------------------------------------------------------------------
# HKDF-SHA256
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("idx", range(3))
def test_hkdf_extract_vectors(vectors: dict, idx: int):
    """HKDF extract matches C++ vector PRK."""
    v = vectors["hkdf"][idx]
    salt = bytes.fromhex(v["salt_hex"])
    ikm = bytes.fromhex(v["ikm_hex"])
    prk = hkdf_extract(salt, ikm)
    assert prk.hex() == v["prk_hex"]


@pytest.mark.parametrize("idx", range(3))
def test_hkdf_expand_vectors(vectors: dict, idx: int):
    """HKDF expand matches C++ vector OKM."""
    v = vectors["hkdf"][idx]
    prk = bytes.fromhex(v["prk_hex"])
    context = v["context"].encode("utf-8")
    output_len = v["output_len"]
    okm = hkdf_expand(prk, context, output_len)
    assert okm.hex() == v["okm_hex"]


def test_hkdf_empty_salt(vectors: dict):
    """HKDF with empty salt matches C++ (critical: uses zero-filled 32-byte salt per RFC 5869)."""
    # Find the empty-salt vector
    empty_salt_vectors = [v for v in vectors["hkdf"] if v["salt_hex"] == ""]
    assert len(empty_salt_vectors) > 0, "No empty-salt HKDF vector found"
    v = empty_salt_vectors[0]
    prk = hkdf_extract(b"", bytes.fromhex(v["ikm_hex"]))
    assert prk.hex() == v["prk_hex"]


def test_hkdf_derive_matches_extract_expand(vectors: dict):
    """hkdf_derive convenience equals extract + expand."""
    v = vectors["hkdf"][0]
    salt = bytes.fromhex(v["salt_hex"])
    ikm = bytes.fromhex(v["ikm_hex"])
    context = v["context"].encode("utf-8")
    output_len = v["output_len"]

    # One-shot
    result = hkdf_derive(salt, ikm, context, output_len)
    # Manual
    prk = hkdf_extract(salt, ikm)
    expected = hkdf_expand(prk, context, output_len)
    assert result == expected


def test_hkdf_expand_invalid_length():
    """HKDF expand raises ValueError for invalid length."""
    prk = b"\x00" * 32
    with pytest.raises(ValueError):
        hkdf_expand(prk, b"info", 0)
    with pytest.raises(ValueError):
        hkdf_expand(prk, b"info", 255 * 32 + 1)


# ---------------------------------------------------------------------------
# AEAD (ChaCha20-Poly1305 IETF)
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("idx", range(3))
def test_aead_encrypt_vectors(vectors: dict, idx: int):
    """AEAD encrypt matches C++ vector ciphertext."""
    v = vectors["aead"][idx]
    plaintext = bytes.fromhex(v["plaintext_hex"])
    ad = bytes.fromhex(v["ad_hex"])
    nonce = bytes.fromhex(v["nonce_hex"])
    key = bytes.fromhex(v["key_hex"])
    result = aead_encrypt(plaintext, ad, nonce, key)
    assert result.hex() == v["ciphertext_hex"]


@pytest.mark.parametrize("idx", range(3))
def test_aead_decrypt_vectors(vectors: dict, idx: int):
    """AEAD decrypt of C++ ciphertext produces original plaintext."""
    v = vectors["aead"][idx]
    ciphertext = bytes.fromhex(v["ciphertext_hex"])
    ad = bytes.fromhex(v["ad_hex"])
    nonce = bytes.fromhex(v["nonce_hex"])
    key = bytes.fromhex(v["key_hex"])
    plaintext = bytes.fromhex(v["plaintext_hex"])
    result = aead_decrypt(ciphertext, ad, nonce, key)
    assert result == plaintext


def test_aead_decrypt_wrong_key(vectors: dict):
    """AEAD decrypt with wrong key returns None (no exception)."""
    v = vectors["aead"][0]
    ciphertext = bytes.fromhex(v["ciphertext_hex"])
    ad = bytes.fromhex(v["ad_hex"])
    nonce = bytes.fromhex(v["nonce_hex"])
    wrong_key = b"\xff" * AEAD_KEY_SIZE
    result = aead_decrypt(ciphertext, ad, nonce, wrong_key)
    assert result is None


def test_aead_encrypt_wrong_nonce_size():
    """AEAD encrypt with wrong nonce size raises CryptoError."""
    with pytest.raises(CryptoError, match="nonce must be"):
        aead_encrypt(b"test", b"ad", b"\x00" * 8, b"\x00" * AEAD_KEY_SIZE)


def test_aead_encrypt_wrong_key_size():
    """AEAD encrypt with wrong key size raises CryptoError."""
    with pytest.raises(CryptoError, match="key must be"):
        aead_encrypt(b"test", b"ad", b"\x00" * AEAD_NONCE_SIZE, b"\x00" * 16)


def test_aead_decrypt_short_ciphertext():
    """AEAD decrypt of ciphertext shorter than tag returns None."""
    result = aead_decrypt(
        b"\x00" * (AEAD_TAG_SIZE - 1),
        b"",
        b"\x00" * AEAD_NONCE_SIZE,
        b"\x00" * AEAD_KEY_SIZE,
    )
    assert result is None


# ---------------------------------------------------------------------------
# Canonical signing input
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("idx", range(3))
def test_signing_input_vectors(vectors: dict, idx: int):
    """build_signing_input matches all C++ test vectors."""
    v = vectors["signing_input"][idx]
    result = build_signing_input(
        bytes.fromhex(v["namespace_hex"]),
        bytes.fromhex(v["data_hex"]),
        v["ttl"],
        v["timestamp"],
    )
    assert result.hex() == v["expected_hex"]


def test_signing_input_zero_ttl_timestamp(vectors: dict):
    """build_signing_input with zero ttl/timestamp matches vector if present."""
    # Find a vector with zero values
    zero_vectors = [
        v for v in vectors["signing_input"]
        if v["ttl"] == 0 and v["timestamp"] == 0
    ]
    if zero_vectors:
        v = zero_vectors[0]
        result = build_signing_input(
            bytes.fromhex(v["namespace_hex"]),
            bytes.fromhex(v["data_hex"]),
            0,
            0,
        )
        assert result.hex() == v["expected_hex"]
    else:
        # Verify zero encoding manually
        result = build_signing_input(b"\x00" * 32, b"", 0, 0)
        assert len(result) == SHA3_256_SIZE


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

def test_constants():
    """Crypto constants match C++ values."""
    assert SHA3_256_SIZE == 32
    assert AEAD_KEY_SIZE == 32
    assert AEAD_NONCE_SIZE == 12
    assert AEAD_TAG_SIZE == 16
