---
phase: 94-protocol-sdk-documentation
plan: 01
subsystem: documentation
tags: [protocol, wire-format, delegation, revocation, key-rotation, groups, envelope-encryption]

# Dependency graph
requires:
  - phase: 91-sdk-delegation-revocation
    provides: revoke_delegation workflow and DelegationList-based tombstoning
  - phase: 92-kem-key-versioning
    provides: UserEntry v2 format, key ring fallback, rotate_kem
  - phase: 93-group-membership-revocation
    provides: remove_member, write_to_group refresh, GroupEntry format
provides:
  - PROTOCOL.md delegation revocation subsection with propagation bounds
  - PROTOCOL.md UserEntry v2 binary format specification
  - PROTOCOL.md key version semantics documentation
  - PROTOCOL.md GroupEntry binary format specification
  - PROTOCOL.md group membership revocation with forward exclusion
  - PROTOCOL.md key ring fallback decryption replacing binary search
affects: [94-02 (SDK tutorial references same features)]

# Tech tracking
tech-stack:
  added: []
  patterns: [extend existing sections rather than new top-level sections (D-01)]

key-files:
  created: []
  modified: [db/PROTOCOL.md]

key-decisions:
  - "All new content placed as subsections within existing sections per D-01 (no new ## sections)"
  - "UserEntry/GroupEntry/group revocation placed under ## Client-Side Envelope Encryption because envelope_encrypt resolves via these structures"
  - "Document correct behavior only per D-03 -- no changelog callouts for Phase 93 fixes"

patterns-established:
  - "Directory formats documented alongside envelope encryption since they are envelope-layer dependencies"

requirements-completed: [DOC-01]

# Metrics
duration: 2min
completed: 2026-04-07
---

# Phase 94 Plan 01: PROTOCOL.md v2.1.1 Feature Documentation Summary

**Delegation revocation workflow, UserEntry v2 binary format with key versioning, GroupEntry format, group membership revocation, and key ring fallback decryption documented in PROTOCOL.md**

## Performance

- **Duration:** 2 min
- **Started:** 2026-04-07T08:59:00Z
- **Completed:** 2026-04-07T09:01:23Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments
- Documented delegation revocation workflow with tombstone propagation bounds (BlobNotify for connected peers, sync-on-connect/safety-net for disconnected)
- Added UserEntry v2 binary format table with key_version field, kem_sig binding, and version semantics (highest-version-wins cache)
- Added GroupEntry binary format table and group membership revocation with forward exclusion semantics
- Replaced old binary search decryption with key ring map approach and added Key Ring Fallback subsection

## Task Commits

Each task was committed atomically:

1. **Task 1: Add delegation revocation, directory formats, and group membership sections** - `4e4cd1b` (docs)
2. **Task 2: Update decryption section for key ring fallback** - `086a2ac` (docs)

## Files Created/Modified
- `db/PROTOCOL.md` - Added 4 new subsections: delegation revocation under Namespace Delegation; UserEntry v2, GroupEntry, and group membership revocation under Client-Side Envelope Encryption; updated Decryption with key ring fallback

## Decisions Made
- All new content placed as ### / #### / ##### subsections within existing ## sections per D-01 -- no new top-level sections created
- Directory formats (UserEntry, GroupEntry) documented under Client-Side Envelope Encryption because envelope_encrypt depends on UserEntry for recipient resolution and write_to_group depends on GroupEntry for member resolution
- Correct behavior documented only per D-03 -- no mentions of old binary search approach or Phase 93 bug fixes

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- PROTOCOL.md now documents all v2.1.1 features
- Plan 94-02 (SDK getting-started tutorial) can proceed independently

## Self-Check: PASSED

- db/PROTOCOL.md: FOUND
- 94-01-SUMMARY.md: FOUND
- Commit 4e4cd1b: FOUND
- Commit 086a2ac: FOUND

---
*Phase: 94-protocol-sdk-documentation*
*Completed: 2026-04-07*
