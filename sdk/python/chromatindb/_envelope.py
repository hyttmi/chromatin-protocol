"""PQ envelope encryption for chromatindb SDK.

Implements multi-recipient envelope encryption using the KEM-then-Wrap pattern:
- Random DEK encrypts data once with ChaCha20-Poly1305
- Per-recipient: ML-KEM-1024 encap -> HKDF KEK -> AEAD-wrap DEK
- Versioned binary format: [magic:4][version:1][suite:1][count:2 BE][nonce:12][N x stanza:1648][ciphertext+tag]

Internal module -- import from chromatindb instead.
"""

from __future__ import annotations

import secrets
import struct
from typing import TYPE_CHECKING

import brotli
import oqs

from chromatindb.crypto import AEAD_TAG_SIZE, aead_decrypt, aead_encrypt, sha3_256
from chromatindb._hkdf import hkdf_derive
from chromatindb.exceptions import (
    DecompressionError,
    DecryptionError,
    MalformedEnvelopeError,
    NotARecipientError,
)

if TYPE_CHECKING:
    from chromatindb.identity import Identity

# Envelope format constants (per D-01 through D-08)
ENVELOPE_MAGIC = b"CENV"                     # 0x43454E56 (per D-01)
ENVELOPE_VERSION = 0x01                       # per D-02
CIPHER_SUITE_ML_KEM_CHACHA = 0x01            # per D-03
_STANZA_SIZE = 1648                           # 32 + 1568 + 48 (per D-06)
_FIXED_HEADER_SIZE = 20                       # 4 + 1 + 1 + 2 + 12
_KEM_CT_SIZE = 1568                           # ML-KEM-1024 ciphertext
_KEM_PK_HASH_SIZE = 32                        # SHA3-256 of KEM pubkey
_WRAPPED_DEK_SIZE = 48                        # 32-byte DEK + 16-byte AEAD tag
_MAX_RECIPIENTS = 256                         # per D-04
_HKDF_LABEL = b"chromatindb-envelope-kek-v1"  # per D-06/D-11
_ZERO_NONCE = b"\x00" * 12                    # per D-09

# Brotli compression constants (Phase 87)
CIPHER_SUITE_ML_KEM_CHACHA_BROTLI = 0x02   # ML-KEM-1024 + ChaCha20-Poly1305 + Brotli (per D-02)
_COMPRESS_THRESHOLD = 256                    # Skip compression below this size (per D-05)
_BROTLI_QUALITY = 6                          # Brotli quality level (per D-04)
MAX_DECOMPRESSED_SIZE = 100 * 1024 * 1024    # 100 MiB, matches MAX_BLOB_DATA_SIZE (per D-09)


def _safe_decompress(compressed: bytes, max_size: int) -> bytes:
    """Decompress Brotli data with output size cap.

    Uses streaming Decompressor to avoid allocating full output
    before checking size. Raises DecompressionError if output
    exceeds max_size.
    """
    dec = brotli.Decompressor()
    chunks: list[bytes] = []
    total = 0

    result = dec.process(compressed)
    total += len(result)
    if total > max_size:
        raise DecompressionError(
            f"Decompressed size {total} exceeds maximum {max_size}"
        )
    chunks.append(result)

    while not dec.is_finished():
        result = dec.process(b"")
        total += len(result)
        if total > max_size:
            raise DecompressionError(
                f"Decompressed size {total} exceeds maximum {max_size}"
            )
        chunks.append(result)

    return b"".join(chunks)


