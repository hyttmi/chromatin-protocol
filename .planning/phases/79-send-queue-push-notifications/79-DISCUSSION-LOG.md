# Phase 79: Send Queue & Push Notifications - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-02
**Phase:** 79-send-queue-push-notifications
**Areas discussed:** Send queue scope, Slow peer policy, Storm suppression semantics, Notification trigger path

---

## Send Queue Scope

| Option | Description | Selected |
|--------|-------------|----------|
| All messages (Recommended) | Every send_message() call goes through the queue. Fixes existing AEAD nonce race for pub/sub too. One serialization point, no special-casing. | ✓ |
| Notifications only | Only BlobNotify messages are queued. Other send paths keep direct co_await pattern. Leaves existing nonce race unfixed. | |
| All except handshake | Queue all post-handshake messages. Handshake messages stay direct since they run before message loop. | |

**User's choice:** All messages (Recommended)
**Notes:** None

### Follow-up: Queue API

| Option | Description | Selected |
|--------|-------------|----------|
| Internal (Recommended) | send_message() becomes non-blocking: enqueues and returns. Dedicated drain coroutine serializes writes. All existing callers work unchanged. | ✓ |
| Explicit enqueue | New enqueue_message() method. send_message() stays synchronous. Callers choose which path. | |
| You decide | Claude chooses based on codebase patterns. | |

**User's choice:** Internal (Recommended)
**Notes:** None

### Follow-up: Await model

| Option | Description | Selected |
|--------|-------------|----------|
| Awaitable (Recommended) | send_message() enqueues and co_awaits until drain coroutine has written to socket. Caller gets backpressure signal. | ✓ |
| Fire-and-forget | send_message() enqueues and returns true immediately. Loses error propagation. | |
| Both modes | send_message() awaitable + send_message_queued() for fan-out. | |

**User's choice:** Awaitable (Recommended)
**Notes:** None

---

## Slow Peer Policy

| Option | Description | Selected |
|--------|-------------|----------|
| Bounded + disconnect (Recommended) | Cap queue (e.g., 1000 messages). Peer falls behind → disconnect. Recovers via reconcile-on-connect (Phase 82). Bounded memory. | ✓ |
| Unbounded queue | Queue grows without limit. Relies on TCP backpressure. Risks memory exhaustion. | |
| Bounded + drop oldest | Drop oldest queued message when full. Peer stays connected but may miss notifications. | |

**User's choice:** Bounded + disconnect (Recommended)
**Notes:** None

### Follow-up: Queue limit

| Option | Description | Selected |
|--------|-------------|----------|
| Message count: 1024 | Simple count-based cap. At 77 bytes per notification, 1024 = ~77 KiB. | ✓ |
| Byte-based: 10 MiB | Cap by total queued bytes. More precise memory control. | |
| You decide | Claude picks reasonable defaults. | |

**User's choice:** Message count: 1024
**Notes:** None

---

## Storm Suppression Semantics

| Option | Description | Selected |
|--------|-------------|----------|
| Suppress to syncing peer only (Recommended) | Don't send BlobNotify back to sync source. DO notify all other peers. Source exclusion + storm suppression combined. | ✓ |
| Suppress to ALL peers during sync | While any sync is active, suppress ALL BlobNotify for sync-received blobs. Delays propagation. | |
| No sync-specific suppression | Source exclusion (PUSH-07) already handles it. No additional suppression. | |

**User's choice:** Suppress to syncing peer only (Recommended)
**Notes:** None

### Follow-up: Client writes during sync

| Option | Description | Selected |
|--------|-------------|----------|
| Only sync-received blobs (Recommended) | Client-written blobs are NOT suppressed during sync. Only blobs from sync source suppressed back to that source. | ✓ |
| All blobs to syncing peer | During sync with peer A, suppress ALL BlobNotify to A. Simpler but slower propagation. | |

**User's choice:** Only sync-received blobs (Recommended)
**Notes:** None

---

## Notification Trigger Path

| Option | Description | Selected |
|--------|-------------|----------|
| Engine callback (Recommended) | BlobEngine::ingest() fires on_blob_ingested on every successful ingest. PeerManager registers callback. Single hook point. | ✓ |
| PeerManager dispatch | Add notification calls in both Data handler and sync ingest path. Two call sites. | |
| You decide | Claude chooses cleanest architecture. | |

**User's choice:** Engine callback (Recommended)
**Notes:** None

### Follow-up: Source parameter

| Option | Description | Selected |
|--------|-------------|----------|
| Optional source pointer (Recommended) | ingest() takes optional Connection::Ptr source (nullptr for client writes). Callback passes it through for fan-out exclusion. | ✓ |
| Separate callback context | Callback carries context struct. Engine doesn't know about connections. | |
| You decide | Claude decides how to thread source identity. | |

**User's choice:** Optional source pointer (Recommended)
**Notes:** None

### Follow-up: Unify with pub/sub

| Option | Description | Selected |
|--------|-------------|----------|
| Unify (Recommended) | One engine callback for both BlobNotify (peers) and Notification (subscribed clients). Single trigger point. | ✓ |
| Keep separate | BlobNotify uses engine callback. Pub/sub keeps sync_protocol callback. Two systems coexist. | |

**User's choice:** Unify (Recommended)
**Notes:** None

---

## Claude's Discretion

- Queue data structure choice
- Drain coroutine lifecycle
- Handshake message bypass mechanism
- Error handling when queue is full
- Refactoring of existing notify_subscribers()

## Deferred Ideas

None — discussion stayed within phase scope
