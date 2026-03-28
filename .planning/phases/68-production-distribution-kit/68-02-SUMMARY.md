---
phase: 68-production-distribution-kit
plan: 02
subsystem: infra
tags: [install-script, deployment, FHS, systemd, bare-metal]

# Dependency graph
requires:
  - phase: 68-01
    provides: "Six dist/ artifacts (systemd units, configs, sysusers.d, tmpfiles.d)"
provides:
  - "Production install.sh script for bare-metal deployment of chromatindb"
  - "Uninstall mode that preserves data and config directories"
affects: [69]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "POSIX sh install script with set -eu strict error handling"
    - "Config preservation: skip install if target already exists"
    - "Identity key generation at install time via binary keygen subcommands"

key-files:
  created:
    - dist/install.sh
  modified: []

key-decisions:
  - "POSIX sh (not bash) for maximum portability across Linux distributions"
  - "case/esac argument dispatch for clean install vs uninstall mode switching"
  - "Keygen runs post-install using the just-installed binaries from /usr/local/bin"

patterns-established:
  - "Quiet success pattern: no output on success, errors only to stderr"
  - "Preserve-on-reinstall: check file existence before config install"

requirements-completed: [DIST-07]

# Metrics
duration: 2min
completed: 2026-03-28
---

# Phase 68 Plan 02: Install Script Summary

**POSIX install.sh with install/uninstall modes, FHS deployment of binaries + configs + systemd units, identity key generation, and config preservation on reinstall**

## Performance

- **Duration:** 2 min
- **Started:** 2026-03-28T06:31:57Z
- **Completed:** 2026-03-28T06:33:34Z
- **Tasks:** 2
- **Files created:** 1

## Accomplishments
- Created 106-line POSIX sh install script that deploys all dist/ artifacts to FHS-standard locations
- Install mode: copies 2 binaries, installs sysusers.d/tmpfiles.d, preserves existing configs, installs systemd units, generates identity keys if missing
- Uninstall mode: stops/disables services, removes installed files, preserves data and config directories
- Script validated: passes sh -n syntax check, zero bashisms, all variables double-quoted, 8 install -m commands covering all artifacts

## Task Commits

Each task was committed atomically:

1. **Task 1: Create install.sh with install and uninstall modes** - `35f4e2d` (feat)
2. **Task 2: Validate install script with shellcheck and structural tests** - no commit (script passed all checks without modification)

## Files Created
- `dist/install.sh` - Production deployment script: installs chromatindb node and relay to FHS-standard paths, generates keys, supports uninstall

## Decisions Made
- Used POSIX sh (not bash) for maximum Linux distribution portability
- Used case/esac dispatch with ${1:-} default for clean argument parsing
- Keygen runs after binary install so the just-installed /usr/local/bin binaries generate keys
- chown to chromatindb:chromatindb applied to generated keys immediately after keygen

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Complete dist/ production kit ready: 6 static artifacts (Plan 01) + install script (Plan 02)
- Phase 68 complete -- all distribution artifacts created
- Phase 69 (documentation) can proceed independently

## Self-Check: PASSED

All files exist (dist/install.sh with executable bit). Task commit verified (35f4e2d). SUMMARY.md created.

---
*Phase: 68-production-distribution-kit*
*Completed: 2026-03-28*
