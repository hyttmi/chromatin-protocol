"""Unit tests for auto-reconnect: backoff, state machine, reconnect loop, callbacks.

Tests cover:
- backoff_delay calculation (CONN-03)
- ConnectionState enum values
- invoke_callback safe dispatch (sync, async, exception handling)
- ChromatinClient state transitions (CONN-03)
- Multi-relay connect and rotation (SDK-01)
- Reconnect loop with mocked _do_connect (CONN-03)
- Multi-relay reconnect cycling (SDK-02)
- Subscription restoration after reconnect (CONN-04)
- on_disconnect / on_reconnect callbacks (CONN-05)
- close() suppresses reconnect (CONN-03/CONN-05)
- wait_connected() behavior
- notifications() survives reconnect
"""

from __future__ import annotations

import asyncio
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

from chromatindb._reconnect import ConnectionState, backoff_delay, invoke_callback
from chromatindb.client import ChromatinClient
from chromatindb.identity import Identity
from chromatindb.wire import TransportMsgType


# ---------------------------------------------------------------------------
# TestBackoffDelay (CONN-03 backoff formula)
# ---------------------------------------------------------------------------


class TestBackoffDelay:
    """Verify jittered exponential backoff produces correct ranges."""

    def test_attempt_1_range(self) -> None:
        """attempt=1 -> all values in [0, 1.0]."""
        for _ in range(1000):
            val = backoff_delay(1)
            assert 0 <= val <= 1.0

    def test_attempt_2_range(self) -> None:
        """attempt=2 -> all values in [0, 2.0]."""
        for _ in range(1000):
            val = backoff_delay(2)
            assert 0 <= val <= 2.0

    def test_attempt_5_range(self) -> None:
        """attempt=5 -> all values in [0, 16.0]."""
        for _ in range(1000):
            val = backoff_delay(5)
            assert 0 <= val <= 16.0

    def test_cap_at_30(self) -> None:
        """attempt=10 -> all values in [0, 30.0] (cap applies)."""
        for _ in range(1000):
            val = backoff_delay(10)
            assert 0 <= val <= 30.0

    def test_custom_base_and_cap(self) -> None:
        """Custom base=2.0, cap=10.0 produces correct ranges."""
        for _ in range(1000):
            val = backoff_delay(1, base=2.0, cap=10.0)
            assert 0 <= val <= 2.0
        for _ in range(1000):
            val = backoff_delay(5, base=2.0, cap=10.0)
            assert 0 <= val <= 10.0

    def test_jitter_is_not_constant(self) -> None:
        """100 samples produce more than one distinct value (jitter works)."""
        values = {round(backoff_delay(3), 6) for _ in range(100)}
        assert len(values) > 1


# ---------------------------------------------------------------------------
# TestConnectionState
# ---------------------------------------------------------------------------


class TestConnectionState:
    """Verify ConnectionState enum completeness."""

    def test_enum_values(self) -> None:
        """All 4 states exist with correct string values."""
        assert ConnectionState.DISCONNECTED.value == "disconnected"
        assert ConnectionState.CONNECTING.value == "connecting"
        assert ConnectionState.CONNECTED.value == "connected"
        assert ConnectionState.CLOSING.value == "closing"

    def test_member_count(self) -> None:
        """Exactly 4 members in the enum."""
        assert len(ConnectionState) == 4


# ---------------------------------------------------------------------------
# TestInvokeCallback (CONN-05 callback safety)
# ---------------------------------------------------------------------------


class TestInvokeCallback:
    """Verify safe callback dispatch for sync, async, None, and error cases."""

    async def test_sync_callback(self) -> None:
        """Sync function is called with correct args."""
        called_with = []

        def cb(a, b):
            called_with.append((a, b))

        await invoke_callback(cb, 1, "hello")
        assert called_with == [(1, "hello")]

    async def test_async_callback(self) -> None:
        """Async function is awaited with correct args."""
        called_with = []

        async def cb(a, b):
            called_with.append((a, b))

        await invoke_callback(cb, 42, 3.14)
        assert called_with == [(42, 3.14)]

    async def test_none_callback(self) -> None:
        """invoke_callback(None) does not raise."""
        await invoke_callback(None)

    async def test_exception_suppressed(self) -> None:
        """Sync callback that raises RuntimeError does not propagate."""

        def cb():
            raise RuntimeError("boom")

        await invoke_callback(cb)  # Should not raise

    async def test_async_exception_suppressed(self) -> None:
        """Async callback that raises does not propagate."""

        async def cb():
            raise ValueError("async boom")

        await invoke_callback(cb)  # Should not raise


