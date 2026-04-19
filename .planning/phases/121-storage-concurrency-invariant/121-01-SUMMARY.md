---
phase: 121-storage-concurrency-invariant
plan: 01
subsystem: infra
tags: [concurrency, storage, mdbx, tsan, asio, regression-proofing]

# Dependency graph
requires:
  - phase: 99-peer-manager-decomposition
    provides: strand-confined NodeMetrics vocabulary (db/peer/peer_types.h:58) reused here
  - phase: 0.8.0-thread-pool-crypto-offload
    provides: crypto::offload + post-back idiom in db/crypto/thread_pool.h:26-31
provides:
  - STORAGE_THREAD_CHECK() macro at every public Storage method entry (32 call sites)
  - chromatindb::storage::ThreadOwner helper in db/storage/thread_check.h
  - Concurrent-ingest Catch2 test driving 128 simultaneous ingests through BlobEngine
  - Audit document (121-AUDIT.md) classifying every Storage call site in db/
  - TSAN ship-gate results document (121-TSAN-RESULTS.md)
  - Post-back fix in BlobEngine::ingest + BlobEngine::delete_blob closing the 9-site race
affects:
  - "122-schema-change: schema rewrite now lands on top of proven concurrency invariant"
  - "123-tombstone-overwrite: storage semantics inspected here carry forward"
  - "future contributors: runtime check catches any new off-thread Storage access in debug builds"

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "STORAGE_THREAD_CHECK() lazy-capture owner-thread assertion (no-op under NDEBUG)"
    - "co_await asio::post(this_coro::executor, use_awaitable) after crypto::offload in engine.cpp"

key-files:
  created:
    - db/storage/thread_check.h
    - db/tests/util/test_storage_thread_check.cpp
    - db/tests/storage/test_storage_concurrency_tsan.cpp
    - .planning/phases/121-storage-concurrency-invariant/121-AUDIT.md
    - .planning/phases/121-storage-concurrency-invariant/121-TSAN-RESULTS.md
  modified:
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/engine/engine.h
    - db/engine/engine.cpp
    - db/peer/peer_manager.h
    - db/sync/sync_protocol.h
    - db/CMakeLists.txt

key-decisions:
  - "Chose post-back over strand confinement — smaller fix, matches uniform codebase idiom; D-06 permitted both, this is the minimal correct change"
  - "Macro compiles to ((void)0) under NDEBUG so production builds carry zero cost"
  - "Owner TID captured lazily on FIRST public call (not in constructor) to handle startup-before-ioc path; constructor resets owner after rebuild_quota_aggregates()"
  - "Task 3 and Task 6 merged into one commit because Task 3's verification step was blocked by the bug Task 6 fixes; net code is identical either way"

patterns-established:
  - "Pattern: thread-confined runtime invariant via atomic<thread::id> + ((void)0) release macro"
  - "Pattern: after every crypto::offload(pool, ...), co_await asio::post(executor, use_awaitable) before touching ioc-bound state"
  - "Pattern: unit tests use a non-asserting try_check() sibling for cross-thread behavior so abort() doesn't unwind Catch2"

requirements-completed: []

# Metrics
duration: 115min
completed: 2026-04-19
---

# Phase 121 Plan 01: Storage Concurrency Invariant Summary

**Runtime-asserted thread confinement for Storage + post-back fix closing 9 off-thread call sites identified by trace audit, proven TSAN-clean on 128-coroutine concurrent ingest.**

## Performance

- **Duration:** ~115 min (including 2x cmake reconfigure for TSAN build, TSAN build itself)
- **Started:** 2026-04-19T14:15Z (approx; agent spawn)
- **Completed:** 2026-04-19T16:12Z
- **Tasks:** 6 (1 audit + 2 TDD macro+test + 3 apply + 4 comments + 5 TSAN driver + 6 ship gate)
- **Files modified:** 7 (+ 5 created)

## Accomplishments

- Audited all 48 Storage::* call sites in db/; classified each by executor family (A/B/C/D) with file:line evidence and preceding co_await chain.
- Found and documented 9 executor-C race sites in db/engine/engine.cpp where `storage_.*` was reached on a thread_pool worker after `crypto::offload(pool_, ...)` with no post-back.
- Added STORAGE_THREAD_CHECK() macro + ThreadOwner helper (db/storage/thread_check.h). 32 public Storage methods now assert thread identity on entry in debug builds; release builds expand to `((void)0)`.
- Fixed the 9 race sites with 4 post-back statements in BlobEngine::ingest + BlobEngine::delete_blob. No new strand, no new mutex — matches the uniform post-back idiom already used in message_dispatcher, blob_push_manager, pex_manager, connection_manager, sync_orchestrator.
- TSAN ship gate: `[tsan][storage][concurrency]` passes with 0 warnings, `[storage],[engine],[sync]` regression suite passes with 1473 assertions across 233 tests — all under `-DSANITIZER=tsan`.
- Four "NOT thread-safe" class-level comments (storage.h, engine.h, peer_manager.h, sync_protocol.h) replaced with precise "thread-confined" language citing STORAGE_THREAD_CHECK.

