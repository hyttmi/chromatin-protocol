---
phase: 51-ttl-lifecycle-e2e-primitives
plan: 03
subsystem: testing
tags: [integration-tests, ttl, expiry, gc, tombstone, backfill, docker]

# Dependency graph
requires:
  - phase: 49-network-resilience-reconciliation
    provides: "helpers.sh, late-joiner pattern, loadgen bulk ingest patterns"
provides:
  - "TTL-02 tombstone TTL expiry and GC integration test"
  - "E2E-02 history backfill integration test at 1000-blob scale"
  - "Fix for timestamp/TTL units mismatch in expiry system"
affects: [ttl-lifecycle, e2e-primitives, storage-expiry]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Integrity scan (restart + grep blobs=) for verifying actual blobs_map entries vs SIGUSR1 seq_num sum"
    - "Microsecond timestamps normalized to seconds for expiry computation"

key-files:
  created:
    - tests/integration/test_ttl02_tombstone_ttl_expiry.sh
    - tests/integration/test_e2e02_history_backfill.sh
  modified:
    - db/storage/storage.cpp
    - db/sync/sync_protocol.cpp
    - db/tests/storage/test_storage.cpp
    - db/tests/engine/test_engine.cpp
    - db/tests/sync/test_sync_protocol.cpp

key-decisions:
  - "Integrity scan output (blobs= from startup scan) used for GC verification instead of SIGUSR1 metrics, which sum latest_seq_num and never decrease"
  - "Fixed timestamp/TTL units mismatch: timestamps are microseconds, TTL is seconds. Expiry computation now divides timestamp by 1000000 before adding TTL"
  - "grep 'integrity scan: blobs=' to filter out warning lines from integrity scan log output"

patterns-established:
  - "Integrity scan for storage verification: restart container, grep 'integrity scan: blobs=' from logs, parse blobs/tombstone/expiry values"
  - "Microsecond timestamp normalization: timestamp / 1000000 + ttl for expiry computation in both storage and sync"

requirements-completed: [TTL-02, E2E-02]

# Metrics
duration: 32min
completed: 2026-03-21
---

# Phase 51 Plan 03: TTL Lifecycle and E2E Primitives Summary

**Tombstone TTL=60 expiry verified via GC (blobs_map 20->0, tombstone_map 20->0), 1000-blob late-joiner backfill with incremental sync and tombstone propagation confirmed**

## Performance

- **Duration:** 32 min
- **Started:** 2026-03-21T18:43:20Z
- **Completed:** 2026-03-21T19:15:03Z
- **Tasks:** 2
- **Files modified:** 8

## Accomplishments
- TTL-02: 20 tombstones with TTL=60 garbage collected on both nodes -- blobs_map 20->0, tombstone_map 20->0, expiry_map 20->0
- E2E-02: 1000-blob backfill to late-joining node3 with cursor_misses=1 confirming reconciliation-based sync, incremental sync to 1010, tombstone propagation to all 3 nodes
- Fixed critical bug: timestamp/TTL units mismatch in expiry system that prevented all TTL-based GC from working in production

## Task Commits

Each task was committed atomically:

1. **Task 1: TTL-02 tombstone TTL expiry and GC** - `a22d611` (feat)
2. **Task 2: E2E-02 history backfill** - `ec276c9` (feat)

## Files Created/Modified
- `tests/integration/test_ttl02_tombstone_ttl_expiry.sh` - TTL-02 tombstone expiry and GC integration test (2-node, TTL=60, ~220s runtime)
- `tests/integration/test_e2e02_history_backfill.sh` - E2E-02 history backfill at 1000-blob scale (3-node, ~90s runtime)
- `db/storage/storage.cpp` - Fixed timestamp/TTL units mismatch in store_blob and delete_blob_data expiry computation
- `db/sync/sync_protocol.cpp` - Fixed same units mismatch in is_blob_expired
- `db/tests/storage/test_storage.cpp` - Updated test timestamps to microseconds (1000000000ULL)
- `db/tests/engine/test_engine.cpp` - Updated test timestamps to microseconds
- `db/tests/sync/test_sync_protocol.cpp` - Updated test timestamps to microseconds

## Decisions Made
- Integrity scan output (blobs= from startup) used for GC verification instead of SIGUSR1 metrics -- SIGUSR1 sums latest_seq_num which never decreases even after GC purges blobs from blobs_map
- Fixed timestamp/TTL units mismatch by normalizing timestamp to seconds (timestamp / 1000000) in expiry computation, keeping the clock in seconds and unit test fake clocks unchanged

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed timestamp/TTL units mismatch in expiry system**
- **Found during:** Task 1 (TTL-02 tombstone TTL expiry and GC)
- **Issue:** Loadgen sets blob timestamps in microseconds for uniqueness, but storage expiry computation adds TTL (seconds) directly to timestamp (microseconds). The GC comparison uses system_clock_seconds(). Result: expiry_ts (~1.7 trillion) always exceeds now (~1.7 billion), so GC never fires in production.
- **Fix:** Normalize timestamp to seconds before adding TTL: `expiry_time = timestamp / 1000000 + ttl`. Applied in storage.cpp (store_blob, delete_blob_data) and sync_protocol.cpp (is_blob_expired). Updated all unit test timestamps to microsecond values (1000000000ULL = 1000 seconds).
- **Files modified:** db/storage/storage.cpp, db/sync/sync_protocol.cpp, db/tests/storage/test_storage.cpp, db/tests/engine/test_engine.cpp, db/tests/sync/test_sync_protocol.cpp
- **Verification:** All 81 storage tests pass (265 assertions), all 63 engine tests pass, all expiry tests pass, TTL-02 integration test passes
- **Committed in:** a22d611 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Critical bug fix necessary for TTL-based GC to work in production. Without this fix, no blob with TTL>0 would ever be garbage collected. No scope creep.

## Issues Encountered
- SIGUSR1 metrics `blobs=` count sums latest_seq_num across namespaces (monotonically increasing, never decreases) -- cannot be used to verify GC. Resolved by using integrity scan output from startup which reads actual blobs_map.ms_entries.
- Docker logs accumulate across container restarts. `grep "integrity scan:" | tail -1` picked up warning line instead of info line. Fixed by grepping for `"integrity scan: blobs="` specifically.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Both TTL-02 and E2E-02 integration tests are discoverable by run-integration.sh
- Expiry system now correctly handles production microsecond timestamps
- Ready for remaining phase 51 plans

---
*Phase: 51-ttl-lifecycle-e2e-primitives*
*Completed: 2026-03-21*
