"""Encrypted frame IO layer for chromatindb transport.

Provides length-prefixed raw frame send/recv and AEAD-encrypted
frame send/recv with counter-based nonce management.  Matches the
C++ wire format in db/net/framing.h exactly.

Wire format:
  Raw:       [4-byte BE length][payload]
  Encrypted: [4-byte BE length][AEAD ciphertext (includes 16-byte tag)]
  Nonce:     [4 zero bytes][8-byte BE counter]
"""

from __future__ import annotations

import asyncio
import struct

from chromatindb.crypto import aead_decrypt, aead_encrypt
from chromatindb.exceptions import (
    ConnectionError as ChromatinConnectionError,
    DecryptionError,
    ProtocolError,
)

# 110 MiB -- matches db/net/framing.h MAX_FRAME_SIZE
MAX_FRAME_SIZE: int = 110 * 1024 * 1024  # 115343360


def make_nonce(counter: int) -> bytes:
    """Build 12-byte AEAD nonce: [4 zero bytes][8-byte BE counter].

    Args:
        counter: Monotonically increasing frame counter (uint64).

    Returns:
        12-byte nonce suitable for ChaCha20-Poly1305 IETF.
    """
    return b"\x00\x00\x00\x00" + struct.pack(">Q", counter)


async def send_raw(writer: asyncio.StreamWriter, data: bytes) -> None:
    """Send a length-prefixed frame: [4-byte BE len(data)][data].

    Args:
        writer: asyncio stream writer.
        data: Frame payload bytes.
    """
    writer.write(struct.pack(">I", len(data)) + data)
    await writer.drain()


async def recv_raw(reader: asyncio.StreamReader) -> bytes:
    """Read a length-prefixed frame from the stream.

    Reads a 4-byte big-endian length header, validates against
    MAX_FRAME_SIZE, then reads exactly that many payload bytes.

    Args:
        reader: asyncio stream reader.

    Returns:
        Frame payload bytes.

    Raises:
        ProtocolError: If declared frame length exceeds MAX_FRAME_SIZE.
        chromatindb.exceptions.ConnectionError: If the connection drops
            mid-read (asyncio.IncompleteReadError).
    """
    try:
        header = await reader.readexactly(4)
    except asyncio.IncompleteReadError as exc:
        raise ChromatinConnectionError(
            "connection closed while reading frame header"
        ) from exc

    (length,) = struct.unpack(">I", header)

    if length > MAX_FRAME_SIZE:
        raise ProtocolError(
            f"frame size {length} exceeds maximum {MAX_FRAME_SIZE}"
        )

    try:
        return await reader.readexactly(length)
    except asyncio.IncompleteReadError as exc:
        raise ChromatinConnectionError(
            "connection closed while reading frame payload"
        ) from exc


async def send_encrypted(
    writer: asyncio.StreamWriter,
    plaintext: bytes,
    key: bytes,
    counter: int,
) -> int:
    """Encrypt plaintext with AEAD and send as a raw frame.

    Uses empty associated data (b"") matching the C++ node.

    Args:
        writer: asyncio stream writer.
        plaintext: Data to encrypt and send.
        key: 32-byte AEAD key.
        counter: Current send nonce counter.

    Returns:
        Incremented counter (counter + 1).
    """
    nonce = make_nonce(counter)
    ciphertext = aead_encrypt(plaintext, b"", nonce, key)
    await send_raw(writer, ciphertext)
    return counter + 1


async def recv_encrypted(
    reader: asyncio.StreamReader,
    key: bytes,
    counter: int,
) -> tuple[bytes, int]:
    """Read a raw frame, decrypt with AEAD, return plaintext.

    Uses empty associated data (b"") matching the C++ node.

    Args:
        reader: asyncio stream reader.
        key: 32-byte AEAD key.
        counter: Current receive nonce counter.

    Returns:
        Tuple of (plaintext, incremented counter).

    Raises:
        DecryptionError: If AEAD authentication fails (wrong key,
            corrupted frame, or nonce desync).
    """
    ciphertext = await recv_raw(reader)
    nonce = make_nonce(counter)
    plaintext = aead_decrypt(ciphertext, b"", nonce, key)
    if plaintext is None:
        raise DecryptionError(
            "AEAD decryption failed -- nonce desync or corrupted frame"
        )
    return plaintext, counter + 1
