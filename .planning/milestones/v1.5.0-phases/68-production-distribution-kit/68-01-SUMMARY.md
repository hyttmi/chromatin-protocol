---
phase: 68-production-distribution-kit
plan: 01
subsystem: infra
tags: [systemd, sysusers.d, tmpfiles.d, distribution, bare-metal]

# Dependency graph
requires: []
provides:
  - "Hardened systemd units for chromatindb node and relay"
  - "Minimal production JSON configs (node.json, relay.json)"
  - "sysusers.d definition for chromatindb system user/group"
  - "tmpfiles.d definition for /var/lib, /var/log, /run, /etc directories"
affects: [68-02]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "FHS-standard paths: binaries in /usr/local/bin, configs in /etc/chromatindb, data in /var/lib/chromatindb"
    - "Minimal JSON configs: only fields differing from compiled defaults"
    - "Hardened-by-default systemd units with 16 security directives"

key-files:
  created:
    - dist/systemd/chromatindb.service
    - dist/systemd/chromatindb-relay.service
    - dist/config/node.json
    - dist/config/relay.json
    - dist/sysusers.d/chromatindb.conf
    - dist/tmpfiles.d/chromatindb.conf
  modified: []

key-decisions:
  - "Binaries at /usr/local/bin (consistent with Dockerfile pattern)"
  - "Configs at /etc/chromatindb (FHS standard for config)"
  - "16 security directives per unit (ProtectSystem=strict + 13 additional hardening)"
  - "RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX (network + UDS only)"

patterns-established:
  - "Minimal config pattern: only override compiled defaults for path fields"
  - "Hardened systemd pattern: ProtectSystem=strict as baseline, ReadWritePaths for writable dirs"

requirements-completed: [DIST-01, DIST-02, DIST-03, DIST-04, DIST-05, DIST-06]

# Metrics
duration: 2min
completed: 2026-03-28
---

# Phase 68 Plan 01: Static Distribution Artifacts Summary

**Six dist/ files for bare-metal deployment: hardened systemd units, minimal JSON configs, sysusers.d user/group, tmpfiles.d directory structure**

## Performance

- **Duration:** 2 min
- **Started:** 2026-03-28T06:27:36Z
- **Completed:** 2026-03-28T06:29:17Z
- **Tasks:** 2
- **Files created:** 6

## Accomplishments
- Created sysusers.d config that creates chromatindb system user/group with /var/lib/chromatindb home and nologin shell
- Created tmpfiles.d config for four FHS-standard directories with correct ownership and permissions
- Created minimal node.json (3 fields: data_dir, log_file, uds_path) and relay.json (3 fields: uds_path, identity_key_path, log_file)
- Created hardened systemd units with 16 security directives each, Type=simple, Restart=on-failure, and relay After=chromatindb.service ordering

## Task Commits

Each task was committed atomically:

1. **Task 1: Create sysusers.d, tmpfiles.d, and default JSON configs** - `56d76eb` (feat)
2. **Task 2: Create hardened systemd service units for node and relay** - `fb67d9c` (feat)

## Files Created
- `dist/sysusers.d/chromatindb.conf` - systemd-sysusers definition for chromatindb user/group
- `dist/tmpfiles.d/chromatindb.conf` - systemd-tmpfiles definition for data, log, run, config directories
- `dist/config/node.json` - Minimal production node config (3 fields)
- `dist/config/relay.json` - Minimal production relay config (3 fields)
- `dist/systemd/chromatindb.service` - Hardened systemd unit for chromatindb node
- `dist/systemd/chromatindb-relay.service` - Hardened systemd unit for chromatindb relay

## Decisions Made
- Binaries placed at /usr/local/bin (consistent with existing Dockerfile COPY destination)
- Configs placed at /etc/chromatindb (FHS standard for configuration files)
- 16 security directives per unit beyond the 3 mandatory (ProtectSystem, NoNewPrivileges, MemoryDenyWriteExecute)
- RestrictAddressFamilies limited to AF_INET, AF_INET6, AF_UNIX (network sockets + UDS only)
- ReadWritePaths includes /var/lib, /var/log, /run chromatindb directories for both units

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- All six static distribution artifacts ready for Plan 02 (install script)
- Install script can reference these files by their dist/ paths for deployment to FHS-standard targets
- No logrotate config (correct per locked decision: spdlog handles rotation internally)

## Self-Check: PASSED

All 6 distribution files exist. Both task commits verified (56d76eb, fb67d9c). SUMMARY.md created.

---
*Phase: 68-production-distribution-kit*
*Completed: 2026-03-28*
