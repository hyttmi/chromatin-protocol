---
phase: 97-protocol-crypto-safety
plan: 03
subsystem: crypto
tags: [handshake, authentication, ML-DSA-87, AEAD, lightweight-handshake]

# Dependency graph
requires:
  - phase: 97-protocol-crypto-safety/02
    provides: auth_helpers.h, verify_helpers.h, pubkey validation
provides:
  - Lightweight handshake AuthSignature exchange (initiator + responder)
  - Pubkey binding verification in lightweight path (prevents MitM)
  - Integration tests for lightweight auth (happy path + rejection)
affects: [peer-manager, connection-lifecycle]

# Tech tracking
tech-stack:
  added: []
  patterns: [AuthSignature exchange in all handshake paths, initiator-sends-first ordering]

key-files:
  created: []
  modified:
    - db/net/connection.cpp
    - db/tests/net/test_connection.cpp

key-decisions:
  - "AuthSignature exchange added to lightweight path mirrors PQ path pattern exactly"
  - "Initiator sends first, responder receives then sends (prevents AEAD nonce desync)"

patterns-established:
  - "All handshake paths (PQ, lightweight, fallback) now exchange AuthSignatures before setting authenticated_"

requirements-completed: [CRYPTO-03]

# Metrics
duration: 72min
completed: 2026-04-08
---

# Phase 97 Plan 03: Lightweight Handshake AuthSignature Exchange Summary

**Challenge-response authentication added to lightweight handshake, closing the identity impersonation gap where knowing a peer's pubkey sufficed for trusted-path access**

## Performance

- **Duration:** 72 min
- **Started:** 2026-04-08T13:53:49Z
- **Completed:** 2026-04-08T15:05:49Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Added encrypted AuthSignature exchange to both lightweight handshake paths (initiator and responder), ensuring peers prove they hold their signing secret key
- Pubkey from AuthSignature verified against pubkey from TrustedHello, preventing MitM substitution attacks
- Integration test confirms both happy path (mutual auth with correct peer pubkeys) and rejection path (malicious responder with wrong pubkey rejected)
- Full sanitizer gate passed: 3206 assertions in 667 test cases, zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Add AuthSignature exchange to lightweight handshake initiator and responder** - `ab0819f` (feat)
2. **Task 2: Add lightweight handshake authentication tests and sanitizer gate** - `3b05ec1` (test)

## Files Created/Modified
- `db/net/connection.cpp` - AuthSignature exchange in do_handshake_initiator_trusted and do_handshake_responder_trusted
- `db/tests/net/test_connection.cpp` - Two new tests: happy path auth verification + malicious pubkey mismatch rejection

## Decisions Made
- AuthSignature exchange in lightweight path mirrors PQ path pattern exactly (same encode/decode/verify helpers)
- Initiator sends first, responder receives then sends -- matches PQ path order, prevents AEAD nonce desync

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Build system: worktree required separate CMake configure since FetchContent dependencies are source-tree-specific. Resolved by configuring build directory within the worktree.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All Phase 97 requirements (PROTO-01 through PROTO-04, CRYPTO-01 through CRYPTO-03) are now satisfied
- Phase 97 complete, ready for Phase 98 (TTL Enforcement) or Phase 99 (Sync, Resource & Concurrency Correctness)

## Self-Check: PASSED

- All created/modified files verified to exist on disk
- All task commits verified in git log (ab0819f, 3b05ec1)

---
*Phase: 97-protocol-crypto-safety*
*Completed: 2026-04-08*
