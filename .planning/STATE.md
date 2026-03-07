---
gsd_state_version: 1.0
milestone: v3.0
milestone_name: Real-time & Delegation
status: active
last_updated: "2026-03-07"
progress:
  total_phases: 0
  completed_phases: 0
  total_plans: 0
  completed_plans: 0
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-07)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** v3.0 Real-time & Delegation

## Current Position

Phase: Not started (defining requirements)
Plan: —
Status: Defining requirements
Last activity: 2026-03-07 — Milestone v3.0 started

## Accumulated Context

### Decisions

All decisions logged in PROJECT.md Key Decisions table (27 decisions total across v1.0 and v2.0).

### v3.0 Design Decisions (from questioning)

- Pub/sub notifications: metadata-rich (namespace + seq + hash + size), subscriber fetches blob if wanted
- Any authenticated peer can subscribe to any namespace (ACL is the gate, not per-namespace restrictions)
- Delegation via signed blob in owner's namespace containing delegate pubkey
- Delegates can write only (no deletion) — deletion is owner-privileged
- Delegate signs with own key; verification: check ownership OR valid delegation blob exists
- Revocation by deleting delegation blob (uses tombstone feature)
- Tombstones are permanent (TTL=0) — deleted means deleted forever
- Tombstone replicates like any blob; receiving nodes delete target + keep tombstone

### Pending Todos

None.

### Blockers/Concerns

None.

## Session Continuity

Last session: 2026-03-07
Stopped at: Defining v3.0 requirements
Resume file: None
