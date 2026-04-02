---
gsd_state_version: 1.0
milestone: v2.0.0
milestone_name: Event-Driven Architecture
status: defining
stopped_at: Defining requirements
last_updated: "2026-04-02"
last_activity: 2026-04-02
progress:
  total_phases: 0
  completed_phases: 0
  total_plans: 0
  completed_plans: 0
  percent: 0
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-02)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** v2.0.0 Event-Driven Architecture — defining requirements

## Current Position

Phase: Not started (defining requirements)
Plan: —
Status: Defining requirements
Last activity: 2026-04-02 — Milestone v2.0.0 started

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Previous milestone decisions archived to milestones/v1.7.0-ROADMAP.md.

- Breaking protocol changes OK — only deployed on home KVM, no production users
- Push-based sync model: blob ingested → notify peers → targeted fetch
- Reconciliation becomes fallback (on-connect + rare safety-net), not primary sync
- Event-driven expiry: next-expiry timer replaces periodic full scan
- Cursor cleanup on disconnect, not 6-hour timer

### Pending Todos

None.

### Blockers/Concerns

None.

### Quick Tasks Completed

| # | Description | Date | Commit | Directory |
|---|-------------|------|--------|-----------|
| 260402-a2o | Split allowed_keys into allowed_client_keys and allowed_peer_keys | 2026-04-02 | 681f92b | [260402-a2o-split-allowed-keys-into-allowed-client-k](./quick/260402-a2o-split-allowed-keys-into-allowed-client-k/) |

## Session Continuity

Last session: 2026-04-02
Last activity: 2026-04-02 — Milestone v2.0.0 started
Stopped at: Defining requirements
Resume file: None
