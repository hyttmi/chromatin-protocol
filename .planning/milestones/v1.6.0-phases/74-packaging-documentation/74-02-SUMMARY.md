---
phase: 74-packaging-documentation
plan: 02
subsystem: docs
tags: [protocol, hkdf, sdk-notes, documentation]

# Dependency graph
requires:
  - phase: 70-crypto-foundation-identity
    provides: PQ handshake implementation revealing HKDF empty salt
  - phase: 71-transport-pq-handshake
    provides: Transport layer revealing nonce counter and Pong behavior
  - phase: 72-core-data-operations
    provides: Codec revealing endianness and FlatBuffers non-determinism
  - phase: 73-extended-queries-pub-sub
    provides: Full SDK revealing ML-DSA-87 non-determinism
provides:
  - Corrected HKDF salt documentation in PROTOCOL.md (empty salt, not SHA3-256(pubkeys))
  - SDK Client Notes section with 6 implementation gotchas for future SDK developers
affects: [future-sdks, sdk-c, sdk-rust, sdk-js]

# Tech tracking
tech-stack:
  added: []
  patterns: []

key-files:
  created: []
  modified:
    - db/PROTOCOL.md

key-decisions:
  - "HKDF empty salt documented as matching C++ implementation -- previous SHA3-256(pubkeys) description was never implemented"
  - "SDK Client Notes section covers 6 protocol edge cases discovered during Python SDK development"

patterns-established:
  - "SDK Client Notes: centralized location for cross-language protocol gotchas"

requirements-completed: [DOCS-01]

# Metrics
duration: 1min
completed: 2026-03-31
---

# Phase 74 Plan 02: PROTOCOL.md Corrections Summary

**Fixed HKDF salt discrepancy (empty salt, not SHA3-256(pubkeys)) and added SDK Client Notes section with 6 protocol implementation gotchas**

## Performance

- **Duration:** 1 min
- **Started:** 2026-03-31T13:59:11Z
- **Completed:** 2026-03-31T14:01:00Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments
- Fixed HKDF salt in both diagram (line 69) and prose (line 91) to match C++ implementation (empty salt)
- Removed misleading sentence about responder pubkey being "initially unknown"
- Added SDK Client Notes section with 6 implementation details: AEAD nonce counters, Pong request_id behavior, mixed endianness, exception hierarchy, FlatBuffers determinism, ML-DSA-87 non-determinism

## Task Commits

Each task was committed atomically:

1. **Task 1: Fix HKDF salt discrepancy in PQ handshake section** - `2404dd4` (docs)
2. **Task 2: Add SDK Client Notes section to PROTOCOL.md** - `4c2dd19` (docs)

## Files Created/Modified
- `db/PROTOCOL.md` - Fixed HKDF salt from SHA3-256(pubkeys) to empty; added SDK Client Notes section at end of file

## Decisions Made
None - followed plan as specified

## Deviations from Plan
None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- PROTOCOL.md is now accurate and includes SDK implementation notes
- Future SDK developers (C, Rust, JS) can reference the SDK Client Notes section for protocol edge cases
- Combined with Plan 01 (packaging/tutorial/README), Phase 74 is complete

---
*Phase: 74-packaging-documentation*
*Completed: 2026-03-31*

## Self-Check: PASSED