# ---------------------------------------------------------------------------
# TestClientStateTransitions (CONN-03 state machine)
# ---------------------------------------------------------------------------


class TestClientStateTransitions:
    """Verify connection state properties on client creation paths."""

    async def test_direct_init_is_connected(self) -> None:
        """ChromatinClient(transport) starts in CONNECTED state."""
        mock_transport = MagicMock()
        mock_transport.closed = False
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock()
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()

        client = ChromatinClient(mock_transport)
        assert client.connection_state == ConnectionState.CONNECTED

    async def test_connect_classmethod_initial_state(self) -> None:
        """ChromatinClient.connect() creates client in DISCONNECTED state."""
        identity = Identity.generate()
        client = ChromatinClient.connect([("127.0.0.1", 9999)], identity)
        assert client.connection_state == ConnectionState.DISCONNECTED

    async def test_connection_state_property(self) -> None:
        """connection_state returns a ConnectionState enum value."""
        mock_transport = MagicMock()
        mock_transport.closed = False
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock()
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()

        client = ChromatinClient(mock_transport)
        assert isinstance(client.connection_state, ConnectionState)


# ---------------------------------------------------------------------------
# TestMultiRelayConnect (SDK-01)
# ---------------------------------------------------------------------------


class TestMultiRelayConnect:
    """Verify multi-relay connect() API (SDK-01)."""

    def test_connect_accepts_relay_list(self) -> None:
        """connect() with 3 relays stores them in _relays."""
        identity = Identity.generate()
        relays = [("r1", 4201), ("r2", 4202), ("r3", 4203)]
        client = ChromatinClient.connect(relays, identity)
        assert client._relays == relays
        assert client._relay_index == 0

    def test_connect_rejects_empty_relays(self) -> None:
        """connect([]) raises ValueError."""
        identity = Identity.generate()
        with pytest.raises(ValueError, match="relays must be a non-empty list"):
            ChromatinClient.connect([], identity)

    def test_connect_single_relay(self) -> None:
        """connect() with single relay works."""
        identity = Identity.generate()
        client = ChromatinClient.connect([("host", 4201)], identity)
        assert client._relays == [("host", 4201)]

    def test_current_relay_default(self) -> None:
        """current_relay returns first relay after connect()."""
        identity = Identity.generate()
        client = ChromatinClient.connect([("r1", 4201), ("r2", 4202)], identity)
        assert client.current_relay == ("r1", 4201)

    async def test_initial_connect_rotates_on_failure(self) -> None:
        """__aenter__ tries all relays; first 2 fail, third succeeds (SDK-01)."""
        identity = Identity.generate()
        client = ChromatinClient.connect(
            [("r1", 4201), ("r2", 4202), ("r3", 4203)], identity
        )
        connect_calls: list[tuple[str, int]] = []

        async def mock_do_connect(host, port):
            connect_calls.append((host, port))
            if host in ("r1", "r2"):
                raise OSError(f"{host} down")
            # r3 succeeds
            mock_transport = MagicMock()
            mock_transport.closed = False
            mock_transport.stop = AsyncMock()
            mock_transport.send_message = AsyncMock()
            mock_transport.send_goodbye = AsyncMock()
            mock_transport.notifications = asyncio.Queue()
            mock_transport.start = MagicMock()
            client._transport = mock_transport

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            result = await client.__aenter__()
            assert result is client

        assert connect_calls == [("r1", 4201), ("r2", 4202), ("r3", 4203)]
        assert client.current_relay == ("r3", 4203)
        assert client.connection_state == ConnectionState.CONNECTED

        # Cleanup
        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass

    async def test_initial_connect_all_fail_raises(self) -> None:
        """__aenter__ raises if all relays fail (no backoff, no retry)."""
        identity = Identity.generate()
        client = ChromatinClient.connect(
            [("r1", 4201), ("r2", 4202)], identity
        )

        async def mock_do_connect(host, port):
            raise OSError(f"{host} down")

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with pytest.raises(OSError, match="r2 down"):
                await client.__aenter__()

    async def test_current_relay_after_initial_rotation(self) -> None:
        """current_relay reflects which relay we connected to."""
        identity = Identity.generate()
        client = ChromatinClient.connect(
            [("r1", 4201), ("r2", 4202)], identity
        )

        async def mock_do_connect(host, port):
            if host == "r1":
                raise OSError("r1 down")
            mock_transport = MagicMock()
            mock_transport.closed = False
            mock_transport.stop = AsyncMock()
            mock_transport.send_message = AsyncMock()
            mock_transport.send_goodbye = AsyncMock()
            mock_transport.notifications = asyncio.Queue()
            mock_transport.start = MagicMock()
            client._transport = mock_transport

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            await client.__aenter__()

        assert client.current_relay == ("r2", 4202)

        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass


