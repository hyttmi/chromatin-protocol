"""Tests for the framing layer (_framing.py).

Covers: nonce construction, raw length-prefixed frame IO,
AEAD-encrypted frame IO with counter management, MAX_FRAME_SIZE
validation, and error handling for incomplete reads and decrypt failures.
"""

from __future__ import annotations

import asyncio
import os
import struct
from unittest.mock import AsyncMock, MagicMock

import pytest

from chromatindb._framing import (
    MAX_FRAME_SIZE,
    make_nonce,
    recv_encrypted,
    recv_raw,
    send_encrypted,
    send_raw,
)
from chromatindb.exceptions import (
    ConnectionError as ChromatinConnectionError,
    DecryptionError,
    HandshakeError,
    ProtocolError,
)


# ---------------------------------------------------------------------------
# Exception hierarchy tests
# ---------------------------------------------------------------------------


class TestExceptionSubclasses:
    """HandshakeError and ConnectionError are correct subclasses."""

    def test_handshake_error_is_protocol_error(self) -> None:
        assert issubclass(HandshakeError, ProtocolError)

    def test_connection_error_is_protocol_error(self) -> None:
        assert issubclass(ChromatinConnectionError, ProtocolError)

    def test_connection_error_does_not_shadow_builtin(self) -> None:
        """chromatindb.exceptions.ConnectionError is NOT builtins.ConnectionError."""
        import builtins

        assert ChromatinConnectionError is not builtins.ConnectionError


# ---------------------------------------------------------------------------
# make_nonce tests
# ---------------------------------------------------------------------------


class TestMakeNonce:
    """Nonce construction: [4 zero bytes][8-byte BE counter]."""

    def test_nonce_zero(self) -> None:
        result = make_nonce(0)
        assert result == b"\x00" * 4 + b"\x00" * 8

    def test_nonce_one(self) -> None:
        result = make_nonce(1)
        assert result == b"\x00" * 4 + b"\x00" * 7 + b"\x01"

    def test_nonce_256(self) -> None:
        result = make_nonce(256)
        assert result == b"\x00" * 4 + b"\x00" * 6 + b"\x01\x00"

    def test_nonce_is_12_bytes(self) -> None:
        for counter in (0, 1, 255, 256, 2**32 - 1, 2**63 - 1):
            assert len(make_nonce(counter)) == 12

    def test_nonce_large_counter(self) -> None:
        """Counter 0x0102030405060708 encodes correctly in big-endian."""
        counter = 0x0102030405060708
        result = make_nonce(counter)
        assert result == b"\x00\x00\x00\x00" + struct.pack(">Q", counter)


# ---------------------------------------------------------------------------
# Mock helpers for asyncio streams
# ---------------------------------------------------------------------------


class MockStreamWriter:
    """Minimal mock of asyncio.StreamWriter for testing send_raw."""

    def __init__(self) -> None:
        self.written = bytearray()
        self.drain_called = False

    def write(self, data: bytes) -> None:
        self.written.extend(data)

    async def drain(self) -> None:
        self.drain_called = True


class MockStreamReader:
    """Minimal mock of asyncio.StreamReader for testing recv_raw."""

    def __init__(self, data: bytes) -> None:
        self._data = bytearray(data)
        self._pos = 0

    async def readexactly(self, n: int) -> bytes:
        if self._pos + n > len(self._data):
            raise asyncio.IncompleteReadError(
                partial=bytes(self._data[self._pos :]),
                expected=n,
            )
        result = bytes(self._data[self._pos : self._pos + n])
        self._pos += n
        return result


# ---------------------------------------------------------------------------
# send_raw / recv_raw tests
# ---------------------------------------------------------------------------


class TestSendRaw:
    """send_raw writes [4-byte BE length][data] and drains."""

    @pytest.mark.asyncio
    async def test_send_raw_writes_length_prefix_and_data(self) -> None:
        writer = MockStreamWriter()
        data = b"hello"
        await send_raw(writer, data)  # type: ignore[arg-type]

        expected = struct.pack(">I", len(data)) + data
        assert bytes(writer.written) == expected
        assert writer.drain_called

    @pytest.mark.asyncio
    async def test_send_raw_empty_payload(self) -> None:
        writer = MockStreamWriter()
        await send_raw(writer, b"")  # type: ignore[arg-type]

        expected = struct.pack(">I", 0)
        assert bytes(writer.written) == expected


