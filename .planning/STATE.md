---
gsd_state_version: 1.0
milestone: v4.1.0
milestone_name: milestone
status: ready_to_plan
stopped_at: Phase 127 context gathered
last_updated: "2026-04-22T11:28:21.300Z"
last_activity: 2026-04-22 -- Phase 127 execution started
progress:
  total_phases: 26
  completed_phases: 1
  total_plans: 0
  completed_plans: 0
  percent: 4
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-22)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** Phase 127 — nodeinforesponse-capability-extensions

## Current Position

Phase: 999.1
Plan: Not started
Status: Ready to plan
Last activity: 2026-04-22

Progress: [█         ] ~17% within v4.2.0 (1/6 phases)

## v4.2.0 Phase Map

| Phase | Name | Requirements | Depends on |
|-------|------|--------------|------------|
| 126 | Pre-shrink Audit | AUDIT-01, AUDIT-02 | (gate) |
| 127 | NodeInfoResponse Capability Extensions | NODEINFO-01..04, VERI-02 | 126 |
| 128 | Configurable Blob Cap + Frame Shrink + Config Gauges | BLOB-01..04, FRAME-01..02, METRICS-01..02, VERI-01, VERI-04 | 127 |
| 129 | Sync Cap Divergence | SYNC-01..04, METRICS-03, VERI-03, VERI-05 | 128 |
| 130 | CLI Auto-tuning | CLI-01..05, VERI-06 | 127 (soft: 128) |
| 131 | Documentation Reconciliation | DOCS-01..08 | 130 |

Execution order: 126 → 127 → 128 → 129 → 130 → 131 (strict linear; 126 gates frame shrink, 127 lands the wire-format extension that 128/129/130 all consume).

## Performance Metrics

**Velocity:**

- Total plans completed: 25 (v4.1.0)
- Average duration: -
- Total execution time: 0 hours (v4.2.0)

**By Phase (v4.1.0 baseline):**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 116 | 2 | - | - |
| 117 | 2 | - | - |
| 118 | 2 | - | - |
| 119 | 3 | - | - |
| 121 | 1 | - | - |
| 124 | 5 | - | - |
| 125 | 5 | - | - |
| 126 | 1 | - | - |
| 127 | 4 | - | - |

**Recent Trend (from v4.1.0):**

- Phase 125 P01-P05: split-session / 4m / 8m / 13m / 7m
- Trend: Fast — docs-only phases well under 15 min each

*Updated after each plan completion*

| Phase 124 P02 | 25 | 2 tasks | 5 files |
| Phase 124 P03 | 6m | 2 tasks | 2 files |
| Phase 124 P04 | 18m | 3 tasks | 6 files |
| Phase 124 P05 | 180m | 7 tasks | 6 files (initial 90m + rerun 30m + docs 60m) |
| Phase 125 P01 | split-session | 5 tasks | 1 file (db/PROTOCOL.md: 1386 → 1622 lines; 5 atomic commits spanning a mid-plan budget-limit resume) |
| Phase 125 P02 | 4m | 4 tasks | 2 files |
| Phase 125 P03 | 8m | 3 tasks | 2 files (1 created, 1 modified) |
| Phase 125 P04 | 13m | 3 tasks | 7 files |
| Phase 125 P05 | 7m | 3 tasks | 40 files |
| Phase 126 P01 | ~20m | 5 tasks | 4 code files + 3 planning files |

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Previous milestone decisions archived to milestones/.

**v4.2.0 roadmap decisions (2026-04-22):**

- [v4.2.0]: Phase 126 is a gating audit — no frame shrink code change lands until the 2 MiB invariant is proven for every non-chunked single-frame response type
- [v4.2.0]: NodeInfoResponse extensions (Phase 127) are protocol-breaking; pre-MVP posture means no compat shim — pre-v4.2.0 clients fail cleanly on the new wire format
- [v4.2.0]: `blob_max_bytes` bounds are `[1 MiB, 64 MiB]`; sub-MiB caps excluded (STREAMING_THRESHOLD floor), >64 MiB caps excluded (predictable memory working set)
- [v4.2.0]: Peer blob cap is session-constant at handshake — no mid-session renegotiation (SIGHUP on a peer takes effect on next reconnect; marked Out-of-Scope)
- [v4.2.0]: `MAX_CHUNKS` policy decision (65536 = 256 GiB ceiling vs `1 << 20` = 4 TiB ceiling) deferred to discuss-phase within Phase 130; not pre-decided
- [v4.2.0]: Phase 128 is the largest phase (10 requirements) — bundles blob cap config + frame shrink + Prometheus config-gauge infra because all three touch overlapping config/metrics surface and benefit from landing together
- [v4.2.0]: Phase 130 soft-depends on Phase 128 (strictly depends only on 127 for the advertised field) — execution order keeps them linear because CLI auto-tune is easier to verify against a node that actually ships a non-default cap

