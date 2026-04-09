---
phase: 97-protocol-crypto-safety
plan: 01
subsystem: protocol
tags: [overflow, checked-arithmetic, protocol-parsing, security-hardening]

# Dependency graph
requires:
  - phase: 95-code-deduplication
    provides: db/util/endian.h shared utility header with BE read/write helpers
provides:
  - checked_mul and checked_add overflow-safe arithmetic in endian.h
  - All protocol decode/encode paths hardened against integer overflow
affects: [97-02, 97-03, any future protocol parsing code]

# Tech tracking
tech-stack:
  added: []
  patterns: [overflow-checked arithmetic for all wire-data size computations]

key-files:
  created: []
  modified:
    - db/util/endian.h
    - db/sync/sync_protocol.cpp
    - db/sync/reconciliation.cpp
    - db/peer/peer_manager.cpp
    - db/peer/message_dispatcher.cpp
    - db/tests/util/test_endian.cpp
    - db/tests/sync/test_sync_protocol.cpp
    - db/tests/sync/test_reconciliation.cpp

key-decisions:
  - "checked_mul/checked_add return std::optional<size_t> (D-02)"
  - "Overflow in decode returns nullopt/empty -- same error path as malformed input (D-03)"
  - "Encode reserve() calls silently skip reservation on overflow (safe -- vector grows dynamically)"
  - "Response builders record_strike and co_return on overflow (defensive, practically unreachable with local data)"

patterns-established:
  - "All protocol parsing integer arithmetic must use checked_mul/checked_add from endian.h"
  - "Overflow rejection tests use store_u32_be with 0xFFFFFFFF count to trigger overflow"

requirements-completed: [PROTO-01]

# Metrics
duration: 37min
completed: 2026-04-08
---

# Phase 97 Plan 01: Overflow-Checked Arithmetic Summary

**checked_mul/checked_add helpers in endian.h wired into all 14 protocol decode/encode/response paths, preventing integer overflow from untrusted wire counts**

## Performance

- **Duration:** 37 min
- **Started:** 2026-04-08T10:13:52Z
- **Completed:** 2026-04-08T10:51:24Z
- **Tasks:** 2
- **Files modified:** 8

## Accomplishments
- Added checked_mul and checked_add to db/util/endian.h with std::optional<size_t> return type
- Hardened 6 decode paths (untrusted wire data) with overflow-checked arithmetic
- Hardened 4 encode paths (reserve() calls) with overflow-checked arithmetic
- Hardened 4 response builder allocations (message_dispatcher.cpp) with overflow-checked arithmetic
- Added 7 unit tests for the overflow helpers and 3 overflow rejection tests for decode functions
- All 195 related test cases pass with 1126 assertions, zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Add checked_mul/checked_add to endian.h and write unit tests** - `6f2fa90` (feat)
2. **Task 2: Wire overflow-checked arithmetic into all protocol decode/encode paths** - `add7283` (feat)

## Files Created/Modified
- `db/util/endian.h` - Added checked_mul and checked_add overflow-safe arithmetic helpers
- `db/sync/sync_protocol.cpp` - Overflow-checked decode_namespace_list, decode_blob_request, decode_blob_transfer, encode_namespace_list, encode_blob_request
- `db/sync/reconciliation.cpp` - Overflow-checked decode_reconcile_ranges ItemList, decode_reconcile_items, encode_reconcile_ranges, encode_reconcile_items
- `db/peer/peer_manager.cpp` - Overflow-checked decode_namespace_list (SyncNamespaceAnnounce)
- `db/peer/message_dispatcher.cpp` - Overflow-checked ListResponse, NamespaceListResponse, DelegationListResponse, TimeRangeResponse
- `db/tests/util/test_endian.cpp` - 7 new tests for checked_mul/checked_add
- `db/tests/sync/test_sync_protocol.cpp` - 2 overflow rejection tests for decode functions
- `db/tests/sync/test_reconciliation.cpp` - 1 overflow rejection test for decode_reconcile_items

## Decisions Made
- Overflow in decode paths returns nullopt/empty (same error path as malformed input, per D-03)
- Encode reserve() calls silently skip reservation on overflow (vector grows dynamically, no harm)
- Response builders in message_dispatcher.cpp record strike and co_return on overflow (defensive, practically unreachable since counts come from local data bounded by storage limits)
- No new headers or dependencies -- everything added to existing endian.h

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Build required CMake reconfiguration in worktree (shared build directory points to main repo source). Resolved by configuring a local build with pre-downloaded dependency sources.

## User Setup Required

None - no external service configuration required.

## Known Stubs

None - all overflow-checked paths are fully implemented and tested.

## Next Phase Readiness
- PROTO-01 fully satisfied -- all protocol parsing integer arithmetic uses overflow-checked helpers
- Ready for 97-02 (auth payload validation, AEAD bounds, nonce exhaustion) and 97-03 (lightweight handshake auth)

## Self-Check: PASSED

All 8 modified files exist. Both task commits (6f2fa90, add7283) verified in git log.

---
*Phase: 97-protocol-crypto-safety*
*Completed: 2026-04-08*
