# Phase 84: SDK Auto-Reconnect - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.

**Date:** 2026-04-04
**Phase:** 84-sdk-auto-reconnect
**Areas discussed:** Reconnect trigger & backoff, Subscription restore, App notification API, Intentional vs unintentional close

---

## Reconnect Trigger & Backoff

| Option | Description | Selected |
|--------|-------------|----------|
| Any read/write failure | Covers both clean disconnects and network failures | ✓ |
| Read failure only | Only message loop read failure triggers reconnect | |
| You decide | | |

---

| Option | Description | Selected |
|--------|-------------|----------|
| Infinite with backoff | 1s base, 30s cap, jittered. App calls close() to stop | ✓ |
| Bounded (10 attempts) | Try N times then raise permanent failure | |
| You decide | | |

---

## Subscription Restore

| Option | Description | Selected |
|--------|-------------|----------|
| Silent re-subscribe | Re-send Subscribe after reconnect, log debug, warn on failure | ✓ |
| Fail reconnect if fails | Treat as part of reconnect, retry on failure | |
| You decide | | |

---

## App Notification API

| Option | Description | Selected |
|--------|-------------|----------|
| Callback functions | on_disconnect/on_reconnect in constructor | ✓ |
| Async event/signal | asyncio.Event or queue | |
| You decide | | |

---

## Intentional vs Unintentional Close

| Option | Description | Selected |
|--------|-------------|----------|
| Simple _closing flag | Boolean, no state machine | |
| State machine | disconnected/connecting/connected/closing states | ✓ |
| You decide | | |

---

## Claude's Discretion

- State machine implementation details
- on_disconnect timing relative to reconnect
- Pending send handling during reconnect
- Message loop lifecycle on reconnect
- wait_connected() awaitable
- Test approach

## Deferred Ideas

None