# ---------------------------------------------------------------------------
# TestReconnectLoop (CONN-03 reconnect behavior)
# ---------------------------------------------------------------------------


class TestReconnectLoop:
    """Verify reconnect loop trigger, retry, close-stop, and auto_reconnect=False."""

    def _make_client_with_transport(
        self,
        *,
        auto_reconnect: bool = True,
        on_disconnect=None,
        on_reconnect=None,
    ) -> ChromatinClient:
        """Create a client via connect() then inject a mock transport."""
        identity = Identity.generate()
        client = ChromatinClient.connect(
            [("127.0.0.1", 9999)],
            identity,
            auto_reconnect=auto_reconnect,
            on_disconnect=on_disconnect,
            on_reconnect=on_reconnect,
        )
        # Simulate that initial connect succeeded
        mock_transport = MagicMock()
        mock_transport.closed = True  # Will be detected as connection loss
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock()
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()
        client._transport = mock_transport
        client._state = ConnectionState.CONNECTED
        client._connected_event.set()
        return client

    async def test_reconnect_triggers_on_connection_lost(self) -> None:
        """_on_connection_lost() transitions to DISCONNECTED, then reconnect succeeds."""
        client = self._make_client_with_transport()
        reconnected = asyncio.Event()

        async def mock_do_connect(host, port):
            # Simulate successful reconnect
            new_transport = MagicMock()
            new_transport.closed = False
            new_transport.stop = AsyncMock()
            new_transport.send_message = AsyncMock()
            new_transport.send_goodbye = AsyncMock()
            new_transport.notifications = asyncio.Queue()
            new_transport.start = MagicMock()
            client._transport = new_transport
            reconnected.set()

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", return_value=0):
                client._on_connection_lost()
                assert client.connection_state == ConnectionState.DISCONNECTED
                await asyncio.wait_for(reconnected.wait(), timeout=2.0)
                # Allow reconnect loop to finish setting state
                await asyncio.sleep(0.05)
                assert client.connection_state == ConnectionState.CONNECTED

        # Cleanup
        if client._reconnect_task and not client._reconnect_task.done():
            client._reconnect_task.cancel()
            try:
                await client._reconnect_task
            except asyncio.CancelledError:
                pass
        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass

    async def test_reconnect_retries_on_failure(self) -> None:
        """_do_connect fails twice then succeeds -- 3 cycles total (single relay)."""
        client = self._make_client_with_transport()
        attempt_count = 0

        async def mock_do_connect(host, port):
            nonlocal attempt_count
            attempt_count += 1
            if attempt_count < 3:
                raise ConnectionError("simulated failure")
            # Success on attempt 3
            new_transport = MagicMock()
            new_transport.closed = False
            new_transport.stop = AsyncMock()
            new_transport.send_message = AsyncMock()
            new_transport.send_goodbye = AsyncMock()
            new_transport.notifications = asyncio.Queue()
            new_transport.start = MagicMock()
            client._transport = new_transport

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", return_value=0):
                client._on_connection_lost()
                # Wait for reconnect to complete
                await asyncio.wait_for(
                    client._connected_event.wait(), timeout=2.0
                )
                await asyncio.sleep(0.05)

        assert attempt_count == 3
        assert client.connection_state == ConnectionState.CONNECTED

        # Cleanup
        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass

    async def test_close_stops_reconnect(self) -> None:
        """Setting state to CLOSING causes reconnect loop to exit."""
        client = self._make_client_with_transport()

        async def mock_do_connect(host, port):
            # Simulate slow connect -- gives time for close to happen
            await asyncio.sleep(10)

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", return_value=0):
                client._on_connection_lost()
                # Let reconnect loop start
                await asyncio.sleep(0.05)

                # Simulate close
                client._state = ConnectionState.CLOSING
                client._connected_event.clear()

                # Cancel reconnect task
                if client._reconnect_task:
                    client._reconnect_task.cancel()
                    try:
                        await asyncio.wait_for(
                            client._reconnect_task, timeout=2.0
                        )
                    except (asyncio.CancelledError, asyncio.TimeoutError):
                        pass

                # Verify task is done
                assert (
                    client._reconnect_task is None
                    or client._reconnect_task.done()
                )

    async def test_reconnect_not_triggered_when_auto_reconnect_false(self) -> None:
        """auto_reconnect=False: _on_connection_lost() does not spawn reconnect task."""
        client = self._make_client_with_transport(auto_reconnect=False)
        client._on_connection_lost()
        assert client._reconnect_task is None
        assert client.connection_state == ConnectionState.DISCONNECTED


