---
phase: 33-crypto-throughput-optimization
plan: 02
subsystem: engine
tags: [dedup-before-crypto, store-path-optimization, ingest-pipeline, flatbuffers, sha3-256]

# Dependency graph
requires:
  - phase: 33-01-hash-then-sign
    provides: build_signing_input() returns 32-byte SHA3-256 digest, OQS_SIG caching
provides:
  - store_blob overload accepting pre-computed hash + encoded bytes (skip redundant encode+hash)
  - Dedup-before-crypto in engine.ingest() (has_blob check skips ML-DSA-87 verify for duplicates)
  - Single encode_blob() + blob_hash() per ingest (no redundant re-encode in store path)
affects: [33-03-benchmarking, future-sync-optimization]

# Tech tracking
tech-stack:
  added: []
  patterns: [pre-computed passthrough to storage, dedup-before-crypto pipeline ordering]

key-files:
  created: []
  modified: [db/storage/storage.h, db/storage/storage.cpp, db/engine/engine.cpp, db/tests/engine/test_engine.cpp]

key-decisions:
  - "Dedup short-circuit returns seq_num=0 (no consumer requires valid seq_num for duplicate acks)"
  - "Original store_blob(blob) delegates to new overload (single implementation for both paths)"
  - "delete_blob() path unchanged -- uses original store_blob overload (tombstones are tiny, redundant encode negligible)"

patterns-established:
  - "Pre-computed passthrough: compute expensive values once in engine, pass through to storage"
  - "Dedup-before-crypto: cheap has_blob check before expensive ML-DSA-87 verify"

requirements-completed: [PERF-01, PERF-03, PERF-05]

# Metrics
duration: 14min
completed: 2026-03-17
---

# Phase 33 Plan 02: Store Path Optimization Summary

**Dedup-before-crypto skips ML-DSA-87 verify for duplicate blobs; encode_blob() + blob_hash() computed once per ingest with pre-computed passthrough to storage**

## Performance

- **Duration:** 14 min
- **Started:** 2026-03-17T14:10:04Z
- **Completed:** 2026-03-17T14:24:32Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Added store_blob overload accepting pre-computed content hash and encoded bytes, eliminating redundant encode_blob() + blob_hash() inside storage layer
- Rewrote engine.ingest() pipeline: compute encode+hash once at Step 2.5, check dedup before expensive ML-DSA-87 signature verification, pass pre-computed values to store_blob
- Original store_blob(blob) refactored to delegate to new overload -- single implementation for both paths
- All 313 tests pass with no regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Add store_blob overload with pre-computed hash and encoded bytes** - `4be9966` (feat)
2. **Task 2: Rewrite engine.ingest() with dedup-before-crypto and pre-computed passthrough** - `661d741` (feat)

## Files Created/Modified
- `db/storage/storage.h` - New store_blob overload declaration accepting precomputed_hash + precomputed_encoded
- `db/storage/storage.cpp` - New overload implementation; original store_blob delegates to it
- `db/engine/engine.cpp` - Rewritten ingest() pipeline with dedup-before-crypto and single encode+hash
- `db/tests/engine/test_engine.cpp` - Updated duplicate test for seq_num=0 dedup short-circuit behavior

## Decisions Made
- Dedup short-circuit returns seq_num=0: no consumer requires valid seq_num for duplicate acks (PeerManager logs only, SyncProtocol notification fires only for stored, not duplicate)
- Original store_blob(blob) delegates to new overload rather than maintaining two separate implementations
- delete_blob() path left unchanged -- uses original store_blob(blob) overload since tombstones are tiny (36 bytes)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Updated duplicate ingest test for new seq_num=0 behavior**
- **Found during:** Task 2 (engine.ingest() rewrite)
- **Issue:** Test "BlobEngine duplicate returns existing seq_num" expected seq_num match between first and second ingest, but dedup short-circuit now returns seq_num=0
- **Fix:** Updated test to verify seq_num==0 for duplicates and blob_hash match. Renamed test to "BlobEngine duplicate returns duplicate status"
- **Files modified:** db/tests/engine/test_engine.cpp
- **Verification:** All 313 tests pass
- **Committed in:** 661d741 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 bug - test expectation)
**Impact on plan:** Expected consequence of dedup short-circuit optimization (documented in RESEARCH.md Pitfall 5). No scope creep.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- PERF-01, PERF-03, PERF-05 complete -- all five crypto throughput optimizations now implemented
- All 313 tests pass, ready for benchmarking/validation phase
- encode_blob() + blob_hash() called exactly once per ingest (verified via grep)
- has_blob() dedup check positioned before ML-DSA-87 verify (verified via grep)

## Self-Check: PASSED

All files verified present. All commits verified in git log.

---
*Phase: 33-crypto-throughput-optimization*
*Completed: 2026-03-17*
