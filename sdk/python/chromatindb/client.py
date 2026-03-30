"""Public ChromatinClient API for chromatindb SDK.

Usage per D-01:
    async with ChromatinClient.connect(host, port, identity) as conn:
        result = await conn.write_blob(data=b"hello", ttl=3600)

All transport internals are private per D-03.
"""

from __future__ import annotations

import asyncio
import time
from types import TracebackType

from chromatindb._codec import (
    decode_delete_ack,
    decode_exists_response,
    decode_list_response,
    decode_read_response,
    decode_write_ack,
    encode_blob_payload,
    encode_exists_request,
    encode_list_request,
    encode_read_request,
    make_tombstone_data,
)
from chromatindb._handshake import perform_handshake
from chromatindb._transport import Transport
from chromatindb.crypto import build_signing_input
from chromatindb.exceptions import (
    ConnectionError as ChromatinConnectionError,
    HandshakeError,
    ProtocolError,
)
from chromatindb.identity import Identity
from chromatindb.types import (
    BlobRef,
    DeleteResult,
    ListPage,
    ReadResult,
    WriteResult,
)
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
            async with ChromatinClient.connect("192.168.1.200", 4201, identity) as conn:
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
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self._host, self._port),
                timeout=self._timeout,
            )
        except asyncio.TimeoutError:
            raise HandshakeError(
                f"connection timed out after {self._timeout}s"
            ) from None

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
        await self._transport.send_ping()

    # ------------------------------------------------------------------
    # Private helper
    # ------------------------------------------------------------------

    async def _request_with_timeout(
        self, msg_type: int, payload: bytes
    ) -> tuple[int, bytes]:
        """Send request with per-request timeout (per D-16).

        Raises:
            ConnectionError: If request times out.
        """
        try:
            return await asyncio.wait_for(
                self._transport.send_request(msg_type, payload),
                timeout=self._timeout,
            )
        except asyncio.TimeoutError:
            raise ChromatinConnectionError(
                f"request timed out after {self._timeout}s"
            ) from None

    # ------------------------------------------------------------------
    # Data operations
    # ------------------------------------------------------------------

    async def write_blob(self, data: bytes, ttl: int) -> WriteResult:
        """Write a signed blob to the connected node.

        Args:
            data: Blob payload bytes.
            ttl: Time-to-live in seconds (required, per D-02).

        Returns:
            WriteResult with server-assigned blob_hash and seq_num.

        Raises:
            ProtocolError: If server rejects the write (StorageFull, QuotaExceeded,
                or unexpected response type).
            ConnectionError: If request times out (per D-16).
        """
        timestamp = int(time.time())
        namespace = self._identity.namespace
        pubkey = self._identity.public_key

        signing_digest = build_signing_input(namespace, data, ttl, timestamp)
        signature = self._identity.sign(signing_digest)

        payload = encode_blob_payload(
            namespace, pubkey, data, ttl, timestamp, signature
        )

        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.Data, payload
        )

        if resp_type == TransportMsgType.StorageFull:
            raise ProtocolError("node storage is full")
        if resp_type == TransportMsgType.QuotaExceeded:
            raise ProtocolError("namespace quota exceeded")
        if resp_type != TransportMsgType.WriteAck:
            raise ProtocolError(f"expected WriteAck (30), got type {resp_type}")

        blob_hash, seq_num, duplicate = decode_write_ack(resp_payload)
        return WriteResult(blob_hash=blob_hash, seq_num=seq_num, duplicate=duplicate)

    async def read_blob(
        self, namespace: bytes, blob_hash: bytes
    ) -> ReadResult | None:
        """Read a blob by namespace and hash.

        Args:
            namespace: 32-byte namespace identifier.
            blob_hash: 32-byte blob hash.

        Returns:
            ReadResult with blob data, or None if not found (per D-14).

        Raises:
            ValueError: If namespace or blob_hash is not 32 bytes.
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out (per D-16).
        """
        payload = encode_read_request(namespace, blob_hash)

        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.ReadRequest, payload
        )

        if resp_type != TransportMsgType.ReadResponse:
            raise ProtocolError(
                f"expected ReadResponse (32), got type {resp_type}"
            )

        result = decode_read_response(resp_payload)
        if result is None:
            return None
        data, ttl, timestamp, signature = result
        return ReadResult(data=data, ttl=ttl, timestamp=timestamp, signature=signature)

    async def delete_blob(self, blob_hash: bytes) -> DeleteResult:
        """Delete a blob by writing a tombstone.

        Args:
            blob_hash: 32-byte hash of the blob to delete.

        Returns:
            DeleteResult with tombstone hash and seq_num.

        Raises:
            ValueError: If blob_hash is not 32 bytes.
            ProtocolError: If server rejects or unexpected response type.
            ConnectionError: If request times out (per D-16).
        """
        if len(blob_hash) != 32:
            raise ValueError(
                f"blob_hash must be 32 bytes, got {len(blob_hash)}"
            )

        tombstone_data = make_tombstone_data(blob_hash)
        timestamp = int(time.time())
        namespace = self._identity.namespace
        pubkey = self._identity.public_key

        signing_digest = build_signing_input(namespace, tombstone_data, 0, timestamp)
        signature = self._identity.sign(signing_digest)

        payload = encode_blob_payload(
            namespace, pubkey, tombstone_data, 0, timestamp, signature
        )

        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.Delete, payload
        )

        if resp_type == TransportMsgType.StorageFull:
            raise ProtocolError("node storage is full")
        if resp_type == TransportMsgType.QuotaExceeded:
            raise ProtocolError("namespace quota exceeded")
        if resp_type != TransportMsgType.DeleteAck:
            raise ProtocolError(
                f"expected DeleteAck (18), got type {resp_type}"
            )

        tombstone_hash, seq_num, duplicate = decode_delete_ack(resp_payload)
        return DeleteResult(
            tombstone_hash=tombstone_hash, seq_num=seq_num, duplicate=duplicate
        )

    async def list_blobs(
        self,
        namespace: bytes,
        *,
        after: int = 0,
        limit: int = 100,
    ) -> ListPage:
        """List blobs in a namespace with cursor-based pagination.

        Args:
            namespace: 32-byte namespace identifier.
            after: Cursor from previous ListPage (seq_num, exclusive). Default 0 = start.
            limit: Maximum blobs per page (capped at 100 by node).

        Returns:
            ListPage with BlobRef entries and cursor for next page.

        Raises:
            ValueError: If namespace is not 32 bytes.
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out (per D-16).
        """
        payload = encode_list_request(namespace, after, limit)

        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.ListRequest, payload
        )

        if resp_type != TransportMsgType.ListResponse:
            raise ProtocolError(
                f"expected ListResponse (34), got type {resp_type}"
            )

        entries, has_more = decode_list_response(resp_payload)
        blobs = [BlobRef(blob_hash=h, seq_num=s) for h, s in entries]
        cursor = blobs[-1].seq_num if has_more and blobs else None
        return ListPage(blobs=blobs, cursor=cursor)

    async def exists(self, namespace: bytes, blob_hash: bytes) -> bool:
        """Check if a blob exists without data transfer.

        Args:
            namespace: 32-byte namespace identifier.
            blob_hash: 32-byte blob hash.

        Returns:
            True if blob exists, False otherwise (per D-14).

        Raises:
            ValueError: If namespace or blob_hash is not 32 bytes.
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out (per D-16).
        """
        payload = encode_exists_request(namespace, blob_hash)

        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.ExistsRequest, payload
        )

        if resp_type != TransportMsgType.ExistsResponse:
            raise ProtocolError(
                f"expected ExistsResponse (38), got type {resp_type}"
            )

        exists_flag, _ = decode_exists_response(resp_payload)
        return exists_flag