## Task Commits

1. **Task 1: Trace audit → 121-AUDIT.md** — `24f9a876` (docs)
2. **Task 2: STORAGE_THREAD_CHECK macro + unit test (TDD)** — `2eda58b7` (feat, 5 TEST_CASEs all passing)
3. **Task 3+6: Apply macro + post-back fix (merged)** — `c4cdfeeb` (feat; Task 3 verification was failing because of the Task 6 race — fix had to land atomically with the macro so tests could pass)
4. **Task 4: Rewrite four "NOT thread-safe" comments** — `f840e33a` (docs)
5. **Task 5: TSAN concurrent-ingest driver** — `09e0eb5c` (test, 11 assertions)
6. **Task 6: TSAN-RESULTS.md ship gate** — `ec614041` (docs, INVARIANT PROVEN verdict)

## Files Created/Modified

- `db/storage/thread_check.h` — ThreadOwner helper + STORAGE_THREAD_CHECK macro (NDEBUG-gated)
- `db/storage/storage.h` — include thread_check.h; thread-safety comment rewritten
- `db/storage/storage.cpp` — Impl gains `ThreadOwner thread_owner_{}`; STORAGE_THREAD_CHECK at top of 32 public methods; constructor resets owner after rebuild_quota_aggregates()
- `db/engine/engine.h` — thread-safety comment rewritten with offload+post-back reminder
- `db/engine/engine.cpp` — 4 post-back statements after each `co_await crypto::offload(pool_, ...)` in ingest() and delete_blob()
- `db/peer/peer_manager.h` — thread-safety comment rewritten
- `db/sync/sync_protocol.h` — thread-safety comment rewritten
- `db/tests/util/test_storage_thread_check.cpp` — 5 unit tests (116 assertions)
- `db/tests/storage/test_storage_concurrency_tsan.cpp` — 1 TEST_CASE, 128 concurrent ingests across 8 namespaces
- `db/CMakeLists.txt` — added 2 new test sources to chromatindb_tests
- `.planning/phases/121-storage-concurrency-invariant/121-AUDIT.md` — trace audit
- `.planning/phases/121-storage-concurrency-invariant/121-TSAN-RESULTS.md` — ship-gate results

## Decisions Made

- **Fix shape = post-back, not strand.** D-06 listed strand confinement as preferred-but-not-locked. The fix turned out to be obvious once the audit was complete: the codebase has a uniform `co_await asio::post(ioc_, asio::use_awaitable)` idiom for exactly this scenario, used in 5+ files. Adding a strand would be a second, redundant concurrency primitive where the existing pattern is already doing the job everywhere except engine.cpp. The 4-line fix in engine.cpp restores consistency.
- **Lazy-capture owner TID, not constructor capture.** Storage is constructed on the main thread during daemon startup and during tests before ioc runs. The first public call installs the owner; constructor resets after its one built-in public call (`rebuild_quota_aggregates()`).
- **Merged Task 3 with Task 6.** Plan assumed Task 3 would pass existing tests because "single-threaded fixtures all touch Storage from the test thread only" — that assumption was wrong for tests using `run_async` + thread_pool + crypto::offload. With the macro applied, those tests abort. The only way to ship Task 3 green was to land Task 6 in the same commit.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Task 3 verification required applying Task 6's fix atomically**
- **Found during:** Task 3 (apply macro to public methods)
- **Issue:** Plan expected Task 3's verification `./build/db/chromatindb_tests "[storage],[engine],[sync]"` to pass unchanged, but once STORAGE_THREAD_CHECK() is live at every public method, the existing `test_engine.cpp:183` test immediately aborts because BlobEngine::ingest accesses Storage after crypto::offload() without a post-back. That abort is EXACTLY the executor-C race Task 1 identified; the plan's Task 6 is the designed fix.
- **Fix:** Applied Task 6's post-back fix (4 `co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable)` statements) in the same commit as Task 3 so tests go from red to green in one atomic transition. No net code difference vs. running Tasks 3 and 6 sequentially; the outcome is identical.
- **Files modified:** `db/engine/engine.cpp` (4 post-back statements added)
- **Verification:** After merge, `[storage],[engine],[sync]` passes 232 tests (1462 assertions) in debug and 233 tests (1473 assertions) under TSAN.
- **Committed in:** `c4cdfeeb` (Task 3+6 combined commit)

