---
phase: 69-documentation-refresh
plan: 01
subsystem: docs
tags: [readme, version-bump, cmake, relay, deployment, wire-protocol]

# Dependency graph
requires:
  - phase: 68-production-distribution-kit
    provides: dist/ deployment kit (systemd units, install.sh, configs)
provides:
  - CMakeLists.txt version bumped to 1.5.0
  - README.md updated to v1.5.0
  - db/README.md fully refreshed with relay, deployment, and v1.4.0 query documentation
affects: [69-02-protocol-docs]

# Tech tracking
tech-stack:
  added: []
  patterns: []

key-files:
  created: []
  modified:
    - CMakeLists.txt
    - README.md
    - db/README.md

key-decisions:
  - "Used exact ctest count (567) and integration script count (49) from build/filesystem rather than stale values"

patterns-established: []

requirements-completed: [DOCS-01, DOCS-02, DOCS-03, DOCS-05]

# Metrics
duration: 2min
completed: 2026-03-28
---

# Phase 69 Plan 01: Documentation Refresh Summary

**Version bump to v1.5.0 across CMakeLists.txt and README.md, plus full db/README.md refresh with relay architecture, deployment kit, 58 message types, and 9 v1.4.0 query features**

## Performance

- **Duration:** 2 min
- **Started:** 2026-03-28T12:04:06Z
- **Completed:** 2026-03-28T12:06:31Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Bumped CMakeLists.txt project version from 1.1.0 to 1.5.0 and README.md from v1.3.0 to v1.5.0
- Refreshed db/README.md with accurate metrics: 567 unit tests, 49 integration test scripts, 58 message types
- Added Relay section documenting chromatindb_relay architecture, default-deny message filter (38 client types), and relay configuration reference
- Added Deployment section with dist/install.sh instructions, FHS paths table, systemd security hardening directives, and upgrade/uninstall commands
- Added 3 missing config fields (expiry_scan_interval_seconds, compaction_interval_hours, uds_path) to both JSON example and descriptions
- Added 9 v1.4.0 query features: NamespaceList, StorageStatus, NamespaceStats, BlobMetadata, BatchExists, DelegationList, BatchRead, PeerInfo, TimeRange

## Task Commits

Each task was committed atomically:

1. **Task 1: Bump CMakeLists.txt version and update root README.md to v1.5.0** - `5eefe39` (chore)
2. **Task 2: Refresh db/README.md with v1.5.0 state, relay section, and dist/ deployment** - `35cfea5` (docs)

## Files Created/Modified
- `CMakeLists.txt` - project version 1.1.0 -> 1.5.0
- `README.md` - current release v1.3.0 -> v1.5.0
- `db/README.md` - Full documentation refresh: test counts, message types, config fields, relay section, deployment section, v1.4.0 query features

## Decisions Made
- Used exact ctest count (567) and integration test script count (49) from build output rather than stale values from prior documentation

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All user-facing README documentation is current with v1.5.0 state
- Ready for plan 69-02 (PROTOCOL.md refresh) which covers wire format documentation

---
*Phase: 69-documentation-refresh*
*Completed: 2026-03-28*
