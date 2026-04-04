# Phase 83: Bidirectional Keepalive - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.

**Date:** 2026-04-04
**Phase:** 83-bidirectional-keepalive
**Areas discussed:** Keepalive timer placement, Silence detection, Disconnect behavior

---

## Keepalive Timer Placement

| Option | Description | Selected |
|--------|-------------|----------|
| Single PeerManager coroutine | One 30s timer, iterates all peers | ✓ |
| Per-connection timer | Each Connection has own timer | |
| You decide | | |

**User's choice:** Single PeerManager coroutine

---

## Silence Detection

| Option | Description | Selected |
|--------|-------------|----------|
| Any received message | Update last-activity on all traffic | ✓ |
| Pong only | Only Pong resets silence counter | |
| You decide | | |

**User's choice:** Any received message

---

| Option | Description | Selected |
|--------|-------------|----------|
| On Connection object | last_recv_time_ in Connection, updated in message_loop | ✓ |
| On PeerInfo struct | last_activity in PeerManager | |
| You decide | | |

**User's choice:** On Connection object

---

## Disconnect Behavior

| Option | Description | Selected |
|--------|-------------|----------|
| Immediate TCP close | conn->close(), no Goodbye | ✓ |
| Attempt Goodbye then close | Send Goodbye first, then close | |
| You decide | | |

**User's choice:** Immediate TCP close

---

## Claude's Discretion

- Clock type for last_recv_time_
- Skip Ping during sync or not
- Keepalive interval configurability
- Log level for keepalive disconnect

## Deferred Ideas

None
