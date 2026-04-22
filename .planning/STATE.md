---
gsd_state_version: 1.0
milestone: v4.1.0
milestone_name: milestone
status: executing
stopped_at: Completed 125-02-PLAN.md
last_updated: "2026-04-22T03:09:13.664Z"
last_activity: 2026-04-22
progress:
  total_phases: 26
  completed_phases: 9
  total_plans: 33
  completed_plans: 30
  percent: 91
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-15)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** Phase 125 — docs-update-for-mvp-protocol

## Current Position

Phase: 125 (docs-update-for-mvp-protocol) — EXECUTING
Plan: 3 of 5 (01 complete; 02 pending)
Status: Ready to execute
Last activity: 2026-04-22

Progress: [██████████] 88%

## Performance Metrics

**Velocity:**

- Total plans completed: 15
- Average duration: -
- Total execution time: 0 hours

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 116 | 2 | - | - |
| 117 | 2 | - | - |
| 118 | 2 | - | - |
| 119 | 3 | - | - |
| 121 | 1 | - | - |
| 124 | 5 | - | - |

**Recent Trend (from v4.0.0):**

- Phase 115 P01-P04: 108min, 13min, 8min, 26min
- Trend: Stable

*Updated after each plan completion*
| Phase 124 P02 | 25 | 2 tasks | 5 files |
| Phase 124 P03 | 6m | 2 tasks | 2 files |
| Phase 124 P04 | 18m | 3 tasks | 6 files |
| Phase 124 P05 | 180m | 7 tasks | 6 files (initial 90m + rerun 30m + docs 60m) |
| Phase 125 P01 | split-session | 5 tasks | 1 file (db/PROTOCOL.md: 1386 → 1622 lines; 5 atomic commits spanning a mid-plan budget-limit resume) |
| Phase 125 P02 | 4m | 4 tasks | 2 files |

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Previous milestone decisions archived to milestones/.

