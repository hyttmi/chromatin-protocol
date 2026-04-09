---
phase: 99-sync-resource-concurrency-correctness
verified: 2026-04-09T07:55:00Z
status: gaps_found
score: 7/8 must-haves verified
gaps:
  - truth: "pending_fetches_ entries are cleaned for ALL ingest outcomes (accepted, rejected, not-found), not just accepted+ack"
    status: partial
    reason: "Not-found (status=0x01) and exception paths do not clean the pending entry immediately — they rely on disconnect cleanup via clean_pending_fetches. Accepted, rejected, and duplicate paths are unconditionally cleaned. This was an intentional design decision (SUMMARY key-decisions) because the not-found wire format carries no namespace+hash to build the composite key. REQUIREMENTS.md still marks SYNC-01 as Pending due to a merge conflict overwriting the checked state."
    artifacts:
      - path: "db/peer/blob_push_manager.cpp"
        issue: "handle_blob_fetch_response: status=0x01 (not-found) returns at line 180 without erasing pending entry. Exception catch path at line 226-231 also defers cleanup to disconnect."
    missing:
      - "REQUIREMENTS.md needs SYNC-01, SYNC-02, SYNC-03 checked [x] — merge conflict in commit 6d78e34 reverted the [x] marks from eb94a63 back to [ ]"
  - truth: "REQUIREMENTS.md correctly reflects completion status of all Phase 99 requirements"
    status: failed
    reason: "Merge commit 6d78e34 resolved wave 1 conflicts by taking the old (unchecked) SYNC-01/02/03 state instead of the updated state from eb94a63. Current HEAD REQUIREMENTS.md shows SYNC-01/02/03 as Pending when the code clearly has the fixes committed."
    artifacts:
      - path: ".planning/REQUIREMENTS.md"
        issue: "SYNC-01, SYNC-02, SYNC-03 show [ ] and Pending in traceability table. Should be [x] and Complete."
    missing:
      - "Update .planning/REQUIREMENTS.md: check SYNC-01, SYNC-02, SYNC-03 boxes and update traceability table to Complete"
---

# Phase 99: Sync, Resource & Concurrency Correctness Verification Report

**Phase Goal:** Sync state is leak-free, resource limits are race-free, and coroutine counters are safe across co_await boundaries
**Verified:** 2026-04-09T07:55:00Z
**Status:** gaps_found
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | pending_fetches_ uses 64-byte namespace\|\|hash composite key (SYNC-02) | VERIFIED | `array<uint8_t, 64>` key with `ArrayHash64` in blob_push_manager.h:79; `make_pending_key` used in on_blob_notify:128-130 and handle_blob_fetch_response:200 |
| 2 | pending_fetches_ entries cleaned on accepted/rejected/duplicate ingests (SYNC-01 core cases) | VERIFIED | blob_push_manager.cpp:195-201: unconditional `pending_fetches_.erase(pending_key)` before result branch |
| 3 | pending_fetches_ not-found and exception paths defer to disconnect cleanup | PARTIAL | Lines 176-180: status=0x01 returns without erase. Lines 226-231: exception path no erase. Intentional — no composite key available. clean_pending_fetches handles these on disconnect. |
| 4 | SYNC-03: collect_namespace_hashes has MDBX MVCC snapshot consistency documentation | VERIFIED | sync_protocol.cpp:21-27: block comment explaining MDBX MVCC safety for hash snapshot |
| 5 | Subscribe handler enforces per-connection subscription limit with rejection (RES-01) | VERIFIED | message_dispatcher.cpp:226-239: `max_subscriptions_` check + QuotaExceeded rejection via co_spawn |
| 6 | Bootstrap detection compares full host:port endpoint (RES-02) | VERIFIED | connection_manager.cpp:98-104: `if (bp == info.address)` replaces old host-only substr comparison |
| 7 | Capacity and quota checks inside store_blob write transaction eliminate TOCTOU (RES-03) | VERIFIED | storage.cpp:392-418: atomic checks inside write txn; engine.cpp:282-286: passes limits to store_blob |
| 8 | Quota rebuild iterator correctly erases all entries (RES-04) | VERIFIED | storage.cpp:1419-1426: erase-restart-from-first pattern |
| 9 | All NodeMetrics increments are strand-confined to io_context (CORO-01) | VERIFIED | peer_types.h:57-64: updated comment with per-file verification audit; TSAN run confirmed zero data races |

