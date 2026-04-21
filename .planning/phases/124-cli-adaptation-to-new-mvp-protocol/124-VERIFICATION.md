---
phase: 124-cli-adaptation-to-new-mvp-protocol
verified: 2026-04-21T14:45:00Z
status: passed
score: 7/7 must-haves verified
overrides_applied: 1
overrides:
  - must_have: "Delegate writes use SHA3(delegate_pubkey) as signer_hint; node resolves via delegation_map — live E2E verification"
    reason: "Live-E2E half of SC-124-4 scope-reduced to unit-test coverage per D-02 (no --as owner_ns selector in CLI). Substitute: [pubk] TEST_CASE #5 (CLI-side D-01a guard — delegate target_ns != own_ns skips auto-PUBK) + phase-122-04 node-side delegate-ingest tests. CLI-structural delegate-to-foreign-namespace write is infeasible under the current D-02 design; the SHA3(delegate_pk) signer_hint invariant is structurally enforced by build_owned_blob regardless."
    accepted_by: "mika"
    accepted_at: "2026-04-21T11:22:00Z"
---

# Phase 124: CLI Adaptation to New MVP Protocol — Verification Report

**Phase Goal:** `cdb` updated to emit blobs in the new 122+123 wire format — signer_hint instead of inline pubkey, auto-PUBK on first write, NAME-tagged overwrite, BOMB batched deletes. No backward compat. Live-node E2E matrix green on local + home.
**Verified:** 2026-04-21T14:45:00Z
**Status:** passed
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths (ROADMAP Success Criteria SC-124-1 … SC-124-7)