def envelope_encrypt(
    plaintext: bytes,
    recipients: list[Identity],
    sender: Identity,
    compress: bool = True,
) -> bytes:
    """Encrypt data for multiple PQ recipients using KEM-then-Wrap.

    The sender is always auto-included as a recipient (cannot lock yourself
    out). Duplicate recipients (by KEM public key hash) are deduplicated.
    Stanzas are sorted by pk_hash for O(log N) lookup during decryption.

    Args:
        plaintext: Arbitrary data to encrypt (may be empty).
        recipients: List of recipient identities (each must have KEM pubkey).
        sender: Sender identity (must have KEM keypair).

    Returns:
        Binary envelope: header + per-recipient stanzas + ciphertext.

    Raises:
        ValueError: If sender lacks KEM, any recipient lacks KEM, or
            too many recipients (> 256).
    """
    if not sender.has_kem:
        raise ValueError("Sender must have KEM keypair")

    # Build unique recipient set: sender first, then others (per D-15, D-17)
    all_recipients = [sender] + list(recipients)
    seen_hashes: dict[bytes, Identity] = {}
    for r in all_recipients:
        if not r.has_kem:
            raise ValueError("All recipients must have KEM public keys")
        pk_hash = sha3_256(r.kem_public_key)
        if pk_hash not in seen_hashes:
            seen_hashes[pk_hash] = r
    unique_recipients = list(seen_hashes.values())

    # Enforce recipient cap (per D-04)
    if len(unique_recipients) > _MAX_RECIPIENTS:
        raise ValueError(
            f"Too many recipients: {len(unique_recipients)} > {_MAX_RECIPIENTS}"
        )

    # Generate random DEK and data nonce (per D-05)
    dek = secrets.token_bytes(32)
    data_nonce = secrets.token_bytes(12)

    # Pass 1: KEM encapsulation for each recipient
    encap_results: list[tuple[bytes, bytes, bytes]] = []  # (pk_hash, kem_ct, kem_ss)
    for recipient in unique_recipients:
        kem = oqs.KeyEncapsulation("ML-KEM-1024")
        kem_ct, kem_ss = kem.encap_secret(recipient.kem_public_key)
        pk_hash = sha3_256(recipient.kem_public_key)
        encap_results.append((pk_hash, bytes(kem_ct), bytes(kem_ss)))

    # Sort by pk_hash for O(log N) binary search during decrypt (per D-07)
    encap_results.sort(key=lambda t: t[0])

    # Determine compression and suite (per D-01, D-02, D-05, D-06, D-07)
    actual_suite = CIPHER_SUITE_ML_KEM_CHACHA
    data_to_encrypt = plaintext

    if compress and len(plaintext) >= _COMPRESS_THRESHOLD:
        compressed = brotli.compress(plaintext, quality=_BROTLI_QUALITY)
        if len(compressed) < len(plaintext):
            data_to_encrypt = compressed
            actual_suite = CIPHER_SUITE_ML_KEM_CHACHA_BROTLI

    # Build partial header and wrap AD (per D-10)
    recipient_count = len(encap_results)
    partial_header = bytearray()
    partial_header.extend(ENVELOPE_MAGIC)
    partial_header.extend(struct.pack("B", ENVELOPE_VERSION))
    partial_header.extend(struct.pack("B", actual_suite))
    partial_header.extend(struct.pack(">H", recipient_count))
    partial_header.extend(data_nonce)

    # Wrap AD = partial header + all (pk_hash + kem_ct) pairs
    wrap_ad = bytearray(partial_header)
    for pk_hash, kem_ct, _ in encap_results:
        wrap_ad.extend(pk_hash)
        wrap_ad.extend(kem_ct)
    wrap_ad = bytes(wrap_ad)

    # Pass 2: Wrap DEK for each recipient (per D-09, D-11)
    stanzas: list[tuple[bytes, bytes, bytes]] = []  # (pk_hash, kem_ct, wrapped_dek)
    for pk_hash, kem_ct, kem_ss in encap_results:
        kek = hkdf_derive(salt=b"", ikm=kem_ss, info=_HKDF_LABEL, length=32)
        # Zero nonce is safe: KEM shared secret (and thus KEK) is unique
        # per encapsulation -- the key is never reused.
        wrapped_dek = aead_encrypt(dek, ad=wrap_ad, nonce=_ZERO_NONCE, key=kek)
        stanzas.append((pk_hash, kem_ct, wrapped_dek))

    # Build full header (per D-08)
    full_header = bytearray(partial_header)
    for pk_hash, kem_ct, wrapped_dek in stanzas:
        full_header.extend(pk_hash)
        full_header.extend(kem_ct)
        full_header.extend(wrapped_dek)
    full_header = bytes(full_header)

    # Encrypt data with full header as AD (per D-04, ENV-01)
    ciphertext = aead_encrypt(data_to_encrypt, ad=full_header, nonce=data_nonce, key=dek)

    return full_header + ciphertext


