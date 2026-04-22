---
phase: 121-storage-concurrency-invariant
verified: 2026-04-19T00:00:00Z
status: passed
score: 5/5
overrides_applied: 0
---

# Phase 121: Storage Concurrency Invariant — Verification Report

**Phase Goal:** Prove storage is concurrency-safe (strand-confined or single-threaded guarantee) or fix it before the schema-change phases land — avoid debugging data corruption and protocol changes simultaneously.
**Verified:** 2026-04-19
**Status:** passed
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | Every db/ call path reaching Storage::* is documented with its executor in 121-AUDIT.md | VERIFIED | 121-AUDIT.md exists; covers all 7 D-03 path families; 48 call sites classified A/B/C/D; all required source files appear in the Audit Table; Verdict=NEEDS_FIX with 9 executor-C sites identified in engine.cpp |
| 2 | STORAGE_THREAD_CHECK() fires at the top of every public Storage::* method in debug builds, no-op under NDEBUG | VERIFIED | `grep -cE '^\s*STORAGE_THREAD_CHECK\(\);' db/storage/storage.cpp` returns 32, exceeding the 31 required; `thread_check.h` has both `#if defined(NDEBUG)` → `((void)0)` and non-NDEBUG → `check(__func__)` branches; `ThreadOwner thread_owner_{}` confirmed in `Storage::Impl` at storage.cpp:136; `#include "db/storage/thread_check.h"` present in storage.h:13 |
| 3 | A Catch2 TSAN test drives N>=4 concurrent ingest coroutines through engine->storage and completes TSAN-clean with all blobs stored exactly once | VERIFIED | `db/tests/storage/test_storage_concurrency_tsan.cpp` has 1 TEST_CASE tagged `[tsan][storage][concurrency]`; spawns 8 coroutines × 16 ingests = 128 total; uses one NodeIdentity per coroutine to prevent dedup collisions; 121-TSAN-RESULTS.md records 11/11 assertions passing, 0 TSAN warnings; regression suite `[storage],[engine],[sync]` TSAN-clean with 1473 assertions across 233 tests |
| 4 | The four "NOT thread-safe" comments in storage.h, engine.h, peer_manager.h, sync_protocol.h are replaced with precise enforcement-citing language | VERIFIED | Zero matches for "NOT thread-safe" in all four headers; each now contains "thread-confined to the io_context executor" and a reference to STORAGE_THREAD_CHECK(); confirmed at storage.h:105, engine.h:71, peer_manager.h:42, sync_protocol.h:35 |
| 5 | If the audit found a race, it is fixed AND recorded in AUDIT.md; if no race, "no fix applied" is recorded | VERIFIED | Audit found Verdict=NEEDS_FIX (9 executor-C sites in engine.cpp); fix applied as 5 `co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable)` statements after every `crypto::offload()` site in BlobEngine::ingest and BlobEngine::delete_blob (engine.cpp:196, 254, 278, 409, 426); fix recorded in 121-AUDIT.md Findings section and 121-TSAN-RESULTS.md Conditional Fix section |

**Score:** 5/5 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/storage/thread_check.h` | STORAGE_THREAD_CHECK() macro + ThreadOwner helper | VERIFIED | File exists; defines `chromatindb::storage::ThreadOwner`; NDEBUG and non-NDEBUG branches present; lazy-capture on first call, assert on mismatch |
| `db/tests/util/test_storage_thread_check.cpp` | Unit test proving assertion fires across threads | VERIFIED | File exists; 5 TEST_CASEs tagged `[thread_check][storage]` (exceeds required 4); covers first-capture, same-thread idempotence, cross-thread detection via try_check, NDEBUG noop, reset semantics |
| `db/tests/storage/test_storage_concurrency_tsan.cpp` | Concurrent-ingest Catch2 test under TSAN | VERIFIED | File exists; 1 TEST_CASE tagged `[tsan][storage][concurrency]`; kCoroutines=8, kIngestsPerCoroutine=16; no TSAN-specific API references |
| `.planning/phases/121-storage-concurrency-invariant/121-AUDIT.md` | Trace audit with executor analysis | VERIFIED | File exists; contains H1 title "Audit Results" and all 5 required H2 sections (Summary, Audit Table, Path Families, Findings, Proceed Decision); Verdict=NEEDS_FIX |
| `db/storage/storage.h` | Updated comment citing STORAGE_THREAD_CHECK | VERIFIED | `#include "db/storage/thread_check.h"` at line 13; class-level comment at lines 105-113 cites STORAGE_THREAD_CHECK; "NOT thread-safe" absent |
| `db/storage/storage.cpp` | STORAGE_THREAD_CHECK() at top of every public method | VERIFIED | 32 macro call sites; `ThreadOwner thread_owner_{}` in Impl at line 136 |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `db/storage/storage.cpp` | `db/storage/thread_check.h` | `#include` + `STORAGE_THREAD_CHECK()` at every public method entry | VERIFIED | 32 `STORAGE_THREAD_CHECK();` occurrences; `thread_check.h` included via storage.h:13 |
| `db/tests/storage/test_storage_concurrency_tsan.cpp` | `db/engine/engine.h` + `db/storage/storage.h` | concurrent coroutines issuing BlobEngine::ingest posted back to a single io_context | VERIFIED | `co_spawn` with `asio::detached`; `engine.ingest(blob)` co_awaited per coroutine; `ioc.run()` drives all 8 coroutines |
| `db/CMakeLists.txt` | Both new test sources | added to chromatindb_tests sources list | VERIFIED | `tests/storage/test_storage_concurrency_tsan.cpp` at line 243; `tests/util/test_storage_thread_check.cpp` at line 251 |

