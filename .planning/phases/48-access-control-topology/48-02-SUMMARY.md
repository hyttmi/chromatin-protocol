---
phase: 48-access-control-topology
plan: 02
subsystem: testing
tags: [integration-tests, acl, docker, delegation, revocation, tombstone, loadgen]

# Dependency graph
requires:
  - phase: 48-access-control-topology
    plan: 01
    provides: "Loadgen --delegate flag, docker-compose.acl.yml, ACL-01/02 tests"
provides:
  - "Loadgen --namespace flag for delegation writes to foreign namespace"
  - "ACL-03 delegation write + write-only enforcement integration test"
  - "ACL-04 delegation revocation propagation integration test"
  - "node3-acl.json config for 3-node mesh connectivity"
affects: [48-03]

# Tech tracking
tech-stack:
  added: []
  patterns: ["Loadgen --namespace override for delegation writes targeting foreign namespace"]

key-files:
  created:
    - tests/integration/test_acl03_delegation_write.sh
    - tests/integration/test_acl04_revocation.sh
    - tests/integration/configs/node3-acl.json
  modified:
    - loadgen/loadgen_main.cpp
    - tests/integration/docker-compose.acl.yml

key-decisions:
  - "Added --namespace flag to loadgen for delegation writes (delegate must target owner's namespace_id, not its own)"
  - "Delegate tombstone rejection comes from delete handler's namespace_mismatch check, not engine's delegation check"
  - "Fixed-sleep propagation waits (30s) instead of wait_sync for cross-node tombstone propagation"

patterns-established:
  - "Delegation write pattern: --identity-file delegate_dir --namespace OWNER_NS_HEX targets the owner's namespace with delegate's key"
  - "Revocation pattern: owner tombstones delegation blob hash via --delete --hashes-from stdin --ttl 0"

requirements-completed: [ACL-03, ACL-04]

# Metrics
duration: 32min
completed: 2026-03-21
---

# Phase 48 Plan 02: Delegation Lifecycle Integration Tests Summary

**ACL-03 delegation write (cross-node, write-only enforcement) and ACL-04 revocation propagation (tombstone TTL=0, post-revocation rejection) integration tests with loadgen --namespace flag**

## Performance

- **Duration:** 32 min
- **Started:** 2026-03-21T09:25:19Z
- **Completed:** 2026-03-21T09:57:48Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments
- ACL-03: Delegate writes 5 blobs to owner's namespace on Node2 (0 errors), blobs sync to all 3 nodes, delegate tombstone rejected (owner-only delete path)
- ACL-04: Pre-revocation delegate writes succeed, owner tombstones delegation blob (TTL=0), post-revocation delegate writes rejected with "no ownership or delegation", blob count stable
- Added --namespace flag to loadgen enabling delegation writes to foreign namespaces (required for delegate to target owner's namespace)

## Task Commits

Each task was committed atomically:

1. **Task 1: ACL-03 delegation write and write-only enforcement test** - `074239d` (test)
2. **Task 2: ACL-04 delegation revocation propagation test** - `2992c4c` (test)

## Files Created/Modified
- `loadgen/loadgen_main.cpp` - Added --namespace flag, ns_override parameter to make_signed_blob/make_tombstone_request, target_namespace_ member in LoadGenerator
- `tests/integration/test_acl03_delegation_write.sh` - Delegation write + write-only enforcement test (3-node cluster)
- `tests/integration/test_acl04_revocation.sh` - Delegation revocation propagation test (3-node cluster)
- `tests/integration/configs/node3-acl.json` - Node3 config with bootstrap to Node1 for 3-node mesh
- `tests/integration/docker-compose.acl.yml` - Fixed Node3 to use node3-acl.json for mesh connectivity

## Decisions Made
- **--namespace flag required for delegation writes:** Loadgen always sets namespace_id = SHA3(own_pubkey). For a delegate to write to the OWNER's namespace, it must override namespace_id. The --namespace flag decodes a 64-char hex namespace and passes it through blob construction.
- **Delegate tombstone rejection path:** The delete handler (engine.cpp) checks namespace ownership directly (SHA3(pubkey) != namespace_id) without delegation support. This is correct by design: deletion is owner-privileged. The rejection message is "SHA3-256(pubkey) != namespace_id" rather than "delegates cannot create tombstone" (which would fire if the tombstone went through the Data message handler instead).
- **Fixed-sleep propagation for tombstone sync:** Used 30s fixed sleep for tombstone propagation instead of wait_sync polling, since tombstone propagation involves complex state changes (delegation invalidation) across the cluster.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Added --namespace flag to loadgen for delegation writes**
- **Found during:** Task 1 (ACL-03 test)
- **Issue:** Loadgen hardcodes namespace_id = SHA3(identity pubkey). Delegates need to write to the owner's namespace, not their own. Without --namespace, delegation writes are actually owner writes to the delegate's own namespace, making delegation untestable.
- **Fix:** Added --namespace NS_HEX flag with ns_override parameter threaded through make_signed_blob, make_tombstone_request, subscribe_and_send, and LoadGenerator class
- **Files modified:** loadgen/loadgen_main.cpp
- **Verification:** ACL-03 passes: delegate writes 5 blobs to owner's namespace (0 errors)
- **Committed in:** 074239d (Task 1 commit)

**2. [Rule 1 - Bug] Fixed Node3 isolation in docker-compose.acl.yml**
- **Found during:** Task 1 (ACL-03 test)
- **Issue:** Node3 used node1.json (no bootstrap_peers), leaving it disconnected from the cluster. Blobs never synced to Node3.
- **Fix:** Created node3-acl.json with bootstrap to 172.28.0.2:4200, updated docker-compose.acl.yml to use it
- **Files modified:** tests/integration/docker-compose.acl.yml, tests/integration/configs/node3-acl.json
- **Verification:** All 3 nodes form mesh, blobs sync to Node3
- **Committed in:** 074239d (Task 1 commit)

---

**Total deviations:** 2 auto-fixed (1 blocking, 1 bug)
**Impact on plan:** Both fixes necessary for test correctness. The --namespace flag is a critical loadgen enhancement enabling the entire delegation test category. No scope creep.

## Issues Encountered
- Plan assumed loadgen's --identity-file was sufficient for delegation writes. In reality, loadgen always writes to its own namespace (SHA3(pubkey) == namespace_id). The --namespace flag was required to override this for delegation scenarios where the delegate targets the owner's namespace.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- ACL-03 and ACL-04 delegation lifecycle tests complete
- Loadgen --namespace flag ready for any future tests requiring cross-namespace writes
- Ready for ACL-05 (SIGHUP reload) and TOPO-01 (connection dedup) in plan 48-03

## Self-Check: PASSED

All files exist, all commits verified.

---
*Phase: 48-access-control-topology*
*Completed: 2026-03-21*