def envelope_decrypt(data: bytes, identity: Identity) -> bytes:
    """Decrypt a PQ envelope for the given identity.

    Finds the recipient stanza by matching stanza pk_hashes against the
    identity's KEM key ring map (supports decryption with any historical
    KEM key after rotation). Decapsulates the KEM ciphertext, derives
    KEK via HKDF, unwraps DEK, and decrypts the payload.

    Args:
        data: Complete binary envelope.
        identity: Identity with KEM secret key for decryption.

    Returns:
        Decrypted plaintext bytes.

    Raises:
        MalformedEnvelopeError: If envelope has invalid magic, version,
            cipher suite, zero recipients, or is truncated.
        NotARecipientError: If identity is not in recipient list or has
            no KEM keypair.
        DecryptionError: If AEAD authentication fails (tampered envelope).
    """
    # Validate minimum size
    if len(data) < _FIXED_HEADER_SIZE + _STANZA_SIZE + AEAD_TAG_SIZE:
        raise MalformedEnvelopeError("Envelope too short")

    # Parse fixed header
    magic = data[:4]
    if magic != ENVELOPE_MAGIC:
        raise MalformedEnvelopeError(
            f"Invalid magic: expected CENV, got {magic!r}"
        )

    version = data[4]
    if version != ENVELOPE_VERSION:
        raise MalformedEnvelopeError(f"Unsupported envelope version: {version}")

    suite = data[5]
    if suite not in (CIPHER_SUITE_ML_KEM_CHACHA, CIPHER_SUITE_ML_KEM_CHACHA_BROTLI):
        raise MalformedEnvelopeError(f"Unsupported cipher suite: {suite}")

    recipient_count = struct.unpack(">H", data[6:8])[0]
    if recipient_count == 0:
        raise MalformedEnvelopeError("Envelope has zero recipients")

    data_nonce = data[8:20]

    # Validate total size
    expected_header_size = _FIXED_HEADER_SIZE + recipient_count * _STANZA_SIZE
    if len(data) < expected_header_size + AEAD_TAG_SIZE:
        raise MalformedEnvelopeError("Envelope truncated")

    # Parse stanza pk_hashes for binary search
    stanza_offset = _FIXED_HEADER_SIZE
    pk_hashes: list[bytes] = []
    for i in range(recipient_count):
        offset = stanza_offset + i * _STANZA_SIZE
        pk_hashes.append(data[offset : offset + _KEM_PK_HASH_SIZE])

    # Find recipient stanza via key ring map lookup (per D-08)
    if not identity.has_kem:
        raise NotARecipientError("Identity has no KEM keypair for decryption")

    ring_map = identity._build_kem_ring_map()
    if not ring_map:
        raise NotARecipientError("Identity has no KEM secret key for decryption")

    # Scan stanza pk_hashes against the ring map
    matched_idx = None
    matched_kem = None
    for i, pk_hash in enumerate(pk_hashes):
        if pk_hash in ring_map:
            matched_idx = i
            matched_kem = ring_map[pk_hash]
            break

    if matched_idx is None:
        raise NotARecipientError("Identity not in envelope recipient list")

    # Extract stanza fields using matched index
    stanza_base = stanza_offset + matched_idx * _STANZA_SIZE
    kem_ct = data[stanza_base + _KEM_PK_HASH_SIZE : stanza_base + _KEM_PK_HASH_SIZE + _KEM_CT_SIZE]
    wrapped_dek = data[stanza_base + _KEM_PK_HASH_SIZE + _KEM_CT_SIZE : stanza_base + _STANZA_SIZE]

    # Decapsulate with matched key from ring
    kem_ss = bytes(matched_kem.decap_secret(kem_ct))

    # Derive KEK and unwrap DEK (per D-09, D-11)
    kek = hkdf_derive(salt=b"", ikm=kem_ss, info=_HKDF_LABEL, length=32)

    # Build wrap AD: partial header + all (pk_hash + kem_ct) pairs
    partial_header = data[:_FIXED_HEADER_SIZE]
    wrap_ad = bytearray(partial_header)
    for i in range(recipient_count):
        offset = stanza_offset + i * _STANZA_SIZE
        wrap_ad.extend(data[offset : offset + _KEM_PK_HASH_SIZE + _KEM_CT_SIZE])
    wrap_ad = bytes(wrap_ad)

    # Unwrap DEK
    dek = aead_decrypt(wrapped_dek, ad=wrap_ad, nonce=_ZERO_NONCE, key=kek)
    if dek is None:
        raise DecryptionError("DEK unwrap failed -- key authentication error")

    # Decrypt data (full header as AD per D-10)
    full_header = data[:expected_header_size]
    ciphertext = data[expected_header_size:]
    plaintext = aead_decrypt(ciphertext, ad=full_header, nonce=data_nonce, key=dek)
    if plaintext is None:
        raise DecryptionError("Data decryption failed -- authentication error")

    # Decompress if suite=0x02 (per D-03, D-09)
    if suite == CIPHER_SUITE_ML_KEM_CHACHA_BROTLI:
        plaintext = _safe_decompress(plaintext, MAX_DECOMPRESSED_SIZE)

    return plaintext


def envelope_parse(data: bytes) -> dict[str, int]:
    """Parse envelope metadata without decryption.

    Returns version, cipher suite, and recipient count from the header.
    Does not require a KEM secret key.

    Args:
        data: Complete or partial binary envelope (at least 8 bytes).

    Returns:
        Dict with keys: version, suite, recipient_count.

    Raises:
        MalformedEnvelopeError: If magic, version, or suite is invalid,
            or data is too short.
    """
    if len(data) < 8:
        raise MalformedEnvelopeError("Envelope too short for header parse")

    magic = data[:4]
    if magic != ENVELOPE_MAGIC:
        raise MalformedEnvelopeError(
            f"Invalid magic: expected CENV, got {magic!r}"
        )

    version = data[4]
    if version != ENVELOPE_VERSION:
        raise MalformedEnvelopeError(f"Unsupported envelope version: {version}")

    suite = data[5]
    if suite not in (CIPHER_SUITE_ML_KEM_CHACHA, CIPHER_SUITE_ML_KEM_CHACHA_BROTLI):
        raise MalformedEnvelopeError(f"Unsupported cipher suite: {suite}")

    recipient_count = struct.unpack(">H", data[6:8])[0]

    return {
        "version": version,
        "suite": suite,
        "recipient_count": recipient_count,
    }
