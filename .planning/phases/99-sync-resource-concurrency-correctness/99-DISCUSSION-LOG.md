# Phase 99: Sync, Resource & Concurrency Correctness - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-09
**Phase:** 99-sync-resource-concurrency-correctness
**Areas discussed:** sync cleanup, Phase B snapshot, subscription limits, bootstrap/TOCTOU/quota bugs, coroutine counters
**Mode:** auto (all recommended defaults selected)

---

## Sync State Cleanup (SYNC-01, SYNC-02)

| Option | Description | Selected |
|--------|-------------|----------|
| Composite key array<64> | namespace||hash concatenation, new ArrayHash64 | |
| std::pair<array<32>,array<32>> | pair key with custom hash | |

**User's choice:** [auto] Composite key array<uint8_t,64> — simplest, single contiguous allocation
**Notes:** pending_fetches cleanup must cover rejected ingests too, not just accepted

---

## Phase B Snapshot Consistency (SYNC-03)

| Option | Description | Selected |
|--------|-------------|----------|
| Verify MDBX read txn scope | Check if collect_namespace_hashes is already snapshot-isolated | |
| Add explicit read txn | Wrap in MDBX read transaction if not already | |

**User's choice:** [auto] Verify first, add explicit txn only if needed — MDBX MVCC likely already provides this
**Notes:** collect_namespace_hashes returns by value, so the vector is a copy regardless

---

## Subscription Limit (RES-01)

| Option | Description | Selected |
|--------|-------------|----------|
| 256 per connection (configurable) | Matches relay Phase 88 cap, reject with error | |
| Unlimited (no limit) | Trust clients, no enforcement | |

**User's choice:** [auto] 256 per connection, configurable, with rejection message

---

## Bootstrap/TOCTOU/Quota (RES-02, RES-03, RES-04)

**User's choice:** [auto] All recommended defaults — host+port comparison, atomic check-and-reserve, erase-returns-next pattern

---

## Coroutine Counter Safety (CORO-01)

| Option | Description | Selected |
|--------|-------------|----------|
| Verify strand confinement | Check all increment sites are on io_context strand | |
| Use std::atomic | Paper over with atomics | |

**User's choice:** [auto] Verify strand confinement — fix the design, don't paper over
**Notes:** NodeMetrics documents single-thread assumption. TSAN is the acceptance gate.

---

## Claude's Discretion

- Implementation order within categories
- Error message text for subscription limit rejection
- Debug logging for each fix (recommended: yes)

## Deferred Ideas

None
