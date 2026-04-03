---
phase: 81-event-driven-expiry
verified: 2026-04-03T16:00:00Z
status: passed
score: 9/9 must-haves verified
re_verification: false
---

# Phase 81: Event-Driven Expiry Verification Report

**Phase Goal:** Expired blobs are purged at exactly the right time instead of waiting for a periodic scan
**Verified:** 2026-04-03T16:00:00Z
**Status:** passed
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

Truths are drawn from ROADMAP.md success criteria and both PLAN must_haves sections.

| #  | Truth                                                                                                     | Status     | Evidence |
|----|-----------------------------------------------------------------------------------------------------------|------------|----------|
| 1  | `get_earliest_expiry()` returns the earliest expiry timestamp from storage in O(1)                        | VERIFIED   | `storage.cpp:1135` — read-only MDBX transaction, `cursor.to_first()` on expiry_map |
| 2  | `get_earliest_expiry()` returns nullopt when no expiring blobs exist                                      | VERIFIED   | `storage.cpp:1140` — `if (!first.done) return std::nullopt` |
| 3  | `get_earliest_expiry()` reflects changes after `run_expiry_scan()` purges entries                         | VERIFIED   | Test case `[storage][earliest-expiry]` "updates after scan" at `test_storage.cpp:485` |
| 4  | Expiry timer fires at the exact second of the earliest blob's expiry time, not up to 60s later            | VERIFIED   | `expiry_scan_loop` at `peer_manager.cpp:2886` computes duration = target - now; no fixed 60s interval |
| 5  | After purging expired blobs, the timer automatically rearms to the next earliest expiry                   | VERIFIED   | `peer_manager.cpp:2913-2920` calls `get_earliest_expiry()` post-scan and sets `next_expiry_target_` |
| 6  | Ingesting a blob with a shorter TTL than the current timer target causes immediate rearm                  | VERIFIED   | `peer_manager.cpp:3028-3036` in `on_blob_ingested`: cancels timer when `expiry_time < next_expiry_target_` |
| 7  | On startup with existing expiring blobs, the timer is armed to the earliest expiry                        | VERIFIED   | `peer_manager.cpp:246-250` in `start()`: calls `get_earliest_expiry()`, sets target, spawns loop |
| 8  | When no expiring blobs exist, no timer is armed                                                           | VERIFIED   | `start()` only spawns loop when `earliest.has_value()`; loop exits when `next_expiry_target_ == 0` |
| 9  | PeerManager event-driven expiry behavior is covered by automated unit tests                               | VERIFIED   | `db/tests/peer/test_event_expiry.cpp` — 5 TEST_CASEs tagged `[event-expiry]` |

**Score:** 9/9 truths verified

---

### Required Artifacts

#### Plan 01 Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/storage/storage.h` | `get_earliest_expiry()` declaration | VERIFIED | Line 208: `std::optional<uint64_t> get_earliest_expiry() const;` |
| `db/storage/storage.cpp` | `get_earliest_expiry()` implementation using MDBX cursor | VERIFIED | Lines 1135-1151: read-only txn, cursor.to_first(), decode_be_u64 |
| `db/tests/storage/test_storage.cpp` | Unit tests tagged `[storage][earliest-expiry]` | VERIFIED | 4 TEST_CASEs at lines 451, 457, 475, 485 |

