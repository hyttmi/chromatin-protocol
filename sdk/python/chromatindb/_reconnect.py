"""Auto-reconnect primitives for chromatindb SDK. Internal module."""

from __future__ import annotations

import asyncio
import enum
import logging
import random
from typing import Awaitable, Callable

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Connection state machine
# ---------------------------------------------------------------------------


class ConnectionState(enum.Enum):
    """Connection lifecycle states (per D-05)."""

    DISCONNECTED = "disconnected"
    CONNECTING = "connecting"
    CONNECTED = "connected"
    CLOSING = "closing"


# ---------------------------------------------------------------------------
# Callback type aliases
# ---------------------------------------------------------------------------

OnDisconnect = Callable[[], Awaitable[None] | None]
OnReconnect = Callable[[int, float, str, int], Awaitable[None] | None]
# on_reconnect receives (cycle_count: int, downtime_seconds: float, relay_host: str, relay_port: int)


# ---------------------------------------------------------------------------
# Backoff
# ---------------------------------------------------------------------------


def backoff_delay(attempt: int, base: float = 1.0, cap: float = 30.0) -> float:
    """Full jitter: uniform random in [0, min(cap, base * 2^(attempt-1))].

    attempt=1 -> [0, 1.0], attempt=2 -> [0, 2.0], ..., attempt=6+ -> [0, 30.0]
    """
    exp = min(cap, base * (2 ** (attempt - 1)))
    return random.uniform(0, exp)


# ---------------------------------------------------------------------------
# Safe callback invocation
# ---------------------------------------------------------------------------


async def invoke_callback(callback: Callable | None, *args: object) -> None:
    """Safely call a sync or async callback, catching all exceptions."""
    if callback is None:
        return
    try:
        result = callback(*args)
        if asyncio.iscoroutine(result):
            await result
    except Exception:
        log.warning("reconnect callback raised", exc_info=True)
