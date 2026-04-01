"""Tests for chromatindb PQ envelope encryption."""

from __future__ import annotations

import secrets
import struct

import pytest

from tests.conftest import load_vectors

import oqs
from chromatindb.crypto import sha3_256
from chromatindb.identity import Identity
from chromatindb._envelope import (
    CIPHER_SUITE_ML_KEM_CHACHA,
    ENVELOPE_MAGIC,
    ENVELOPE_VERSION,
    _FIXED_HEADER_SIZE,
    _HKDF_LABEL,
    _STANZA_SIZE,
    envelope_decrypt,
    envelope_encrypt,
    envelope_parse,
)
from chromatindb.exceptions import (
    DecryptionError,
    MalformedEnvelopeError,
    NotARecipientError,
)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def vectors() -> dict:
    """Load envelope test vectors."""
    return load_vectors("envelope_vectors.json")


# ---------------------------------------------------------------------------
# Basic encrypt/decrypt roundtrip
# ---------------------------------------------------------------------------

def test_encrypt_decrypt_roundtrip():
    """Encrypt for 1 recipient + sender, both can decrypt, plaintext matches."""
    sender = Identity.generate()
    recipient = Identity.generate()
    plaintext = b"hello envelope roundtrip"
    envelope = envelope_encrypt(plaintext, [recipient], sender)
    assert envelope_decrypt(envelope, sender) == plaintext
    assert envelope_decrypt(envelope, recipient) == plaintext


def test_encrypt_decrypt_empty_plaintext():
    """Encrypt empty bytes roundtrips correctly."""
    sender = Identity.generate()
    envelope = envelope_encrypt(b"", [], sender)
    assert envelope_decrypt(envelope, sender) == b""


def test_encrypt_decrypt_large_plaintext():
    """Encrypt 1 MiB of random data roundtrips correctly."""
    sender = Identity.generate()
    plaintext = secrets.token_bytes(1024 * 1024)
    envelope = envelope_encrypt(plaintext, [], sender)
    assert envelope_decrypt(envelope, sender) == plaintext


# ---------------------------------------------------------------------------
# Sender auto-include
# ---------------------------------------------------------------------------

def test_sender_auto_included():
    """Encrypt with empty recipients list, sender can still decrypt (per D-15, D-16)."""
    sender = Identity.generate()
    envelope = envelope_encrypt(b"sender only", [], sender)
    meta = envelope_parse(envelope)
    assert meta["recipient_count"] == 1  # Only sender
    assert envelope_decrypt(envelope, sender) == b"sender only"


def test_sender_in_recipients_deduped():
    """Sender explicitly in recipients list, recipient_count not doubled (per D-17)."""
    sender = Identity.generate()
    envelope = envelope_encrypt(b"dedup test", [sender], sender)
    meta = envelope_parse(envelope)
    assert meta["recipient_count"] == 1  # Sender deduplicated


# ---------------------------------------------------------------------------
# Multi-recipient
# ---------------------------------------------------------------------------

def test_three_recipients():
    """Encrypt for 3 recipients, all can decrypt, sender can decrypt, same plaintext."""
    sender = Identity.generate()
    r1 = Identity.generate()
    r2 = Identity.generate()
    r3 = Identity.generate()
    plaintext = b"three recipients"
    envelope = envelope_encrypt(plaintext, [r1, r2, r3], sender)
    meta = envelope_parse(envelope)
    assert meta["recipient_count"] == 4  # sender + 3 recipients
    assert envelope_decrypt(envelope, sender) == plaintext
    assert envelope_decrypt(envelope, r1) == plaintext
    assert envelope_decrypt(envelope, r2) == plaintext
    assert envelope_decrypt(envelope, r3) == plaintext


def test_non_recipient_cannot_decrypt():
    """Identity not in recipient list raises NotARecipientError."""
    sender = Identity.generate()
    recipient = Identity.generate()
    outsider = Identity.generate()
    envelope = envelope_encrypt(b"secret", [recipient], sender)
    with pytest.raises(NotARecipientError, match="not in envelope recipient list"):
        envelope_decrypt(envelope, outsider)


# ---------------------------------------------------------------------------
# Binary format validation
# ---------------------------------------------------------------------------

def test_envelope_magic():
    """First 4 bytes of output are b'CENV' (per D-01)."""
    sender = Identity.generate()
    envelope = envelope_encrypt(b"magic test", [], sender)
    assert envelope[:4] == b"CENV"


