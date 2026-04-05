---
phase: 85-documentation-refresh
plan: 01
subsystem: documentation
tags: [protocol-spec, wire-format, mermaid, push-sync, keepalive]

# Dependency graph
requires:
  - phase: 79-send-queue-push-notifications
    provides: BlobNotify wire type and push notification model
  - phase: 80-targeted-blob-fetch
    provides: BlobFetch/BlobFetchResponse wire types
  - phase: 82-reconcile-on-connect-safety-net
    provides: safety_net_interval_seconds config, reconcile-on-connect behavior
  - phase: 83-bidirectional-keepalive
    provides: 30s Ping / 60s silence keepalive spec
provides:
  - Complete v2.0.0 wire protocol specification in PROTOCOL.md
  - Byte-level wire format tables for BlobNotify (59), BlobFetch (60), BlobFetchResponse (61)
  - Keepalive timing specification replacing inactivity detection
  - Mermaid sequence diagrams for push-then-fetch and keepalive flows
affects: [85-02, sdk-documentation, client-implementations]

# Tech tracking
tech-stack:
  added: []
  patterns: [mermaid-sequence-diagrams, connection-lifecycle-structure]

key-files:
  created: []
  modified: [db/PROTOCOL.md]

key-decisions:
  - "Restructured PROTOCOL.md around connection lifecycle per D-05: Transport -> Connection Lifecycle -> Storing -> Sync Protocol -> Additional Interactions -> Client Protocol"
  - "BlobNotify/BlobFetch/BlobFetchResponse documented under new Sync Protocol section before Full Reconciliation"
  - "Inactivity Detection section fully removed and replaced by Keepalive under Connection Lifecycle"

patterns-established:
  - "Sync Protocol section ordering: Push Notifications -> Targeted Blob Fetch -> Full Reconciliation -> Reconcile-on-Connect -> Safety-Net Reconciliation"

requirements-completed: [DOC-01]

# Metrics
duration: 5min
completed: 2026-04-05
---

# Phase 85 Plan 01: PROTOCOL.md Restructure Summary

**Full PROTOCOL.md restructure with push-sync wire formats (BlobNotify/BlobFetch/BlobFetchResponse), bidirectional keepalive spec, and Mermaid diagrams for event-driven sync model**

## Performance

- **Duration:** 5 min
- **Started:** 2026-04-05T05:10:56Z
- **Completed:** 2026-04-05T05:16:37Z
- **Tasks:** 1
- **Files modified:** 1

## Accomplishments
- Restructured PROTOCOL.md from 964 to 1052 lines around connection lifecycle ordering per D-05
- Added byte-level wire format tables for BlobNotify (type 59, 77 bytes), BlobFetch (type 60, 64 bytes), and BlobFetchResponse (type 61, variable)
- Replaced stale Inactivity Detection section with Keepalive section specifying 30s Ping interval and 60s silence disconnect
- Added Push Notifications, Targeted Blob Fetch, Reconcile-on-Connect, and Safety-Net Reconciliation sections under new Sync Protocol heading
- Added Mermaid sequence diagrams for push-then-fetch flow and keepalive lifecycle
- Updated Message Type Reference table with types 59, 60, 61
- Removed all stale v1.x references (inactivity_timeout_seconds, sync_interval_seconds, v1.4.0 heading prefix)

## Task Commits

Each task was committed atomically:

1. **Task 1: Restructure PROTOCOL.md -- new sections, wire formats, keepalive** - `00e3595` (docs)

## Files Created/Modified
- `db/PROTOCOL.md` - Complete v2.0.0 wire protocol specification with push-sync types, keepalive, and Mermaid diagrams

## Decisions Made
- Restructured document around connection lifecycle per D-05: Transport Layer -> Connection Lifecycle (with Keepalive) -> Storing a Blob -> Sync Protocol (Push -> Fetch -> Reconciliation -> Safety Net) -> Additional Interactions -> Client Protocol -> Message Type Reference -> Query Extensions -> SDK Notes -> Envelope Encryption
- Documented BlobFetchResponse status byte inversion (0x00=found vs ReadResponse 0x01=found) with explicit note per Pitfall 2 from RESEARCH.md
- Updated server-initiated message list in Plaintext Format section to include BlobNotify alongside Notification

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Known Stubs
None - all wire format tables are complete with values verified against source code.

## Next Phase Readiness
- PROTOCOL.md is fully restructured and ready for cross-referencing from README.md and SDK documentation
- Plan 85-02 (README + SDK docs) can reference the new PROTOCOL.md sections

---
*Phase: 85-documentation-refresh*
*Completed: 2026-04-05*
