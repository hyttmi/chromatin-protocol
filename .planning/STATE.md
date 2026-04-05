---
gsd_state_version: 1.0
milestone: v2.1.0
milestone_name: Compression, Filtering & Observability
status: planning
stopped_at: Phase 86 context gathered
last_updated: "2026-04-05T08:29:32.403Z"
last_activity: 2026-04-05 — Roadmap created for v2.1.0
progress:
  total_phases: 5
  completed_phases: 0
  total_plans: 0
  completed_plans: 0
  percent: 0
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-05)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** v2.1.0 Phase 86 — Namespace Filtering & Hot Reload

## Current Position

Phase: 86 (1 of 5) — Namespace Filtering & Hot Reload
Plan: —
Status: Ready to plan
Last activity: 2026-04-05 — Roadmap created for v2.1.0

Progress: [░░░░░░░░░░] 0%

## Performance Metrics

**Velocity:**

- Total plans completed: 0
- Average duration: —
- Total execution time: 0 hours

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| - | - | - | - |

**Recent Trend:**

- Last 5 plans: —
- Trend: —

*Updated after each plan completion*

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Previous milestone decisions archived to milestones/v2.0.0-ROADMAP.md.

- Breaking protocol changes OK -- only deployed on home KVM, no production users
- Brotli compression: compress inside TransportCodec::encode, before AEAD, flag byte prefix (0x00/0x01)
- BlobNotify filter by sync_namespaces (node replication config), NOT subscribed_namespaces (client state)
- Prometheus /metrics: localhost-only default, disabled by default, no crypto material in labels
- Relay subscription forwarding: stateful per-session tracking with 256-namespace cap, connection-scoped cleanup
- Relay auto-reconnect: ACTIVE/RECONNECTING/DEAD state machine, new socket per attempt
- SDK multi-relay: randomized relay order at startup, jittered failover, circuit breaker after full list

### Pending Todos

None.

### Blockers/Concerns

- Phase 87 (Brotli): Verify FetchContent integration builds cleanly before writing compression code. Decompression bomb mitigation must be designed first.
- Phase 88 (Relay auto-reconnect): Three-state lifecycle with concurrent client sessions — race conditions between reconnect and client disconnect coroutines.

## Session Continuity

Last session: 2026-04-05T08:29:32.398Z
Stopped at: Phase 86 context gathered
Resume file: .planning/phases/86-namespace-filtering-hot-reload/86-CONTEXT.md