#### Plan 02 Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/peer/peer_manager.h` | `next_expiry_target_` and `expiry_loop_running_` members, updated `on_blob_ingested` signature | VERIFIED | Line 203: `uint64_t expiry_time` param; lines 343-344: new members; `on_blob_ingested` in public section |
| `db/peer/peer_manager.cpp` | Rewritten `expiry_scan_loop`, updated `on_blob_ingested`, updated `start()` | VERIFIED | Lines 2886-2923 (loop), 2992+ (on_blob_ingested), 245-250 (start) |
| `db/sync/sync_protocol.h` | Updated `OnBlobIngested` typedef with `expiry_time` parameter | VERIFIED | Line 67: `uint64_t expiry_time` in OnBlobIngested typedef |
| `db/sync/sync_protocol.cpp` | Updated callback invocation with `expiry_time` | VERIFIED | Lines 88-97: computes and passes `expiry_time` |
| `db/config/config.h` | Deprecation comment on `expiry_scan_interval_seconds` | VERIFIED | Line 46: `// Deprecated (v2.0.0): ignored, expiry is event-driven.` |
| `db/tests/peer/test_event_expiry.cpp` | 5 unit tests tagged `[event-expiry]` | VERIFIED | 5 TEST_CASEs at lines 31, 79, 136, 199, 228 |
| `db/CMakeLists.txt` | `test_event_expiry.cpp` registered in test sources | VERIFIED | Line 232: `tests/peer/test_event_expiry.cpp` |

---

### Key Link Verification

#### Plan 01 Key Links

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `db/storage/storage.cpp` | `impl_->expiry_map` | `cursor.to_first()` | WIRED | Line 1139: `cursor.to_first(false)` in `get_earliest_expiry()` |

#### Plan 02 Key Links

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `peer_manager.cpp (on_blob_ingested)` | `expiry_timer_->cancel()` | Timer rearm when new blob expires sooner | WIRED | Lines 3031-3033: cancel when loop running and shorter expiry |
| `peer_manager.cpp (expiry_scan_loop)` | `storage_.get_earliest_expiry()` | Post-scan rearm query | WIRED | Line 2914: called after `run_expiry_scan()` |
| `peer_manager.cpp (start)` | `storage_.get_earliest_expiry()` | Initial timer arm on startup | WIRED | Line 246: first call in startup sequence |
| `peer_manager.cpp (on_blob_ingested)` | `asio::co_spawn(ioc_, expiry_scan_loop())` | Spawn loop when transitioning from disarmed to armed | WIRED | Line 3036: spawns when `!expiry_loop_running_` |
| `db/tests/peer/test_event_expiry.cpp` | PeerManager event-driven expiry | Catch2 tests | WIRED | 5 tests exercise timer fires, chain rearm, ingest rearm |

---

### Data-Flow Trace (Level 4)

The expiry system produces side effects (blob purges), not rendered UI data. Data flow is verified by tracing the full chain from storage to timer to purge:

| Component | Data Variable | Source | Produces Real Effect | Status |
|-----------|---------------|--------|----------------------|--------|
| `get_earliest_expiry()` | return value | MDBX expiry_map cursor read | Real DB query | FLOWING |
| `expiry_scan_loop` | `next_expiry_target_` | `get_earliest_expiry()` result | Set from real storage value | FLOWING |
| `start()` | `next_expiry_target_` | `get_earliest_expiry()` result | Primes loop with real expiry | FLOWING |
| `on_blob_ingested()` | `expiry_time` | Computed `blob.timestamp + blob.ttl` at all 4 call sites | Real blob data | FLOWING |
| `expiry_scan_loop` | purge count | `storage_.run_expiry_scan()` | Real DB deletions | FLOWING |

---

### Behavioral Spot-Checks

| Behavior | Check | Status |
|----------|-------|--------|
| `get_earliest_expiry()` declaration present | `grep "std::optional<uint64_t> get_earliest_expiry" db/storage/storage.h` | PASS — line 208 |
| `get_earliest_expiry()` implementation present | `grep -c "Storage::get_earliest_expiry" db/storage/storage.cpp` = 2 | PASS — lines 1135, 1148 |
| 4 earliest-expiry test cases exist | `grep -c "earliest-expiry" db/tests/storage/test_storage.cpp` = 4 | PASS |
| `next_expiry_target_` member variable | `grep "next_expiry_target_" db/peer/peer_manager.h` | PASS — line 343 |
| `expiry_loop_running_` member variable | `grep "expiry_loop_running_" db/peer/peer_manager.h` | PASS — line 344 |
| `expiry_time` in `OnBlobIngested` | `grep "uint64_t expiry_time" db/sync/sync_protocol.h` | PASS — line 67 |
| 5 event-expiry test cases | `grep -c "[event-expiry]" db/tests/peer/test_event_expiry.cpp` = 5 | PASS |
| `test_event_expiry.cpp` in CMakeLists.txt | `grep "test_event_expiry" db/CMakeLists.txt` | PASS — line 232 |
| `expiry_scan_interval_seconds_` absent from `expiry_scan_loop` | present only in constructor init (line 93) and SIGHUP reload (lines 2836-2837) | PASS — not inside the loop |
| All 5 commits present in git log | `git log 2e264ef a545373 e2a0e6d af39d08 aeb0547` | PASS — all confirmed |
| `on_blob_ingested` in public section | Between `public:` (line 103) and `private:` (line 206) in `peer_manager.h` | PASS |
| Underflow guard in `expiry_scan_loop` | `peer_manager.cpp:2894`: `(target > now) ? ... : seconds(0)` | PASS |