- [v4.1.0]: Phase 118 depends only on 116 (not 117) -- can parallelize with 117 if desired
- [v4.1.0]: Chunked files (119) depends on type indexing (117) for CPAR/CDAT type awareness
- [v4.1.0]: Request pipelining (120) after chunked files (119) -- primary customer is chunked downloads
- [v4.1.0]: Phase 124 plan 01 retains MsgType::Delete=17 (node still emits DeleteAck regardless of BlobWriteBody payload shape)
- [v4.1.0]: Phase 124 plan 01 forward-declares Identity in wire.h (compile-graph tightness; include only in wire.cpp)
- [v4.1.0]: Phase 124 plan 01 parks 20 TEMP-124 compile-fix stubs in commands.cpp + chunked.cpp; plan 03 migrates them to build_owned_blob + encode_blob_write_body
- [Phase 124]: Phase 124 plan 02: Option A template extraction -- ensure_pubk_impl<Sender,Receiver> in header; tests drive it with CapturingSender + ScriptedSource, no asio.
- [Phase 124]: Phase 124 plan 02: cache + delegate-skip live in wrapper; template is stateless. D-01a delegate skip short-circuits at target_ns != id.namespace_id(), T-124-02 structurally impossible.
- [Phase 124]: Plan 03: 12 blob-construction sites (9 commands.cpp + 3 chunked.cpp) migrated to build_owned_blob + encode_blob_write_body; MsgType binding per RESEARCH Q3 (BlobWrite for owner writes, Delete retained for tombstones); zero TEMP-124 markers remain
- [Phase 124]: Plan 04: Auto-PUBK wired into 7 owner-write flows; cmd::publish bypasses via mark_pubk_present_for_invocation post-WriteAck cache-seed
- [Phase 124]: Plan 04: D-05 error decoder (commands_internal.h) covers codes 0x07-0x0B; 5 generic-error sites routed through it; no phase-number or token leaks in user strings
- [Phase 124]: Plan 04: D-06 BOMB cascade in cmd::rm_batch (warn-and-continue per RESEARCH Q6); classify_rm_target + classify_rm_target_impl<> template pattern mirrors plan 02's ensure_pubk; 2 [cascade] TEST_CASEs green
- [Phase 124]: Plan 05: Rule-1 bug fix mid-plan: submit_bomb_blob routed BOMBs via MsgType::Delete (node rejects non-tombstone-format). Changed to MsgType::BlobWrite; BOMBs ingested correctly. Pre-existing Phase 123-03 latent defect (commit 5d337da0).
- [Phase 124]: Plan 05: Rule-2 UX fixes: `cdb ls --type BOMB` and `--type NAME` now recognised; opts.host threaded into submit_bomb_blob so D-05 wording names real host (commits 5d337da0, a6b282dd).
- [Phase 124]: Plan 05: extracted decode_error_response to cli/src/error_decoder.cpp (TU extraction for test linkage). [error_decoder] TEST_CASE: 7 literal-equality assertions for codes 0x07-0x0B.
- [Phase 124]: Plan 05: D-08 E2E Phase Gate INITIAL = FAIL (home daemon stale, delegate scope call pending).
- [Phase 124]: Plan 05: D-08 E2E Phase Gate FINAL = PASS (2026-04-21T11:22Z) after user restarted home daemon on post-124 binary + SC-124-4 live-half scope-down. All 7 D-08 items PASS or SCOPE; cross-node sync, BOMB propagation (count=3 size=104), 750 MiB chunked roundtrip (both directions), --replace BOMB-of-1, and D-06 cascade (48-target BOMB) all verified on both local and home.
- [Phase 124]: Plan 05 retrospective: the pre-122-binary diagnosis of B1 was wrong — home binary on disk was always current (g9248f01d); the DAEMON PROCESS (PID 822, uptime 19h54m) was stale from 2026-04-20, never restarted after the binary swap. `info` was reading the old process image. Post-redeploy sanity checks must cross-reference Uptime against rebuild timestamp, not just Version.
- [Phase 124]: Plan 05 SC-124-4 scope-down (user-approved 2026-04-21): D-02's rejection of `--as <owner_ns>` means the CLI has no structural path to emit a delegate write landing under a foreign owner's namespace. Live-E2E half replaced by `[pubk]` TC#5 (test_auto_pubk.cpp) + phase-122-04 test_ingest_delegate.cpp. Documented in 124-VALIDATION.md §SC-124-4 + 124-E2E.md §Item 5.
- [Phase 125]: Plan 01 Task 5 code-inspection finding: `db/peer/message_dispatcher.cpp:942,953,974-978` emits the 32-byte `signer_hint` in MetadataResponse (hard-coded `signer_hint_len=32`, `std::memcpy` of `blob.signer_hint`) — NOT the resolved 2592-byte signing_pk. PROTOCOL.md documents the PUBK-cache pattern for offline signature verification as the client-side mitigation.
- [Phase 125]: Plan 01 BOMB routing rule documented at wire level: BOMBs ride `BlobWrite (64)`, not `Delete (17)`, because the Delete dispatcher accepts only 36-byte single-target tombstone `data`. This is the Phase 124 Plan 05 Rule-1 bug raised to doc status so external implementers don't reimplement it.
- [Phase 125]: Plan 01 Data=8 handling: row retained in Message Type Reference marked DELETED (dispatcher returns `ErrorResponse{unknown_type=0x02}`). Hard enum-slot removal is Plan 04's scope (flatc regen + schema update).
- [Phase 125]: Plan 02: README (+9 lines, ~1 screen per D-01) + cli/README (+136 lines) refreshed for v4.1.0 shipping surface. Six new cli/README sections: Hello World, Mutable Names, Batched Deletion + CPAR Cascade, Chunked Large Files, Request Pipelining, Auto-PUBK + First-Write Errors. All byte-level detail cross-linked to PROTOCOL.md anchors per D-02 (no duplication).
- [Phase 125]: Plan 02 anchor fix: cli/README links to PROTOCOL.md#errorresponse-type-63 (actual slug from Plan 01 heading 'ErrorResponse (Type 63)'), not plan-spec placeholder #errorresponse.
- [Phase 125]: Plan 02 omission: no --chunk-size CLI flag documented (verified absent in cli/src/main.cpp put parser; chunk size is compile-time constant in cli/src/wire.h). Plan's Task 2.C explicitly allowed omission-if-not-exposed.

### Pending Todos

None.

### Blockers/Concerns

- PITFALL: connection.cpp:626 has unchecked total_size in chunked reassembly -- fix in Phase 119
- PITFALL: SQLite schema versioning needed before adding group tables (Phase 116)
- FLAG (Phase 123 D-15): `cdb put --name X --replace` within a single second can lose the blob_hash DESC tiebreak against the prior NAME blob. Phase 125+ fix: either bump NAME timestamp to max(seen+1, now), or tiebreak on (ts DESC, target_hash DESC), or document the 1-second granularity contract.
- TODO (Phase 124 Plan 05 observation): `node-reconnect-loop-to-ephemeral-client-ports.md` — peers.json retains ephemeral-port poison from prior cdb command invocations; affects peer-to-peer sync logging only, NOT cdb-to-node command success.
- RESOLVED (Phase 124 Plan 05): home-daemon-stale-process (restarted 2026-04-21, PID 7444, rerun items PASS).
- RESOLVED (Phase 124 Plan 05): SC-124-4 delegate CLI-design constraint — scope-reduced to unit-test coverage per user approval 2026-04-21.

## Session Continuity

Last session: 2026-04-22T03:09:02.404Z
Stopped at: Completed 125-02-PLAN.md
Resume file: None

**Planned Phase:** 124 (cli-adaptation-to-new-mvp-protocol) — 5 plans — 2026-04-21T05:18:49.479Z