# ---------------------------------------------------------------------------
# TestSubscriptionRestore (CONN-04)
# ---------------------------------------------------------------------------


class TestSubscriptionRestore:
    """Verify subscriptions are re-sent after reconnect."""

    async def test_subscriptions_resent_after_reconnect(self) -> None:
        """_restore_subscriptions sends Subscribe for each tracked namespace."""
        mock_transport = MagicMock()
        mock_transport.closed = False
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock()
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()

        client = ChromatinClient(mock_transport)
        ns1 = b"\x01" * 32
        ns2 = b"\x02" * 32
        client._subscriptions = {ns1, ns2}

        await client._restore_subscriptions()

        assert mock_transport.send_message.call_count == 2
        # Verify Subscribe type was used for both calls
        for call in mock_transport.send_message.call_args_list:
            assert call.args[0] == TransportMsgType.Subscribe

    async def test_subscribe_failure_logged_not_fatal(self) -> None:
        """send_message raising during _restore_subscriptions does not propagate."""
        mock_transport = MagicMock()
        mock_transport.closed = False
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock(side_effect=Exception("send failed"))
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()

        client = ChromatinClient(mock_transport)
        ns1 = b"\x01" * 32
        client._subscriptions = {ns1}

        # Should not raise
        await client._restore_subscriptions()
        # Subscription should still be tracked
        assert ns1 in client._subscriptions


# ---------------------------------------------------------------------------
# TestCallbacks (CONN-05)
# ---------------------------------------------------------------------------


