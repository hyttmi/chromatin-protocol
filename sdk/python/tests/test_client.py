"""Unit tests for Transport background reader and ChromatinClient lifecycle.

Tests cover:
- Transport._reader_loop request_id dispatch
- Transport._reader_loop notification routing
- Transport._reader_loop Ping/Pong auto-response
- Transport._reader_loop Goodbye handling
- Transport._reader_loop disconnect error propagation
- Transport.send_request auto-incrementing request_id
- Transport.send_request raises when closed
- ChromatinClient.connect() async context manager
- ChromatinClient __aexit__ sends Goodbye
- ChromatinClient.ping()
"""

from __future__ import annotations

import asyncio
import struct
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

from chromatindb._framing import make_nonce, send_encrypted, send_raw
from chromatindb._transport import Transport
from chromatindb.client import ChromatinClient
from chromatindb.crypto import aead_decrypt, aead_encrypt
from chromatindb.exceptions import ConnectionError as ChromatinConnectionError
from chromatindb.identity import Identity
from chromatindb.wire import (
    TransportMsgType,
    decode_transport_message,
    encode_transport_message,
)


# ---------------------------------------------------------------------------
# Helpers: in-memory async stream pair
# ---------------------------------------------------------------------------


def make_stream_pair() -> tuple[asyncio.StreamReader, asyncio.StreamWriter]:
    """Create a reader/writer pair backed by in-memory transport.

    Returns reader that can be fed data, and a writer that captures output.
    """
    reader = asyncio.StreamReader()
    # Build a minimal mock writer
    writer_transport = MagicMock()
    writer_transport.is_closing = MagicMock(return_value=False)
    writer_protocol = MagicMock()
    writer = asyncio.StreamWriter(
        writer_transport, writer_protocol, reader, asyncio.get_event_loop()
    )
    return reader, writer


class CaptureWriter:
    """Writer that captures all written bytes."""

    def __init__(self) -> None:
        self.data = bytearray()
        self._closed = False

    def write(self, data: bytes) -> None:
        self.data.extend(data)

    async def drain(self) -> None:
        pass

    def close(self) -> None:
        self._closed = True

    async def wait_closed(self) -> None:
        pass

    def is_closing(self) -> bool:
        return self._closed


def feed_encrypted_frame(
    reader: asyncio.StreamReader,
    msg_type: int,
    payload: bytes,
    key: bytes,
    counter: int,
    request_id: int = 0,
) -> int:
    """Feed an encrypted transport message frame into a reader.

    Returns the next counter value.
    """
    msg = encode_transport_message(msg_type, payload, request_id)
    nonce = make_nonce(counter)
    ciphertext = aead_encrypt(msg, b"", nonce, key)
    # Length-prefix it
    header = struct.pack(">I", len(ciphertext))
    reader.feed_data(header + ciphertext)
    return counter + 1


# ---------------------------------------------------------------------------
# Transport._reader_loop tests
# ---------------------------------------------------------------------------


