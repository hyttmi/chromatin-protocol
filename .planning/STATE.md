---
gsd_state_version: 1.0
milestone: v3.0.0
milestone_name: Relay v2
status: requirements
stopped_at: Defining requirements
last_updated: "2026-04-09"
last_activity: 2026-04-09
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
**Current focus:** v3.0.0 Relay v2 — defining requirements

## Current Position

Phase: Not started (defining requirements)
Plan: —
Status: Defining requirements
Last activity: 2026-04-09 — Milestone v3.0.0 started

## Accumulated Context

### Decisions

- Relay is closed source, database is open source
- Delete old relay/ and sdk/python/ at milestone start (clean break)
- Test against local node on dev laptop (UDS), no KVM swarm needed
- TLS via cert_path + key_path in relay config
- ML-DSA-87 challenge-response auth over WebSocket
- All 38 relay-allowed message types supported

### Pending Todos

None.

### Blockers/Concerns

None.
