---
phase: 72-core-data-operations
plan: 02
subsystem: sdk
tags: [python, async, flatbuffers, client-api, blob-lifecycle]

# Dependency graph
requires:
  - phase: 72-01
    provides: "_codec.py encode/decode functions, types.py result dataclasses"
  - phase: 71
    provides: "Transport.send_request(), ChromatinClient connect/ping lifecycle"
  - phase: 70
    provides: "Identity.sign/namespace/public_key, crypto.build_signing_input"
provides:
  - "ChromatinClient.write_blob() -- signed blob write with auto-timestamp"
  - "ChromatinClient.read_blob() -- blob fetch returning ReadResult or None"
  - "ChromatinClient.delete_blob() -- tombstone-based deletion"
  - "ChromatinClient.list_blobs() -- cursor-paginated namespace listing"
  - "ChromatinClient.exists() -- lightweight existence check"
  - "ChromatinClient._request_with_timeout() -- D-16 timeout wrapping"
  - "31 unit tests for all 5 methods"
affects: [73-extended-queries, 74-tutorial-docs]

# Tech tracking
tech-stack:
  added: [pytest-asyncio]
  patterns: [mock-transport-testing, timeout-wrapping-d16, tombstone-construction]

key-files:
  created:
    - sdk/python/tests/test_client_ops.py
  modified:
    - sdk/python/chromatindb/client.py
    - sdk/python/chromatindb/__init__.py

key-decisions:
  - "Use chromatindb.exceptions.ConnectionError (custom SDK class) for timeout wrapping, not builtin ConnectionError"
  - "Decode FlatBuffer fields individually in tombstone test instead of byte-for-byte payload comparison (ML-DSA-87 signatures are non-deterministic)"

patterns-established:
  - "Mock transport pattern: MagicMock with AsyncMock send_request for unit testing client methods"
  - "Response payload helpers: make_write_ack_payload(), make_list_response() etc. for constructing test fixtures"
  - "_request_with_timeout wraps all transport calls to convert asyncio.TimeoutError to ConnectionError per D-16"

requirements-completed: [DATA-01, DATA-02, DATA-03, DATA-04, DATA-05, DATA-06]

# Metrics
duration: 5min
completed: 2026-03-30
---

# Phase 72 Plan 02: Client Data Operations Summary

**5 async blob lifecycle methods (write/read/delete/list/exists) on ChromatinClient with 31 unit tests using mock transport**

## Performance

- **Duration:** 5 min
- **Started:** 2026-03-30T02:29:18Z
- **Completed:** 2026-03-30T02:34:33Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- ChromatinClient now exposes all 5 core data operations as public async methods
- Each method composes codec (72-01), transport (71), identity (70), and crypto (70) layers correctly
- _request_with_timeout helper centralizes D-16 timeout wrapping -- asyncio.TimeoutError never leaks to callers
- 31 comprehensive unit tests cover success, error responses, input validation, edge cases, and timeout behavior

## Task Commits

Each task was committed atomically:

1. **Task 1: Add 5 data operation methods to ChromatinClient** - `3ba91c8` (feat)
2. **Task 2: Unit tests for all 5 client data operation methods** - `b4e8a64` (test)

## Files Created/Modified
- `sdk/python/chromatindb/client.py` - Added _request_with_timeout, write_blob, read_blob, delete_blob, list_blobs, exists methods (+243 lines)
- `sdk/python/chromatindb/__init__.py` - Exports WriteResult, ReadResult, DeleteResult, BlobRef, ListPage
- `sdk/python/tests/test_client_ops.py` - 31 unit tests with mock transport covering all 5 methods

## Decisions Made
- Used SDK's custom `chromatindb.exceptions.ConnectionError` (inherits ProtocolError) for D-16 timeout wrapping, not Python's builtin `ConnectionError`. Consistent with transport layer's pattern (alias `ChromatinConnectionError`).
- Tombstone TTL=0 test decodes FlatBuffer fields individually rather than comparing full serialized payloads, because ML-DSA-87 signatures are non-deterministic (calling sign() twice with same input produces different output).

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All 5 core blob lifecycle operations ready for integration testing (Plan 03)
- Extended query methods (Phase 73) can follow same _request_with_timeout + codec pattern
- Mock transport testing pattern established for future method tests

---
*Phase: 72-core-data-operations*
*Completed: 2026-03-30*
