# Phase 104: Pub/Sub & UDS Resilience - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-10
**Phase:** 104-pub-sub-uds-resilience
**Areas discussed:** Subscription Aggregation, Notification Fan-out, UDS Reconnect Lifecycle, Pending Request Cleanup
**Mode:** --auto (all areas auto-selected, recommended defaults chosen)

---

## Subscription Aggregation

| Option | Description | Selected |
|--------|-------------|----------|
| SubscriptionTracker class | Separate class in relay/core/, follows component-per-concern pattern | [auto] |

**User's choice:** [auto] New SubscriptionTracker class in relay/core/subscription_tracker.h/cpp
**Notes:** Data structure: unordered_map<Namespace32, unordered_set<uint64_t>> for namespace-to-session-ids mapping. Reference counting: Subscribe to node on first subscriber, Unsubscribe on last. 256 per-client cap matching node default. Interception in WsSession before RequestRouter.

## Notification Fan-out

| Option | Description | Selected |
|--------|-------------|----------|
| Route via SubscriptionTracker | Extract namespace from payload, look up subscribers, send JSON to each | [auto] |
| Text frames | Small notifications as text (opcode 0x1), consistent with Phase 103 D-20 | [auto] |

**User's choice:** [auto] Fan-out via SubscriptionTracker lookup + text frames for notifications
**Notes:** StorageFull/QuotaExceeded with request_id=0 broadcast to ALL sessions (operational warnings). Translate-once optimization for popular namespaces.

## UDS Reconnect Lifecycle

| Option | Description | Selected |
|--------|-------------|----------|
| Extend connect_loop | Reconnect-aware connect_loop, reset AEAD, replay subscriptions after handshake | [auto] |
| Infinite reconnect | No max attempts, relay never gives up (same-machine assumption) | [auto] |

**User's choice:** [auto] Extend existing connect_loop with AEAD reset and subscription replay
**Notes:** Clients stay connected during reconnect, receive node_unavailable for new requests. No message queuing (explicit out-of-scope per REQUIREMENTS.md).

## Pending Request Cleanup

| Option | Description | Selected |
|--------|-------------|----------|
| Bulk-fail on disconnect | Iterate all pending, send node_disconnected error, clear map | [auto] |
| Cleanup-first ordering | Fail pending -> reset AEAD -> reconnect | [auto] |

**User's choice:** [auto] Bulk-fail all pending requests with node_disconnected error, then reconnect
**Notes:** Clean state before retry. Clients re-send after UDS reconnects.

## Claude's Discretion

- SubscriptionTracker internal API design
- Translate-once optimization for notification fan-out
- Log messages and spdlog levels
- Test organization

## Deferred Ideas

None -- discussion stayed within phase scope.
