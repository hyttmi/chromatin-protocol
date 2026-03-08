---
gsd_state_version: 1.0
milestone: v3.0
milestone_name: Real-time & Delegation
status: active
last_updated: "2026-03-08"
progress:
  total_phases: 4
  completed_phases: 3
  total_plans: 6
  completed_plans: 6
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-07)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** Phase 14 complete. Phase 15 next.

## Current Position

Phase: 14 of 15 (Pub/Sub Notifications) -- COMPLETE
Plan: 2 of 2 complete in current phase
Status: Phase 14 complete. Ready for Phase 15.
Last activity: 2026-03-08 -- Phase 14 complete (pub/sub notifications)

Progress: [##########] 100%

## Performance Metrics

**Velocity:**
- Total plans completed: 35 (across v1.0 + v2.0 + v3.0)
- Average duration: ~25 min (historical)
- Total execution time: ~14 hours (v1.0 + v2.0 + v3.0 Phase 12-14)

**By Phase (v3.0):**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 12. Blob Deletion | 2/2 | ~55 min | ~28 min |
| 13. Namespace Delegation | 2/2 | ~27 min | ~14 min |
| 14. Pub/Sub Notifications | 2/2 | ~23 min | ~12 min |
| 15. Polish & Benchmarks | 0/? | - | - |

**Recent Trend:**
- v2.0 shipped in 2 days (8 plans, 3 phases)
- Phase 12 completed in ~1 hour (2 plans)
- Phase 13 completed in ~30 min (2 plans)
- Phase 14 completed in ~23 min (2 plans)
- Trend: Accelerating

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

### Phase 13 Decisions (execution)

- DELEGATION_MAGIC = {0xDE, 0x1E, 0x6A, 0x7E} -- "delegate" mnemonic
- Delegation index key = [namespace:32][SHA3-256(delegate_pubkey):32] -- compact fixed-size keys
- max_maps bumped 4->5 for delegation_map sub-database
- no_delegation error code subsumes namespace_mismatch in ingest() for non-owners
- Delegates cannot create delegation or tombstone blobs (guards before sig check)
- Zero engine changes needed for delegation blob creation (existing pipeline handles it)
- 28 new tests (9 storage, 6 engine creation, 10 engine write, 3 sync integration)

### Phase 14 Decisions (execution)

- Subscribe=20, Unsubscribe=21, Notification=22 as TransportMsgType enum values
- Namespace list uses strict length validation (rejects truncated payloads)
- OnBlobIngested callback on SyncProtocol for sync-received blob notifications
- Three trigger paths: Data handler, Delete handler, SyncProtocol callback
- No self-exclusion: writing peer receives its own notifications
- 11 new tests (5 wire encoding unit + 6 E2E integration)

### Pending Todos

None.

### Blockers/Concerns

None.

## Session Continuity

Last session: 2026-03-08
Stopped at: Phase 14 verified and complete. Ready for Phase 15.
Resume file: None