def test_envelope_version():
    """Byte at offset 4 is 0x01 (per D-02)."""
    sender = Identity.generate()
    envelope = envelope_encrypt(b"version test", [], sender)
    assert envelope[4] == 0x01


def test_envelope_suite():
    """Byte at offset 5 is 0x01 (per D-03)."""
    sender = Identity.generate()
    envelope = envelope_encrypt(b"suite test", [], sender)
    assert envelope[5] == 0x01


def test_envelope_recipient_count():
    """Bytes 6-7 big-endian match actual unique recipient count (per D-04)."""
    sender = Identity.generate()
    r1 = Identity.generate()
    r2 = Identity.generate()
    envelope = envelope_encrypt(b"count test", [r1, r2], sender)
    count = struct.unpack(">H", envelope[6:8])[0]
    assert count == 3  # sender + r1 + r2


def test_envelope_nonce_is_12_bytes():
    """Bytes 8-19 are the nonce (12 bytes, non-zero with high probability) (per D-05)."""
    sender = Identity.generate()
    envelope = envelope_encrypt(b"nonce test", [], sender)
    nonce = envelope[8:20]
    assert len(nonce) == 12
    # Random nonce should not be all zeros (probability 2^-96)
    assert nonce != b"\x00" * 12


def test_stanza_size():
    """Each stanza is exactly 1648 bytes (per D-06)."""
    sender = Identity.generate()
    r1 = Identity.generate()
    envelope = envelope_encrypt(b"stanza test", [r1], sender)
    # Total stanzas area: from offset 20 to 20 + 2*1648 = 3316
    meta = envelope_parse(envelope)
    n = meta["recipient_count"]
    stanzas_area = envelope[_FIXED_HEADER_SIZE : _FIXED_HEADER_SIZE + n * _STANZA_SIZE]
    assert len(stanzas_area) == n * _STANZA_SIZE


def test_stanzas_sorted_by_pk_hash():
    """pk_hashes in stanzas are in ascending order (per D-07)."""
    sender = Identity.generate()
    r1 = Identity.generate()
    r2 = Identity.generate()
    r3 = Identity.generate()
    envelope = envelope_encrypt(b"sort test", [r1, r2, r3], sender)
    meta = envelope_parse(envelope)
    n = meta["recipient_count"]
    pk_hashes = []
    for i in range(n):
        offset = _FIXED_HEADER_SIZE + i * _STANZA_SIZE
        pk_hashes.append(envelope[offset : offset + 32])
    # Verify ascending order
    for i in range(len(pk_hashes) - 1):
        assert pk_hashes[i] < pk_hashes[i + 1], (
            f"Stanza {i} pk_hash not less than stanza {i+1}"
        )


def test_envelope_total_size():
    """Total size = 20 + N*1648 + len(plaintext) + 16 (per D-08)."""
    sender = Identity.generate()
    r1 = Identity.generate()
    plaintext = b"size check"
    envelope = envelope_encrypt(plaintext, [r1], sender)
    n = 2  # sender + r1
    expected = _FIXED_HEADER_SIZE + n * _STANZA_SIZE + len(plaintext) + 16
    assert len(envelope) == expected


# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------

def test_decrypt_bad_magic():
    """Modified magic raises MalformedEnvelopeError (per D-19)."""
    sender = Identity.generate()
    envelope = bytearray(envelope_encrypt(b"bad magic", [], sender))
    envelope[0] = 0xFF  # Corrupt magic
    with pytest.raises(MalformedEnvelopeError, match="Invalid magic"):
        envelope_decrypt(bytes(envelope), sender)


def test_decrypt_bad_version():
    """Modified version byte raises MalformedEnvelopeError (per D-19)."""
    sender = Identity.generate()
    envelope = bytearray(envelope_encrypt(b"bad version", [], sender))
    envelope[4] = 0x99  # Unsupported version
    with pytest.raises(MalformedEnvelopeError, match="Unsupported envelope version"):
        envelope_decrypt(bytes(envelope), sender)


def test_decrypt_truncated():
    """Truncated envelope raises MalformedEnvelopeError."""
    sender = Identity.generate()
    envelope = envelope_encrypt(b"truncated", [], sender)
    # Truncate to just the fixed header
    with pytest.raises(MalformedEnvelopeError, match="too short"):
        envelope_decrypt(envelope[:_FIXED_HEADER_SIZE], sender)