class TestTransportReaderLoop:
    async def test_dispatches_response_by_request_id(self) -> None:
        """Reader loop dispatches response by request_id to pending future."""
        import os

        send_key = os.urandom(32)
        recv_key = os.urandom(32)

        reader = asyncio.StreamReader()
        writer = CaptureWriter()

        transport = Transport(reader, writer, send_key, recv_key, 1, 1)

        # Register a pending future for request_id=42
        loop = asyncio.get_event_loop()
        fut: asyncio.Future = loop.create_future()
        transport._pending[42] = fut

        # Feed a response frame with request_id=42
        feed_encrypted_frame(
            reader,
            msg_type=10,  # Some response type
            payload=b"response-data",
            key=recv_key,
            counter=1,
            request_id=42,
        )

        # Start reader, let it process one frame, then close
        transport.start()
        result = await asyncio.wait_for(fut, timeout=2.0)

        assert result == (10, b"response-data")

        # Cleanup
        reader.feed_eof()
        await transport.stop()

    async def test_routes_notifications_to_queue(self) -> None:
        """Reader loop routes notifications (request_id=0) to notification queue."""
        import os

        send_key = os.urandom(32)
        recv_key = os.urandom(32)

        reader = asyncio.StreamReader()
        writer = CaptureWriter()

        transport = Transport(reader, writer, send_key, recv_key, 1, 1)

        # Feed a notification (request_id=0, non-Ping/Pong/Goodbye type)
        feed_encrypted_frame(
            reader,
            msg_type=20,  # Some notification type
            payload=b"notification-data",
            key=recv_key,
            counter=1,
            request_id=0,
        )

        transport.start()
        # Wait for notification to appear in queue
        notification = await asyncio.wait_for(
            transport.notifications.get(), timeout=2.0
        )

        assert notification == (20, b"notification-data", 0)

        reader.feed_eof()
        await transport.stop()

    async def test_auto_responds_ping_with_pong(self) -> None:
        """Reader loop auto-responds to Ping with Pong."""
        import os

        send_key = os.urandom(32)
        recv_key = os.urandom(32)

        reader = asyncio.StreamReader()
        writer = CaptureWriter()

        transport = Transport(reader, writer, send_key, recv_key, 1, 1)

        # Feed a Ping
        feed_encrypted_frame(
            reader,
            msg_type=TransportMsgType.Ping,
            payload=b"",
            key=recv_key,
            counter=1,
        )

        transport.start()
        # Give reader loop time to process and send pong
        await asyncio.sleep(0.05)

        # Verify a Pong was sent (encrypted frame in writer.data)
        assert len(writer.data) > 0

        # Decrypt the response: extract length-prefixed frame
        frame_len = struct.unpack(">I", bytes(writer.data[:4]))[0]
        ciphertext = bytes(writer.data[4 : 4 + frame_len])
        nonce = make_nonce(1)  # send_counter starts at 1
        plaintext = aead_decrypt(ciphertext, b"", nonce, send_key)
        assert plaintext is not None
        msg_type, _, _ = decode_transport_message(plaintext)
        assert msg_type == TransportMsgType.Pong

        reader.feed_eof()
        await transport.stop()

    async def test_goodbye_sets_closed_and_cancels_pending(self) -> None:
        """Reader loop sets closed on Goodbye and cancels pending futures."""
        import os

        send_key = os.urandom(32)
        recv_key = os.urandom(32)

        reader = asyncio.StreamReader()
        writer = CaptureWriter()

        transport = Transport(reader, writer, send_key, recv_key, 1, 1)

        # Register a pending future
        loop = asyncio.get_event_loop()
        fut: asyncio.Future = loop.create_future()
        transport._pending[1] = fut

        # Feed Goodbye
        feed_encrypted_frame(
            reader,
            msg_type=TransportMsgType.Goodbye,
            payload=b"",
            key=recv_key,
            counter=1,
        )

        transport.start()
        await asyncio.sleep(0.05)

        assert transport.closed is True
        assert fut.done()
        with pytest.raises(ChromatinConnectionError):
            fut.result()

    async def test_disconnect_cancels_pending_with_error(self) -> None:
        """Reader loop cancels all pending futures with ConnectionError on disconnect."""
        import os

        send_key = os.urandom(32)
        recv_key = os.urandom(32)

        reader = asyncio.StreamReader()
        writer = CaptureWriter()

        transport = Transport(reader, writer, send_key, recv_key, 1, 1)

        loop = asyncio.get_event_loop()
        fut: asyncio.Future = loop.create_future()
        transport._pending[1] = fut

        # Feed EOF (connection drop)
        transport.start()
        reader.feed_eof()
        await asyncio.sleep(0.05)

        assert transport.closed is True
        assert fut.done()
        with pytest.raises(ChromatinConnectionError):
            fut.result()


# ---------------------------------------------------------------------------
# Transport.send_request tests
# ---------------------------------------------------------------------------


