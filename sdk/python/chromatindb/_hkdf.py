"""Pure-Python HKDF-SHA256 implementation (RFC 5869).

Uses stdlib hmac + hashlib. Byte-identical to libsodium's
crypto_kdf_hkdf_sha256_* since both implement the same RFC.

Internal module -- import from chromatindb.crypto instead.
"""

from __future__ import annotations

import hashlib
import hmac

_HASH_LEN = 32  # SHA-256 output size


def hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    """HKDF-SHA256 Extract: PRK = HMAC-SHA256(salt, IKM).

    Args:
        salt: Optional salt value. Empty bytes uses zero-filled
              32-byte salt per RFC 5869 Section 2.2.
        ikm: Input keying material.

    Returns:
        32-byte pseudorandom key (PRK).
    """
    if not salt:
        salt = b"\x00" * _HASH_LEN
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    """HKDF-SHA256 Expand: derive OKM from PRK + info.

    Args:
        prk: Pseudorandom key from extract (32 bytes).
        info: Context and application-specific info.
        length: Output length in bytes (1 to 255*32).

    Returns:
        Derived key material of requested length.

    Raises:
        ValueError: If length is out of range.
    """
    if length < 1 or length > 255 * _HASH_LEN:
        msg = f"HKDF expand length must be 1..{255 * _HASH_LEN}, got {length}"
        raise ValueError(msg)
    n = (length + _HASH_LEN - 1) // _HASH_LEN
    okm = b""
    t = b""
    for i in range(1, n + 1):
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        okm += t
    return okm[:length]


def hkdf_derive(salt: bytes, ikm: bytes, info: bytes, length: int) -> bytes:
    """HKDF-SHA256 one-shot: extract then expand.

    Args:
        salt: Salt value (empty for zero-filled default).
        ikm: Input keying material.
        info: Context info for expand.
        length: Output length in bytes.

    Returns:
        Derived key material of requested length.
    """
    prk = hkdf_extract(salt, ikm)
    return hkdf_expand(prk, info, length)