def test_decrypt_tampered_ciphertext():
    """Flipped bit in ciphertext raises DecryptionError."""
    sender = Identity.generate()
    envelope = bytearray(envelope_encrypt(b"tamper ciphertext", [], sender))
    # Flip a bit in the last byte (ciphertext area)
    envelope[-1] ^= 0x01
    with pytest.raises(DecryptionError, match="Data decryption failed"):
        envelope_decrypt(bytes(envelope), sender)


def test_decrypt_tampered_header():
    """Flipped bit in header (wrapped_dek area) raises DecryptionError (AD mismatch)."""
    sender = Identity.generate()
    envelope = bytearray(envelope_encrypt(b"tamper header", [], sender))
    # Tamper a byte in the wrapped_dek area of the first stanza.
    # Stanza layout: [pk_hash:32][kem_ct:1568][wrapped_dek:48]
    # The wrapped_dek starts at offset 20 + 32 + 1568 = 1620 within envelope.
    # Flipping a bit there will cause DEK unwrap AEAD failure.
    wrapped_dek_byte = _FIXED_HEADER_SIZE + 32 + 1568 + 1
    envelope[wrapped_dek_byte] ^= 0x01
    with pytest.raises(DecryptionError):
        envelope_decrypt(bytes(envelope), sender)


def test_decrypt_no_kem_identity():
    """Identity without KEM (from_public_key) raises NotARecipientError."""
    sender = Identity.generate()
    envelope = envelope_encrypt(b"no kem", [], sender)
    # Create signing-only identity (no KEM)
    verify_only = Identity.from_public_key(sender.public_key)
    with pytest.raises(NotARecipientError, match="no KEM keypair"):
        envelope_decrypt(envelope, verify_only)


# ---------------------------------------------------------------------------
# AEAD AD verification (per D-10, ENV-04)
# ---------------------------------------------------------------------------

def test_stanza_substitution_fails():
    """Swap stanza from different envelope, decrypt raises DecryptionError.

    The wrap AD includes all (pk_hash + kem_ct) pairs from the partial header,
    so substituting a stanza from another envelope will cause AEAD unwrap failure.
    """
    sender = Identity.generate()
    recipient = Identity.generate()

    # Create two envelopes with different data
    env_a = bytearray(envelope_encrypt(b"data A", [recipient], sender))
    env_b = envelope_encrypt(b"data B", [recipient], sender)

    # Find recipient's stanza index in both envelopes
    # Parse both to find which stanza belongs to recipient
    my_hash = sha3_256(recipient.kem_public_key)
    n_a = struct.unpack(">H", env_a[6:8])[0]

    # Find recipient stanza in env_a
    idx_a = None
    for i in range(n_a):
        offset = _FIXED_HEADER_SIZE + i * _STANZA_SIZE
        if env_a[offset : offset + 32] == my_hash:
            idx_a = i
            break
    assert idx_a is not None, "Recipient stanza not found in env_a"

    # Find recipient stanza in env_b
    n_b = struct.unpack(">H", env_b[6:8])[0]
    idx_b = None
    for i in range(n_b):
        offset = _FIXED_HEADER_SIZE + i * _STANZA_SIZE
        if env_b[offset : offset + 32] == my_hash:
            idx_b = i
            break
    assert idx_b is not None, "Recipient stanza not found in env_b"

    # Replace recipient stanza in env_a with stanza from env_b
    src_offset = _FIXED_HEADER_SIZE + idx_b * _STANZA_SIZE
    dst_offset = _FIXED_HEADER_SIZE + idx_a * _STANZA_SIZE
    env_a[dst_offset : dst_offset + _STANZA_SIZE] = env_b[src_offset : src_offset + _STANZA_SIZE]

    with pytest.raises(DecryptionError):
        envelope_decrypt(bytes(env_a), recipient)


# ---------------------------------------------------------------------------
# envelope_parse
# ---------------------------------------------------------------------------

def test_parse_returns_metadata():
    """parse returns version=1, suite=1, recipient_count=N."""
    sender = Identity.generate()
    r1 = Identity.generate()
    envelope = envelope_encrypt(b"parse test", [r1], sender)
    meta = envelope_parse(envelope)
    assert meta["version"] == 1
    assert meta["suite"] == 1
    assert meta["recipient_count"] == 2


def test_parse_bad_magic():
    """Bad magic raises MalformedEnvelopeError."""
    with pytest.raises(MalformedEnvelopeError, match="Invalid magic"):
        envelope_parse(b"BAAD\x01\x01\x00\x01")


def test_parse_too_short():
    """Too short raises MalformedEnvelopeError."""
    with pytest.raises(MalformedEnvelopeError, match="too short"):
        envelope_parse(b"CENV\x01")


