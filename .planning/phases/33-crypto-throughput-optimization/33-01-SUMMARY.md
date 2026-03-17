---
phase: 33-crypto-throughput-optimization
plan: 01
subsystem: crypto
tags: [sha3-256, ml-dsa-87, liboqs, incremental-hashing, thread-local, signing]

# Dependency graph
requires:
  - phase: 32-test-relocation
    provides: test infrastructure in db/tests/ layout
provides:
  - build_signing_input() returns 32-byte SHA3-256 digest (zero intermediate allocation)
  - Signer::verify() uses thread_local OQS_SIG context (zero per-call allocation)
  - Updated PROTOCOL.md documents hash-then-sign scheme
affects: [33-02-store-path-optimization, 33-03-dedup-before-crypto]

# Tech tracking
tech-stack:
  added: []
  patterns: [incremental SHA3-256 via OQS_SHA3_sha3_256_inc_*, thread_local OQS_SIG caching]

key-files:
  created: []
  modified: [db/wire/codec.h, db/wire/codec.cpp, db/crypto/signing.cpp, db/tests/wire/test_codec.cpp]

key-decisions:
  - "Incremental SHA3-256 via liboqs OQS_SHA3_sha3_256_inc_* eliminates 1 MiB intermediate allocation"
  - "thread_local OQS_SIG* in Signer::verify() is future-safe for v0.8.0 thread pool offload"

patterns-established:
  - "Incremental hashing: feed large data directly into SHA3 sponge via OQS_SHA3_sha3_256_inc_absorb"
  - "Process-lifetime caching: thread_local for expensive crypto context objects"

requirements-completed: [PERF-04, PERF-02]

# Metrics
duration: 9min
completed: 2026-03-17
---

# Phase 33 Plan 01: Hash-then-sign + OQS_SIG Caching Summary

**build_signing_input() returns 32-byte SHA3-256 digest via incremental hashing, Signer::verify() uses thread_local OQS_SIG -- both zero-allocation optimizations**

## Performance

- **Duration:** 9 min
- **Started:** 2026-03-17T13:58:29Z
- **Completed:** 2026-03-17T14:07:00Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Rewrote build_signing_input() to use incremental SHA3-256 (OQS_SHA3_sha3_256_inc_*), eliminating the ~1 MiB intermediate allocation for large blob signing
- Changed return type from std::vector<uint8_t> to std::array<uint8_t, 32> -- all 10+ callers across production code, tests, loadgen, and bench compiled without code changes (auto + span implicit conversion)
- Added thread_local OQS_SIG* caching in Signer::verify(), eliminating per-call OQS_SIG_new/OQS_SIG_free pair
- Updated 3 build_signing_input test cases to verify SHA3-256 digest correctness

## Task Commits

Each task was committed atomically:

1. **Task 1: Rewrite build_signing_input() + OQS_SIG caching + production callers** - `a97b074` (feat)
2. **Task 2: Update all test helpers and verify full suite passes** - `a3a0c52` (test)

## Files Created/Modified
- `db/wire/codec.h` - Changed build_signing_input() return type to std::array<uint8_t, 32>
- `db/wire/codec.cpp` - Incremental SHA3-256 implementation using OQS_SHA3_sha3_256_inc_* API
- `db/crypto/signing.cpp` - thread_local OQS_SIG* in Signer::verify()
- `db/tests/wire/test_codec.cpp` - Updated 3 tests for SHA3-256 digest format

## Decisions Made
- No production callers (engine.cpp, loadgen, bench) required code changes -- auto type inference + span implicit conversion handled the return type change cleanly
- Test helpers in test_engine.cpp, test_peer_manager.cpp, test_sync_protocol.cpp, test_daemon.cpp also required zero changes -- same auto + span pattern
- PROTOCOL.md already documented the SHA3-256 scheme (updated in prior context session), verified correct

## Deviations from Plan

None -- plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- PERF-04 and PERF-02 complete, ready for PERF-03 (dedup-before-crypto) and PERF-01/PERF-05 (store path optimization)
- All 313 tests pass, protocol change is clean break (pre-MVP, no backward compat needed)

## Self-Check: PASSED

All files verified present. All commits verified in git log.

---
*Phase: 33-crypto-throughput-optimization*
*Completed: 2026-03-17*
