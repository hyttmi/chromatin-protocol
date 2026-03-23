---
gsd_state_version: 1.0
milestone: v1.2.0
milestone_name: Relay & Client Protocol
status: unknown
stopped_at: Completed 59-02-PLAN.md
last_updated: "2026-03-23T15:58:44.229Z"
progress:
  total_phases: 4
  completed_phases: 3
  total_plans: 6
  completed_plans: 6
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-22)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** Phase 59 — relay-core

## Current Position

Phase: 999.1
Plan: Not started

## Accumulated Context

### Decisions

All decisions logged in PROJECT.md Key Decisions table.

- [Phase 57]: WriteAck payload format identical to DeleteAck (41 bytes) for wire consistency; sent for stored + duplicate ingests
- [Phase 57]: Client read/list/stats handlers do not filter by sync_namespaces_ -- reads serve whatever is in storage
- [Phase 58]: bind_port as uint32_t (not uint16_t) to catch out-of-range JSON values in validation
- [Phase 58]: Relay config requires file (throws on missing) unlike node config which returns defaults
- [Phase 58]: RelayIdentity uses direct key_path (not data_dir) matching SSH-style identity management
- [Phase 59]: Message forwarding gated on node on_ready to prevent sends before TrustedHello completes
- [Phase 59]: Accept loop as lambda coroutine capturing cmd_run() locals -- simpler than separate class for relay

### Pending Todos

None.

### Blockers/Concerns

- Pre-existing full-suite hang in release build when running all 469 tests together (port conflict in test infrastructure) -- deferred

## Session Continuity

Last session: 2026-03-23T15:54:03.078Z
Stopped at: Completed 59-02-PLAN.md
Resume file: None