class TestCallbacks:
    """Verify on_disconnect and on_reconnect callback behavior."""

    async def test_on_disconnect_fires(self) -> None:
        """on_disconnect callback is invoked when connection is lost."""
        disconnect_called = asyncio.Event()

        async def on_disconnect():
            disconnect_called.set()

        identity = Identity.generate()
        client = ChromatinClient.connect(
            [("127.0.0.1", 9999)],
            identity,
            auto_reconnect=False,
            on_disconnect=on_disconnect,
        )
        mock_transport = MagicMock()
        mock_transport.closed = True
        mock_transport.stop = AsyncMock()
        client._transport = mock_transport
        client._state = ConnectionState.CONNECTED
        client._connected_event.set()

        client._on_connection_lost()
        await asyncio.wait_for(disconnect_called.wait(), timeout=2.0)

    async def test_on_reconnect_fires_with_args(self) -> None:
        """on_reconnect receives (cycle_count, downtime_seconds, host, port) after reconnect."""
        reconnect_args = []

        async def on_reconnect(cycle_count, downtime, host, port):
            reconnect_args.append((cycle_count, downtime, host, port))

        identity = Identity.generate()
        client = ChromatinClient.connect(
            [("127.0.0.1", 9999)],
            identity,
            auto_reconnect=True,
            on_reconnect=on_reconnect,
        )
        mock_transport = MagicMock()
        mock_transport.closed = True
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock()
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()
        client._transport = mock_transport
        client._state = ConnectionState.CONNECTED
        client._connected_event.set()

        async def mock_do_connect(host, port):
            new_transport = MagicMock()
            new_transport.closed = False
            new_transport.stop = AsyncMock()
            new_transport.send_message = AsyncMock()
            new_transport.send_goodbye = AsyncMock()
            new_transport.notifications = asyncio.Queue()
            new_transport.start = MagicMock()
            client._transport = new_transport

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", return_value=0):
                client._on_connection_lost()
                await asyncio.wait_for(
                    client._connected_event.wait(), timeout=2.0
                )
                await asyncio.sleep(0.05)

        assert len(reconnect_args) == 1
        cycle_count, downtime, host, port = reconnect_args[0]
        assert cycle_count == 1
        assert isinstance(downtime, float)
        assert downtime >= 0
        assert host == "127.0.0.1"
        assert port == 9999

        # Cleanup
        if client._reconnect_task and not client._reconnect_task.done():
            client._reconnect_task.cancel()
            try:
                await client._reconnect_task
            except asyncio.CancelledError:
                pass
        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass

    async def test_callback_exception_does_not_kill_reconnect(self) -> None:
        """on_reconnect raising does not prevent state from reaching CONNECTED."""

        async def bad_on_reconnect(cycle_count, downtime, host, port):
            raise RuntimeError("callback exploded")

        identity = Identity.generate()
        client = ChromatinClient.connect(
            [("127.0.0.1", 9999)],
            identity,
            auto_reconnect=True,
            on_reconnect=bad_on_reconnect,
        )
        mock_transport = MagicMock()
        mock_transport.closed = True
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock()
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()
        client._transport = mock_transport
        client._state = ConnectionState.CONNECTED
        client._connected_event.set()

        async def mock_do_connect(host, port):
            new_transport = MagicMock()
            new_transport.closed = False
            new_transport.stop = AsyncMock()
            new_transport.send_message = AsyncMock()
            new_transport.send_goodbye = AsyncMock()
            new_transport.notifications = asyncio.Queue()
            new_transport.start = MagicMock()
            client._transport = new_transport

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", return_value=0):
                client._on_connection_lost()
                await asyncio.wait_for(
                    client._connected_event.wait(), timeout=2.0
                )
                await asyncio.sleep(0.05)

        assert client.connection_state == ConnectionState.CONNECTED

        # Cleanup
        if client._reconnect_task and not client._reconnect_task.done():
            client._reconnect_task.cancel()
            try:
                await client._reconnect_task
            except asyncio.CancelledError:
                pass
        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass


# ---------------------------------------------------------------------------
# TestCloseNoReconnect (CONN-03/CONN-05 close suppression)
# ---------------------------------------------------------------------------


class TestCloseNoReconnect:
    """Verify CLOSING state prevents reconnect."""

    async def test_close_sets_closing_state(self) -> None:
        """__aexit__ sets state to CLOSING."""
        mock_transport = MagicMock()
        mock_transport.closed = False
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock()
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()

        client = ChromatinClient(mock_transport)
        assert client.connection_state == ConnectionState.CONNECTED

        await client.__aexit__(None, None, None)
        assert client.connection_state == ConnectionState.CLOSING

    async def test_closing_state_prevents_reconnect(self) -> None:
        """_on_connection_lost() with CLOSING state does not spawn reconnect."""
        identity = Identity.generate()
        client = ChromatinClient.connect(
            [("127.0.0.1", 9999)], identity, auto_reconnect=True
        )
        mock_transport = MagicMock()
        mock_transport.closed = True
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock()
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()
        client._transport = mock_transport
        client._state = ConnectionState.CLOSING

        client._on_connection_lost()
        assert client._reconnect_task is None
        assert client.connection_state == ConnectionState.CLOSING


# ---------------------------------------------------------------------------
# TestWaitConnected
# ---------------------------------------------------------------------------