**2. [Rule 2 - Missing Critical] Constructor must reset ThreadOwner after rebuild_quota_aggregates**
- **Found during:** Task 3 (adding macro to public methods)
- **Issue:** The Storage constructor calls `rebuild_quota_aggregates()` — a public method — which installs the *constructor thread* as the owner. When the ioc thread later invokes Storage methods, the assertion fires. CONTEXT.md D-09 explicitly allowed for this ("handle during planning"), but the plan didn't prescribe the specific carve-out.
- **Fix:** Added `impl_->thread_owner_.reset()` at the end of the constructor (with rationale comment). Also renamed `reset_for_test` to `reset` (keeping the old name as a deprecated alias) since the method now has a legitimate non-test use case.
- **Files modified:** `db/storage/thread_check.h`, `db/storage/storage.cpp`
- **Committed in:** `c4cdfeeb` (part of Task 3+6 commit)

**3. [Plan clarification] peer_manager.h comment used "runs on a single io_context thread" before settling on "thread-confined"**
- **Found during:** Task 4 (comment rewrites)
- **Issue:** First draft of peer_manager.h used the phrase "runs on a single io_context thread" (paraphrased from original). Acceptance criterion required "thread-confined" wording.
- **Fix:** One-word tweak to "thread-confined to the io_context executor" in the first follow-up edit within Task 4.
- **Files modified:** `db/peer/peer_manager.h`
- **Committed in:** `f840e33a` (Task 4 commit, includes this tweak)

---

**Total deviations:** 3 auto-fixed (1 blocking Task 3 merge with Task 6, 1 missing critical constructor carve-out, 1 comment wording tweak)
**Impact on plan:** All auto-fixes necessary to make Task 3 verifiable without deferring the already-agreed-upon fix from the audit. No scope creep — the fix shape was already locked in D-06 and Task 6's Step E.

## Issues Encountered

- **Full test suite slow (but not hung).** `./build/db/chromatindb_tests` takes several minutes to run 711 test cases because the peer-network tests use real timers (30s keepalive, reconnect backoff). Unrelated to this phase. The `[storage],[engine],[sync],[thread_check]` subset (233 tests) runs in under 5 seconds and is sufficient for the ship gate.
- **Asio `atomic_thread_fence` TSAN warnings.** The TSAN build emits `-Wtsan` warnings about `std::atomic_thread_fence` inside Asio's `std_fenced_block.hpp`. Known upstream Asio limitation, predates this phase, not a suppression target — just noise.

## User Setup Required

None — this phase is pure internal hardening.

## Next Phase Readiness

- **Phase 122 (schema change) can proceed.** The concurrency question is closed: Storage is provably thread-confined, the race is fixed, and `STORAGE_THREAD_CHECK()` will catch any regression in debug/test builds before Phase 122's schema work lands on top.
- **No blockers or concerns.** Verdict in 121-TSAN-RESULTS.md is **INVARIANT PROVEN**.
- **For future phases touching engine.cpp:** the post-back idiom is now uniformly applied — any new `co_await crypto::offload(pool_, ...)` MUST be followed by a post-back before touching Storage, or STORAGE_THREAD_CHECK will abort in debug builds.

## Self-Check: PASSED

All 5 created files verified present on disk:
- `db/storage/thread_check.h` ✓
- `db/tests/util/test_storage_thread_check.cpp` ✓
- `db/tests/storage/test_storage_concurrency_tsan.cpp` ✓
- `.planning/phases/121-storage-concurrency-invariant/121-AUDIT.md` ✓
- `.planning/phases/121-storage-concurrency-invariant/121-TSAN-RESULTS.md` ✓

All 6 task commits verified present in git log:
- `24f9a876` (Task 1 audit) ✓
- `2eda58b7` (Task 2 macro + test) ✓
- `c4cdfeeb` (Task 3+6 apply + fix) ✓
- `f840e33a` (Task 4 comments) ✓
- `09e0eb5c` (Task 5 TSAN driver) ✓
- `ec614041` (Task 6 TSAN results) ✓

Ship-gate verification re-run:
- `[tsan][storage][concurrency]` (TSAN build): 11 assertions, 1 test case, PASS, 0 warnings
- `[storage],[engine],[sync],[thread_check]` (TSAN build): 1473 assertions, 233 test cases, PASS
- Macro count in storage.cpp: 32 ≥ 31 required ✓
- No "NOT thread-safe" strings remain in the 4 target headers ✓
- `sanitizers/tsan.supp` unchanged vs. phase start ✓

---
*Phase: 121-storage-concurrency-invariant*
*Completed: 2026-04-19*
