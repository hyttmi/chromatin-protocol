---
gsd_state_version: 1.0
milestone: v4.1.0
milestone_name: milestone
status: executing
stopped_at: Completed 124-04-PLAN.md
last_updated: "2026-04-21T09:12:23.151Z"
last_activity: 2026-04-21
progress:
  total_phases: 26
  completed_phases: 8
  total_plans: 28
  completed_plans: 27
  percent: 96
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-15)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** Phase 124 — cli-adaptation-to-new-mvp-protocol

## Current Position

Phase: 124 (cli-adaptation-to-new-mvp-protocol) — EXECUTING
Plan: 5 of 5 (plan 01 complete; plan 02 next)
Status: Ready to execute
Last activity: 2026-04-21

Progress: [##########] 100%

## Performance Metrics

**Velocity:**

- Total plans completed: 10
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

**Recent Trend (from v4.0.0):**

- Phase 115 P01-P04: 108min, 13min, 8min, 26min
- Trend: Stable

*Updated after each plan completion*
| Phase 124 P02 | 25 | 2 tasks | 5 files |
| Phase 124 P03 | 6m | 2 tasks | 2 files |
| Phase 124 P04 | 18m | 3 tasks tasks | 6 files files |

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
- [Phase 124]: Plan 05: D-08 E2E Phase Gate = FAIL. Local-side items PASS (1, 4, 6, 7 + live 0x07). Cross-node items BLOCKED by home node running 2.3.0-gf038faee (23 commits pre-Phase-122). Item 5 delegate BLOCKED by CLI design (D-02 no --as flag, so put --share does not re-target to foreign ns).

### Pending Todos

None.

### Blockers/Concerns

- PITFALL: connection.cpp:626 has unchecked total_size in chunked reassembly -- fix in Phase 119
- PITFALL: SQLite schema versioning needed before adding group tables (Phase 116)
- BLOCKER (Phase 124 Plan 05): home node at 192.168.1.73 is on pre-Phase-122 binary `2.3.0-gf038faee`. User must rebuild chromatindb from master (post-commit a6b282dd) and redeploy to home with data dir wipe. Then re-run D-08 items 2, 3, 5 (if resolved), and cross-node halves of 4, 6, 7.
- BLOCKER (Phase 124 Plan 05, design): `cdb put --share @contact` does not re-target writes to a foreign owner's namespace. Plan Task 5 required-minimum scenario presumes this surface. Needs a D-02 revisit (architectural) or a scope-down to unit-test-only coverage for SC-124-4. See 124-05-SUMMARY.md deviations #5.
- FLAG (Phase 123 D-15): `cdb put --name X --replace` within a single second can lose the blob_hash DESC tiebreak against the prior NAME blob. Phase 125+ fix: either bump NAME timestamp to max(seen+1, now), or tiebreak on (ts DESC, target_hash DESC), or document the 1-second granularity contract.

## Session Continuity

Last session: 2026-04-21T14:40:00.000Z
Stopped at: Plan 124-05 executed with Phase Gate FAIL; blockers surfaced for user resolution.
Resume file: .planning/phases/124-cli-adaptation-to-new-mvp-protocol/124-05-SUMMARY.md

**Planned Phase:** 124 (cli-adaptation-to-new-mvp-protocol) — 5 plans — 2026-04-21T05:18:49.479Z