class TestWaitConnected:
    """Verify wait_connected() behavior for various states."""

    async def test_returns_true_when_connected(self) -> None:
        """CONNECTED state -> returns True immediately."""
        mock_transport = MagicMock()
        mock_transport.closed = False
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock()
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()

        client = ChromatinClient(mock_transport)
        result = await client.wait_connected(timeout=1.0)
        assert result is True

    async def test_returns_false_when_closing(self) -> None:
        """CLOSING state -> returns False immediately."""
        mock_transport = MagicMock()
        mock_transport.closed = False
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock()
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()

        client = ChromatinClient(mock_transport)
        client._state = ConnectionState.CLOSING
        result = await client.wait_connected(timeout=1.0)
        assert result is False

    async def test_times_out(self) -> None:
        """DISCONNECTED with event unset -> returns False after timeout."""
        identity = Identity.generate()
        client = ChromatinClient.connect([("127.0.0.1", 9999)], identity)
        # State is DISCONNECTED, event is not set
        result = await client.wait_connected(timeout=0.1)
        assert result is False

    async def test_unblocks_on_reconnect(self) -> None:
        """DISCONNECTED, then event set -> wait_connected returns True."""
        identity = Identity.generate()
        client = ChromatinClient.connect([("127.0.0.1", 9999)], identity)
        # State is DISCONNECTED, event is not set

        async def set_event_soon():
            await asyncio.sleep(0.05)
            client._connected_event.set()

        asyncio.get_event_loop().create_task(set_event_soon())
        result = await client.wait_connected(timeout=5.0)
        assert result is True


# ---------------------------------------------------------------------------
# TestMultiRelayReconnect (SDK-02)
# ---------------------------------------------------------------------------


