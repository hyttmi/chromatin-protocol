---
gsd_state_version: 1.0
milestone: v4.0.0
milestone_name: relay-architecture-v3
status: defining-requirements
last_updated: "2026-04-14"
last_activity: 2026-04-14
progress:
  total_phases: 0
  completed_phases: 0
  total_plans: 0
  completed_plans: 0
  percent: 0
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-09)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** v3.1.0 Relay Live Hardening -- thread-safety overhaul for multi-threaded HTTP.

## Current Position

Phase: 999.10-relay-thread-safety-overhaul-for-multi-threaded-http
Plan: 2 of 02 complete
Status: Ready to execute
Last activity: 2026-04-14

Progress: [#####-----] 50%

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Previous milestone decisions archived to milestones/v2.2.0-ROADMAP.md.

- [Phase 110]: httpx AsyncClient for benchmark HTTP, FlatBuffer manual vtable construction, UDS baseline deferred
- [Phase 999.10-01]: Global strand via asio::make_strand(ioc), strand passed by reference to UdsMultiplexer, shared_ptr preserved in ResponsePromiseMap (lifetime fix)
- [Phase 999.10]: All HTTP handlers post to strand before shared state access; heavy work stays off-strand per D-16

### Pending Todos

None.

### Blockers/Concerns

None.
