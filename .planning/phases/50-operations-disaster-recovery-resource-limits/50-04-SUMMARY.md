---
phase: 50-operations-disaster-recovery-resource-limits
plan: 04
subsystem: testing
tags: [integration-test, docker, storage-full, namespace-quota, thread-pool, dos-resistance, sighup]

# Dependency graph
requires:
  - phase: 50-03-PLAN
    provides: "DOS-01/02/03 resource DoS tests (rate limiting, sync cooldown, session limits)"
provides:
  - "DOS-04 storage full signaling + SIGHUP recovery integration test"
  - "DOS-05 namespace quota enforcement + isolation integration test"
  - "DOS-06 thread pool saturation resilience integration test"
  - "Engine::set_max_storage_bytes() for SIGHUP config reload"
affects: [v1.0.0-milestone, integration-tests]

# Tech tracking
tech-stack:
  added: []
  patterns: ["SIGHUP max_storage_bytes reload", "standalone Docker test topology with named volumes"]

key-files:
  created:
    - tests/integration/test_dos04_storage_full.sh
    - tests/integration/test_dos05_namespace_quotas.sh
    - tests/integration/test_dos06_thread_pool_saturation.sh
  modified:
    - db/engine/engine.h
    - db/engine/engine.cpp
    - db/peer/peer_manager.cpp

key-decisions:
  - "max_storage_bytes minimum 2 MiB for DOS-04 test (mdbx file starts at ~1 MiB empty)"
  - "Added Engine::set_max_storage_bytes() for SIGHUP reload (was missing from reload path)"
  - "DOS-06 verifies handshake completion count rather than live peer count (loadgens disconnect before check)"
  - "METRICS DUMP marker used for SIGUSR1 response counting (avoids metrics: prefix ambiguity)"

patterns-established:
  - "SIGHUP reload for max_storage_bytes: consistent with existing quota/rate-limit reload path"
  - "Standalone Docker topology per test: unique network, named volumes, direct docker run (not compose)"

requirements-completed: [DOS-04, DOS-05, DOS-06]

# Metrics
duration: 22min
completed: 2026-03-21
---

# Phase 50 Plan 04: Resource DoS Resistance Tests Summary

**Storage full signaling with SIGHUP recovery, namespace quota enforcement with isolation, and thread pool saturation resilience -- verified via Docker integration tests**

## Performance

- **Duration:** 22 min
- **Started:** 2026-03-21T16:54:24Z
- **Completed:** 2026-03-21T17:16:32Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- DOS-04: StorageFull signaled at 2 MiB limit, SIGHUP recovery raises to 10 MiB, post-recovery writes succeed and sync to peer
- DOS-05: namespace_quota_count=5 enforced per namespace, 3 namespaces verified independently (5+5+3=13 blobs), quota_rejections=5 confirmed
- DOS-06: 4 concurrent loadgen instances (800 ML-DSA-87 verify/sec) do not starve event loop -- new connections accepted and SIGUSR1 responds
- Added Engine::set_max_storage_bytes() to support SIGHUP config reload of storage capacity limit

## Task Commits

Each task was committed atomically:

1. **Task 1: DOS-04 storage full + DOS-05 namespace quotas** - `a40a2a3` (feat)
2. **Task 2: DOS-06 thread pool saturation** - `5714813` (feat, includes DOS-06 fixes)

## Files Created/Modified
- `tests/integration/test_dos04_storage_full.sh` - 2-node storage full signaling + SIGHUP recovery test
- `tests/integration/test_dos05_namespace_quotas.sh` - Single-node namespace quota enforcement + isolation test
- `tests/integration/test_dos06_thread_pool_saturation.sh` - 4-loadgen thread pool saturation resilience test
- `db/engine/engine.h` - Added set_max_storage_bytes() method declaration
- `db/engine/engine.cpp` - Added set_max_storage_bytes() implementation
- `db/peer/peer_manager.cpp` - Wire max_storage_bytes reload into SIGHUP reload_config()

## Decisions Made
- max_storage_bytes set to 2 MiB (not 200KB as planned): mdbx file starts at ~1 MiB even empty, so 200KB would be below minimum (1 MiB) and instant full. 2 MiB gives ~1 MiB headroom for ~78 blobs.
- Added Engine::set_max_storage_bytes(): SIGHUP reload_config() did not reload max_storage_bytes (only quotas, rates, ACLs). Added setter for recovery test to work.
- DOS-06 verifies handshake completion via Node1 log count rather than live peer count: loadgens disconnect before the verification check, so live peers=0 is expected.
- METRICS DUMP marker for SIGUSR1 check: the `metrics:` prefix in the one-liner format is ambiguous (matches quota_rejections, sync_rejections). Using `METRICS DUMP` is unique to SIGUSR1.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing Critical] Added Engine::set_max_storage_bytes() for SIGHUP reload**
- **Found during:** Task 1 (DOS-04 test design)
- **Issue:** PeerManager::reload_config() did not reload max_storage_bytes from config on SIGHUP. Only quota, rate limit, ACL, and sync config were reloaded. This made storage full recovery via SIGHUP impossible.
- **Fix:** Added Engine::set_max_storage_bytes(uint64_t) setter method and wired it into reload_config() with info-level logging.
- **Files modified:** db/engine/engine.h, db/engine/engine.cpp, db/peer/peer_manager.cpp
- **Verification:** DOS-04 test confirms max_storage_bytes=10485760 log after SIGHUP, and subsequent writes succeed.
- **Committed in:** a40a2a3 (Task 1 commit)

**2. [Rule 1 - Bug] Fixed max_storage_bytes from 200KB to 2 MiB**
- **Found during:** Task 1 (initial DOS-04 test run)
- **Issue:** Plan specified max_storage_bytes=204800 (200KB) but config validation requires >= 1048576 (1 MiB). Additionally, mdbx used_bytes() starts at ~1 MiB even empty, so 1 MiB would be instantly full.
- **Fix:** Used max_storage_bytes=2097152 (2 MiB) giving ~1 MiB headroom for blob storage.
- **Files modified:** tests/integration/test_dos04_storage_full.sh
- **Verification:** Node accepts ~78 blobs before filling, StorageFull signaled correctly.
- **Committed in:** a40a2a3 (Task 1 commit)

---

**Total deviations:** 2 auto-fixed (1 missing critical, 1 bug)
**Impact on plan:** Both auto-fixes necessary for the DOS-04 test to work. The max_storage_bytes reload is a genuine missing feature that should have been in the SIGHUP path. No scope creep.

## Issues Encountered
- metrics: grep pattern matched quota_rejections and sync_rejections (resolved by using lookbehind assertion `(?<= )rejections=`)
- METRICS DUMP count via grep -c produced multiline output on some Docker log streams (resolved by piping through `tr -d '[:space:]'`)
- Node2 bootstrap connection to Node1 under load races with loadgen completion (resolved by checking Node1 handshake count rather than Node2 connection log)

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All 6 DOS resource resistance tests (DOS-01 through DOS-06) now verified via integration tests
- Phase 50 test suite covers: OPS signals (01), DARE/master key (02), session limits (03), and resource limits (04)
- Ready for next milestone planning or additional hardening

---
## Self-Check: PASSED

All files found, all commits verified, set_max_storage_bytes confirmed in engine.h and peer_manager.cpp reload path.

---
*Phase: 50-operations-disaster-recovery-resource-limits*
*Completed: 2026-03-21*
