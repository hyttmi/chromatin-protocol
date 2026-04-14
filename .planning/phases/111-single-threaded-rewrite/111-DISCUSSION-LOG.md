# Phase 111: Single-Threaded Rewrite - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-14
**Phase:** 111-single-threaded-rewrite
**Areas discussed:** TLS handshake offloading, Offload code location, JSON offload threshold, SIGHUP under single thread, Test adaptation, Thread pool config

---

## TLS Handshake Offloading

| Option | Description | Selected |
|--------|-------------|----------|
| Inline on event loop | TLS handshake runs on event loop via Asio's ssl::stream. ~1-2ms acceptable. Same as node. | ✓ |
| Offload via raw socket transfer | Accept TCP on event loop, transfer to thread pool for SSL_do_handshake, transfer back. Complex. | |
| You decide | Claude picks pragmatic option | |

**User's choice:** Inline on event loop (Recommended)
**Notes:** No custom SSL plumbing needed. Event loop blocking during handshake is acceptable at relay scale.

---

## Offload Code Location

| Option | Description | Selected |
|--------|-------------|----------|
| Copy to relay/ | Copy thread_pool.h to relay/util/thread_pool.h under relay namespace. Self-contained. | ✓ |
| Share from db/crypto/ | Include db/crypto/thread_pool.h directly. Cross-layer dependency. | |
| Extract to shared/ | New shared/ top-level directory. Clean but adds build layer. | |

**User's choice:** Copy to relay/ (Recommended)
**Notes:** Relay stays self-contained with no cross-layer build dependency.

---

## JSON Offload Threshold

| Option | Description | Selected |
|--------|-------------|----------|
| None -- all JSON stays inline | JSON parse/serialize is fast. Only ML-DSA-87 verify worth offloading. Keeps code simple. | ✓ |
| Size threshold | Offload when body exceeds threshold (e.g., 64KB). Adds branching. | |
| Specific types only | Offload known-large operations (ListResponse, BatchReadResponse). Hardcoded list. | |

**User's choice:** None -- all JSON stays inline (Recommended)
**Notes:** Only ML-DSA-87 signature verification (~2ms) justifies the offload overhead.

---

## SIGHUP Under Single Thread

| Option | Description | Selected |
|--------|-------------|----------|
| Convert to plain members | Replace std::atomic with plain uint32_t. Same thread, no races. Documents invariant. | ✓ |
| Keep atomics (defensive) | Harmless overhead, serves as runtime-change documentation. | |
| You decide | Claude picks based on conventions | |

**User's choice:** Convert to plain members (Recommended)
**Notes:** SIGHUP coroutine runs on the same event loop thread as all readers.

---

## Test Adaptation

| Option | Description | Selected |
|--------|-------------|----------|
| Single-threaded tests | Tests use plain io_context, matching production. Remove strand params. | ✓ |
| Keep multi-threaded test harness | Tests retain strands for stress testing. | |
| You decide | Claude adapts tests to match production | |

**User's choice:** Single-threaded tests (Recommended)
**Notes:** Tests should mirror the production concurrency model.

---

## Thread Pool Config

| Option | Description | Selected |
|--------|-------------|----------|
| relay config field | Add offload_threads to relay_config.h (uint32_t, default 0). SIGHUP doesn't reload. | |
| Hardcode hardware_concurrency | No config field. Always hardware_concurrency(). Simple. | ✓ |
| You decide | Claude picks based on CONC-05 | |

**User's choice:** Hardcode hardware_concurrency
**Notes:** User chose simplicity over configurability despite CONC-05 mentioning "configurable."

---

## Claude's Discretion

- Order of refactoring within the phase
- offload() wrapper design (relay-specific vs direct template copy)
- Transfer-back pattern after offload
- Plan decomposition

## Deferred Ideas

None -- discussion stayed within phase scope.
