"""Background reader and request dispatch for chromatindb SDK.

Manages the post-handshake connection state: background frame reader,
request-response correlation by request_id, and notification routing.

Internal module -- not part of public API (per D-03).
"""

from __future__ import annotations

import asyncio
from collections import deque

from chromatindb._framing import recv_encrypted, send_encrypted
from chromatindb.exceptions import ConnectionError as ChromatinConnectionError
from chromatindb.wire import (
    TransportMsgType,
    decode_transport_message,
    encode_transport_message,
)


class Transport:
    """Post-handshake transport: reader coroutine + request dispatch.

    Args:
        reader: asyncio StreamReader (TCP).
        writer: asyncio StreamWriter (TCP).
        send_key: 32-byte AEAD key for outgoing frames.
        recv_key: 32-byte AEAD key for incoming frames.
        send_counter: Initial send nonce counter (1 after handshake).
        recv_counter: Initial recv nonce counter (1 after handshake).
    """

    def __init__(
        self,
        reader: asyncio.StreamReader,
        writer: object,
        send_key: bytes,
        recv_key: bytes,
        send_counter: int,
        recv_counter: int,
    ) -> None:
        self._reader = reader
        self._writer = writer
        self._send_key = send_key
        self._recv_key = recv_key
        self._send_counter = send_counter
        self._recv_counter = recv_counter
        self._closed = False
        self._pending: dict[int, asyncio.Future] = {}
        self._notifications: asyncio.Queue = asyncio.Queue(maxsize=1000)
        self._next_request_id = 1  # per D-09: auto-assigned, starts at 1
        self._pending_pings: deque[asyncio.Future] = deque()
        self._reader_task: asyncio.Task | None = None
        self._send_lock = asyncio.Lock()  # serialize send_encrypted calls

    def start(self) -> None:
        """Spawn background reader task."""
        self._reader_task = asyncio.get_event_loop().create_task(
            self._reader_loop()
        )

    async def _reader_loop(self) -> None:
        """Continuously read and decrypt frames, dispatch by request_id."""
        try:
            while not self._closed:
                plaintext, self._recv_counter = await recv_encrypted(
                    self._reader,
                    self._recv_key,
                    self._recv_counter,
                )
                msg_type, payload, request_id = decode_transport_message(
                    plaintext
                )

                if msg_type == TransportMsgType.Ping:
                    await self._send_pong()
                elif msg_type == TransportMsgType.Pong:
                    # C++ relay sends Pong with request_id=0 (doesn't echo
                    # client request_id), so resolve oldest pending ping.
                    if self._pending_pings:
                        self._pending_pings.popleft().set_result(None)
                elif msg_type == TransportMsgType.Goodbye:
                    self._closed = True
                    break
                elif request_id != 0 and request_id in self._pending:
                    self._pending.pop(request_id).set_result(
                        (msg_type, payload)
                    )
                else:
                    # Notifications (request_id=0) or unmatched responses
                    try:
                        self._notifications.put_nowait(
                            (msg_type, payload, request_id)
                        )
                    except asyncio.QueueFull:
                        pass  # Drop new notification if queue full
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            self._close_with_error(exc)
            return
        # Clean shutdown (Goodbye received)
        self._cancel_all_pending(
            ChromatinConnectionError("connection closed by peer")
        )

    async def _send_pong(self) -> None:
        """Send Pong response to server Ping."""
        pong_msg = encode_transport_message(TransportMsgType.Pong, b"")
        async with self._send_lock:
            self._send_counter = await send_encrypted(
                self._writer,
                pong_msg,
                self._send_key,
                self._send_counter,
            )

    def _close_with_error(self, exc: Exception) -> None:
        """Set closed state and cancel all pending futures."""
        self._closed = True
        err = ChromatinConnectionError(f"connection lost: {exc}")
        self._cancel_all_pending(err)

    def _cancel_all_pending(self, exc: Exception) -> None:
        """Cancel all pending request futures with the given exception."""
        for fut in self._pending.values():
            if not fut.done():
                fut.set_exception(exc)
        self._pending.clear()
        for fut in self._pending_pings:
            if not fut.done():
                fut.set_exception(exc)
        self._pending_pings.clear()

    async def send_request(
        self,
        msg_type: int,
        payload: bytes,
    ) -> tuple[int, bytes]:
        """Send request and wait for response. Returns (msg_type, payload).

        Auto-assigns request_id per D-09. Raises ConnectionError if closed.
        """
        if self._closed:
            raise ChromatinConnectionError("connection is closed")

        request_id = self._next_request_id
        self._next_request_id += 1

        loop = asyncio.get_event_loop()
        fut: asyncio.Future = loop.create_future()
        self._pending[request_id] = fut

        msg = encode_transport_message(msg_type, payload, request_id)
        async with self._send_lock:
            self._send_counter = await send_encrypted(
                self._writer,
                msg,
                self._send_key,
                self._send_counter,
            )

        return await fut

    async def send_ping(self) -> None:
        """Send Ping and wait for Pong.

        Uses request_id=0 because the C++ relay replies with Pong
        without echoing request_id. Pending pings are tracked in a
        FIFO queue resolved by the reader loop.
        """
        if self._closed:
            raise ChromatinConnectionError("connection is closed")

        fut: asyncio.Future = asyncio.get_event_loop().create_future()
        self._pending_pings.append(fut)

        msg = encode_transport_message(TransportMsgType.Ping, b"")
        async with self._send_lock:
            self._send_counter = await send_encrypted(
                self._writer,
                msg,
                self._send_key,
                self._send_counter,
            )

        await fut

    async def send_goodbye(self) -> None:
        """Send Goodbye message. Does not wait for response."""
        if self._closed:
            return
        goodbye_msg = encode_transport_message(TransportMsgType.Goodbye, b"")
        try:
            async with self._send_lock:
                self._send_counter = await send_encrypted(
                    self._writer,
                    goodbye_msg,
                    self._send_key,
                    self._send_counter,
                )
        except Exception:
            pass  # Best-effort on disconnect

    async def stop(self) -> None:
        """Cancel reader task and close writer."""
        self._closed = True
        if self._reader_task is not None:
            self._reader_task.cancel()
            try:
                await self._reader_task
            except asyncio.CancelledError:
                pass
        self._cancel_all_pending(
            ChromatinConnectionError("transport stopped")
        )
        try:
            self._writer.close()
            await self._writer.wait_closed()
        except Exception:
            pass

    @property
    def closed(self) -> bool:
        """Whether the transport is closed."""
        return self._closed

    @property
    def notifications(self) -> asyncio.Queue:
        """Queue of (msg_type, payload, request_id) notification tuples."""
        return self._notifications