---

### Requirements Coverage

| Requirement | Source Plans | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| MAINT-01 | 81-01, 81-02 | Expiry uses a next-expiry timer that fires at exactly the earliest blob's expiry time (O(1) via MDBX cursor) | SATISFIED | `get_earliest_expiry()` O(1) cursor query; `expiry_scan_loop` computes exact duration; `[event-expiry]` test "timer fires at exact expiry" |
| MAINT-02 | 81-02 | After processing an expired blob, timer rearms to the next earliest expiry (chain processing) | SATISFIED | `peer_manager.cpp:2913-2920` post-scan `get_earliest_expiry()` sets next target; test "chain rearm after scan" |
| MAINT-03 | 81-02 | Expiry timer rearms when a blob with an earlier expiry is ingested | SATISFIED | `peer_manager.cpp:3028-3036` in `on_blob_ingested`: cancels/respawns on shorter expiry; test "ingest rearm with shorter TTL" |

All 3 requirements are marked `[x]` in REQUIREMENTS.md and listed as Phase 81 / Complete in the requirements table (lines 96-98).

No orphaned requirements — REQUIREMENTS.md maps exactly MAINT-01, MAINT-02, MAINT-03 to Phase 81, matching both plans' `requirements:` frontmatter fields.

---

### Anti-Patterns Found

No TODO/FIXME/PLACEHOLDER/stub patterns found in any of the 8 modified files.

`expiry_scan_interval_seconds` deprecation comment on `config.h:46` is intentional and correctly classified as info-level (field kept for config-file compatibility, not a stub).

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| (none) | — | — | — | — |

---

### Human Verification Required

The following behaviors require a running node to verify end-to-end:

#### 1. Real-Time Expiry Under Load

**Test:** Run a node, ingest a blob with a 5-second TTL, observe that exactly 5 seconds later the blob is gone (not up to 60s later as with the old periodic scan).
**Expected:** `has_blob()` returns false within 1 second of the TTL expiry.
**Why human:** Requires a running io_context with real wall-clock time; integration tests use fake clocks.

#### 2. SIGHUP Reload Logs Deprecation Warning

**Test:** Send SIGHUP to a running node that has `expiry_scan_interval_seconds = 30` in config. Check logs.
**Expected:** Log line shows `expiry_scan_interval=30s` (the SIGHUP reload path still logs it). Expiry behavior is unaffected.
**Why human:** Requires a live process and log observation.

---

### Gaps Summary

No gaps. All 9 observable truths verified. All 10 artifacts substantive and wired. All 3 key link patterns confirmed. All 3 requirements satisfied. No anti-patterns found in phase-modified files.

The event-driven expiry system is fully implemented: `get_earliest_expiry()` provides O(1) storage lookup, `expiry_scan_loop()` computes exact wall-clock-to-steady-timer duration, `on_blob_ingested()` rearms on shorter-TTL ingests, and `start()` arms the timer from existing storage state. All behaviors are covered by 9 unit tests (4 storage + 5 peer).

---

_Verified: 2026-04-03T16:00:00Z_
_Verifier: Claude (gsd-verifier)_