# ---------------------------------------------------------------------------
# Recipient cap
# ---------------------------------------------------------------------------

def test_too_many_recipients():
    """257 recipients raises ValueError."""
    sender = Identity.generate()
    # Create 257 recipients (sender is auto-included, so total would be 258)
    # But we need 257 unique _after_ dedup including sender = 257 unique total
    # So we need 256 additional recipients (sender + 256 = 257 > 256 max)
    recipients = [Identity.generate() for _ in range(256)]
    with pytest.raises(ValueError, match="Too many recipients"):
        envelope_encrypt(b"too many", recipients, sender)


# ---------------------------------------------------------------------------
# HKDF domain label (per D-11, ENV-06)
# ---------------------------------------------------------------------------

def test_hkdf_domain_label_in_source():
    """Verify _HKDF_LABEL constant equals b'chromatindb-envelope-kek-v1'."""
    assert _HKDF_LABEL == b"chromatindb-envelope-kek-v1"


# ---------------------------------------------------------------------------
# Cross-SDK test vectors
# ---------------------------------------------------------------------------

def test_decrypt_known_vector(vectors: dict):
    """Load envelope_vectors.json, decrypt with provided secret key, verify plaintext."""
    for vec in vectors["vectors"]:
        envelope = bytes.fromhex(vec["envelope_hex"])
        expected_plaintext = bytes.fromhex(vec["plaintext_hex"])

        # Reconstruct sender identity for decryption
        sender_kem_secret = bytes.fromhex(vec["sender_kem_secret_hex"])
        sender_kem_public = bytes.fromhex(vec["sender_kem_public_hex"])
        sender_signing_public = bytes.fromhex(vec["sender_signing_public_hex"])

        # Create identity with KEM secret key for decryption
        kem = oqs.KeyEncapsulation("ML-KEM-1024", secret_key=sender_kem_secret)
        namespace = sha3_256(sender_signing_public)
        sender = Identity(sender_signing_public, namespace, None, sender_kem_public, kem)

        plaintext = envelope_decrypt(envelope, sender)
        assert plaintext == expected_plaintext, (
            f"Plaintext mismatch for vector: {vec['description']}"
        )

        # Also test recipient if present
        if "recipient_kem_secret_hex" in vec:
            r_kem_secret = bytes.fromhex(vec["recipient_kem_secret_hex"])
            r_kem_public = bytes.fromhex(vec["recipient_kem_public_hex"])
            r_signing_public = bytes.fromhex(vec["recipient_signing_public_hex"])
            r_kem = oqs.KeyEncapsulation("ML-KEM-1024", secret_key=r_kem_secret)
            r_namespace = sha3_256(r_signing_public)
            recipient = Identity(r_signing_public, r_namespace, None, r_kem_public, r_kem)
            r_plaintext = envelope_decrypt(envelope, recipient)
            assert r_plaintext == expected_plaintext


def test_format_known_vector(vectors: dict):
    """Load envelope_vectors.json, verify magic/version/suite/count match expected."""
    for vec in vectors["vectors"]:
        envelope = bytes.fromhex(vec["envelope_hex"])
        meta = envelope_parse(envelope)
        assert meta["version"] == vec["expected_version"], (
            f"Version mismatch for vector: {vec['description']}"
        )
        assert meta["suite"] == vec["expected_suite"], (
            f"Suite mismatch for vector: {vec['description']}"
        )
        assert meta["recipient_count"] == vec["expected_recipient_count"], (
            f"Recipient count mismatch for vector: {vec['description']}"
        )


# ---------------------------------------------------------------------------
# Additional edge case tests
# ---------------------------------------------------------------------------

def test_different_envelopes_different_ciphertext():
    """Two encryptions of same plaintext produce different ciphertext (random DEK+nonce)."""
    sender = Identity.generate()
    plaintext = b"determinism check"
    env1 = envelope_encrypt(plaintext, [], sender)
    env2 = envelope_encrypt(plaintext, [], sender)
    # Headers will differ (different nonces, different KEM encapsulations)
    assert env1 != env2


def test_envelope_constants():
    """Verify all format constants match spec."""
    assert ENVELOPE_MAGIC == b"CENV"
    assert ENVELOPE_VERSION == 0x01
    assert CIPHER_SUITE_ML_KEM_CHACHA == 0x01
    assert _STANZA_SIZE == 1648
    assert _FIXED_HEADER_SIZE == 20