class TestTransportSendRequest:
    async def test_auto_incrementing_request_id(self) -> None:
        """send_request assigns auto-incrementing request_id starting at 1."""
        import os

        send_key = os.urandom(32)
        recv_key = os.urandom(32)

        reader = asyncio.StreamReader()
        writer = CaptureWriter()

        transport = Transport(reader, writer, send_key, recv_key, 1, 1)

        # Don't start reader -- we'll just test send side
        # Manually resolve futures to prevent hanging

        async def send_and_check(expected_id: int) -> None:
            # Use a generic message type (not Ping, which now uses send_ping)
            task = asyncio.create_task(
                transport.send_request(10, b"test")
            )
            await asyncio.sleep(0.01)
            # Check that the pending dict has the expected request_id
            assert expected_id in transport._pending
            # Resolve the future so the task can complete
            transport._pending[expected_id].set_result((10, b"ok"))
            await task

        await send_and_check(1)
        await send_and_check(2)
        await send_and_check(3)

    async def test_raises_when_closed(self) -> None:
        """send_request raises ConnectionError if transport is closed."""
        import os

        send_key = os.urandom(32)
        recv_key = os.urandom(32)

        reader = asyncio.StreamReader()
        writer = CaptureWriter()

        transport = Transport(reader, writer, send_key, recv_key, 1, 1)
        transport._closed = True

        with pytest.raises(ChromatinConnectionError, match="connection is closed"):
            await transport.send_request(TransportMsgType.Ping, b"")


# ---------------------------------------------------------------------------
# ChromatinClient lifecycle tests
# ---------------------------------------------------------------------------


class TestChromatinClient:
    async def test_connect_returns_context_manager(self) -> None:
        """ChromatinClient.connect() returns an async context manager."""
        identity = Identity.generate()
        client = ChromatinClient.connect("127.0.0.1", 9999, identity)
        # Should have __aenter__ and __aexit__
        assert hasattr(client, "__aenter__")
        assert hasattr(client, "__aexit__")

    async def test_aexit_sends_goodbye(self) -> None:
        """ChromatinClient __aexit__ sends Goodbye, cancels reader, closes writer."""
        import os

        # Create a transport directly and inject it
        send_key = os.urandom(32)
        recv_key = os.urandom(32)

        reader = asyncio.StreamReader()
        writer = CaptureWriter()

        transport = Transport(reader, writer, send_key, recv_key, 1, 1)
        # Don't start reader for this test -- stop() handles it

        client = ChromatinClient(transport)

        await client.__aexit__(None, None, None)

        # Verify Goodbye was sent (encrypted frame in writer.data)
        assert len(writer.data) > 0
        frame_len = struct.unpack(">I", bytes(writer.data[:4]))[0]
        ciphertext = bytes(writer.data[4 : 4 + frame_len])
        nonce = make_nonce(1)  # send_counter=1
        plaintext = aead_decrypt(ciphertext, b"", nonce, send_key)
        assert plaintext is not None
        msg_type, _, _ = decode_transport_message(plaintext)
        assert msg_type == TransportMsgType.Goodbye

        assert transport.closed is True

    async def test_ping_sends_ping_receives_pong(self) -> None:
        """ChromatinClient.ping() sends Ping and receives Pong."""
        import os

        send_key = os.urandom(32)
        recv_key = os.urandom(32)

        reader = asyncio.StreamReader()
        writer = CaptureWriter()

        transport = Transport(reader, writer, send_key, recv_key, 1, 1)

        # Reader loop must be running to dispatch Pong
        transport.start()

        client = ChromatinClient(transport)

        # Create ping task
        ping_task = asyncio.create_task(client.ping())

        # Wait for the ping to be sent
        await asyncio.sleep(0.02)

        # C++ relay sends Pong with request_id=0 (doesn't echo client's id)
        feed_encrypted_frame(
            reader,
            msg_type=TransportMsgType.Pong,
            payload=b"",
            key=recv_key,
            counter=1,
            request_id=0,
        )

        # ping should complete
        await asyncio.wait_for(ping_task, timeout=2.0)

        reader.feed_eof()
        await transport.stop()
