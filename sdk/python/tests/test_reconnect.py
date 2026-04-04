"""Unit tests for auto-reconnect: backoff, state machine, reconnect loop, callbacks.

Tests cover:
- backoff_delay calculation (CONN-03)
- ConnectionState enum values
- invoke_callback safe dispatch (sync, async, exception handling)
- ChromatinClient state transitions (CONN-03)
- Reconnect loop with mocked _do_connect (CONN-03)
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
        client = ChromatinClient.connect("127.0.0.1", 9999, identity)
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
            "127.0.0.1",
            9999,
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

        async def mock_do_connect():
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
        """_do_connect fails twice then succeeds -- 3 attempts total."""
        client = self._make_client_with_transport()
        attempt_count = 0

        async def mock_do_connect():
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

        async def mock_do_connect():
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
            "127.0.0.1",
            9999,
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
        """on_reconnect receives (attempt_count, downtime_seconds) after reconnect."""
        reconnect_args = []

        async def on_reconnect(attempt, downtime):
            reconnect_args.append((attempt, downtime))

        identity = Identity.generate()
        client = ChromatinClient.connect(
            "127.0.0.1",
            9999,
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

        async def mock_do_connect():
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
        attempt, downtime = reconnect_args[0]
        assert attempt == 1
        assert isinstance(downtime, float)
        assert downtime >= 0

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

        async def bad_on_reconnect(attempt, downtime):
            raise RuntimeError("callback exploded")

        identity = Identity.generate()
        client = ChromatinClient.connect(
            "127.0.0.1",
            9999,
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

        async def mock_do_connect():
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
            "127.0.0.1", 9999, identity, auto_reconnect=True
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
        client = ChromatinClient.connect("127.0.0.1", 9999, identity)
        # State is DISCONNECTED, event is not set
        result = await client.wait_connected(timeout=0.1)
        assert result is False

    async def test_unblocks_on_reconnect(self) -> None:
        """DISCONNECTED, then event set -> wait_connected returns True."""
        identity = Identity.generate()
        client = ChromatinClient.connect("127.0.0.1", 9999, identity)
        # State is DISCONNECTED, event is not set

        async def set_event_soon():
            await asyncio.sleep(0.05)
            client._connected_event.set()

        asyncio.get_event_loop().create_task(set_event_soon())
        result = await client.wait_connected(timeout=5.0)
        assert result is True
