---
phase: 102-authentication-json-schema
plan: 02
subsystem: relay
tags: [json, schema, type-registry, message-filter, websocket, allowlist]

# Dependency graph
requires:
  - phase: 102-01
    provides: WsSession auth state machine, Authenticator, SessionState enum
provides:
  - Bidirectional type registry mapping 40 JSON type strings to wire type integers
  - JSON schema metadata with field encoding rules (HEX_32, BASE64, UINT64_STRING, etc.)
  - Message type allowlist filter for 38 client-allowed types
  - WsSession AUTHENTICATED path with JSON parsing and message filtering
affects: [103-json-translation, 104-subscription-routing]

# Tech tracking
tech-stack:
  added: []
  patterns: [constexpr sorted array with binary_search for O(log n) lookups, FieldSpec/MessageSchema metadata-driven schema]

key-files:
  created:
    - relay/translate/type_registry.h
    - relay/translate/type_registry.cpp
    - relay/translate/json_schema.h
    - relay/translate/json_schema.cpp
    - relay/core/message_filter.h
    - relay/core/message_filter.cpp
    - relay/tests/test_type_registry.cpp
    - relay/tests/test_message_filter.cpp
  modified:
    - relay/ws/ws_session.cpp
    - relay/CMakeLists.txt
    - relay/tests/CMakeLists.txt

key-decisions:
  - "TYPE_REGISTRY has 40 entries: 38 client types + StorageFull(22) + QuotaExceeded(25) for outbound translation"
  - "Message filter allowlist is exactly 38 client-sendable types; storage_full/quota_exceeded excluded from is_type_allowed"
  - "Schema lookup uses sorted wire_type array for binary search in schema_for_type()"
  - "FlatBuffer types (Data/ReadResponse/BatchReadResponse) marked is_flatbuffer=true with empty field specs"

patterns-established:
  - "Sorted constexpr arrays with std::binary_search for type lookups"
  - "FieldSpec/MessageSchema metadata-driven schema definition (Phase 103 implements translation from this)"
  - "Message filter check at JSON parse time before any further processing"

requirements-completed: [PROT-02, PROT-03, PROT-05]

# Metrics
duration: 10min
completed: 2026-04-09
---

# Phase 102 Plan 02: JSON Schema & Message Filter Summary

**Type registry with 40 bidirectional mappings, JSON schema metadata with 12 field encoding types, and constexpr message allowlist wired into WsSession AUTHENTICATED path**

## Performance

- **Duration:** 10 min
- **Started:** 2026-04-09T16:10:20Z
- **Completed:** 2026-04-09T16:20:42Z
- **Tasks:** 2
- **Files modified:** 11

## Accomplishments
- Type registry maps all 40 type entries (38 client + 2 node signals) with binary search on sorted json_name array
- JSON schema defines FieldEncoding enum with 12 encoding types and per-message FieldSpec arrays for all non-FlatBuffer types
- Message filter accepts exactly 38 client-allowed types and blocks peer-internal types (sync, PEX, KEM, etc.)
- WsSession AUTHENTICATED path parses JSON, checks type allowlist, returns error with request_id for blocked types
- 24 new test cases (93 total relay tests), all passing

## Task Commits

Each task was committed atomically:

1. **Task 1: Type registry, JSON schema metadata, message filter, and unit tests** - `9921a95` (feat)
2. **Task 2: Wire message filter into WsSession AUTHENTICATED path** - `f39268a` (feat)

## Files Created/Modified
- `relay/translate/type_registry.h` - Constexpr TYPE_REGISTRY array (40 entries), type_from_string/type_to_string declarations
- `relay/translate/type_registry.cpp` - Binary search lookup for string->wire, linear scan for wire->string
- `relay/translate/json_schema.h` - FieldEncoding enum, FieldSpec/MessageSchema structs, per-type field arrays for all message types
- `relay/translate/json_schema.cpp` - SCHEMAS array sorted by wire_type, schema_for_type/schema_for_name lookups
- `relay/core/message_filter.h` - is_type_allowed/is_wire_type_allowed declarations, ALLOWED_TYPE_COUNT=38
- `relay/core/message_filter.cpp` - Sorted allowlists (38 type strings, 40 wire types) with binary_search
- `relay/ws/ws_session.cpp` - AUTHENTICATED path: JSON parse, type extract, filter check, error with request_id
- `relay/CMakeLists.txt` - Added translate/type_registry.cpp, translate/json_schema.cpp, core/message_filter.cpp
- `relay/tests/test_type_registry.cpp` - 15 test cases: sort validation, roundtrip, schema lookups, FlatBuffer detection
- `relay/tests/test_message_filter.cpp` - 9 test cases: allow/block string types, wire types, auth types, node signals
- `relay/tests/CMakeLists.txt` - Added test_type_registry.cpp, test_message_filter.cpp

## Decisions Made
- TYPE_REGISTRY sorted alphabetically requires "delegation_list_*" before "delete*" (lexicographic: "deleg" < "delet") -- fixed during implementation
- schema_for_type uses binary search on wire_type-sorted SCHEMAS array for efficient lookup
- FlatBuffer types (Data=8, ReadResponse=32, BatchReadResponse=54) have empty field specs; their schema is in .fbs files
- Complex response types (BatchExistsResponse, DelegationListResponse, TimeRangeResponse) have simplified field specs; Phase 103 will refine

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed TYPE_REGISTRY sort order**
- **Found during:** Task 1 (type registry creation)
- **Issue:** Plan's TYPE_REGISTRY had "delete"/"delete_ack" before "delegation_list_*", but lexicographic order requires "delegation_list_*" first ("deleg" < "delet")
- **Fix:** Reordered entries in both TYPE_REGISTRY and ALLOWED_TYPE_STRINGS
- **Files modified:** relay/translate/type_registry.h, relay/core/message_filter.cpp
- **Verification:** Sort validation test passes, all binary search lookups correct
- **Committed in:** 9921a95 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Sort order fix was essential for binary search correctness. No scope creep.

## Issues Encountered
None beyond the sort order fix documented above.

## Known Stubs
None. All schema metadata is complete for Phase 103 to implement translation.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 103 has a clear contract: type_registry for string<->wire mapping, json_schema for field encoding rules, message_filter for validation
- The translate/ directory has schema metadata ready for JSON<->FlatBuffers translation implementation
- WsSession AUTHENTICATED path is wired and ready for forwarding logic (currently logs accepted messages)

## Self-Check: PASSED

All 8 created files verified present. Both task commits (9921a95, f39268a) verified in git log. 93/93 relay tests passing.

---
*Phase: 102-authentication-json-schema*
*Completed: 2026-04-09*
