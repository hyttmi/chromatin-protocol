"""Public ChromatinClient API for chromatindb SDK.

Usage per D-01:
    async with ChromatinClient.connect(host, port, identity) as conn:
        await conn.ping()

All transport internals are private per D-03.
Only connect, disconnect, ping exposed for Phase 71 per D-04.
"""

from __future__ import annotations

import asyncio
from types import TracebackType

from chromatindb._handshake import perform_handshake
from chromatindb._transport import Transport
from chromatindb.exceptions import HandshakeError
from chromatindb.identity import Identity
from chromatindb.wire import TransportMsgType


class ChromatinClient:
    """Async client for chromatindb relay.

    Create via ChromatinClient.connect() context manager.
    """

    def __init__(self, transport: Transport) -> None:
        self._transport = transport

    @classmethod
    def connect(
        cls,
        host: str,
        port: int,
        identity: Identity,
        *,
        timeout: float = 10.0,
    ) -> ChromatinClient:
        """Create a connection context manager.

        Args:
            host: Relay hostname or IP.
            port: Relay port.
            identity: Client ML-DSA-87 identity.
            timeout: Handshake timeout in seconds (per D-07, default 10s).

        Returns:
            Async context manager that yields ChromatinClient.

        Usage:
            async with ChromatinClient.connect("192.168.1.200", 4433, identity) as conn:
                await conn.ping()
        """
        client = cls.__new__(cls)
        client._host = host
        client._port = port
        client._identity = identity
        client._timeout = timeout
        client._transport = None  # type: ignore[assignment]
        return client

    async def __aenter__(self) -> ChromatinClient:
        reader, writer = await asyncio.open_connection(self._host, self._port)
        try:
            result = await asyncio.wait_for(
                perform_handshake(reader, writer, self._identity),
                timeout=self._timeout,
            )
            send_key, recv_key, send_counter, recv_counter, _ = result
        except asyncio.TimeoutError:
            writer.close()
            await writer.wait_closed()
            raise HandshakeError(
                f"handshake timed out after {self._timeout}s"
            ) from None
        except Exception:
            writer.close()
            await writer.wait_closed()
            raise

        self._transport = Transport(
            reader,
            writer,
            send_key,
            recv_key,
            send_counter,
            recv_counter,
        )
        self._transport.start()
        return self

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None:
        if self._transport is not None:
            await self._transport.send_goodbye()
            await self._transport.stop()

    async def ping(self) -> None:
        """Send Ping and wait for Pong. Raises ConnectionError if disconnected."""
        await self._transport.send_request(TransportMsgType.Ping, b"")
