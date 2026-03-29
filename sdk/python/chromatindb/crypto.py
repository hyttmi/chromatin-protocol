"""Cryptographic primitives for chromatindb SDK.

All functions produce byte-identical output to the C++ node:
- SHA3-256 via hashlib (stdlib)
- ChaCha20-Poly1305 IETF via PyNaCl (libsodium)
- HKDF-SHA256 via pure-Python stdlib implementation
- Canonical signing input: SHA3-256(namespace || data || ttl_le32 || timestamp_le64)
"""

from __future__ import annotations

import hashlib
import struct

from nacl.bindings import (
    crypto_aead_chacha20poly1305_ietf_decrypt,
    crypto_aead_chacha20poly1305_ietf_encrypt,
)

from chromatindb._hkdf import hkdf_derive, hkdf_expand, hkdf_extract
from chromatindb.exceptions import CryptoError

# SHA3-256 constants
SHA3_256_SIZE: int = 32

# ChaCha20-Poly1305 IETF constants (match db/crypto/aead.h)
AEAD_KEY_SIZE: int = 32
AEAD_NONCE_SIZE: int = 12
AEAD_TAG_SIZE: int = 16

# HKDF-SHA256 constants (match db/crypto/kdf.h)
HKDF_PRK_SIZE: int = 32

# Re-export HKDF functions for convenience
__all__ = [
    "AEAD_KEY_SIZE",
    "AEAD_NONCE_SIZE",
    "AEAD_TAG_SIZE",
    "HKDF_PRK_SIZE",
    "SHA3_256_SIZE",
    "aead_decrypt",
    "aead_encrypt",
    "build_signing_input",
    "hkdf_derive",
    "hkdf_expand",
    "hkdf_extract",
    "sha3_256",
]


def sha3_256(data: bytes) -> bytes:
    """Compute SHA3-256 hash.

    Args:
        data: Input bytes.

    Returns:
        32-byte digest.
    """
    return hashlib.sha3_256(data).digest()


def aead_encrypt(
    plaintext: bytes, ad: bytes, nonce: bytes, key: bytes
) -> bytes:
    """Encrypt with ChaCha20-Poly1305 IETF.

    Args:
        plaintext: Data to encrypt.
        ad: Associated data (authenticated but not encrypted).
        nonce: 12-byte nonce (AEAD_NONCE_SIZE).
        key: 32-byte key (AEAD_KEY_SIZE).

    Returns:
        Ciphertext with appended 16-byte authentication tag.

    Raises:
        CryptoError: If nonce or key size is wrong.
    """
    if len(nonce) != AEAD_NONCE_SIZE:
        raise CryptoError(
            f"AEAD nonce must be {AEAD_NONCE_SIZE} bytes, got {len(nonce)}"
        )
    if len(key) != AEAD_KEY_SIZE:
        raise CryptoError(
            f"AEAD key must be {AEAD_KEY_SIZE} bytes, got {len(key)}"
        )
    return crypto_aead_chacha20poly1305_ietf_encrypt(
        plaintext, ad if ad else None, nonce, key
    )


def aead_decrypt(
    ciphertext: bytes, ad: bytes, nonce: bytes, key: bytes
) -> bytes | None:
    """Decrypt with ChaCha20-Poly1305 IETF.

    Args:
        ciphertext: Data to decrypt (includes 16-byte tag).
        ad: Associated data used during encryption.
        nonce: 12-byte nonce used during encryption.
        key: 32-byte key used during encryption.

    Returns:
        Decrypted plaintext, or None if authentication fails.

    Raises:
        CryptoError: If nonce or key size is wrong.
    """
    if len(nonce) != AEAD_NONCE_SIZE:
        raise CryptoError(
            f"AEAD nonce must be {AEAD_NONCE_SIZE} bytes, got {len(nonce)}"
        )
    if len(key) != AEAD_KEY_SIZE:
        raise CryptoError(
            f"AEAD key must be {AEAD_KEY_SIZE} bytes, got {len(key)}"
        )
    if len(ciphertext) < AEAD_TAG_SIZE:
        return None
    try:
        return crypto_aead_chacha20poly1305_ietf_decrypt(
            ciphertext, ad if ad else None, nonce, key
        )
    except Exception:
        return None


def build_signing_input(
    namespace_id: bytes,
    data: bytes,
    ttl: int,
    timestamp: int,
) -> bytes:
    """Build canonical signing input matching C++ node exactly.

    Computes SHA3-256(namespace || data || ttl_le32 || timestamp_le64).
    The ttl is encoded as little-endian uint32, timestamp as little-endian uint64.

    Args:
        namespace_id: 32-byte namespace identifier.
        data: Blob payload data.
        ttl: Time-to-live in seconds (uint32).
        timestamp: Unix timestamp in seconds (uint64).

    Returns:
        32-byte signing digest.
    """
    h = hashlib.sha3_256()
    h.update(namespace_id)
    h.update(data)
    h.update(struct.pack("<I", ttl))      # Little-endian uint32
    h.update(struct.pack("<Q", timestamp))  # Little-endian uint64
    return h.digest()
