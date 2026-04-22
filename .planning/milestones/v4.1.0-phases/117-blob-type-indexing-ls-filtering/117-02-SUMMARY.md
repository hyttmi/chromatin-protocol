---
phase: 117-blob-type-indexing-ls-filtering
plan: 02
subsystem: cli
tags: [cli, blob-type, filtering, wire-format, listresponse]

# Dependency graph
requires:
  - phase: 117-01
    provides: "44-byte ListResponse entries with type field, flags byte in ListRequest"
provides:
  - "CLI type constants (CENV, TOMB, DLGT, CDAT magic bytes)"
  - "type_label() maps 4-byte prefix to human-readable label"
  - "is_hidden_type() for default ls filtering"
  - "cdb ls with --raw and --type flags"
  - "44-byte entry parsing in ls() and list_hashes()"
affects: [119-chunked-files, cli-commands]

# Tech tracking
tech-stack:
  added: []
  patterns: ["CLI-side type magic constants with _CLI suffix to avoid node header collision"]

key-files:
  created: []
  modified:
    - cli/src/wire.h
    - cli/src/commands.h
    - cli/src/commands.cpp
    - cli/src/main.cpp

key-decisions:
  - "Used _CLI suffix for TOMBSTONE_MAGIC_CLI and DELEGATION_MAGIC_CLI to avoid name collision with node codec.h constants"
  - "Unrecognized type prefixes display as DATA (not error or hex dump)"
  - "Hidden types (PUBK, CDAT, DLGT) silently filtered with no footer count"

patterns-established:
  - "CLI type constants in wire.h with _CLI suffix convention"
  - "type_label() centralizes all type prefix to label mapping"

requirements-completed: [ERGO-02, ERGO-03]

# Metrics
duration: 17min
completed: 2026-04-16
---

# Phase 117 Plan 02: CLI ls Filtering Summary

**CLI-side type-aware ls with hide list filtering, --raw/--type flags, and 44-byte ListResponse entry parsing**

## Performance

- **Duration:** 17 min
- **Started:** 2026-04-16T08:47:14Z
- **Completed:** 2026-04-16T09:04:12Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Added type magic constants (CENV, TOMBSTONE, DELEGATION, CDAT) and type_label()/is_hidden_type() helpers to wire.h
- Updated ls() to parse 44-byte entries, hide PUBK/CDAT/DLGT by default, show "hash  TYPE" format
- Added --raw (show all including infrastructure) and --type TYPE (server-side filter) flags
- Fixed list_hashes() to parse 44-byte entries (prevents rm/reshare breakage)

## Task Commits

Each task was committed atomically:

1. **Task 1: Wire constants, type_label, is_hidden_type, updated ls/list_hashes** - `de957650` (feat)
2. **Task 2: ls --raw and --type flag parsing in main.cpp** - `51d2eace` (feat)

## Files Created/Modified
- `cli/src/wire.h` - Added CENV_MAGIC, TOMBSTONE_MAGIC_CLI, DELEGATION_MAGIC_CLI, CDAT_MAGIC; type_label() and is_hidden_type() inline functions
- `cli/src/commands.h` - Extended ls() signature with bool raw and string type_filter params
- `cli/src/commands.cpp` - ls() builds ListRequest with flags/type_filter, parses 44-byte entries with type filtering and labels; list_hashes() updated to 44-byte entry size
- `cli/src/main.cpp` - Added --raw and --type flag parsing in ls command block, updated help text

## Decisions Made
- Used _CLI suffix for tombstone and delegation magic constants to avoid potential name collision if wire.h ever includes node codec.h
- Unrecognized type prefixes display as "DATA" rather than showing raw hex or erroring
- Hidden type filtering is silent (no "N hidden" footer) per D-23

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Initial cmake configure failed with liboqs AVX2 source issue; resolved by clearing build directory and reconfiguring with OQS_USE_AVX2_INSTRUCTIONS=OFF

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- CLI ls filtering complete; node-side type indexing (Plan 01) and CLI display (Plan 02) both shipped
- Phase 119 (chunked files) can use CDAT type awareness for chunk display
- All 41 CLI tests pass, zero build errors

## Self-Check: PASSED

All created/modified files verified on disk. All commit hashes found in git log.

---
*Phase: 117-blob-type-indexing-ls-filtering*
*Completed: 2026-04-16*
