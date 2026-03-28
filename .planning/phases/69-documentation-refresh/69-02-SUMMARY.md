---
phase: 69-documentation-refresh
plan: 02
subsystem: docs
tags: [protocol, wire-format, byte-offsets, verification]

# Dependency graph
requires:
  - phase: 67-extended-query-batch-peer-time
    provides: "v1.4.0 query extension message types 41-58 encoder implementations"
provides:
  - "Verified PROTOCOL.md byte-level documentation for all 58 message types"
affects: [python-sdk, client-libraries]

# Tech tracking
tech-stack:
  added: []
  patterns: []

key-files:
  created: []
  modified:
    - "db/PROTOCOL.md"

key-decisions:
  - "Encoder source code in peer_manager.cpp is ground truth; PROTOCOL.md corrected to match"

patterns-established: []

requirements-completed: [DOCS-04]

# Metrics
duration: 3min
completed: 2026-03-28
---

# Phase 69 Plan 02: Protocol Wire Format Verification Summary

**Verified all 58 message types in PROTOCOL.md against peer_manager.cpp encoder source, fixed 2 byte-offset discrepancies in NamespaceListResponse and StorageStatusResponse**

## Performance

- **Duration:** 3 min
- **Started:** 2026-03-28T12:04:09Z
- **Completed:** 2026-03-28T12:07:35Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments
- Verified types 30-40 (WriteAck through NodeInfoResponse) byte-for-byte against encoder source
- Verified types 41-58 (all v1.4.0 query extensions) byte-for-byte against encoder source
- Fixed NamespaceListResponse field order (count before has_more, not reversed)
- Fixed StorageStatusResponse total_blobs size (8 bytes uint64, not 4) and removed phantom padding field
- Confirmed Message Type Reference table has all 59 entries (0-58) with correct enum values

## Task Commits

Each task was committed atomically:

1. **Task 1: Verify client protocol types 30-40 byte formats** - `096daf6` (fix)
2. **Task 2: Verify v1.4.0 query extension types 41-58 byte formats** - verification only, no additional changes needed (all fixes were in types 41-58 section, committed with Task 1)

## Files Created/Modified
- `db/PROTOCOL.md` - Fixed NamespaceListResponse field order and StorageStatusResponse total_blobs size/offset

## Decisions Made
- Encoder source code in peer_manager.cpp is the single source of truth; PROTOCOL.md was corrected to match where they disagreed

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] NamespaceListResponse field order incorrect**
- **Found during:** Task 1 (types 30-40 verification)
- **Issue:** PROTOCOL.md documented NamespaceListResponse as [has_more:1][count:4] but encoder writes [count:4][has_more:1]
- **Fix:** Swapped field order in PROTOCOL.md table to match encoder
- **Files modified:** db/PROTOCOL.md
- **Verification:** Compared against peer_manager.cpp lines 997-1016
- **Committed in:** 096daf6

**2. [Rule 1 - Bug] StorageStatusResponse total_blobs size and offsets incorrect**
- **Found during:** Task 1 (types 30-40 verification)
- **Issue:** PROTOCOL.md documented total_blobs as 4 bytes at offset 28 with mmap_bytes at offset 32 and a 4-byte padding at offset 40. Encoder writes total_blobs as 8-byte uint64 at offset 28, mmap_bytes at offset 36, no padding.
- **Fix:** Changed total_blobs to 8 bytes, mmap_bytes to offset 36, removed padding row
- **Files modified:** db/PROTOCOL.md
- **Verification:** Compared against peer_manager.cpp lines 1041-1062; total = 8+8+8+4+8+8 = 44 bytes confirmed
- **Committed in:** 096daf6

---

**Total deviations:** 2 auto-fixed (2 documentation bugs)
**Impact on plan:** Both fixes are documentation corrections -- SDK developers relying on these byte offsets would have built incompatible clients.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- PROTOCOL.md is now fully verified against encoder source for all 58 message types
- SDK developers can use this document as the authoritative wire format reference

## Self-Check: PASSED

- FOUND: db/PROTOCOL.md
- FOUND: commit 096daf6
- FOUND: 69-02-SUMMARY.md

---
*Phase: 69-documentation-refresh*
*Completed: 2026-03-28*
