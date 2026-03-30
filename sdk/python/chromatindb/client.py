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
    decode_batch_exists_response,
    decode_batch_read_response,
    decode_delegation_list_response,
    decode_delete_ack,
    decode_exists_response,
    decode_list_response,
    decode_metadata_response,
    decode_namespace_list_response,
    decode_namespace_stats_response,
    decode_node_info_response,
    decode_notification,
    decode_peer_info_response,
    decode_read_response,
    decode_storage_status_response,
    decode_time_range_response,
    decode_write_ack,
    encode_batch_exists_request,
    encode_batch_read_request,
    encode_blob_payload,
    encode_delegation_list_request,
    encode_exists_request,
    encode_list_request,
    encode_metadata_request,
    encode_namespace_list_request,
    encode_namespace_stats_request,
    encode_read_request,
    encode_subscribe,
    encode_time_range_request,
    encode_unsubscribe,
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
    BatchReadResult,
    BlobRef,
    DelegationList,
    DeleteResult,
    ListPage,
    MetadataResult,
    NamespaceListResult,
    NamespaceStats,
    NodeInfo,
    Notification,
    PeerInfo,
    ReadResult,
    StorageStatus,
    TimeRangeResult,
    WriteResult,
)
from chromatindb.wire import TransportMsgType

class ChromatinClient:
    """Async client for chromatindb relay.

    Create via ChromatinClient.connect() context manager.
    """

    def __init__(self, transport: Transport) -> None:
        self._transport = transport
        self._subscriptions: set[bytes] = set()

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
        client._subscriptions: set[bytes] = set()
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
            # D-06: auto-cleanup subscriptions on disconnect
            if self._subscriptions:
                try:
                    payload = encode_unsubscribe(list(self._subscriptions))
                    await self._transport.send_message(
                        TransportMsgType.Unsubscribe, payload
                    )
                except Exception:
                    pass  # Best-effort on disconnect
                self._subscriptions.clear()
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

    # ------------------------------------------------------------------
    # Query operations
    # ------------------------------------------------------------------

    async def metadata(
        self, namespace: bytes, blob_hash: bytes
    ) -> MetadataResult | None:
        """Query blob metadata without payload (QUERY-01).

        Args:
            namespace: 32-byte namespace identifier.
            blob_hash: 32-byte blob hash.

        Returns:
            MetadataResult with blob metadata, or None if not found.

        Raises:
            ValueError: If namespace or blob_hash is not 32 bytes.
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out.
        """
        payload = encode_metadata_request(namespace, blob_hash)
        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.MetadataRequest, payload
        )
        if resp_type != TransportMsgType.MetadataResponse:
            raise ProtocolError(
                f"expected MetadataResponse (48), got type {resp_type}"
            )
        return decode_metadata_response(resp_payload)

    async def batch_exists(
        self, namespace: bytes, hashes: list[bytes]
    ) -> dict[bytes, bool]:
        """Batch-check blob existence (QUERY-02, per D-07/D-08).

        Args:
            namespace: 32-byte namespace identifier.
            hashes: List of 32-byte blob hashes to check.

        Returns:
            Dict mapping each hash to existence boolean.

        Raises:
            ValueError: If namespace or any hash is not 32 bytes.
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out.
        """
        payload = encode_batch_exists_request(namespace, hashes)
        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.BatchExistsRequest, payload
        )
        if resp_type != TransportMsgType.BatchExistsResponse:
            raise ProtocolError(
                f"expected BatchExistsResponse (50), got type {resp_type}"
            )
        return decode_batch_exists_response(resp_payload, hashes)

    async def batch_read(
        self,
        namespace: bytes,
        hashes: list[bytes],
        *,
        cap_bytes: int = 0,
    ) -> BatchReadResult:
        """Batch-read multiple blobs (QUERY-03, per D-09).

        Args:
            namespace: 32-byte namespace identifier.
            hashes: List of 32-byte blob hashes to read.
            cap_bytes: Max response size in bytes. 0 = server default (4 MiB).

        Returns:
            BatchReadResult with blobs dict and truncation flag.

        Raises:
            ValueError: If namespace or any hash is not 32 bytes.
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out.
        """
        payload = encode_batch_read_request(namespace, hashes, cap_bytes)
        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.BatchReadRequest, payload
        )
        if resp_type != TransportMsgType.BatchReadResponse:
            raise ProtocolError(
                f"expected BatchReadResponse (54), got type {resp_type}"
            )
        return decode_batch_read_response(resp_payload)

    async def time_range(
        self,
        namespace: bytes,
        start_ts: int,
        end_ts: int,
        *,
        limit: int = 100,
    ) -> TimeRangeResult:
        """Query blobs by time range (QUERY-04).

        Args:
            namespace: 32-byte namespace identifier.
            start_ts: Start timestamp in seconds (inclusive).
            end_ts: End timestamp in seconds (inclusive).
            limit: Max results (default 100, server clamps to [1, 100]).

        Returns:
            TimeRangeResult with entries and truncation flag.

        Raises:
            ValueError: If namespace is not 32 bytes.
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out.
        """
        payload = encode_time_range_request(namespace, start_ts, end_ts, limit)
        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.TimeRangeRequest, payload
        )
        if resp_type != TransportMsgType.TimeRangeResponse:
            raise ProtocolError(
                f"expected TimeRangeResponse (58), got type {resp_type}"
            )
        return decode_time_range_response(resp_payload)

    async def namespace_list(
        self,
        *,
        after: bytes = b"\x00" * 32,
        limit: int = 100,
    ) -> NamespaceListResult:
        """List namespaces with pagination (QUERY-05).

        Args:
            after: 32-byte namespace cursor. All zeros for first page.
            limit: Max namespaces to return (default 100).

        Returns:
            NamespaceListResult with namespaces and cursor.

        Raises:
            ValueError: If after is not 32 bytes.
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out.
        """
        payload = encode_namespace_list_request(after, limit)
        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.NamespaceListRequest, payload
        )
        if resp_type != TransportMsgType.NamespaceListResponse:
            raise ProtocolError(
                f"expected NamespaceListResponse (42), got type {resp_type}"
            )
        return decode_namespace_list_response(resp_payload)

    async def namespace_stats(self, namespace: bytes) -> NamespaceStats:
        """Query per-namespace statistics (QUERY-06).

        Args:
            namespace: 32-byte namespace identifier.

        Returns:
            NamespaceStats with blob count, bytes, quotas.

        Raises:
            ValueError: If namespace is not 32 bytes.
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out.
        """
        payload = encode_namespace_stats_request(namespace)
        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.NamespaceStatsRequest, payload
        )
        if resp_type != TransportMsgType.NamespaceStatsResponse:
            raise ProtocolError(
                f"expected NamespaceStatsResponse (46), got type {resp_type}"
            )
        return decode_namespace_stats_response(resp_payload)

    async def storage_status(self) -> StorageStatus:
        """Query node storage status (QUERY-07).

        Returns:
            StorageStatus with disk usage, quotas, counts.

        Raises:
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out.
        """
        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.StorageStatusRequest, b""
        )
        if resp_type != TransportMsgType.StorageStatusResponse:
            raise ProtocolError(
                f"expected StorageStatusResponse (44), got type {resp_type}"
            )
        return decode_storage_status_response(resp_payload)

    async def node_info(self) -> NodeInfo:
        """Query node info and capabilities (QUERY-08).

        Returns:
            NodeInfo with version, capabilities, peer count, storage stats.

        Raises:
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out.
        """
        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.NodeInfoRequest, b""
        )
        if resp_type != TransportMsgType.NodeInfoResponse:
            raise ProtocolError(
                f"expected NodeInfoResponse (40), got type {resp_type}"
            )
        return decode_node_info_response(resp_payload)

    async def peer_info(self) -> PeerInfo:
        """Query peer info (QUERY-09, trust-gated).

        Returns:
            PeerInfo with peer details (full if trusted, summary if untrusted).

        Raises:
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out.
        """
        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.PeerInfoRequest, b""
        )
        if resp_type != TransportMsgType.PeerInfoResponse:
            raise ProtocolError(
                f"expected PeerInfoResponse (56), got type {resp_type}"
            )
        return decode_peer_info_response(resp_payload)

    async def delegation_list(self, namespace: bytes) -> DelegationList:
        """List delegations for a namespace (QUERY-10).

        Args:
            namespace: 32-byte namespace identifier.

        Returns:
            DelegationList with delegation entries.

        Raises:
            ValueError: If namespace is not 32 bytes.
            ProtocolError: If response type is unexpected.
            ConnectionError: If request times out.
        """
        payload = encode_delegation_list_request(namespace)
        resp_type, resp_payload = await self._request_with_timeout(
            TransportMsgType.DelegationListRequest, payload
        )
        if resp_type != TransportMsgType.DelegationListResponse:
            raise ProtocolError(
                f"expected DelegationListResponse (52), got type {resp_type}"
            )
        return decode_delegation_list_response(resp_payload)

    # ------------------------------------------------------------------
    # Pub/Sub operations
    # ------------------------------------------------------------------

    @property
    def subscriptions(self) -> frozenset[bytes]:
        """Currently subscribed namespaces (per D-05)."""
        return frozenset(self._subscriptions)

    async def subscribe(self, namespace: bytes) -> None:
        """Subscribe to namespace notifications (PUBSUB-01, per D-04/D-05).

        Fire-and-forget send -- the C++ node processes Subscribe inline
        without sending a response.

        Args:
            namespace: 32-byte namespace identifier.

        Raises:
            ValueError: If namespace is not 32 bytes.
            ConnectionError: If the transport is closed.
        """
        if len(namespace) != 32:
            raise ValueError(
                f"namespace must be 32 bytes, got {len(namespace)}"
            )
        payload = encode_subscribe([namespace])
        await self._transport.send_message(
            TransportMsgType.Subscribe, payload
        )
        self._subscriptions.add(namespace)

    async def unsubscribe(self, namespace: bytes) -> None:
        """Unsubscribe from namespace notifications (PUBSUB-02, per D-05).

        Fire-and-forget send.

        Args:
            namespace: 32-byte namespace identifier.

        Raises:
            ValueError: If namespace is not 32 bytes.
            ConnectionError: If the transport is closed.
        """
        if len(namespace) != 32:
            raise ValueError(
                f"namespace must be 32 bytes, got {len(namespace)}"
            )
        payload = encode_unsubscribe([namespace])
        await self._transport.send_message(
            TransportMsgType.Unsubscribe, payload
        )
        self._subscriptions.discard(namespace)

    async def notifications(self):
        """Async iterator yielding Notification objects (per D-01, D-02).

        Yields Notification for all subscribed namespaces in a single
        merged stream. Each notification carries its namespace.

        Usage:
            async for notif in conn.notifications():
                print(f"New blob in {notif.namespace.hex()}")

        Yields:
            Notification objects.
        """
        while not self._transport.closed:
            try:
                msg_type, payload, _ = await asyncio.wait_for(
                    self._transport.notifications.get(),
                    timeout=1.0,
                )
            except asyncio.TimeoutError:
                continue
            if msg_type == TransportMsgType.Notification:
                yield decode_notification(payload)
