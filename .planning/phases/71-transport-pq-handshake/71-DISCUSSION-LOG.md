# Phase 71: Transport & PQ Handshake - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-03-29
**Phase:** 71-transport-pq-handshake
**Areas discussed:** Connection API surface, Async/sync model, Frame reader architecture, Integration testing

---

## Connection API surface

| Option | Description | Selected |
|--------|-------------|----------|
| Context manager (Recommended) | async with ChromatinClient.connect(host, port, identity) as conn: -- handshake on enter, Goodbye+close on exit | ✓ |
| Explicit connect/close | client = ChromatinClient(host, port, identity); await client.connect(); ... await client.close() | |
| Both (context manager + explicit) | Support both patterns for different use cases | |

**User's choice:** Context manager
**Notes:** Clean resource management, Pythonic pattern

| Option | Description | Selected |
|--------|-------------|----------|
| ChromatinClient (Recommended) | Matches package name. chromatindb.ChromatinClient | ✓ |
| Connection | Lower-level transport-focused name | |
| Client | Short, generic, potential name collision | |

**User's choice:** ChromatinClient

| Option | Description | Selected |
|--------|-------------|----------|
| High-level only (Recommended) | Only domain operations exposed, transport internals private | ✓ |
| Both layers | Public high-level + underscore-prefixed transport methods | |
| You decide | Claude's discretion | |

**User's choice:** High-level only

| Option | Description | Selected |
|--------|-------------|----------|
| Only what works (Recommended) | Phase 71 exposes connect, disconnect, ping/pong only | ✓ |
| Stub all methods | All 38 operations defined with NotImplementedError | |
| You decide | Claude's discretion | |

**User's choice:** Only what works

---

## Async/sync model

| Option | Description | Selected |
|--------|-------------|----------|
| asyncio streams (Recommended) | asyncio.open_connection(), native Python, zero dependencies | ✓ |
| Trio/anyio | anyio abstracts over asyncio/trio, adds dependency | |
| You decide | Claude picks simplest approach | |

**User's choice:** asyncio streams

| Option | Description | Selected |
|--------|-------------|----------|
| Defer to Phase 74 (Recommended) | Phase 71 async-only, sync wrapper added during packaging | ✓ |
| Build in Phase 71 | Add SyncChromatinClient or sync=True flag now | |
| Never -- async only | SDK is async-only forever | |

**User's choice:** Defer to Phase 74

| Option | Description | Selected |
|--------|-------------|----------|
| Handshake timeout only (Recommended) | Default 10s handshake timeout, configurable. HandshakeError on timeout. | ✓ |
| Both connect + handshake | Separate TCP connect timeout and handshake timeout | |
| You decide | Claude picks sensible defaults | |

**User's choice:** Handshake timeout only

---

## Frame reader architecture

| Option | Description | Selected |
|--------|-------------|----------|
| Background reader (Recommended) | Spawn reader coroutine post-handshake, dispatch by request_id to futures + notification queue | ✓ |
| Sequential reads | Simple send-then-recv, no background task | |
| Minimal reader + hooks | Background reader with basic callback dispatch, no routing | |

**User's choice:** Background reader

| Option | Description | Selected |
|--------|-------------|----------|
| Transport auto-assigns (Recommended) | Internal counter increments per request, callers never see request_id | ✓ |
| Caller-assigned | User passes request_id, matches C++ node behavior | |
| You decide | Claude picks cleanest pattern | |

**User's choice:** Transport auto-assigns

| Option | Description | Selected |
|--------|-------------|----------|
| Cancel all pending + raise (Recommended) | Reader cancels all pending futures with ConnectionError, sets closed state | ✓ |
| Silent close + error on next op | Reader stops quietly, error surfaces on next operation | |
| You decide | Claude picks clearest diagnostics | |

**User's choice:** Cancel all pending + raise

---

## Integration testing

| Option | Description | Selected |
|--------|-------------|----------|
| Docker compose (Recommended) | Spin up node + relay in Docker, self-contained and CI-friendly | |
| KVM swarm (192.168.1.200) | Test against live 3-node KVM swarm, already running | ✓ |
| Both Docker + KVM | Docker for CI, KVM for manual smoke testing | |
| Mock relay in Python | Pure-Python mock implementing handshake protocol | |

**User's choice:** KVM swarm

| Option | Description | Selected |
|--------|-------------|----------|
| Both unit + integration (Recommended) | Unit tests for framing/nonce/AEAD, integration for full handshake | ✓ |
| Integration only | Test everything against live relay | |
| You decide | Claude picks the split | |

**User's choice:** Both unit + integration

| Option | Description | Selected |
|--------|-------------|----------|
| Environment variable (Recommended) | CHROMATINDB_RELAY_HOST/PORT env vars with 192.168.1.200:4433 defaults, skip if unreachable | ✓ |
| Hardcoded defaults | 192.168.1.200:4433 baked in | |
| pytest config file | conftest.py reads from pyproject.toml [tool.pytest] | |

**User's choice:** Environment variable

---

## Claude's Discretion

- Internal module organization (transport.py vs splitting into handshake.py + framing.py + client.py)
- Notification queue implementation details
- Ping/pong implementation and keepalive behavior
- pytest fixture structure for integration tests
- Internal error handling granularity

## Deferred Ideas

None -- discussion stayed within phase scope
