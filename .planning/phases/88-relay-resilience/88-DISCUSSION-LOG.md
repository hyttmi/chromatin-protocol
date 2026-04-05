# Phase 88: Relay Resilience - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-05
**Phase:** 88-relay-resilience
**Areas discussed:** UDS reconnection lifecycle, subscription tracking & filtering, message handling during reconnection, dead state & client fate
**Mode:** --auto (all decisions auto-selected)

---

## UDS Reconnection Lifecycle

| Option | Description | Selected |
|--------|-------------|----------|
| Per-session UDS reconnect | Each RelaySession reconnects its own UDS independently | ✓ |
| Shared UDS | Single UDS connection shared by all clients, relay multiplexes | |

**User's choice:** [auto] Per-session UDS reconnect (recommended — matches existing architecture)
**Notes:** Prior decision in STATE.md: "Relay auto-reconnect: ACTIVE/RECONNECTING/DEAD state machine, new socket per attempt"

| Option | Description | Selected |
|--------|-------------|----------|
| Jittered exponential (1s/30s) | Full jitter, matches SDK Phase 84 pattern | ✓ |
| Fixed delay | Simple but thundering herd on node restart | |
| Linear backoff | Progressive but unbounded | |

**User's choice:** [auto] Jittered exponential 1s base, 30s cap (recommended — consistent with SDK)

---

## Subscription Tracking & Filtering

| Option | Description | Selected |
|--------|-------------|----------|
| Per-session unordered_set | 256 namespace cap, cleaned up with session | ✓ |
| Central SubscriptionManager | Shared tracking, more complex | |

**User's choice:** [auto] Per-session tracking (recommended — matches milestone planning decision)

| Option | Description | Selected |
|--------|-------------|----------|
| Intercept in handle_client_message | Before forwarding, parse and track | ✓ |
| Separate filter layer | New abstraction | |

**User's choice:** [auto] Intercept in handle_client_message (recommended — simplest, existing intercept point)

| Option | Description | Selected |
|--------|-------------|----------|
| Parse first 32 bytes of Notification | namespace_id at offset 0 per encode_notification | ✓ |
| Full FlatBuffer parse | Heavier, unnecessary | |

**User's choice:** [auto] First 32 bytes (recommended — matches wire format)

---

## Message Handling During Reconnection

| Option | Description | Selected |
|--------|-------------|----------|
| Drop silently | Client SDK handles timeouts, simple | ✓ |
| Queue in bounded deque | Memory overhead, drain after reconnect | |
| Send error response | Relay would need to craft node responses | |

**User's choice:** [auto] Drop silently (recommended — SDK has request_id timeouts)

---

## Dead State & Client Fate

| Option | Description | Selected |
|--------|-------------|----------|
| 10 attempts (~5 min) | Reasonable timeout, SDK reconnects to relay after | ✓ |
| Unlimited retries | Never gives up, could hold zombie sessions | |
| 3 attempts (~30s) | Too aggressive for longer node restarts | |

**User's choice:** [auto] 10 attempts (recommended — balances patience and resource cleanup)

---

## Claude's Discretion

- State machine implementation (enum class, transitions)
- Timer/backoff mechanics (asio::steady_timer)
- SubscriptionTracker helper class or inline
- Test strategy
- Edge case: Subscribe during replay_pending_

## Deferred Ideas

None
