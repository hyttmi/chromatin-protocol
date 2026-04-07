---
phase: 95-code-deduplication
plan: 02
subsystem: database
tags: [c++, refactoring, endian, deduplication, peer-manager, codec]

# Dependency graph
requires:
  - phase: 95-01
    provides: "endian.h and blob_helpers.h shared utility headers"
provides:
  - "peer_manager.cpp fully cleaned of inline BE/memcpy/blob-ref duplication"
  - "codec.cpp includes endian.h for consistency"
affects: [95-03]

# Tech tracking
tech-stack:
  added: []
  patterns: ["chromatindb::util:: namespace for all shared utilities", "store_u64_be/store_u32_be for pre-allocated buffer writes", "extract_namespace/extract_namespace_hash for payload parsing", "encode_blob_ref for BlobNotify wire format construction"]

key-files:
  created: []
  modified:
    - "db/peer/peer_manager.cpp"
    - "db/wire/codec.cpp"

key-decisions:
  - "codec.cpp LE patterns in build_signing_input preserved as protocol-defined (not replaced with BE helpers)"
  - "memcpy patterns at non-zero offsets (delegation entries, batch hash extraction) left as-is -- no clean helper fit"
  - "encode_notification body replaced with single encode_blob_ref call (direct delegation to shared utility)"
  - "init.namespace_id = ns direct assignment replaces memcpy in reconciliation (same-type array copy)"

patterns-established:
  - "store_u64_be/store_u32_be for writing into pre-allocated response buffers at known offsets"
  - "extract_namespace_hash with structured bindings for paired ns+hash extraction from payload"
  - "read_u64_be/read_u32_be raw-pointer overloads for pre-validated payload offsets"

requirements-completed: [DEDUP-01, DEDUP-04, DEDUP-05]

# Metrics
duration: 63min
completed: 2026-04-07
---

# Phase 95 Plan 02: Peer Manager & Codec Deduplication Summary

**Replaced 68 inline BE shift, memcpy-32, and blob-ref encoding patterns in peer_manager.cpp with shared chromatindb::util calls -- net 74 LOC reduction, zero test regressions**

## Performance

- **Duration:** 63 min
- **Started:** 2026-04-07T16:47:35Z
- **Completed:** 2026-04-07T17:50:35Z
- **Tasks:** 2/2
- **Files modified:** 2

## Accomplishments
- Eliminated all 23+ inline BE shift loops from peer_manager.cpp (zero remain, was ~45 total sites)
- Replaced ~12 inline namespace/hash memcpy extraction patterns with extract_namespace/extract_namespace_hash
- Replaced encode_notification implementation with single encode_blob_ref delegation
- Replaced encode_namespace_list/decode_namespace_list inline patterns with write_u16_be/read_u16_be/extract_namespace
- Added endian.h include to codec.cpp for consistency (no BE patterns to replace, only protocol-defined LE)
- All 634 unit tests pass (3109 assertions) with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Replace all inline patterns in peer_manager.cpp** - `c22cda4` (refactor)
2. **Task 2: Replace inline patterns in codec.cpp** - `e6986e2` (refactor)

## Files Created/Modified
- `db/peer/peer_manager.cpp` - Replaced 68 inline patterns with chromatindb::util:: calls (108 insertions, 182 deletions)
- `db/wire/codec.cpp` - Added endian.h include for consistency (LE patterns preserved)

## Decisions Made
- codec.cpp has only LE patterns in build_signing_input (protocol-defined canonical signing format) -- these must NOT be replaced with BE helpers
- memcpy patterns copying data INTO response buffers at various offsets (e.g., hash at offset+1, delegation entries) left as-is since they don't extract from standard ns/hash positions
- Used direct array assignment (init.namespace_id = ns) instead of memcpy for same-type std::array copy in reconciliation

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Build system required configuring with FETCHCONTENT_BASE_DIR pointing to main repo's deps (worktree isolation means no pre-built dependencies)
- Test suite (634 tests) required ~3 minutes to complete due to network timeout tests

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- peer_manager.cpp and codec.cpp are fully cleaned
- Plan 03 can proceed with remaining files (storage.cpp, sync_protocol.cpp, reconciliation.cpp, framing.cpp) which were partially cleaned in Plan 01
- All utility headers (endian.h, blob_helpers.h) are proven and stable

## Self-Check: PASSED

- All files exist (peer_manager.cpp, codec.cpp, 95-02-SUMMARY.md)
- All commits verified (c22cda4, e6986e2)

---
*Phase: 95-code-deduplication*
*Completed: 2026-04-07*
