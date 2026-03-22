---
phase: 36-deletion-benchmarks
plan: 01
subsystem: benchmark
tags: [loadgen, tombstone, delete, benchmark, cli]

# Dependency graph
requires:
  - phase: 35-namespace-quotas
    provides: Quota enforcement and wire signaling
provides:
  - Loadgen --delete mode with tombstone construction and DeleteAck latency
  - Loadgen --identity-save / --identity-file for write-then-delete pipeline
  - Loadgen blob_hashes JSON output for hash piping between runs
affects: [36-02-PLAN]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Write-then-delete pipeline: write phase outputs blob_hashes, delete phase reads from stdin"
    - "DeleteAck-based latency measurement for tombstone creation benchmarks"
    - "Identity persistence via NodeIdentity::save_to/load_from across loadgen runs"

key-files:
  created: []
  modified:
    - loadgen/loadgen_main.cpp

key-decisions:
  - "Identity persistence via directory (save_to/load_from) not seed-based determinism"
  - "DeleteAck (type 19) used for delete latency, not Notification"
  - "Skip pub/sub subscription in delete mode since DeleteAck is direct from connected node"
  - "Tombstone data size (36 bytes) always classified as SizeClass::Small"

patterns-established:
  - "from_hex() utility for decoding 64-char hex strings to 32-byte arrays"
  - "make_tombstone_request() free function mirrors make_signed_blob() pattern"
  - "Delete mode send loop branches at blob construction, shares scheduling and drain logic"

requirements-completed: [BENCH-01]

# Metrics
duration: 4min
completed: 2026-03-18
---

# Phase 36 Plan 01: Loadgen Delete Mode Summary

**Loadgen extended with --delete mode, identity persistence, and blob hash output for write-then-delete benchmark pipelines**

## Performance

- **Duration:** 4 min
- **Started:** 2026-03-18T15:44:53Z
- **Completed:** 2026-03-18T15:49:15Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments
- Loadgen --delete mode constructs tombstones via wire::make_tombstone_data() and sends TransportMsgType_Delete (type 18)
- Write mode outputs blob_hashes JSON array in stats for downstream delete consumption
- Identity can be persisted to disk (--identity-save) and reloaded (--identity-file) across runs
- DeleteAck (type 19) handled for latency measurement in delete mode
- Delete mode reads target hashes from stdin (--hashes-from stdin), one 64-char hex hash per line

## Task Commits

Each task was committed atomically:

1. **Task 1: Add identity persistence and blob hash output to write mode** - `e2a60fe` (feat)
2. **Task 2: Add delete mode with tombstone construction and DeleteAck latency** - `eae59a4` (feat)

## Files Created/Modified
- `loadgen/loadgen_main.cpp` - Extended with --delete mode, --identity-save/--identity-file, --hashes-from flags, blob_hashes output, from_hex(), make_tombstone_request(), handle_delete_ack()

## Decisions Made
- Identity persistence uses NodeIdentity::save_to/load_from (directory-based) rather than seed-based determinism -- simpler, no need to modify NodeIdentity constructor
- DeleteAck used as primary ACK for delete latency measurement (Notifications also fire but follow different path via pub/sub fanout)
- Subscription skipped in delete mode since DeleteAck comes directly from the connected node, not via pub/sub
- Tombstones always 36 bytes, classified as SizeClass::Small in stats breakdown

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Loadgen is ready for benchmark shell script orchestration in 36-02
- Write-then-delete pipeline: write with --identity-save, pipe blob_hashes to --delete --hashes-from stdin with --identity-file
- All three blob sizes can be tested (tombstone creation is independent of original blob size)

---
*Phase: 36-deletion-benchmarks*
*Completed: 2026-03-18*
