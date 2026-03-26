---
phase: 65-node-level-queries
plan: 01
subsystem: database
tags: [flatbuffers, mdbx, relay, wire-protocol, storage]

# Dependency graph
requires:
  - phase: 63-query-extensions
    provides: "ExistsRequest/Response and NodeInfoRequest/Response patterns, transport.fbs types 37-40"
provides:
  - "Six new TransportMsgType enum values 41-46 for NamespaceList, StorageStatus, NamespaceStats"
  - "Storage::count_tombstones() O(1) global tombstone count"
  - "Storage::count_delegations(ns) cursor prefix scan for per-namespace delegation count"
  - "Relay filter allows 26 client types (was 20)"
  - "QUERY-05 marked as dropped"
affects: [65-02, 67-integration]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "MDBX map stat for O(1) aggregate counts"
    - "Cursor prefix scan for per-key-prefix counting"

key-files:
  created: []
  modified:
    - "db/schemas/transport.fbs"
    - "db/wire/transport_generated.h"
    - "db/storage/storage.h"
    - "db/storage/storage.cpp"
    - "relay/core/message_filter.h"
    - "relay/core/message_filter.cpp"
    - "db/tests/relay/test_message_filter.cpp"
    - "db/tests/storage/test_storage.cpp"
    - ".planning/REQUIREMENTS.md"

key-decisions:
  - "count_tombstones uses O(1) MDBX get_map_stat, not cursor scan"
  - "count_delegations uses cursor prefix scan (no O(1) path for per-namespace counts)"
  - "QUERY-05 dropped: NodeInfoResponse already serves as health check"

patterns-established:
  - "MDBX map stat for O(1) aggregate counts on sub-databases"
  - "Cursor lower_bound + prefix check for namespace-scoped counting"

requirements-completed: [QUERY-05, QUERY-06, QUERY-07, QUERY-08]

# Metrics
duration: 7min
completed: 2026-03-26
---

# Phase 65 Plan 01: Schema, Storage Methods, and Relay Filter Summary

**FlatBuffers types 41-46 for node-level queries, O(1) tombstone count and cursor-scanned delegation count in Storage, relay filter expanded to 26 client types**

## Performance

- **Duration:** 7 min
- **Started:** 2026-03-26T15:11:34Z
- **Completed:** 2026-03-26T15:18:53Z
- **Tasks:** 2
- **Files modified:** 9

## Accomplishments
- Added 6 new enum values (types 41-46) to TransportMsgType for NamespaceList, StorageStatus, NamespaceStats request/response pairs
- Implemented count_tombstones() using O(1) MDBX map statistics and count_delegations(ns) using cursor prefix scan, with 4 passing unit tests
- Updated relay message filter to allow all 6 new types (26 total client-allowed types), with updated tests
- Marked QUERY-05 as dropped in REQUIREMENTS.md (NodeInfoResponse already serves as health check)

## Task Commits

Each task was committed atomically:

1. **Task 1: Add 6 enum values, update relay filter, mark QUERY-05 dropped** - `6084173` (feat)
2. **Task 2 RED: Failing tests for count_tombstones and count_delegations** - `a825e1d` (test)
3. **Task 2 GREEN: Implement count_tombstones and count_delegations** - `63f91e0` (feat)

## Files Created/Modified
- `db/schemas/transport.fbs` - Added 6 new enum values (types 41-46)
- `db/wire/transport_generated.h` - Regenerated with new enum constants
- `db/storage/storage.h` - Added count_tombstones() and count_delegations() declarations
- `db/storage/storage.cpp` - Implemented both methods (O(1) map stat + cursor prefix scan)
- `relay/core/message_filter.h` - Updated doc comment with new allowed types
- `relay/core/message_filter.cpp` - Added 6 new case labels to is_client_allowed switch
- `db/tests/relay/test_message_filter.cpp` - Added 6 CHECK lines, updated count to 26
- `db/tests/storage/test_storage.cpp` - Added 4 new test cases for tombstone/delegation counting
- `.planning/REQUIREMENTS.md` - Marked QUERY-05 as dropped with rationale

## Decisions Made
- count_tombstones uses O(1) MDBX get_map_stat (consistent with integrity_scan pattern)
- count_delegations uses cursor prefix scan because delegation_map keys are [ns:32][pk_hash:32] and MDBX map stats give global count, not per-namespace
- QUERY-05 dropped per CONTEXT.md decision D-01/D-02: NodeInfoResponse (Phase 63) already serves as health check

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- FlatBuffers transport_generated.h not auto-regenerated on first build (CMake needed explicit target rebuild). Resolved by running `cmake --build build --target flatbuffers_transport_generated` to force regeneration.

## Known Stubs

None - all implementations are complete and wired to data sources.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All 6 enum values exist in generated header, ready for handler implementation in Plan 02
- Storage methods count_tombstones() and count_delegations() ready for StorageStatus and NamespaceStats handlers
- Relay filter already allows the new types through

## Self-Check: PASSED

All 10 files verified present. All 3 commits (6084173, a825e1d, 63f91e0) verified in git log.

---
*Phase: 65-node-level-queries*
*Completed: 2026-03-26*