class TestRecvRaw:
    """recv_raw reads [4-byte BE length] then readexactly(length)."""

    @pytest.mark.asyncio
    async def test_recv_raw_reads_frame(self) -> None:
        payload = b"hello world"
        frame = struct.pack(">I", len(payload)) + payload
        reader = MockStreamReader(frame)
        result = await recv_raw(reader)  # type: ignore[arg-type]
        assert result == payload

    @pytest.mark.asyncio
    async def test_recv_raw_rejects_oversized_frame(self) -> None:
        # Length field claims MAX_FRAME_SIZE + 1
        frame = struct.pack(">I", MAX_FRAME_SIZE + 1)
        reader = MockStreamReader(frame)
        with pytest.raises(ProtocolError, match="exceeds maximum"):
            await recv_raw(reader)  # type: ignore[arg-type]

    @pytest.mark.asyncio
    async def test_recv_raw_raises_connection_error_on_incomplete_read(
        self,
    ) -> None:
        # Only 2 bytes available, header needs 4
        reader = MockStreamReader(b"\x00\x00")
        with pytest.raises(ChromatinConnectionError):
            await recv_raw(reader)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# send_encrypted / recv_encrypted tests
# ---------------------------------------------------------------------------


class TestSendEncrypted:
    """send_encrypted encrypts and sends, returning incremented counter."""

    @pytest.mark.asyncio
    async def test_send_encrypted_returns_incremented_counter(self) -> None:
        key = os.urandom(32)
        writer = MockStreamWriter()
        new_counter = await send_encrypted(
            writer, b"plaintext", key, 0  # type: ignore[arg-type]
        )
        assert new_counter == 1

    @pytest.mark.asyncio
    async def test_send_encrypted_writes_to_writer(self) -> None:
        key = os.urandom(32)
        writer = MockStreamWriter()
        await send_encrypted(writer, b"test", key, 0)  # type: ignore[arg-type]
        # Writer should have data: [4-byte len][ciphertext]
        assert len(writer.written) > 4  # header + at least tag


class TestRecvEncrypted:
    """recv_encrypted reads, decrypts, and returns (plaintext, counter+1)."""

    @pytest.mark.asyncio
    async def test_recv_encrypted_decrypts_and_increments_counter(
        self,
    ) -> None:
        from chromatindb.crypto import aead_encrypt

        key = os.urandom(32)
        plaintext = b"secret data"
        nonce = make_nonce(5)
        ciphertext = aead_encrypt(plaintext, b"", nonce, key)

        # Build raw frame: [4-byte BE len][ciphertext]
        frame = struct.pack(">I", len(ciphertext)) + ciphertext
        reader = MockStreamReader(frame)

        result, new_counter = await recv_encrypted(
            reader, key, 5  # type: ignore[arg-type]
        )
        assert result == plaintext
        assert new_counter == 6

    @pytest.mark.asyncio
    async def test_recv_encrypted_raises_on_corrupt_data(self) -> None:
        key = os.urandom(32)
        # Send garbage that won't decrypt
        garbage = os.urandom(50)
        frame = struct.pack(">I", len(garbage)) + garbage
        reader = MockStreamReader(frame)

        with pytest.raises(DecryptionError):
            await recv_encrypted(reader, key, 0)  # type: ignore[arg-type]

    @pytest.mark.asyncio
    async def test_recv_encrypted_raises_on_wrong_key(self) -> None:
        from chromatindb.crypto import aead_encrypt

        key1 = os.urandom(32)
        key2 = os.urandom(32)
        plaintext = b"data"
        nonce = make_nonce(0)
        ciphertext = aead_encrypt(plaintext, b"", nonce, key1)

        frame = struct.pack(">I", len(ciphertext)) + ciphertext
        reader = MockStreamReader(frame)

        with pytest.raises(DecryptionError):
            await recv_encrypted(reader, key2, 0)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# Roundtrip test
# ---------------------------------------------------------------------------


class TestEncryptedRoundtrip:
    """send_encrypted -> recv_encrypted recovers plaintext."""

    @pytest.mark.asyncio
    async def test_roundtrip_with_matching_counters(self) -> None:
        from chromatindb.crypto import aead_encrypt

        key = os.urandom(32)
        plaintext = b"roundtrip payload"

        # Encrypt with counter=0
        nonce = make_nonce(0)
        ciphertext = aead_encrypt(plaintext, b"", nonce, key)

        # Build the frame as send_encrypted would
        frame = struct.pack(">I", len(ciphertext)) + ciphertext
        reader = MockStreamReader(frame)

        # Decrypt with counter=0
        result, counter = await recv_encrypted(
            reader, key, 0  # type: ignore[arg-type]
        )
        assert result == plaintext
        assert counter == 1

    @pytest.mark.asyncio
    async def test_full_send_recv_roundtrip(self) -> None:
        """Full roundtrip: send_encrypted -> capture bytes -> recv_encrypted."""
        key = os.urandom(32)
        plaintext = b"full roundtrip test data"

        # Send
        writer = MockStreamWriter()
        send_counter = await send_encrypted(
            writer, plaintext, key, 0  # type: ignore[arg-type]
        )
        assert send_counter == 1

        # Receive from what was written
        reader = MockStreamReader(bytes(writer.written))
        result, recv_counter = await recv_encrypted(
            reader, key, 0  # type: ignore[arg-type]
        )
        assert result == plaintext
        assert recv_counter == 1
