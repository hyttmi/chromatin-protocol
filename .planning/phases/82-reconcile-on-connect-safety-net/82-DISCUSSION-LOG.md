# Phase 82: Reconcile-on-Connect & Safety Net - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-04
**Phase:** 82-reconcile-on-connect-safety-net
**Areas discussed:** Cursor grace period, Safety net interval, Connect reconciliation order, Config migration

---

## Cursor Grace Period

| Option | Description | Selected |
|--------|-------------|----------|
| SyncProtocol cursor state per namespace | Preserve per-namespace seq_num cursors keyed by pubkey hash | ✓ |
| Just peer pubkey + timestamp | Track disconnect time, skip reconciliation on reconnect | |
| You decide | | |

**User's choice:** SyncProtocol cursor state per namespace

---

| Option | Description | Selected |
|--------|-------------|----------|
| Lazy cleanup on reconnect check | Store disconnect timestamp, check on reconnect, periodic scan for stale | ✓ |
| Per-peer timer | 5-min timer per disconnected peer | |
| You decide | | |

**User's choice:** Lazy cleanup on reconnect check

---

## Safety Net Interval

| Option | Description | Selected |
|--------|-------------|----------|
| Sync all peers each cycle | Every 600s, run sync_all_peers() | ✓ |
| Rotate one peer per cycle | Spread load across intervals | |
| You decide | | |

**User's choice:** Sync all peers each cycle

---

| Option | Description | Selected |
|--------|-------------|----------|
| Bypass cooldown for safety net | Always runs regardless of cooldown | ✓ |
| Respect cooldown | Skip recently-synced peers | |
| You decide | | |

**User's choice:** Bypass cooldown for safety net

---

## Connect Reconciliation Order

| Option | Description | Selected |
|--------|-------------|----------|
| Use existing syncing flag | Phase 79 storm suppression already gates BlobNotify | ✓ |
| Explicit sync_complete flag | Separate has_initial_sync_completed flag per peer | |
| You decide | | |

**User's choice:** Use existing syncing flag

---

| Option | Description | Selected |
|--------|-------------|----------|
| Keep initiator-only | Current pattern unchanged | ✓ |
| Both sides trigger | Both initiator and responder trigger sync-on-connect | |
| You decide | | |

**User's choice:** Keep initiator-only

---

## Config Migration

| Option | Description | Selected |
|--------|-------------|----------|
| Rename in-place, no backward compat | Replace sync_interval_seconds with safety_net_interval_seconds, default 600s | ✓ |
| Accept both, prefer new | Parse both names, log deprecation | |
| You decide | | |

**User's choice:** Rename in-place, no backward compat

---

## Claude's Discretion

- Cursor state data structure and extraction/restore mechanism
- Safety-net coroutine structure (reuse or new)
- Stale cursor cleanup details
- Validation rules for new config field

## Deferred Ideas

None