class TestMultiRelayReconnect:
    """Verify reconnect cycles through relay list (SDK-02)."""

    def _make_multi_relay_client(
        self,
        relays: list[tuple[str, int]] | None = None,
        *,
        on_reconnect=None,
    ) -> ChromatinClient:
        identity = Identity.generate()
        client = ChromatinClient.connect(
            relays or [("r1", 4201), ("r2", 4202), ("r3", 4203)],
            identity,
            auto_reconnect=True,
            on_reconnect=on_reconnect,
        )
        mock_transport = MagicMock()
        mock_transport.closed = True
        mock_transport.stop = AsyncMock()
        mock_transport.send_message = AsyncMock()
        mock_transport.send_goodbye = AsyncMock()
        mock_transport.notifications = asyncio.Queue()
        client._transport = mock_transport
        client._state = ConnectionState.CONNECTED
        client._connected_event.set()
        return client

    async def test_reconnect_cycles_relay_list(self) -> None:
        """3 relays: r1 and r2 fail, r3 succeeds -- all tried in order."""
        client = self._make_multi_relay_client()
        connect_calls: list[tuple[str, int]] = []

        async def mock_do_connect(host, port):
            connect_calls.append((host, port))
            if host in ("r1", "r2"):
                raise ConnectionError(f"{host} down")
            new_transport = MagicMock()
            new_transport.closed = False
            new_transport.stop = AsyncMock()
            new_transport.send_message = AsyncMock()
            new_transport.send_goodbye = AsyncMock()
            new_transport.notifications = asyncio.Queue()
            new_transport.start = MagicMock()
            client._transport = new_transport

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", return_value=0):
                client._on_connection_lost()
                await asyncio.wait_for(
                    client._connected_event.wait(), timeout=2.0
                )
                await asyncio.sleep(0.05)

        assert connect_calls == [("r1", 4201), ("r2", 4202), ("r3", 4203)]
        assert client.current_relay == ("r3", 4203)

        # Cleanup
        if client._reconnect_task and not client._reconnect_task.done():
            client._reconnect_task.cancel()
            try:
                await client._reconnect_task
            except asyncio.CancelledError:
                pass
        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass

    async def test_reconnect_no_delay_within_cycle(self) -> None:
        """backoff_delay is NOT called between individual relays within a cycle."""
        client = self._make_multi_relay_client()
        backoff_calls = []

        async def mock_do_connect(host, port):
            if host in ("r1", "r2"):
                raise ConnectionError(f"{host} down")
            new_transport = MagicMock()
            new_transport.closed = False
            new_transport.stop = AsyncMock()
            new_transport.send_message = AsyncMock()
            new_transport.send_goodbye = AsyncMock()
            new_transport.notifications = asyncio.Queue()
            new_transport.start = MagicMock()
            client._transport = new_transport

        def mock_backoff(attempt, base=1.0, cap=30.0):
            backoff_calls.append(attempt)
            return 0

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", side_effect=mock_backoff):
                client._on_connection_lost()
                await asyncio.wait_for(
                    client._connected_event.wait(), timeout=2.0
                )
                await asyncio.sleep(0.05)

        # First cycle: no backoff called (cycle_count == 1, so no backoff)
        # r3 succeeds in first cycle, so only 1 cycle total
        assert backoff_calls == []

        # Cleanup
        if client._reconnect_task and not client._reconnect_task.done():
            client._reconnect_task.cancel()
            try:
                await client._reconnect_task
            except asyncio.CancelledError:
                pass
        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass

    async def test_reconnect_backoff_between_cycles(self) -> None:
        """All 3 relays fail on cycle 1, r1 succeeds on cycle 2; backoff called once."""
        client = self._make_multi_relay_client()
        attempt_count = 0
        backoff_calls = []

        async def mock_do_connect(host, port):
            nonlocal attempt_count
            attempt_count += 1
            # Cycle 1: all fail (attempts 1-3). Cycle 2: r1 succeeds (attempt 4).
            if attempt_count <= 3:
                raise ConnectionError(f"{host} down")
            new_transport = MagicMock()
            new_transport.closed = False
            new_transport.stop = AsyncMock()
            new_transport.send_message = AsyncMock()
            new_transport.send_goodbye = AsyncMock()
            new_transport.notifications = asyncio.Queue()
            new_transport.start = MagicMock()
            client._transport = new_transport

        def mock_backoff(attempt, base=1.0, cap=30.0):
            backoff_calls.append(attempt)
            return 0

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", side_effect=mock_backoff):
                client._on_connection_lost()
                await asyncio.wait_for(
                    client._connected_event.wait(), timeout=2.0
                )
                await asyncio.sleep(0.05)

        # backoff_delay called once between cycle 1 and 2 with cycle_count-1 = 1
        assert backoff_calls == [1]
        assert attempt_count == 4

        # Cleanup
        if client._reconnect_task and not client._reconnect_task.done():
            client._reconnect_task.cancel()
            try:
                await client._reconnect_task
            except asyncio.CancelledError:
                pass
        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass

    async def test_reconnect_starts_from_top_each_cycle(self) -> None:
        """Cycle 2 starts from relay[0], not from where cycle 1 left off (D-06)."""
        client = self._make_multi_relay_client()
        connect_calls: list[tuple[str, int]] = []
        attempt_count = 0

        async def mock_do_connect(host, port):
            nonlocal attempt_count
            attempt_count += 1
            connect_calls.append((host, port))
            # Cycle 1: all fail (1-3). Cycle 2: r1 succeeds (attempt 4).
            if attempt_count <= 3:
                raise ConnectionError(f"{host} down")
            new_transport = MagicMock()
            new_transport.closed = False
            new_transport.stop = AsyncMock()
            new_transport.send_message = AsyncMock()
            new_transport.send_goodbye = AsyncMock()
            new_transport.notifications = asyncio.Queue()
            new_transport.start = MagicMock()
            client._transport = new_transport

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", return_value=0):
                client._on_connection_lost()
                await asyncio.wait_for(
                    client._connected_event.wait(), timeout=2.0
                )
                await asyncio.sleep(0.05)

        # Cycle 1: r1, r2, r3 (all fail). Cycle 2: r1 (succeeds).
        assert connect_calls == [
            ("r1", 4201), ("r2", 4202), ("r3", 4203),
            ("r1", 4201),
        ]
        assert client.current_relay == ("r1", 4201)

        # Cleanup
        if client._reconnect_task and not client._reconnect_task.done():
            client._reconnect_task.cancel()
            try:
                await client._reconnect_task
            except asyncio.CancelledError:
                pass
        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass

    async def test_on_reconnect_receives_relay_info(self) -> None:
        """on_reconnect callback receives (cycle_count, downtime, host, port)."""
        reconnect_args = []

        async def on_reconnect(cycle_count, downtime, host, port):
            reconnect_args.append((cycle_count, downtime, host, port))

        client = self._make_multi_relay_client(on_reconnect=on_reconnect)

        async def mock_do_connect(host, port):
            if host == "r1":
                raise ConnectionError("r1 down")
            new_transport = MagicMock()
            new_transport.closed = False
            new_transport.stop = AsyncMock()
            new_transport.send_message = AsyncMock()
            new_transport.send_goodbye = AsyncMock()
            new_transport.notifications = asyncio.Queue()
            new_transport.start = MagicMock()
            client._transport = new_transport

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", return_value=0):
                client._on_connection_lost()
                await asyncio.wait_for(
                    client._connected_event.wait(), timeout=2.0
                )
                await asyncio.sleep(0.05)

        assert len(reconnect_args) == 1
        cycle_count, downtime, host, port = reconnect_args[0]
        assert cycle_count == 1
        assert isinstance(downtime, float)
        assert host == "r2"
        assert port == 4202

        # Cleanup
        if client._reconnect_task and not client._reconnect_task.done():
            client._reconnect_task.cancel()
            try:
                await client._reconnect_task
            except asyncio.CancelledError:
                pass
        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass

    async def test_circuit_breaker_backoff_escalation(self) -> None:
        """3 full cycles of failures: backoff_delay called with 1, 2, 3."""
        client = self._make_multi_relay_client()
        attempt_count = 0
        backoff_calls = []

        async def mock_do_connect(host, port):
            nonlocal attempt_count
            attempt_count += 1
            # Cycles 1-3 (9 attempts total) all fail. Cycle 4: r1 succeeds (attempt 10).
            if attempt_count <= 9:
                raise ConnectionError(f"{host} down")
            new_transport = MagicMock()
            new_transport.closed = False
            new_transport.stop = AsyncMock()
            new_transport.send_message = AsyncMock()
            new_transport.send_goodbye = AsyncMock()
            new_transport.notifications = asyncio.Queue()
            new_transport.start = MagicMock()
            client._transport = new_transport

        def mock_backoff(attempt, base=1.0, cap=30.0):
            backoff_calls.append(attempt)
            return 0

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", side_effect=mock_backoff):
                client._on_connection_lost()
                await asyncio.wait_for(
                    client._connected_event.wait(), timeout=2.0
                )
                await asyncio.sleep(0.05)

        # Backoff called before cycles 2, 3, 4 with cycle_count-1 = 1, 2, 3
        assert backoff_calls == [1, 2, 3]

        # Cleanup
        if client._reconnect_task and not client._reconnect_task.done():
            client._reconnect_task.cancel()
            try:
                await client._reconnect_task
            except asyncio.CancelledError:
                pass
        if client._monitor_task and not client._monitor_task.done():
            client._monitor_task.cancel()
            try:
                await client._monitor_task
            except asyncio.CancelledError:
                pass

    async def test_closing_state_stops_relay_cycling(self) -> None:
        """Setting CLOSING during relay iteration stops the reconnect loop."""
        client = self._make_multi_relay_client()
        connect_calls: list[tuple[str, int]] = []

        async def mock_do_connect(host, port):
            connect_calls.append((host, port))
            if host == "r1":
                # After first relay attempt, set CLOSING
                client._state = ConnectionState.CLOSING
                raise ConnectionError("r1 down")
            raise ConnectionError(f"{host} down")

        with patch.object(client, "_do_connect", side_effect=mock_do_connect):
            with patch("chromatindb.client.backoff_delay", return_value=0):
                client._on_connection_lost()
                # Give the loop time to run
                await asyncio.sleep(0.1)

        # Only r1 should have been attempted; CLOSING stopped further cycling
        assert connect_calls == [("r1", 4201)]

        # Cleanup
        if client._reconnect_task and not client._reconnect_task.done():
            client._reconnect_task.cancel()
            try:
                await client._reconnect_task
            except asyncio.CancelledError:
                pass