**Score:** 7/8 truths verified (SYNC-01 partial — not-found path defers cleanup to disconnect by design)

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/peer/peer_types.h` | ArrayHash64 functor, CORO-01 documentation | VERIFIED | ArrayHash64 at line 88-95; NodeMetrics comment updated with strand confinement evidence at lines 57-64 |
| `db/peer/blob_push_manager.h` | 64-byte key type, make_pending_key helper | VERIFIED | make_pending_key inline free function at lines 22-29; pending_fetches_ uses `array<uint8_t,64>` with ArrayHash64 at line 79 |
| `db/peer/blob_push_manager.cpp` | Unconditional erase, composite key usage | VERIFIED (partial) | on_blob_notify uses make_pending_key (line 128); handle_blob_fetch_response unconditionally erases for found+decoded responses (line 201); not-found defers to disconnect |
| `db/sync/sync_protocol.cpp` | MDBX MVCC snapshot documentation | VERIFIED | Lines 21-27 contain SYNC-03 comment explaining MDBX snapshot isolation safety |
| `db/tests/peer/test_blob_push.cpp` | Unit tests, 50+ lines | VERIFIED | 185 lines, 8 test cases covering make_pending_key correctness, ArrayHash64 functor, cross-namespace collision prevention, clean_pending_fetches pattern |
| `db/peer/message_dispatcher.cpp` | Subscription limit enforcement | VERIFIED | Lines 226-239: max_subscriptions_ check with QuotaExceeded rejection |
| `db/peer/connection_manager.cpp` | Full endpoint bootstrap comparison | VERIFIED | Lines 98-104: `bp == info.address` direct string comparison |
| `db/storage/storage.cpp` | Atomic capacity/quota check, fixed rebuild iterator | VERIFIED | Lines 392-418: capacity/quota inside write txn; lines 1419-1426: erase-restart-from-first |
| `db/storage/storage.h` | CapacityExceeded/QuotaExceeded statuses, 6-param store_blob | VERIFIED | Lines 22-24: new statuses; lines 126-131: new overload |
| `db/engine/engine.cpp` | Passes limits to store_blob, handles new statuses | VERIFIED | Lines 282-286: passes limits; lines 309-314: CapacityExceeded/QuotaExceeded switch cases |
| `db/config/config.h` | max_subscriptions_per_connection field | VERIFIED | Line 21: `uint32_t max_subscriptions_per_connection = 256` |
| `db/peer/message_dispatcher.h` | max_subscriptions_ member | VERIFIED | Line 86: `uint32_t max_subscriptions_ = 256` with set_max_subscriptions setter |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| blob_push_manager.cpp:on_blob_notify | pending_fetches_ | make_pending_key(ns, hash) insert | WIRED | Line 128-130: `make_pending_key(ns, hash)` → `pending_fetches_.emplace` |
| blob_push_manager.cpp:handle_blob_fetch_response | pending_fetches_ | unconditional erase before result check | WIRED | Lines 195-201: erase inside block before `if (result.accepted ...)` |
| engine.cpp:Step 4 | storage.cpp:store_blob | passes max_storage_bytes and quota limits | WIRED | Lines 282-286: `storage_.store_blob(blob, content_hash, encoded, ..., max_storage_bytes_, byte_limit, count_limit)` |
| message_dispatcher.cpp:Subscribe | PeerInfo::subscribed_namespaces | size check before insert | WIRED | Lines 226-228: `max_subscriptions_ > 0 && peer->subscribed_namespaces.size() + namespaces.size() > max_subscriptions_` |
| message_dispatcher.cpp:Subscribe | conn->send_message(QuotaExceeded) | co_spawn rejection on limit breach | WIRED | Lines 233-239: `asio::co_spawn(ioc_, [conn, request_id]() -> ... co_await conn->send_message(wire::TransportMsgType_QuotaExceeded...)` |

### Data-Flow Trace (Level 4)

Not applicable — this phase fixes correctness bugs in C++ server components (dedup maps, storage limits, documentation). No rendering of dynamic UI data.

### Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| Phase 99 unit tests (sync, storage, engine tags) | `ASAN_OPTIONS=detect_leaks=0 ./chromatindb_tests "[sync][correctness],[storage][resource],[engine][resource]"` | All 13 test cases, 102 assertions pass | PASS |
| Bootstrap peer test | `ASAN_OPTIONS=detect_leaks=0 ctest -R "Server connects to bootstrap peer"` | Passed (7.03s) | PASS |
| Build (no sanitizers) | `cmake --build build/` | 100% built, zero errors | PASS |
| ctest with LSAN disabled | `ASAN_OPTIONS=detect_leaks=0 ctest` | All non-liboqs tests pass | PASS (pre-existing liboqs LSAN noise only) |

**Note on ctest failures:** Running `ctest` without `ASAN_OPTIONS=detect_leaks=0` shows 23 failures. Every single failing test passes when liboqs LeakSanitizer reports are suppressed — the test logic succeeds but LSAN counts liboqs's own allocations as leaks. This is a pre-existing issue predating Phase 99, confirmed in the 99-03-SUMMARY.md sanitizer gate section.

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| SYNC-01 | 99-01-PLAN.md | pending_fetches_ entries cleaned on rejected ingest | PARTIAL | Accepted/rejected/duplicate paths cleaned unconditionally. Not-found and exception paths defer to disconnect. REQUIREMENTS.md incorrectly shows Pending (merge conflict). |
| SYNC-02 | 99-01-PLAN.md | 64-byte namespace+hash composite key | SATISFIED | blob_push_manager.h uses `array<uint8_t,64>` with ArrayHash64. REQUIREMENTS.md incorrectly shows Pending. |
| SYNC-03 | 99-01-PLAN.md | Phase B snapshot consistency | SATISFIED | sync_protocol.cpp lines 21-27 document MDBX MVCC safety. No code change needed. REQUIREMENTS.md incorrectly shows Pending. |
| RES-01 | 99-02-PLAN.md | Per-peer subscription count limit at node level | SATISFIED | message_dispatcher.cpp: limit check + QuotaExceeded rejection. Config field max_subscriptions_per_connection added. |
| RES-02 | 99-02-PLAN.md | Bootstrap peer detection considers port | SATISFIED | connection_manager.cpp: full endpoint comparison `bp == info.address` |
| RES-03 | 99-02-PLAN.md | TOCTOU race eliminated with atomic check-and-reserve | SATISFIED | store_blob checks capacity and quota inside write transaction. engine.cpp passes limits. |
| RES-04 | 99-02-PLAN.md | Quota rebuild clear loop iterator bug fixed | SATISFIED | storage.cpp: erase-restart-from-first pattern |
| CORO-01 | 99-03-PLAN.md | Send/recv counters safe across co_await boundaries | SATISFIED | All 20+ NodeMetrics increment sites verified strand-confined. TSAN: zero data races. |

**REQUIREMENTS.md state mismatch:** SYNC-01, SYNC-02, SYNC-03 are marked `[ ]` Pending in the current HEAD REQUIREMENTS.md. This is a documentation error caused by merge commit `6d78e34` resolving wave 1 parallel agent conflicts by discarding the `[x]` updates from commit `eb94a63`. The code implements all three fixes.

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| db/peer/blob_push_manager.cpp | 176-180 | Not-found response returns without pending entry cleanup | Info | Entry leaks until disconnect. Intentional design decision — no composite key buildable from not-found wire format. Documented in SUMMARY key-decisions. |
| db/peer/blob_push_manager.cpp | 226-231 | Exception path no pending cleanup | Info | Same as above — no blob data to build composite key. |
| .planning/REQUIREMENTS.md | Lines 25-29, 82-84 | SYNC-01/02/03 still marked `[ ]` Pending | Warning | Documentation drift from merge conflict. Does not reflect actual implementation state. |

### Human Verification Required

None — all behaviors are verifiable via code inspection and test execution.

### Gaps Summary

**One code gap (partial implementation), one documentation gap:**

1. **SYNC-01 partial (not-found path):** The not-found BlobFetchResponse case (status=0x01) does not immediately clean the pending_fetches_ entry — it logs and returns, relying on disconnect cleanup via `clean_pending_fetches`. This was an explicit design decision because the not-found wire format carries no namespace+hash data to reconstruct the 64-byte composite key. The main cases (accepted, rejected, duplicate) are fully fixed with unconditional erase. This is acceptable per the plan's key-decisions but makes SYNC-01 technically partial.

2. **REQUIREMENTS.md documentation drift:** Merge commit `6d78e34` (parallel wave 1 agent conflict resolution) discarded the SYNC-01/02/03 `[x]` marks from `eb94a63`. Current HEAD shows these three requirements as Pending when the code (since commit `aa706c1`) has the fixes. A simple one-line fix per requirement in REQUIREMENTS.md is needed.

The documentation gap does not affect the codebase — all code changes are present, correct, and tested. Fixing REQUIREMENTS.md is a single-commit documentation update.

---

_Verified: 2026-04-09T07:55:00Z_
_Verifier: Claude (gsd-verifier)_
