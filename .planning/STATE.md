---
gsd_state_version: 1.0
milestone: v3.0
milestone_name: Real-time & Delegation
status: active
last_updated: "2026-03-07"
progress:
  total_phases: 4
  completed_phases: 1
  total_plans: 2
  completed_plans: 2
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-07)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** Phase 13 - Namespace Delegation

## Current Position

Phase: 13 of 15 (Namespace Delegation) -- second phase of v3.0
Plan: 0 of ? in current phase
Status: Ready to plan
Last activity: 2026-03-07 -- Phase 12 complete (blob deletion + liboqs optimization)

Progress: [##░░░░░░░░] 25%

## Performance Metrics

**Velocity:**
- Total plans completed: 31 (across v1.0 + v2.0 + v3.0)
- Average duration: ~25 min (historical)
- Total execution time: ~13 hours (v1.0 + v2.0 + v3.0 Phase 12)

**By Phase (v3.0):**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 12. Blob Deletion | 2/2 | ~55 min | ~28 min |
| 13. Namespace Delegation | 0/? | - | - |
| 14. Pub/Sub Notifications | 0/? | - | - |
| 15. Polish & Benchmarks | 0/? | - | - |

**Recent Trend:**
- v2.0 shipped in 2 days (8 plans, 3 phases)
- Phase 12 completed in ~1 hour (2 plans)
- Trend: Stable

*Updated after each plan completion*

## Accumulated Context

### Decisions

All decisions logged in PROJECT.md Key Decisions table (27 decisions total across v1.0 and v2.0).

### v3.0 Design Decisions (from questioning)

- Tombstones are permanent (TTL=0) -- deleted means deleted forever
- Tombstone replicates like any blob; receiving nodes delete target + keep tombstone
- Delegation via signed blob in owner's namespace containing delegate pubkey
- Delegates can write only (no deletion) -- deletion is owner-privileged
- Delegate signs with own key; verification: check ownership OR valid delegation blob exists
- Revocation by deleting delegation blob (uses tombstone feature)
- Pub/sub: metadata-rich notifications (namespace + seq + hash + size), subscriber fetches blob if wanted
- Any authenticated peer can subscribe to any namespace (ACL is the gate, not per-namespace restrictions)

### Phase 12 Decisions (execution)

- Tombstone data = 4-byte magic (0xDEADBEEF) + 32-byte target hash
- DELETE message payload IS the tombstone BlobData (self-verifiable on any node)
- delete_blob_data removes from all three sub-databases (blobs_map + seq_map + expiry_map)
- DeleteAck sent via asio::co_spawn (on_peer_message is non-coroutine)
- has_tombstone_for uses O(n) namespace scan (deletion is rare)

### Pending Todos

None.

### Blockers/Concerns

None.

## Session Continuity

Last session: 2026-03-07
Stopped at: Phase 12 complete, ready to plan Phase 13
Resume file: None