| #   | Truth (SC ID)                                                                                                     | Status              | Evidence                                                                                                                                                                                            |
| --- | ------------------------------------------------------------------------------------------------------------------ | ------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | SC-124-1: First `cdb` write to any fresh namespace auto-publishes a PUBK blob before the user's blob              | VERIFIED            | `ensure_pubk` wired in 7 owner-write flows (6 sites in commands.cpp:641/1165/1391/1835/2386/2453 + chunked.cpp:1 site); cmd::publish bypass + `mark_pubk_present_for_invocation` seeding; 124-E2E.md Items 1, 2 PASS on both nodes |
| 2   | SC-124-2: `build_blob()` in cli/src/wire.cpp emits signer_hint:32 only; old inline-pubkey path deleted entirely   | VERIFIED            | BlobData struct in wire.h:132-138 has exactly 5 fields (signer_hint, data, ttl, timestamp, signature); `grep -rE "blob\.namespace_id\|blob\.pubkey\.assign" cli/src/` → 0; FlatBuffer vtable offsets SIGNER_HINT=4, DATA=6, TTL=8, TIMESTAMP=10, SIGNATURE=12 byte-identical to db/schemas/blob.fbs |
| 3   | SC-124-3: `build_signing_input()` matches the final canonical form decided in Phase 122                            | VERIFIED            | `[wire][golden]` TEST_CASE with hardcoded 32-byte expected digest `e90ab535631db54ce4f0892f10426e72ca1470f3ffebd3083828d1c50613db89` passes (1 case, 1 assertion); live home + local nodes accept CLI-signed blobs end-to-end |
| 4   | SC-124-4: Delegate writes use SHA3(delegate_pubkey) as signer_hint; node resolves via delegation_map              | PASSED (override)   | Override: Live-E2E half scope-reduced to unit-test coverage. Structural evidence: [pubk] TC#5 in test_auto_pubk.cpp asserts D-01a delegate-skip (target_ns != own_ns → no PUBK emit); build_owned_blob structurally forces signer_hint=SHA3(id.signing_pubkey()) regardless of target_namespace (test_wire.cpp:322 delegate-mismatch TEST_CASE); node-side delegate-ingest covered by phase-122-04 tests. Accepted by mika on 2026-04-21T11:22Z |
| 5   | SC-124-5: `cdb put --name` / `cdb get <name>` / batched `cdb rm` all working end-to-end                           | VERIFIED            | 124-E2E.md Items 1 (SC#7 literal flow), 2 (cross-node sync), 3 (BOMB propagation), 6 (--replace BOMB-of-1), 7 (D-06 cascade) all PASS on both local + home; Rule-1 fix routed BOMBs via BlobWrite=64 (commit 5d337da0); Rule-2 fix added NAME + BOMB to `ls --type` filter |
| 6   | SC-124-6: All existing CLI Catch2 tests pass under new wire format; new tests cover auto-PUBK, NAME, BOMB paths   | VERIFIED            | Full cli_tests suite: 98 cases / 197614 assertions ALL PASSED. Tag-filtered: [wire] 23/568, [pubk] 7/49, [cascade] 2/12, [error_decoder] 1/7 all PASSED |
| 7   | SC-124-7: Live-node E2E against 192.168.1.73 (post-122+123 node): put → get → put --name → get <name> → rm → ls  | VERIFIED            | 124-E2E.md §Phase Gate = PASS. 6 of 7 matrix items PASS on both nodes (Item 5 SCOPE-DOWN, see override). Live 0x07 trigger produces exact D-05 string. Home daemon running post-124 binary confirmed post-restart (PID 7444, version g9248f01d) |

**Score:** 6/7 VERIFIED + 1/7 PASSED (override) = 7/7 pass total

### Required Artifacts

| Artifact                          | Expected                                                                               | Status     | Details                                                                                                                                             |
| --------------------------------- | -------------------------------------------------------------------------------------- | ---------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| `cli/src/wire.h`                  | BlobData signer_hint-only, MsgType::BlobWrite=64, Delete=17, Data=8 deleted             | VERIFIED   | 5-field BlobData struct; `BlobWrite=64` (1 match), `Delete=17` (1 match), `Data=8` (0 matches); target_namespace parameter rename applied            |
| `cli/src/wire.cpp`                | 5-slot FlatBuffer Blob vtable; build_owned_blob + encode_blob_write_body                | VERIFIED   | SIGNER_HINT=4/DATA=6/TTL=8/TIMESTAMP=10/SIGNATURE=12; blob_write_body_vt::TARGET_NAMESPACE=4/BLOB=6                                                   |
| `cli/src/commands.cpp`            | 9 build_owned_blob sites; ensure_pubk at 6 owner flows; decode_error_response; classify_rm_target; 0 TEMP-124 | VERIFIED | grep: 9 build_owned_blob, 9 encode_blob_write_body, 8 ensure_pubk refs (6 call sites + 2 comment/include), 7 decode_error_response, 6 classify_rm_target, 0 TEMP-124, 0 MsgType::Data, 0 blob.namespace_id |
| `cli/src/chunked.cpp`             | 3 build_owned_blob sites; ensure_pubk in put_chunked; 0 TEMP-124                       | VERIFIED   | grep: 3 build_owned_blob, 3 encode_blob_write_body, 2 ensure_pubk refs (1 call + include), 0 TEMP-124, 0 MsgType::Data, 0 blob.namespace_id          |
| `cli/src/pubk_presence.h`         | ensure_pubk + ensure_pubk_impl<> + mark_pubk_present_for_invocation + reset cache      | VERIFIED   | All 4 symbols declared; ensure_pubk_impl template body in header; 5745 bytes                                                                         |
| `cli/src/pubk_presence.cpp`       | Cache + delegate-skip wrapper binding Connection into ensure_pubk_impl                 | VERIFIED   | 2933 bytes; grep finds 4 pubk_cache() refs, synchronous-only (0 send_async/recv_for/co_await)                                                       |
| `cli/src/commands_internal.h`     | decode_error_response decl + RmClassification + classify_rm_target(+impl<>)            | VERIFIED   | All 4 symbols declared; template classify_rm_target_impl body in header (line 98)                                                                    |
| `cli/src/error_decoder.cpp`       | decode_error_response body (TU extraction for test linkage)                            | VERIFIED   | 57 lines; 5 case branches for 0x07/0x08/0x09/0x0A/0x0B + short-read guard + unknown-code default; zero internal-token/phase-number leaks            |
| `cli/tests/test_wire.cpp`         | [wire] migrated + [wire][golden] + [cascade] + [error_decoder] TEST_CASEs              | VERIFIED   | 25+ TEST_CASEs across [wire], [wire][golden], [cascade], [error_decoder] tags; tests PASS                                                            |
| `cli/tests/test_auto_pubk.cpp`    | 7 [pubk]-tagged TEST_CASEs via ensure_pubk_impl<Sender,Receiver>                       | VERIFIED   | 7 TEST_CASEs declared; all use ScriptedSource + CapturingSender (0 asio::io_context); all PASS                                                      |
| `.planning/.../124-E2E.md`        | D-08 matrix execution log with Phase Gate verdict                                      | VERIFIED   | 36267 bytes; 7 items documented with rerun sections; Phase Gate = PASS at 2026-04-21T11:22Z                                                          |

### Key Link Verification

| From                                        | To                                               | Via                                                                | Status    | Details                                                                                                                                                    |
| ------------------------------------------- | ------------------------------------------------ | ------------------------------------------------------------------ | --------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| cli/src/wire.cpp build_owned_blob           | cli/src/identity.h Identity::sign                | Identity::sign(signing_input) called inside build_owned_blob       | WIRED     | encode_blob_write_body composes the output of build_owned_blob; grep shows both are exported from wire.h and consumed by 9 commands.cpp + 3 chunked.cpp sites |
| cli/src/commands.cpp (6 cmd flows)          | cli/src/pubk_presence.h ensure_pubk              | Calls `ensure_pubk(id, conn, ns_span, pubk_rid)` before first write | WIRED     | 6 call sites: cmd::put:641, cmd::rm:1165, cmd::rm_batch:1391, cmd::reshare:1835 (conn2), cmd::delegate:2386, cmd::revoke:2453                                 |
| cli/src/chunked.cpp put_chunked             | cli/src/pubk_presence.h ensure_pubk              | 1 call before Phase A fill loop                                    | WIRED     | 1 call site + include; inherits the invocation cache when called from cmd::put (becomes cache-hit)                                                          |
| cli/src/commands.cpp cmd::publish           | cli/src/pubk_presence.h mark_pubk_present_…     | Seeds cache post-WriteAck; bypasses ensure_pubk                    | WIRED     | awk-window verification: 0 ensure_pubk refs in cmd::publish body, 1 mark_pubk_present_for_invocation call                                                   |
| cli/src/commands.cpp (5 ErrorResponse sites) | cli/src/error_decoder.cpp decode_error_response | `fprintf(stderr, decode_error_response(...).c_str())`              | WIRED     | grep: 7 decode_error_response refs in commands.cpp; `"Error: node rejected request"` string literal count = 0 (was 5 pre-plan)                              |
| cli/src/commands.cpp cmd::rm + cmd::rm_batch | cli/src/commands_internal.h classify_rm_target  | Cascade classification → BOMB target expansion                     | WIRED     | 6 classify_rm_target refs in commands.cpp; cascade summary string `"cascade: %zu manifests fully tombstoned"` present; warn-and-continue policy implemented  |
| cli/src/commands.cpp submit_bomb_blob       | node engine.ingest() via BlobWrite=64            | Rule-1 fix: BOMBs now routed via BlobWrite (was Delete=17)         | WIRED     | commands.cpp:583 `conn.send(MsgType::BlobWrite, envelope, rid)`; WriteAck path accepted; live-verified in 124-E2E.md Items 1, 3, 7                           |

### Data-Flow Trace (Level 4)

| Artifact                              | Data Variable           | Source                                                                       | Produces Real Data | Status  |
| ------------------------------------- | ----------------------- | ---------------------------------------------------------------------------- | ------------------ | ------- |
| BlobWriteBody envelope bytes          | ns_blob / envelope      | `build_owned_blob(id, ns, data, ttl, ts)` → real ML-DSA-87 signature          | Yes                | FLOWING |
| PUBK probe ListRequest payload        | list_payload (49 B)     | Constructed from target_namespace + PUBKEY_MAGIC; no hardcoded empty vals     | Yes                | FLOWING |
| BOMB data bytes                       | bomb_data               | `make_bomb_data(targets)` — concatenates real 32-byte hashes                  | Yes                | FLOWING |
| ErrorResponse decoded string          | payload → text          | Runtime payload from conn.recv; decoder branches on real error code byte      | Yes                | FLOWING |
| RmClassification.cascade_targets      | chunk_hashes vector     | envelope::decrypt → decode_manifest_payload → real chunk_hashes               | Yes                | FLOWING |

### Behavioral Spot-Checks

| Behavior                                        | Command                                                                 | Result                                                   | Status |
| ----------------------------------------------- | ----------------------------------------------------------------------- | -------------------------------------------------------- | ------ |
| [wire] tests pass                               | `./build/cli/tests/cli_tests "[wire]"`                                  | All tests passed (568 assertions in 23 test cases)       | PASS   |
| [pubk] tests pass                               | `./build/cli/tests/cli_tests "[pubk]"`                                  | All tests passed (49 assertions in 7 test cases)         | PASS   |
| [cascade] tests pass                            | `./build/cli/tests/cli_tests "[cascade]"`                               | All tests passed (12 assertions in 2 test cases)         | PASS   |
| [error_decoder] tests pass                      | `./build/cli/tests/cli_tests "[error_decoder]"`                         | All tests passed (7 assertions in 1 test case)           | PASS   |
| Full CLI suite                                  | `./build/cli/tests/cli_tests`                                           | All tests passed (197614 assertions in 98 test cases)    | PASS   |
| cdb binary builds and exports post-124 surface  | `ls -la build/cli/cdb`                                                  | 2455272 bytes, mtime 2026-04-21 13:38                    | PASS   |
| Live-node E2E matrix                            | See 124-E2E.md §Phase Gate                                              | Verdict: PASS (post home-daemon restart + scope-down)    | PASS   |

### Requirements Coverage

| Requirement | Source Plan(s)         | Description                                                                                               | Status                | Evidence                                                                                                                      |
| ----------- | ---------------------- | --------------------------------------------------------------------------------------------------------- | --------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| SC-124-1    | 124-02, 124-04         | First `cdb` write to any fresh namespace auto-publishes a PUBK blob before the user's blob               | SATISFIED             | 7 ensure_pubk wire sites; E2E Item 1 PASS on fresh-wiped namespace (both nodes)                                              |
| SC-124-2    | 124-01, 124-03, 124-04 | wire.cpp emits signer_hint:32 only; old inline-pubkey path deleted entirely                              | SATISFIED             | BlobData 5-field struct; 0 blob.namespace_id / 0 blob.pubkey.assign / 0 MsgType::Data phase-wide                              |
| SC-124-3    | 124-01                 | build_signing_input() matches final Phase 122 canonical form                                              | SATISFIED             | Golden-vector TEST_CASE PASS; live node ingest acceptance                                                                     |
| SC-124-4    | 124-01, 124-03, 124-05 | Delegate writes use SHA3(delegate_pubkey) as signer_hint; node resolves via delegation_map                | SATISFIED (override)  | Structural invariant in build_owned_blob + [wire] delegate TEST_CASE + [pubk] TC#5 + phase-122-04 node tests; live-E2E scope-down per D-02 |
| SC-124-5    | 124-03, 124-04, 124-05 | cdb put --name / cdb get <name> / batched cdb rm all working end-to-end                                   | SATISFIED             | E2E Items 1, 3, 6 PASS on both nodes; Rule-1 (BOMB via BlobWrite) + Rule-2 (ls --type NAME/BOMB) fixes landed                |
| SC-124-6    | 124-01, 124-02, 124-04 | All existing CLI Catch2 tests pass; new tests cover auto-PUBK, NAME, BOMB paths                          | SATISFIED             | 98 cases / 197614 assertions PASSED; new [pubk] (7), [cascade] (2), [error_decoder] (1), [wire][golden] (1) TEST_CASEs green  |
| SC-124-7    | 124-05                 | Live-node E2E against 192.168.1.73 (post-122+123 node): put → get → put --name → get <name> → rm → ls   | SATISFIED             | 124-E2E.md Phase Gate PASS; 6 of 7 items PASS both nodes; Item 5 SCOPE-DOWN documented                                       |

Note: Phase 124 declares no REQUIREMENTS.md IDs — ROADMAP Success Criteria (SC-124-1…7) serve as the requirement set per ROADMAP.md:153. All 7 SCs accounted for, 0 orphaned requirements.

### Anti-Patterns Found

| File                    | Line | Pattern                                               | Severity | Impact                                                                                                                                                                                                                                     |
| ----------------------- | ---- | ----------------------------------------------------- | -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| cli/src/main.cpp        | 619  | `"batched BOMB tombstone (Phase 123). Exit 2..."`    | Info     | Pre-existing phase-number leak in `cdb rm --help` text; violates `feedback_no_phase_leaks_in_user_strings.md`. Introduced by commit 257b5f27 (Phase 123-03 Task 1). Explicitly deferred to Phase 125 docs sweep (see deferred-items.md:7). NOT a phase-124 gap — predates phase and sits in help text unrelated to this phase's scope. |
| cli/src/commands.cpp    | —    | `"Phase 123"` in comments at :467, :608               | Info     | Code comments only, not user-visible output — no policy violation.                                                                                                                                                                        |
| cli/src/chunked.cpp     | ~237 | put_chunked retry loop does not re-validate EOF       | Warning  | 124-REVIEW.md WR-01 (advisory). Low-probability edge case; fix is trivial but out-of-scope for this phase. Not a goal-blocker.                                                                                                             |

All findings advisory. No blockers. Code review (124-REVIEW.md) status = issues_found (0 critical, 2 warning, 4 info) — advisory-only per project convention.

### Human Verification Required

None. All items automated via:
- Grep-verified structural invariants (BlobData shape, MsgType enum, 0 TEMP-124, 0 phase-leak literals)
- Unit-test assertions (98 cases / 197614 assertions PASS)
- Live E2E matrix already executed by Claude (124-E2E.md) with Phase Gate PASS verdict
- Override for SC-124-4 live-half scope-down explicitly accepted by mika on 2026-04-21T11:22Z

### Gaps Summary

No gaps. Every ROADMAP Success Criterion (SC-124-1 through SC-124-7) is backed by code + unit tests + live-E2E evidence (or a user-accepted scope-down override for SC-124-4's live half). All 12 RESEARCH Q1 blob-construction sites migrated. `MsgType::Data=8` fully eliminated; `BlobWrite=64` live-verified at every owner-write path; `Delete=17` retained for tombstones. Auto-PUBK wired at 7 owner-write flows with `cmd::publish` bypass + cache seed. D-05 decoder covers 5 error codes with zero internal-token / phase-number leaks. D-06 BOMB cascade expands CPAR manifests into a single BOMB with warn-and-continue partial-failure policy. Rule-1 (submit_bomb_blob BOMB-via-BlobWrite) and Rule-2 (`ls --type` NAME/BOMB recognition) mid-phase fixes landed and documented as deviations. Full cli_tests suite green at phase tip. 124-E2E.md Phase Gate = PASS.

**Minor notes (not gaps):**
- `cli/src/main.cpp:619` contains the pre-existing string `"(Phase 123)"` in help text (policy violation per `feedback_no_phase_leaks_in_user_strings.md`). Predates phase 124, explicitly deferred to Phase 125 per `deferred-items.md`. Not in-scope for phase-124 gap-closure.
- 124-REVIEW.md flags 2 warnings + 4 info findings (advisory only, 0 critical). None block goal achievement.

---

_Verified: 2026-04-21T14:45:00Z_
_Verifier: Claude (gsd-verifier)_
