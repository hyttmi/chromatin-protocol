"""Public ChromatinClient API for chromatindb SDK.

Usage:
    async with ChromatinClient.connect([("relay1", 4201)], identity) as conn:
        result = await conn.write_blob(data=b"hello", ttl=3600)

Multi-relay failover:
    async with ChromatinClient.connect(
        [("relay1", 4201), ("relay2", 4201)], identity
    ) as conn:
        await conn.ping()

All transport internals are private.
"""

from __future__ import annotations

import asyncio
import logging
import time
from types import TracebackType
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from chromatindb._directory import Directory

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
from chromatindb._envelope import envelope_decrypt, envelope_encrypt
from chromatindb._handshake import perform_handshake
from chromatindb._reconnect import (
    ConnectionState,
    OnDisconnect,
    OnReconnect,
    backoff_delay,
    invoke_callback,
)
from chromatindb._transport import Transport
from chromatindb.crypto import build_signing_input
from chromatindb.exceptions import (
    ConnectionError as ChromatinConnectionError,
    DirectoryError,
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

log = logging.getLogger(__name__)


class ChromatinClient:
    """Async client for chromatindb relay.

    Create via ChromatinClient.connect() context manager.
    """

    def __init__(self, transport: Transport) -> None:
        self._transport = transport
        self._subscriptions: set[bytes] = set()
        # Reconnect state (overridden by connect() classmethod)
        self._relays: list[tuple[str, int]] = []
        self._relay_index: int = 0
        self._identity: Identity | None = None
        self._timeout: float = 10.0
        self._auto_reconnect: bool = False
        self._on_disconnect: OnDisconnect | None = None
        self._on_reconnect: OnReconnect | None = None
        self._state = ConnectionState.CONNECTED
        self._reconnect_task: asyncio.Task | None = None
        self._connected_event = asyncio.Event()
        self._connected_event.set()  # Already connected
        self._monitor_task: asyncio.Task | None = None

    @classmethod
    def connect(
        cls,
        relays: list[tuple[str, int]],
        identity: Identity,
        *,
        timeout: float = 10.0,
        auto_reconnect: bool = True,
        on_disconnect: OnDisconnect | None = None,
        on_reconnect: OnReconnect | None = None,
    ) -> ChromatinClient:
        """Create a connection context manager.

        Args:
            relays: Ordered list of (host, port) relay addresses (SDK-01).
                First relay is preferred/primary. List order defines priority.
            identity: Client ML-DSA-87 identity.
            timeout: Per-relay handshake timeout in seconds (default 10s).
            auto_reconnect: If True, transparently reconnect on connection loss.
            on_disconnect: Callback invoked when connection is lost.
            on_reconnect: Callback invoked after successful reconnect,
                receives (cycle_count, downtime_seconds, relay_host, relay_port).

        Returns:
            Async context manager that yields ChromatinClient.

        Usage:
            async with ChromatinClient.connect(
                [("relay1.example.com", 4201), ("relay2.example.com", 4201)],
                identity,
            ) as conn:
                await conn.ping()
        """
        if not relays:
            raise ValueError(
                "relays must be a non-empty list of (host, port) tuples"
            )
        client = cls.__new__(cls)
        client._relays = list(relays)
        client._relay_index = 0
        client._identity = identity
        client._timeout = timeout
        client._transport = None  # type: ignore[assignment]
        client._subscriptions: set[bytes] = set()
        client._auto_reconnect = auto_reconnect
        client._on_disconnect = on_disconnect
        client._on_reconnect = on_reconnect
        client._state = ConnectionState.DISCONNECTED
        client._reconnect_task = None
        client._connected_event = asyncio.Event()
        client._monitor_task = None
        return client

    async def __aenter__(self) -> ChromatinClient:
        # Initial connection rotates through relay list (SDK-01).
        # No backoff on initial connect -- if all relays fail, raise immediately.
        last_exc: Exception | None = None
        for i, (host, port) in enumerate(self._relays):
            self._relay_index = i
            try:
                await self._do_connect(host, port)
                self._state = ConnectionState.CONNECTED
                self._connected_event.set()
                if self._auto_reconnect:
                    self._monitor_task = asyncio.get_event_loop().create_task(
                        self._connection_monitor()
                    )
                return self
            except (HandshakeError, asyncio.TimeoutError, OSError) as exc:
                last_exc = exc
                log.debug("initial connect to %s:%d failed: %s", host, port, exc)
                continue
        # All relays failed
        raise last_exc  # type: ignore[misc]

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None:
        self._state = ConnectionState.CLOSING
        self._connected_event.clear()
        # Cancel reconnect task if running
        if self._reconnect_task is not None:
            self._reconnect_task.cancel()
            try:
                await self._reconnect_task
            except asyncio.CancelledError:
                pass
            self._reconnect_task = None
        # Cancel monitor task
        if self._monitor_task is not None:
            self._monitor_task.cancel()
            try:
                await self._monitor_task
            except asyncio.CancelledError:
                pass
            self._monitor_task = None
        # Existing cleanup
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
            try:
                await self._transport.send_goodbye()
            except Exception:
                pass
            await self._transport.stop()

    async def ping(self) -> None:
        """Send Ping and wait for Pong. Raises ConnectionError if disconnected."""
        await self._transport.send_ping()

    # ------------------------------------------------------------------
    # Connection state and reconnect
    # ------------------------------------------------------------------

    @property
    def connection_state(self) -> ConnectionState:
        """Current connection state."""
        return self._state

    @property
    def current_relay(self) -> tuple[str, int]:
        """Currently connected (or last attempted) relay address."""
        return self._relays[self._relay_index]

    async def wait_connected(self, timeout: float | None = None) -> bool:
        """Wait until connected. Returns True if connected, False on timeout.

        Args:
            timeout: Maximum seconds to wait. None = wait indefinitely.

        Returns:
            True if now connected, False if timed out or closing.
        """
        if self._state == ConnectionState.CONNECTED:
            return True
        if self._state == ConnectionState.CLOSING:
            return False
        try:
            await asyncio.wait_for(self._connected_event.wait(), timeout=timeout)
            return True
        except asyncio.TimeoutError:
            return False

    async def _connection_monitor(self) -> None:
        """Watch transport state; trigger reconnect on connection loss."""
        try:
            while self._state != ConnectionState.CLOSING:
                await asyncio.sleep(0.5)
                if (
                    self._transport is not None
                    and self._transport.closed
                    and self._state == ConnectionState.CONNECTED
                ):
                    self._on_connection_lost()
        except asyncio.CancelledError:
            return

    def _on_connection_lost(self) -> None:
        """Handle detected connection loss."""
        if self._state == ConnectionState.CLOSING:
            return
        self._state = ConnectionState.DISCONNECTED
        self._connected_event.clear()
        log.info("connection lost, entering reconnect mode")
        # Fire on_disconnect callback (before reconnect starts)
        if self._on_disconnect is not None:
            asyncio.get_event_loop().create_task(
                invoke_callback(self._on_disconnect)
            )
        # Spawn reconnect loop
        if self._auto_reconnect and self._reconnect_task is None:
            self._reconnect_task = asyncio.get_event_loop().create_task(
                self._reconnect_loop()
            )

    async def _reconnect_loop(self) -> None:
        """Background reconnect cycling through relay list with backoff between full cycles."""
        cycle_count = 0
        disconnect_time = time.monotonic()
        try:
            while self._state == ConnectionState.DISCONNECTED:
                cycle_count += 1

                # Backoff between full cycles (D-04), not between individual relays
                if cycle_count > 1:
                    delay = backoff_delay(cycle_count - 1, base=1.0, cap=30.0)
                    log.debug("relay cycle %d, backoff %.2fs", cycle_count, delay)
                    await asyncio.sleep(delay)

                if self._state == ConnectionState.CLOSING:
                    return

                # Try each relay in order (D-03, D-05: no inter-attempt delay)
                for i, (host, port) in enumerate(self._relays):
                    if self._state == ConnectionState.CLOSING:
                        return

                    self._relay_index = i
                    self._state = ConnectionState.CONNECTING
                    try:
                        await self._do_connect(host, port)
                        self._state = ConnectionState.CONNECTED
                        self._connected_event.set()
                        log.info(
                            "reconnected to %s:%d after %d cycle(s) (%.1fs downtime)",
                            host, port, cycle_count,
                            time.monotonic() - disconnect_time,
                        )
                        await self._restore_subscriptions()
                        await invoke_callback(
                            self._on_reconnect,
                            cycle_count,
                            time.monotonic() - disconnect_time,
                            host,
                            port,
                        )
                        # Restart connection monitor
                        if self._monitor_task is not None:
                            self._monitor_task.cancel()
                            try:
                                await self._monitor_task
                            except asyncio.CancelledError:
                                pass
                        self._monitor_task = asyncio.get_event_loop().create_task(
                            self._connection_monitor()
                        )
                        return
                    except asyncio.CancelledError:
                        raise
                    except Exception as exc:
                        log.debug("relay %s:%d failed: %s", host, port, exc)
                        if self._state != ConnectionState.CLOSING:
                            self._state = ConnectionState.DISCONNECTED
                # Full cycle exhausted, loop back to top for backoff
        except asyncio.CancelledError:
            return
        finally:
            self._reconnect_task = None

    async def _do_connect(self, host: str, port: int) -> None:
        """Establish new TCP connection, perform PQ handshake, create Transport."""
        # Teardown old transport
        if self._transport is not None:
            try:
                await self._transport.stop()
            except Exception:
                pass
            self._transport = None

        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port, limit=4 * 1024 * 1024),
            timeout=self._timeout,
        )
        sock = writer.transport.get_extra_info("socket")
        if sock is not None:
            import socket
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        result = await asyncio.wait_for(
            perform_handshake(reader, writer, self._identity),
            timeout=self._timeout,
        )
        send_key, recv_key, send_counter, recv_counter, _ = result

        self._transport = Transport(
            reader, writer, send_key, recv_key, send_counter, recv_counter
        )
        self._transport.start()

    async def _restore_subscriptions(self) -> None:
        """Re-subscribe to all namespaces tracked in _subscriptions."""
        for ns in list(self._subscriptions):
            try:
                payload = encode_subscribe([ns])
                await self._transport.send_message(
                    TransportMsgType.Subscribe, payload
                )
                log.debug("re-subscribed to %s", ns.hex())
            except Exception:
                log.warning("failed to re-subscribe to %s", ns.hex())

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

    async def write_blob(
        self, data: bytes, ttl: int, *, namespace: bytes | None = None
    ) -> WriteResult:
        """Write a signed blob to the connected node.

        Args:
            data: Blob payload bytes.
            ttl: Time-to-live in seconds (required, per D-02).
            namespace: Target namespace (32 bytes). Defaults to own namespace.
                Pass another identity's namespace for delegated writes.

        Returns:
            WriteResult with server-assigned blob_hash and seq_num.

        Raises:
            ProtocolError: If server rejects the write (StorageFull, QuotaExceeded,
                or unexpected response type).
            ConnectionError: If request times out (per D-16).
        """
        timestamp = int(time.time())
        namespace = namespace if namespace is not None else self._identity.namespace
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
        Survives reconnections when auto_reconnect is enabled.

        Usage:
            async for notif in conn.notifications():
                print(f"New blob in {notif.namespace.hex()}")

        Yields:
            Notification objects.
        """
        while self._state != ConnectionState.CLOSING:
            if self._transport is None or self._transport.closed:
                if not self._auto_reconnect:
                    return
                # Wait for reconnect
                await asyncio.sleep(0.5)
                continue
            try:
                msg_type, payload, _ = await asyncio.wait_for(
                    self._transport.notifications.get(),
                    timeout=1.0,
                )
            except asyncio.TimeoutError:
                continue
            except Exception:
                # Transport died mid-read
                continue
            if msg_type == TransportMsgType.Notification:
                yield decode_notification(payload)

    # ------------------------------------------------------------------
    # Encrypted helpers
    # ------------------------------------------------------------------

    async def write_encrypted(
        self,
        data: bytes,
        ttl: int,
        recipients: list[Identity] | None = None,
    ) -> WriteResult:
        """Encrypt data and write as a blob.

        Args:
            data: Plaintext bytes to encrypt.
            ttl: Time-to-live in seconds.
            recipients: Recipient identities (each must have KEM pubkey).
                None or omitted: encrypt to self only (CLI-04).

        Returns:
            WriteResult with blob_hash, seq_num, duplicate.

        Raises:
            ValueError: If sender or any recipient lacks KEM keypair.
        """
        all_recipients = recipients if recipients is not None else []
        envelope = envelope_encrypt(data, all_recipients, self._identity)
        return await self.write_blob(envelope, ttl)

    async def read_encrypted(
        self, namespace: bytes, blob_hash: bytes
    ) -> bytes | None:
        """Fetch and decrypt an encrypted blob.

        Args:
            namespace: 32-byte namespace identifier.
            blob_hash: 32-byte blob hash.

        Returns:
            Decrypted plaintext bytes, or None if blob not found.

        Raises:
            NotARecipientError: If blob exists but caller is not a recipient.
            MalformedEnvelopeError: If blob data is not a valid envelope.
            DecryptionError: If AEAD authentication fails.
        """
        result = await self.read_blob(namespace, blob_hash)
        if result is None:
            return None
        return envelope_decrypt(result.data, self._identity)

    async def write_to_group(
        self,
        data: bytes,
        group_name: str,
        directory: Directory,
        ttl: int,
    ) -> WriteResult:
        """Encrypt data for all group members and write as a blob.

        Forces a directory cache refresh before resolving group membership
        to ensure recently removed members are excluded (GRP-02).

        Args:
            data: Plaintext bytes to encrypt.
            group_name: Name of the group in the directory.
            directory: Directory instance for group and member resolution.
            ttl: Time-to-live in seconds.

        Returns:
            WriteResult with blob_hash, seq_num, duplicate.

        Raises:
            DirectoryError: If group not found in directory.
            ValueError: If any resolved member lacks KEM pubkey.
        """
        directory.refresh()  # GRP-02: force cache refresh before group resolution
        group = await directory.get_group(group_name)
        if group is None:
            raise DirectoryError(f"Group not found: {group_name}")
        recipients: list[Identity] = []
        for member_hash in group.members:
            entry = await directory.get_user_by_pubkey(member_hash)
            if entry is not None:
                recipients.append(entry.identity)
        return await self.write_encrypted(data, ttl, recipients)