### Data-Flow Trace (Level 4)

Not applicable — this phase produces no data-rendering components. All artifacts are enforcement infrastructure (macros, assertions, documentation) and test drivers. The concurrency invariant is verified through TSAN runtime evidence rather than data-flow tracing.

### Behavioral Spot-Checks

Build artifacts and runtime results are taken from 121-TSAN-RESULTS.md and 121-REVIEW.md (per task instructions: do not rebuild). Executor-verified by the TSAN ship gate document.

| Behavior | Evidence Source | Result | Status |
|----------|----------------|--------|--------|
| STORAGE_THREAD_CHECK() macro count >= 31 in storage.cpp | Direct grep: `grep -cE '^\s*STORAGE_THREAD_CHECK\(\);' db/storage/storage.cpp` = 32 | 32 >= 31 | PASS |
| "NOT thread-safe" absent from all 4 target headers | Direct grep: 0 matches across storage.h, engine.h, peer_manager.h, sync_protocol.h | 0 matches | PASS |
| CMakeLists.txt lists both new test files | Direct grep: lines 243, 251 | Both present | PASS |
| TSAN ship gate verdict | 121-TSAN-RESULTS.md: "Verdict: **INVARIANT PROVEN**" | INVARIANT PROVEN | PASS |
| All 6 commits exist in git log | `git log --oneline 24f9a876 2eda58b7 c4cdfeeb f840e33a 09e0eb5c ec614041` | All 6 confirmed | PASS |
| Post-back fix applied in engine.cpp | `grep -n 'co_await asio::post.*this_coro::executor' db/engine/engine.cpp` = 5 sites | 5 post-backs at lines 196, 254, 278, 409, 426 | PASS |
| tsan.supp unchanged | `git diff --exit-code HEAD sanitizers/tsan.supp` = exit 0 | Unchanged | PASS |

### Requirements Coverage

This phase has no explicit REQUIREMENTS.md IDs (retroactive audit/hardening phase — `requirements: []` in PLAN frontmatter). Phase goal is self-contained: prove and enforce the storage concurrency invariant before Phase 122 lands.

### Anti-Patterns Found

No blocking anti-patterns found. The REVIEW.md identifies 2 warnings and 6 info items, none of which block the concurrency invariant goal:

| File | Issue | Severity | Impact on Goal |
|------|-------|----------|---------------|
| `engine.h:71-75`, `sync_protocol.h:35-39`, `storage.h:110-113` | Doc comments reference `ioc_` member that doesn't exist on BlobEngine/SyncProtocol; actual post-back idiom uses `co_await asio::this_coro::executor` | WR-01 (Warning) | None — code behavior is correct; doc is slightly misleading to future readers |
| `thread_check.h:94-95` | `reset_for_test()` alias is public surface; named as test-only but production constructor calls `reset()` | WR-02 (Warning) | None — function works correctly; name is misleading |
| `storage.cpp:346, 355, 365` | Triple-nested STORAGE_THREAD_CHECK across store_blob overload chain | IN-01 (Info) | None — correctness unaffected; minor debug-build overhead |
| `test_storage_thread_check.cpp:66-70` | `#ifdef NDEBUG SKIP(...)` placed after assertions that already ran | IN-02 (Info) | None — test passes correctly |

None of these rise to a blocker level. WR-01 and WR-02 are good candidates for a cleanup commit before Phase 122 but are not required for the concurrency invariant to hold.

### Human Verification Required

None. All must-haves are verifiable programmatically against the codebase and the executor's recorded TSAN output. The TSAN ship gate (recorded in 121-TSAN-RESULTS.md with a PASS verdict, 0 warnings, and full output) is the runtime evidence substitute per task instructions.

### Minor Discrepancy: SUMMARY Claims 4 Post-backs, Code Has 5

The SUMMARY states "4 post-back statements" were added in BlobEngine::ingest + BlobEngine::delete_blob. The actual code has 5: 3 in `ingest()` (lines 196, 254, 278) and 2 in `delete_blob()` (lines 409, 426). This is a SUMMARY inaccuracy — the code is more complete than claimed, not less. All 9 audited race sites are covered; an extra post-back was added at the second `delete_blob` offload point (the ML-DSA-87 verify step). No impact on correctness or goal achievement.

### Minor Discrepancy: AUDIT.md Contains "## Audit Results" Only as H1 Title

The PLAN artifact check `contains: "## Audit Results"` is strictly unmet because the document uses an H1 title (`# Phase 121: Storage Concurrency Invariant — Audit Results`) rather than an H2 section heading. The five required H2 sections (Summary, Audit Table, Path Families, Findings, Proceed Decision) are all present. This is a heading-level formatting choice, not a content gap — the document is substantively complete per its acceptance criteria.

## Gaps Summary

No gaps. All five must-have truths are fully verified against the actual codebase. The two discrepancies noted above (post-back count in SUMMARY, H1 vs H2 title in AUDIT.md) are inconsequential — the code is correct and the document is complete.

---

_Verified: 2026-04-19_
_Verifier: Claude (gsd-verifier)_
