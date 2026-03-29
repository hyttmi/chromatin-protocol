"""Integration tests for chromatindb SDK against live KVM relay.

These tests require a running chromatindb relay.
Configure via environment variables (per D-13):
  CHROMATINDB_RELAY_HOST (default: 192.168.1.200)
  CHROMATINDB_RELAY_PORT (default: 4433)

Run: pytest tests/test_integration.py -v -m integration
Skip: automatically skipped if relay is unreachable
"""

from __future__ import annotations

import os
import socket

import pytest

from chromatindb import ChromatinClient
from chromatindb.exceptions import HandshakeError
from chromatindb.identity import Identity

RELAY_HOST = os.environ.get("CHROMATINDB_RELAY_HOST", "192.168.1.200")
RELAY_PORT = int(os.environ.get("CHROMATINDB_RELAY_PORT", "4433"))


def _relay_reachable() -> bool:
    """Check if relay is reachable via TCP connect (1s timeout)."""
    try:
        sock = socket.create_connection((RELAY_HOST, RELAY_PORT), timeout=1.0)
        sock.close()
        return True
    except (OSError, socket.timeout):
        return False


pytestmark = [
    pytest.mark.integration,
    pytest.mark.skipif(
        not _relay_reachable(),
        reason=f"relay at {RELAY_HOST}:{RELAY_PORT} unreachable",
    ),
]


async def test_handshake_connect_disconnect() -> None:
    """PQ handshake + Goodbye (XPORT-02, XPORT-03, XPORT-07)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        assert not conn._transport.closed
    # After context manager exit, transport should be closed


async def test_ping_pong() -> None:
    """Send Ping, receive Pong over AEAD-encrypted channel (XPORT-04, XPORT-05)."""
    identity = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, identity) as conn:
        await conn.ping()  # Should not raise
        await conn.ping()  # Second ping to verify nonce counters advance correctly
        await conn.ping()  # Third for good measure


async def test_multiple_connections() -> None:
    """Two sequential connections work with different identities."""
    id1 = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, id1) as conn:
        await conn.ping()

    id2 = Identity.generate()
    async with ChromatinClient.connect(RELAY_HOST, RELAY_PORT, id2) as conn:
        await conn.ping()


async def test_handshake_timeout() -> None:
    """Very short timeout raises HandshakeError (XPORT-07)."""
    identity = Identity.generate()
    # Use a non-routable IP to guarantee timeout (not the relay)
    with pytest.raises((HandshakeError, OSError)):
        async with ChromatinClient.connect(
            "192.0.2.1", 4433, identity, timeout=0.5
        ) as conn:
            pass
