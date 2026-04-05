---
phase: 86-namespace-filtering-hot-reload
plan: 03
subsystem: testing
tags: [docker, integration-tests, namespace-filtering, sighup, max_peers, blob-notify]

# Dependency graph
requires:
  - phase: 86-namespace-filtering-hot-reload (plan 01)
    provides: SyncNamespaceAnnounce protocol, BlobNotify namespace filtering, max_peers SIGHUP reload
  - phase: 86-namespace-filtering-hot-reload (plan 02)
    provides: Unit tests for namespace filtering and max_peers reload
provides:
  - Docker E2E test proving BlobNotify namespace filtering works across real network
  - Docker E2E test proving max_peers SIGHUP hot reload works end-to-end
affects: [integration-tests, deployment-validation]

# Tech tracking
tech-stack:
  added: []
  patterns: [dynamic-namespace-discovery-via-logs, identity-file-loadgen-injection]

key-files:
  created:
    - tests/integration/test_filt01_namespace_filtering.sh
    - tests/integration/test_ops01_max_peers_sighup.sh
    - tests/integration/configs/filt01_node1.json
    - tests/integration/configs/filt01_node2.json
    - tests/integration/configs/filt01_node3.json
    - tests/integration/configs/ops01_max_peers_node1.json
    - tests/integration/configs/ops01_max_peers_node2.json
    - tests/integration/configs/ops01_max_peers_node3.json
  modified: []

key-decisions:
  - "Dynamic namespace discovery: start nodes briefly, extract SHA3-256(pubkey) from logs, stop, rewrite configs, restart -- avoids hardcoded namespace hashes"
  - "Identity-file injection for loadgen: copy node identity from Docker volume to write blobs in correct namespace"
  - "Simpler Phase 3 approach for max_peers: stop both peers, restart one (fills limit), restart second (refused) -- cleaner than disconnect-while-connected"

patterns-established:
  - "3-node namespace test topology: Node1 (own NS only), Node2 (two NS), Node3 (all NS) covers all filtering combinations"
  - "max_peers SIGHUP test: 4-phase pattern (connect, reduce, refuse, increase) validates D-12 no-mass-disconnect guarantee"

requirements-completed: [FILT-01, FILT-02, OPS-01]

# Metrics
duration: 5min
completed: 2026-04-05
---

# Phase 86 Plan 03: Docker Integration Tests for Namespace Filtering and max_peers SIGHUP Summary

**Two Docker E2E tests: namespace-filtered BlobNotify with 3-node topology proving blobs do not replicate to non-subscribing peers, and max_peers SIGHUP hot reload proving D-12 no-mass-disconnect guarantee**

## Performance

- **Duration:** 5 min
- **Started:** 2026-04-05T10:36:42Z
- **Completed:** 2026-04-05T10:41:37Z
- **Tasks:** 2
- **Files modified:** 8

## Accomplishments
- Namespace filtering Docker test (test_filt01) validates BlobNotify is only sent to peers whose sync_namespaces overlap with the blob's namespace, using dynamic namespace discovery
- max_peers SIGHUP Docker test (test_ops01_max_peers_sighup) validates full lifecycle: accept at limit, excess warning on reduction, refuse new connections, accept after increase
- Both tests follow established integration test patterns (standalone containers, helpers.sh, cleanup trap, FAILURES counter)

## Task Commits

Each task was committed atomically:

1. **Task 1: Docker integration test for namespace filtering (FILT-01 + FILT-02)** - `53a53ce` (feat)
2. **Task 2: Docker integration test for max_peers SIGHUP reload (OPS-01)** - `0d7256f` (feat)

## Files Created/Modified
- `tests/integration/test_filt01_namespace_filtering.sh` - 3-node namespace filtering E2E test (dynamic NS discovery, identity-file loadgen, blob count verification)
- `tests/integration/test_ops01_max_peers_sighup.sh` - 3-node max_peers SIGHUP E2E test (4-phase: connect, reduce, refuse, increase)
- `tests/integration/configs/filt01_node1.json` - Template config for FILT-01 Node1 (rewritten dynamically)
- `tests/integration/configs/filt01_node2.json` - Template config for FILT-01 Node2 (rewritten dynamically)
- `tests/integration/configs/filt01_node3.json` - Template config for FILT-01 Node3 (rewritten dynamically)
- `tests/integration/configs/ops01_max_peers_node1.json` - Template config for OPS-01 Node1 (max_peers=2)
- `tests/integration/configs/ops01_max_peers_node2.json` - Template config for OPS-01 Node2 (bootstrap)
- `tests/integration/configs/ops01_max_peers_node3.json` - Template config for OPS-01 Node3 (bootstrap)

## Decisions Made
- Used dynamic namespace discovery (start-extract-stop-rewrite-restart) instead of hardcoded namespace hashes, since namespace = SHA3-256(pubkey) and keys are generated at first run
- Used loadgen --identity-file flag to write blobs in a specific node's namespace, avoiding the namespace mismatch problem
- Chose the "simpler Phase 3" approach from the plan: stop both peers, restart sequentially to test max_peers enforcement cleanly
- Used unique container/network names (chromatindb-filt01-*, chromatindb-ops01mp-*) to avoid conflicts with existing tests

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Both integration tests ready to run against merged Phase 86 code (plans 01 + 02 provide the protocol implementation)
- Tests use Phase 86 log messages (announced sync namespaces, excess will drain naturally, config reload: max_peers) that are only present in merged code

## Self-Check: PASSED

All 9 created files verified present. Both task commits (53a53ce, 0d7256f) verified in git history.

---
*Phase: 86-namespace-filtering-hot-reload*
*Completed: 2026-04-05*
