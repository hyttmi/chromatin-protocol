---
phase: 106-bug-fixes
plan: 01
subsystem: relay
tags: [translator, compound-decoder, wire-format, json-schema, binary-to-json]

# Dependency graph
requires:
  - phase: 103-uds-multiplexer-protocol-translation
    provides: translator.cpp compound decoders (original buggy implementations)
provides:
  - Fixed compound decoders matching node wire format for NodeInfoResponse, StatsResponse, TimeRangeResponse, DelegationListResponse
  - StatsRequest namespace field in JSON schema
  - Comprehensive decoder unit tests with binary fixtures
affects: [107-message-type-verification]

# Tech tracking
tech-stack:
  added: []
  patterns: [u8-length-prefix decoding, raw-type-byte translation via type_to_string]

key-files:
  modified:
    - relay/translate/translator.cpp
    - relay/translate/json_schema.h
    - relay/translate/json_schema.cpp
    - relay/tests/test_translator.cpp
    - relay/tests/test_type_registry.cpp

key-decisions:
  - "StatsResponse field names: blob_count, storage_bytes, quota_bytes_limit (per-namespace semantics)"
  - "NodeInfoResponse type_to_string fallback: unknown type bytes rendered as numeric strings"

patterns-established:
  - "Compound decoder bounds checking: every read preceded by off+N > p.size() guard returning nullopt"
  - "Binary test fixtures: inline byte vectors matching node wire format for decoder validation"

requirements-completed: [FIX-01]

# Metrics
duration: 11min
completed: 2026-04-11
---

# Phase 106 Plan 01: Compound Decoder Bug Fixes Summary

**Rewrote 4 compound decoders to match node wire format: u8 string lengths for NodeInfoResponse, 24-byte StatsResponse compound decoder, truncated-first TimeRangeResponse with seq_num, correct DelegationListResponse field names**

## Performance

- **Duration:** 11 min
- **Started:** 2026-04-11T04:05:08Z
- **Completed:** 2026-04-11T04:16:28Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments
- Fixed NodeInfoResponse decoder: u8 length prefixes instead of u16BE, raw type bytes translated via type_to_string() instead of length-prefixed strings
- Created new StatsResponse compound decoder for 24-byte format (blob_count/storage_bytes/quota_bytes_limit), replacing broken flat field decode
- Fixed TimeRangeResponse: truncated byte is first (not last), entries are 48 bytes including seq_num (not 40)
- Fixed DelegationListResponse field names from namespace/pubkey_hash to delegate_pk_hash/delegation_blob_hash
- Added StatsRequest namespace field to JSON schema (was missing entirely)
- Added 17 new unit tests with binary fixtures matching node wire format, including bounds-check regression tests for all compound decoders
- Test count increased from 205 to 222, all passing

## Task Commits

Each task was committed atomically:

1. **Task 1: Fix compound decoders and schema** - `0db1612` (fix)
2. **Task 2: Unit tests for fixed decoders** - `ef87f14` (test)

## Files Created/Modified
- `relay/translate/translator.cpp` - Rewrote 4 compound decoders, added decode_stats_response, wired case 36 dispatch
- `relay/translate/json_schema.h` - Added namespace field to STATS_REQUEST_FIELDS
- `relay/translate/json_schema.cpp` - Changed StatsResponse(36) to is_compound=true with NO_FIELDS
- `relay/tests/test_translator.cpp` - 17 new test cases with binary fixtures, updated 3 existing tests for new wire format
- `relay/tests/test_type_registry.cpp` - Updated FieldEncoding coverage test to check write_ack instead of now-compound stats_response

## Decisions Made
- StatsResponse uses field names blob_count/storage_bytes/quota_bytes_limit matching per-namespace semantics (research recommendation)
- Unknown type bytes in NodeInfoResponse rendered as numeric strings (e.g., "255") rather than silently dropped
- BatchExistsResponse with empty input produces empty results array (not nullopt) -- confirmed correct behavior

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Updated FieldEncoding enum coverage test**
- **Found during:** Task 1
- **Issue:** test_type_registry.cpp checked stats_response for UINT64_STRING encoding, but stats_response is now compound with NO_FIELDS
- **Fix:** Changed test to check write_ack (which has UINT64_STRING for seq_num field)
- **Files modified:** relay/tests/test_type_registry.cpp
- **Verification:** Test passes
- **Committed in:** 0db1612 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug fix)
**Impact on plan:** Necessary to keep existing test suite passing. No scope creep.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All compound decoders now match node wire format, ready for Phase 107 E2E type verification
- StatsRequest properly encodes namespace, enabling per-namespace stats queries through relay
- 222 relay tests passing as foundation for further testing

## Self-Check: PASSED

- All 5 source/test files exist on disk
- Commit 0db1612 (Task 1) verified in git log
- Commit ef87f14 (Task 2) verified in git log
- 222 relay tests pass, 0 failures

---
*Phase: 106-bug-fixes*
*Completed: 2026-04-11*