**v4.1.0 carry-forward:**

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
- [Phase 125-03]: ARCHITECTURE.md landed at 879 lines (within 600-900 target) with 25 PROTOCOL.md cross-links; expiry DBI value shape corrected to namespace:32 against storage.h:104 (plan interfaces block had 0-byte, which was wrong)
- [Phase 125-03]: db/README.md delta: 5 new Phase-118 knobs with actual defaults from config.h:51-58 (blob_transfer_timeout=600s, sync_timeout=30s, pex_interval=300s, strike_threshold=10, strike_cooldown=300s); 3 peer-management subcommands documented; 62-message-types count replaced with forward-looking language citing BlobWrite=64 + Delete=17
- [Phase 125]: Plan 04: deleted TransportMsgType_Data=8 from transport.fbs; regenerated transport_generated.h via flatc --cpp --gen-object-api (matched CMake invocation); migrated all 16 test sites in db/tests/net/ from TransportMsgType_Data to TransportMsgType_BlobWrite (no test case deletions — all sites were generic codec/framing exercises, not dispatcher-branch assertions)
- [Phase 125]: Plan 04: D-12 stripped "(Phase 123)" token from cdb rm help text at cli/src/main.cpp:619 (pre-existing leak deferred from Phase 124); D-11 removed the stale dispatcher-memo line at db/peer/message_dispatcher.cpp:1388; D-10 scan confirmed zero pre-122 field residue in C++ source (BlobData.namespace_id, BlobData.pubkey, old signing shape, MsgType::Data — all zero hits in cli/src/ + db/ *.cpp/*.h)
- [Phase 125]: Plan 04 regression gate: cli_tests 197614 assertions / 98 cases (baseline match); chromatindb_tests [peer] 506 / 77 (baseline match). Zero regressions from schema change + test migration. Both test binaries rebuilt locally with cmake --build -j12; feedback_self_verify_checkpoints.md applied (infra reachable, no user delegation needed)
- [Phase 125]: Plan 05: D-13 complete — zero `// Phase N` / `/// Phase N` comments remain in cli/src/, cli/tests/, or db/ (.cpp/.h/.fbs). 313 breadcrumbs stripped across 49 files over 12 atomic refactor(125-05) commits.
- [Phase 125]: Plan 05: D-14 pass completed on 12 high-density files (plan's scope). 6 files cleaned in 9c3d5e3d + 12c6edd5; the other 6 were already at clean baseline (0 commented-out code, 0 obvious-verb restatements). All load-bearing WHY preserved (CONC-04, AEAD nonce ordering, MDBX mmap geometry, ForceDefaults determinism, 11-step ingest pipeline markers, 20+ requirement-ID annotations).
- [Phase 125]: Plan 05 carve-out: Catch2 TEST_CASE names + SECTION() labels containing "Phase N" tokens are identifiers (user-visible test output); D-13 scope excludes identifiers per plan <truths>. Plan 05 leaves them intact — same policy as Catch2 tags like [phase122].

**Phase 126 decisions (2026-04-22):**

- [Phase 126-01]: MAX_FRAME_SIZE is a DoS bound on the 4-byte length prefix, NOT a payload-size bound. send_message auto-routes payload >= STREAMING_THRESHOLD into send_message_chunked before the frame cap is consulted, so the per-frame invariant to pin is "non-chunked sends fit in one sub-frame with envelope headroom" — not "responses fit in 2 MiB" (CONTEXT.md D-01..D-03 reframe).
- [Phase 126-01]: Assertion site is Connection::enqueue_send's head, not Connection::send_encrypted. enqueue_send has exactly one caller (send_message at db/net/connection.cpp:977); send_encrypted is shared between handshake, chunked header, chunked data sub-frames, chunked sentinel, and non-chunked via drain_send_queue — an assert there would trip on legitimate STREAMING_THRESHOLD-sized chunked sub-frames (RESEARCH §"Anti-Patterns to Avoid" Pitfall 1).
- [Phase 126-01]: D-14 CLI symmetry — both db/ and cli/ pipelines have identical structure with their own file-local MAX_FRAME_SIZE / STREAMING_THRESHOLD / send_encrypted / send_chunked. Landed: CLI file-scope STREAMING_THRESHOLD + TRANSPORT_ENVELOPE_MARGIN + static_assert + runtime assert in Connection::send non-chunked branch. The cli/ and db/ invariants are intentionally NOT extracted into a shared header (constants live in separate TUs, assertions are one-line each — "no shared helper" per CONTEXT.md D-06 sibling).
- [Phase 126-01]: Send-path inventory found ZERO bypass sites (D-09 success condition). 30 send_message callers across db/peer/ all route through the >= STREAMING_THRESHOLD bifurcation; 13 direct Connection::send_raw/send_encrypted sites are all fixed-size handshake payloads <= ~7.3 KiB (AuthSignature with ML-DSA-87 signature is the largest). No fix-in-phase reroute work needed.
- [Phase 126-01]: Tests reference STREAMING_THRESHOLD directly (not MAX_FRAME_SIZE) per D-11, so they survive Phase 128's shrink. Fixture pattern lifted from test_connection.cpp:475-542 (acceptor + create_inbound + create_outbound + on_ready + on_message + ioc.run_for).

### Pending Todos

None.

### Blockers/Concerns

- PITFALL: connection.cpp:626 has unchecked total_size in chunked reassembly -- fix in Phase 119 (resolved in v4.1.0; carried here for historical reference)
- FLAG (Phase 123 D-15): `cdb put --name X --replace` within a single second can lose the blob_hash DESC tiebreak against the prior NAME blob. Phase 125+ fix: either bump NAME timestamp to max(seen+1, now), or tiebreak on (ts DESC, target_hash DESC), or document the 1-second granularity contract.
- TODO (Phase 124 Plan 05 observation): `node-reconnect-loop-to-ephemeral-client-ports.md` — peers.json retains ephemeral-port poison from prior cdb command invocations; affects peer-to-peer sync logging only, NOT cdb-to-node command success.
- RESOLVED (Phase 124 Plan 05): home-daemon-stale-process (restarted 2026-04-21, PID 7444, rerun items PASS).
- RESOLVED (Phase 124 Plan 05): SC-124-4 delegate CLI-design constraint — scope-reduced to unit-test coverage per user approval 2026-04-21.

**v4.2.0 pre-work concerns (flagged for phase execution):**

- Phase 126: Worst-case sizing for `BatchReadResponse` at its current `byte_cap` request field — verify that cap is actually enforced pre-response-build, not post. If post-build, Phase 126 may surface a real bug, not just a doc gap.
- Phase 127: `NodeInfoResponse` already has a `supported_types[]` vector field at the end — new fixed-width fields must go BEFORE the variable-length vector in the encoder, or the wire-format extension changes the offset of `supported_types[]`. Finalize field order during plan.
- Phase 128: Every numeric `Config` field becomes a Prometheus gauge; verify the field list is the set named in `config.h` (ignore `std::string`, `std::vector`, `std::map`, `std::filesystem::path`). A sentinel test that iterates the struct at compile-time would catch drift.
- Phase 129: Announce-side filter must honor already-replicated blobs (don't "re-skip" a blob the peer already has; skip counter should only fire on first attempt). Flag at plan time.
- Phase 130: `cli/src/wire.h` lines 300-303 carry `MAX_CHUNKS=65536`, `CHUNK_SIZE_BYTES_DEFAULT=16 MiB`, `CHUNK_SIZE_BYTES_MIN=1 MiB`, `CHUNK_SIZE_BYTES_MAX=256 MiB` as `inline constexpr`. Converting these to runtime-set values requires renaming or going through an accessor — plan for API shape decision.

## Session Continuity

Last session: --stopped-at
Stopped at: Phase 127 context gathered
Resume file: --resume-file

**Planned Phase:** 127 (NodeInfoResponse Capability Extensions) — 4 plans — 2026-04-22T10:48:58.588Z
