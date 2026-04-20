---
phase: 123-tombstone-batching-and-name-tagged-overwrite
verified: 2026-04-20T00:00:00Z
status: human_needed
score: 7/7 must-haves verified
overrides_applied: 0
re_verification:
  previous_status: none
  previous_score: n/a
  gaps_closed: []
  gaps_remaining: []
  regressions: []
human_verification:
  - test: "Run Phase 123 Catch2 subset and confirm all tests pass"
    expected: "./build/db/chromatindb_tests '[phase123]' --reporter compact → all ~21 test cases green (5 codec + 5 bomb_validation + 2 bomb_side_effect + 1 name_delegate + 2 name_overwrite + 4 transport[list] + 1-2 bonus)"
    why_human: "Per feedback_delegate_tests_to_user.md, Catch2 execution is a user-run step (the orchestrator compiles but does not run tests). Required to certify SC#1-7 behaviorally — code inspection alone cannot prove the test suite is green."
  - test: "Run Phase 122 regression subset"
    expected: "./build/db/chromatindb_tests '[phase122]' --reporter compact → no regressions (Phase 123 touched engine Step 0e/2/3.5 and dispatcher wire-mapping else-if arms; the PUBK-first + signer_hint paths must remain green)"
    why_human: "Same reason — orchestrator compile-checks but does not run tests. Required to confirm Phase 123's engine edits did not regress Phase 122's ingest discipline."
  - test: "Run full Catch2 suite"
    expected: "./build/db/chromatindb_tests → zero failures across all phases"
    why_human: "Final sanity pass before phase closeout. Tests may uncover integration issues that per-phase subsets do not; only a human run surfaces real failure modes."
  - test: "Smoke-test cdb CLI new surface locally"
    expected: "./build-cli/cdb put --help | grep -- --name prints the --name and --replace options; ./build-cli/cdb get --help shows NAME vs hash positional dispatch; ./build-cli/cdb rm --help shows multi-target <hash>... usage"
    why_human: "Help-text rendering and CLI runtime correctness (argv parsing, positional dispatch, exit codes) are best confirmed by a human running the compiled binary; code inspection confirms the strings/branches exist but not that the binary wires them up at runtime without an assertion-compile issue."
  - test: "Live-node E2E (DEFERRED to post-124)"
    expected: "Not executed this phase — CLI remains on pre-122 wire per CONTEXT.md <domain> 'Test against the live 192.168.1.73 node until post-124'"
    why_human: "Explicit, documented deferral — not a gap. Flagged so the developer does not expect E2E coverage for Phase 123 in isolation."
---

# Phase 123: Tombstone Batching + Name-Tagged Overwrite — Verification Report

**Phase Goal:** Ship mutable-name overwrite (`cdb put --name foo`) and shrink tombstone bloat 200-300x by amortizing PQ signatures across batches — entirely via new blob magics, no node changes required (minimal node-side additions for BOMB validation + existing Phase 117 ListRequest reuse).

**Verified:** 2026-04-20
**Status:** human_needed — all code-level must-haves verified; test-suite execution and CLI smoke-tests are hand-off items per `feedback_delegate_tests_to_user.md`.
**Re-verification:** No — initial verification.

---

## Goal Achievement

### Observable Truths (Success Criteria from ROADMAP.md)

| #  | Truth | Status | Evidence |
|----|-------|--------|----------|
| 1  | New `NAME` magic (0x4E414D45) defined in both `cli/src/wire.h` and `db/wire/codec.h` with payload `[magic:4][name_len:2 BE][name:N][target_hash:32]` | VERIFIED | `db/wire/codec.h:160` + `cli/src/wire.h:314` both define `inline constexpr std::array<uint8_t, 4> NAME_MAGIC{_CLI} = {0x4E, 0x41, 0x4D, 0x45}`. Helpers `is_name`/`parse_name_payload`/`make_name_data` declared at codec.h:169/179/183. `NamePayload` struct layout matches the documented D-03 shape. |
| 2  | New `BOMB` magic defined with payload `[magic:4][count:4 BE][target_hash:32 x count]`; **BOMB MUST be written with ttl=0** — ingest rejects any BOMB with ttl != 0 | VERIFIED | `db/wire/codec.h:191` + `cli/src/wire.h:317` define `BOMB_MAGIC{_CLI} = {0x42, 0x4F, 0x4D, 0x42}`. Helpers `is_bomb`/`validate_bomb_structure`/`extract_bomb_targets`/`make_bomb_data` declared at codec.h:200-216. Engine has exactly 4 `wire::is_bomb` call sites (Steps 0e, 1.7, 2, 3.5 — `db/engine/engine.cpp:162, 202, 306, 390`). Step 1.7 (`engine.cpp:202-214`) rejects ttl != 0 with `IngestError::bomb_ttl_nonzero` BEFORE any crypto offload; test `BOMB with ttl!=0 is rejected [phase123][engine][bomb_ttl]` in `test_bomb_validation.cpp:97`. Structural sanity rejects via `bomb_malformed`; count=0 explicitly accepted (A2). |
| 3  | `cdb put --name foo file` writes content blob + NAME blob tagging it | VERIFIED | `cli/src/main.cpp:401` parses `--name`. `cmd::put` body at `cli/src/commands.cpp:573+` extended with `name_opt` and `replace` flags. Step 2 (NAME emission) calls `submit_name_blob` which uses `wire::make_name_data` (helper at wire.h:212, commands.cpp:509). Ordering is content → NAME (write-before-delete). `--name` is rejected for chunked files (Rule 2 auto-fix — documented in summary). |
| 4  | `cdb get foo` resolves the name | VERIFIED (plan deviation D-09 honored) | `cli/src/main.cpp:560` dispatches to `cmd::get_by_name` when positional is not 64-hex. Implementation at `cli/src/commands.cpp:1575-1615`: enumerate via Phase 117 `ListRequest + type_filter=NAME_MAGIC_CLI`, sort by (timestamp DESC, content_hash DESC) per D-01/D-02, fetch winner's target via existing `cmd::get`. **No local cache:** `grep -r "name_cache\|~/.chromatindb/" cli/src/` returns zero matches — D-09 stateless-always-enumerate is honored. ROADMAP mentions `~/.chromatindb/name_cache.json` but CONTEXT.md D-09 explicitly deviates to no-cache with user approval; the observable intent ("cdb get foo resolves") is met. |
| 5  | Overwrite = new NAME blob with higher seq; writer optionally emits BOMB via `--replace` | VERIFIED | `cli/src/main.cpp:417` parses `--replace`. `cmd::put` Step 0 at `commands.cpp:604-611` calls `resolve_name_to_target_hash` for prior binding; Step 3 at `commands.cpp:834-855` emits BOMB-of-1 ONLY AFTER Steps 1+2 succeed (write-before-delete ordering, plan-checker iter-1 fix). Overwrite semantics: resolution uses `blob.timestamp DESC` (D-01) + `content_hash DESC` tiebreak (D-02). Tests: `NAME with higher timestamp wins [phase123][overwrite]` and `Equal timestamps tiebreak on content_hash DESC [phase123][overwrite][tiebreak]` in `test_name_overwrite.cpp:97, 143`. |
| 6  | `cdb rm` accepts multiple targets in one invocation and emits ONE BOMB per invocation | VERIFIED (plan deviation D-06/D-07 honored) | `cli/src/main.cpp:625-674` collects all non-flag tokens into `hash_hexes`, calls `cmd::rm_batch`. Confirmation prompt distinguishes N=1 vs N>1. `cmd::rm_batch` at `commands.cpp:1292-1400+` dedupes targets, builds ONE BOMB via `make_bomb_data(targets)`, emits with ttl=0. ROADMAP wording ("accumulates for N seconds / K targets") deviates from D-06/D-07 per-command-only choice with user approval — the observable intent ("N targets → 1 BOMB") is met. |
| 7  | Node treats NAME/BOMB as opaque signed blobs — no FlatBuffers schema changes, no new TransportMsgType; Phase 117 ListRequest + type_filter reused | VERIFIED | `git diff 991ff295..HEAD -- db/schemas/` returns 0 lines. `git log --oneline 991ff295..HEAD -- db/schemas/transport.fbs` returns nothing. Dispatcher reuses existing `ListRequest` + `type_filter` path (`message_dispatcher.cpp:537-541`); no new transport branch added. Plan 04 integration test `test_list_by_magic.cpp` uses the identical `storage.get_blob_refs_since` + memcmp the dispatcher uses. Negative grep `0x4E.*0x41.*0x4D.*0x45|0x42.*0x4F.*0x4D.*0x42` outside codec.h / wire.h returns 0 — magic bytes live only at their canonical sites. |

**Score:** 7/7 must-haves verified

### Deferred Items

| # | Item | Addressed In | Evidence |
|---|------|--------------|----------|
| 1 | Live-node E2E validation of NAME + BOMB over the wire | Phase 124 (CLI wire adaptation) then post-Phase 125 deploy | CONTEXT.md `<domain>`: "Test against the live 192.168.1.73 node until post-124 (CLI + node are redeployed together after 124 lands)." Both Plan 03 and Plan 04 summaries flag this as an explicit deferral. |
| 2 | CPAR manifest cascade inside `cmd::rm_batch` | Phase 124 (natural home for unified cascade + BOMB) | Plan 03 SUMMARY §"cmd::rm_batch does not cascade chunked CPAR manifests" — BOMB is additive; orphaned CDAT chunks are GC'd by backlog 999.19. |
| 3 | `chromatindb gc admin tombstone cleanup` subcommand | Backlog 999.19 | Plan 04 SUMMARY §"Deferred Items". User-acknowledged future tooling; no correctness impact on Phase 123 scope. |
| 4 | Local `~/.chromatindb/name_cache.json` (ROADMAP text) | Future opt-in phase once ListRequest perf is measured | CONTEXT.md D-09 explicitly chose stateless-always-enumerate with user approval. Not a gap — a descoped optimization. |

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/wire/codec.h` | NAME_MAGIC + BOMB_MAGIC + is_name + is_bomb + validate_bomb_structure + extract_bomb_targets + NamePayload + parse_name_payload + make_name_data + make_bomb_data | VERIFIED | All declared at lines 160, 169, 179, 183, 191, 200, 207, 211, 216. |
| `db/wire/codec.cpp` | Implementations | VERIFIED | No TODO/FIXME/placeholder markers. |
| `cli/src/wire.h` | NAME_MAGIC_CLI + BOMB_MAGIC_CLI byte-identical; `make_name_data` / `make_bomb_data` / `parse_name_payload` declarations | VERIFIED | Lines 314, 317, 212, 217. |
| `cli/src/wire.cpp` | CLI-side helpers implementations | VERIFIED | `make_name_data` at :316, `make_bomb_data` at :337. |
| `db/peer/error_codes.h` | ERROR_BOMB_TTL_NONZERO (0x09), ERROR_BOMB_MALFORMED (0x0A), ERROR_BOMB_DELEGATE_NOT_ALLOWED (0x0B) + string table | VERIFIED | Line 18 + `error_code_string` case at :34. |
| `db/engine/engine.h` | IngestError enum extended with bomb_ttl_nonzero / bomb_malformed / bomb_delegate_not_allowed | VERIFIED | Plan 02 SUMMARY confirms; codec cross-references match at engine.cpp:202-214, :306-311. |
| `db/engine/engine.cpp` | Step 0e exemption, Step 1.7 structural, Step 2 delegate-reject, Step 3.5 side-effect | VERIFIED | Exactly 4 `wire::is_bomb` hits at lines 162, 202, 306, 390 — one per step. Step 1.7 runs BEFORE crypto offload (Pitfall #6). |
| `db/peer/message_dispatcher.cpp` | Wire-mapping arms for bomb_ttl_nonzero / bomb_malformed / bomb_delegate_not_allowed at BOTH BlobWrite + Delete dispatch sites | VERIFIED | Plan 02 SUMMARY documents; negative-grep for magic bytes in dispatcher returns 0 (helpers used, not hand-rolled). |
| `cli/src/main.cpp` | `--name` flag parse, `--replace` flag parse, get NAME-vs-hash dispatch, rm multi-target | VERIFIED | Lines 401, 417, 560, 625-674. `hash_hexes` vector at :493, :625. |
| `cli/src/commands.cpp` | `cmd::put` with name_opt/replace, `cmd::get_by_name`, `cmd::rm_batch`, `submit_name_blob`, `submit_bomb_blob` | VERIFIED | Lines 573, 1575, 1292 respectively. write-before-delete ordering confirmed at commands.cpp:834-855. |
| `db/tests/engine/test_bomb_validation.cpp` | Tests for T-123-01/02/03/04: bomb_ttl, bomb_sanity, bomb_accept (x2), bomb_delegate | VERIFIED | 5 TEST_CASEs present; tags [phase123][engine][bomb_ttl] / [bomb_sanity] / [bomb_accept] / [bomb_delegate] all confirmed. |
| `db/tests/engine/test_bomb_side_effect.cpp` | 2 TEST_CASEs for BOMB Step 3.5 deletion | VERIFIED | Plan 02 SUMMARY verification section confirms count=2. |
| `db/tests/engine/test_name_delegate.cpp` | Delegate NAME accepted (T-123-06 / D-11) | VERIFIED | TEST_CASE at line 34 tagged [phase123][engine][name_delegate]. |
| `db/tests/engine/test_name_overwrite.cpp` | Timestamp-DESC winner + content_hash-DESC tiebreak (T-123-05) | VERIFIED | 2 TEST_CASEs at lines 97 and 143 with correct tags. |
| `db/tests/peer/test_list_by_magic.cpp` | ListRequest+type_filter=NAME / BOMB / empty / no-filter enumeration | VERIFIED | 4 TEST_CASEs at lines 83, 152, 203, 240 all tagged [phase123][transport][list]. |
| `db/tests/test_helpers.h` | `make_name_blob` + `make_bomb_blob` helpers | VERIFIED | Lines 216, 241. |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|----|--------|---------|
| `cli/src/main.cpp` `put` branch | `cmd::put` with --name/--replace | argv parse → signature extension | VERIFIED | main.cpp:401,417 → commands.cpp:573. |
| `cli/src/main.cpp` `get` branch | `cmd::get_by_name` | positional-shape dispatch | VERIFIED | main.cpp:560 → commands.cpp:1575. 64-hex goes to `cmd::get`; non-hex to `cmd::get_by_name`. |
| `cli/src/main.cpp` `rm` branch | `cmd::rm_batch` | multi-target hash_hexes collector | VERIFIED | main.cpp:625-674 → commands.cpp:1292. |
| `cmd::put` --replace | prior-NAME target lookup | `resolve_name_to_target_hash` | VERIFIED | commands.cpp:604-611 calls the helper at Step 0 before content upload. |
| `cmd::put` --replace with prior target | BOMB-of-1 emission after NAME success | Step 3 ordering | VERIFIED | commands.cpp:834-855. Write-before-delete: content → NAME → BOMB. |
| `cmd::rm_batch` | one BOMB per invocation | `make_bomb_data(targets)` + `submit_bomb_blob` | VERIFIED | commands.cpp:1391-1400+. Dedupes targets before BOMB build. |
| `cmd::get_by_name` | enumerate NAME blobs | Phase 117 `ListRequest + type_filter=NAME_MAGIC_CLI` | VERIFIED | `commands.cpp:type_filter` returns 17 hits; Plan 03 SUMMARY §"cmd::get_by_name" confirms reuse. |
| Engine Step 1.7 | `IngestError::bomb_ttl_nonzero` | `wire::validate_bomb_structure` precheck | VERIFIED | engine.cpp:202-214. |
| Engine Step 2 trailer | `IngestError::bomb_delegate_not_allowed` | is_delegate AND wire::is_bomb | VERIFIED | engine.cpp:306-311. |
| Engine Step 3.5 | per-target `storage_.delete_blob_data` | `wire::extract_bomb_targets` loop | VERIFIED | engine.cpp:390-403. |
| Dispatcher BlobWrite | wire error codes 0x09/0x0A/0x0B | else-if arm mapping from IngestError | VERIFIED | Plan 02 SUMMARY confirms `grep -c bomb_ttl_nonzero db/peer/message_dispatcher.cpp = 2` (BlobWrite + Delete). |

### Data-Flow Trace (Level 4)

| Artifact | Data Variable | Source | Produces Real Data | Status |
|----------|---------------|--------|--------------------|--------|
| `cmd::put --name` | NAME payload bytes | `wire::make_name_data(name_bytes, content_hash_captured)` — content_hash captured from server WriteAck | YES | FLOWING — NAME binds to the server-returned content hash, not a hardcoded value. |
| `cmd::get_by_name` | resolution target | enumerate (Phase 117 ListRequest) → parse_name_payload → pick_name_winner → resolve_name_to_target_hash → cmd::get | YES | FLOWING — stateless enumerate-and-sort on every invocation. |
| `cmd::rm_batch` | BOMB payload | `make_bomb_data(targets)` where `targets` is argv-derived + deduped | YES | FLOWING — targets are real hex-decoded user input. |
| Engine Step 3.5 | delete-target loop | `wire::extract_bomb_targets(blob.data)` | YES | FLOWING — iterates real count field from validated payload. |

### Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| Full `[phase123]` Catch2 subset passes | `./build/db/chromatindb_tests '[phase123]' --reporter compact` | Not executed (per feedback_delegate_tests_to_user.md) | SKIP — routed to human |
| Phase 122 regression green | `./build/db/chromatindb_tests '[phase122]' --reporter compact` | Not executed | SKIP — routed to human |
| Full suite zero failures | `./build/db/chromatindb_tests` | Not executed | SKIP — routed to human |
| `cdb put --help` exposes `--name` + `--replace` | `./build-cli/cdb put --help` | Not executed (binary smoke-test belongs to user per hand-off block in Plan 03 SUMMARY) | SKIP — routed to human |

Rationale for SKIP: verifier directive explicitly states "Do NOT run the test suite yourself — per feedback_delegate_tests_to_user.md, tests are hand-off to the user." Compile evidence captured in plan summaries (all four plans report `cmake --build` exit 0 and binaries linked).

### Requirements Coverage

No REQ-ID list in PLAN frontmatters (Phase 123 traces coverage via ROADMAP Success Criteria rather than a separate REQUIREMENTS.md mapping for this phase). The 7 SCs above ARE the requirements contract; all 7 satisfied. Threat-model coverage (T-123-01..07 from Plan 02/04 threat models) is covered under the "Security Threat Model" section below.

### Security Threat Model Coverage

| Threat | Covered By | Status |
|--------|-----------|--------|
| T-123-01: delegate BOMB rejected (D-12) | `test_bomb_validation.cpp` `Delegate BOMB is rejected` (line 172) | VERIFIED |
| T-123-02: malformed BOMB rejected (D-13 structural) | `test_bomb_validation.cpp` `BOMB with wrong payload length is rejected` (line 123) | VERIFIED |
| T-123-03: BOMB ttl != 0 rejected (D-13 ttl=0 mandate) | `test_bomb_validation.cpp` `BOMB with ttl!=0 is rejected` (line 97) | VERIFIED |
| T-123-04: empty BOMB accepted as no-op (A2) | `test_bomb_validation.cpp` `BOMB with count=0 is accepted as no-op` (line 142) | VERIFIED |
| T-123-05: NAME squatting deterministic (D-01/D-02) | `test_name_overwrite.cpp` timestamp + tiebreak tests (lines 97, 143) | VERIFIED |
| T-123-06: delegate NAME allowed (D-11) | `test_name_delegate.cpp` `Delegate NAME is accepted and preserves signer_hint` (line 34) | VERIFIED |
| T-123-07: encrypted bodies not leaked via ListRequest | `test_list_by_magic.cpp` uses `get_blob_refs_since` which returns BlobRef (hash + type), not blob bodies (Plan 04 SUMMARY D-10 note) | VERIFIED |

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| — | — | None | — | Grep across `db/engine/engine.cpp`, `cli/src/commands.cpp`, `db/wire/codec.cpp`, `db/tests/peer/test_list_by_magic.cpp` for `TODO\|FIXME\|XXX\|HACK\|PLACEHOLDER\|not yet implemented` returned zero results. The rm_batch CPAR cascade is deferred, but the deferral is explicitly documented in Plan 03 SUMMARY with a justified reason (BOMB cannot currently cascade chunked manifests; Phase 124 is the natural home). |

### Human Verification Required

Per the `feedback_delegate_tests_to_user.md` hand-off model, the following items require local human execution before phase closeout:

#### 1. Phase 123 Catch2 subset

**Test:** `./build/db/chromatindb_tests '[phase123]' --reporter compact`
**Expected:** ~21 test cases green — `[wire][codec]` round-trips, `[engine][bomb_ttl/sanity/accept/delegate]`, `[engine][bomb_side_effect]`, `[engine][name_delegate]`, `[overwrite]`, `[overwrite][tiebreak]`, `[transport][list]` x4.
**Why human:** Test execution is the user's responsibility by project convention; code inspection cannot substitute.

#### 2. Phase 122 regression subset

**Test:** `./build/db/chromatindb_tests '[phase122]' --reporter compact`
**Expected:** No regressions — engine Step 0e/2/3.5 edits preserve PUBK-first + signer_hint paths; dispatcher additions are append-only `else if` arms.
**Why human:** Same convention; also the most likely site of a hidden regression given the surgical edits to engine.cpp.

#### 3. Full suite

**Test:** `./build/db/chromatindb_tests`
**Expected:** Zero failures across all phases.
**Why human:** Final sanity pass. Only a real run surfaces integration-level issues invisible to per-phase grep.

#### 4. CLI help-text smoke-check

**Test:** `./build-cli/cdb put --help`, `./build-cli/cdb get --help`, `./build-cli/cdb rm --help`
**Expected:** `put --help` shows `--name <name>` and `--replace` options; `get --help` documents NAME-vs-hash positional dispatch; `rm --help` shows `<hash>...` multi-target form.
**Why human:** Binary runtime correctness (argv parsing, help-text rendering) is best confirmed by actually invoking the binary.

#### 5. Live-node E2E — DEFERRED to post-124

**Test:** Not run this phase.
**Expected:** Per CONTEXT.md `<domain>`: "Test against the live 192.168.1.73 node until post-124 (CLI + node are redeployed together after 124 lands)."
**Why human:** Documented deferral; no action required for Phase 123 closeout.

---

## Gaps Summary

**No code-level gaps found.** All seven ROADMAP Success Criteria are satisfied by concrete, wired, substantive code on disk. Two of the seven (SC#4 local cache, SC#6 accumulator-daemon) deviate from the literal ROADMAP wording — but both deviations are explicitly documented in CONTEXT.md (D-09 / D-06 / D-07) with user approval, and the observable intent (`cdb get foo` resolves; N targets → 1 BOMB) is met in both cases. No overrides filed — the deviations land within the stated intent of each SC and require no frontmatter override.

The only outstanding work is **test-suite execution and CLI smoke-tests**, both owned by the user per `feedback_delegate_tests_to_user.md`. This is a process boundary, not an implementation gap. The binaries (`chromatindb_tests`, `cdb`) compile clean per all four plan-summary verification blocks.

**Recommendation:** After user reports green test results + passing CLI smoke-tests, Phase 123 is goal-complete and can be closed out.

---

_Verified: 2026-04-20_
_Verifier: Claude (gsd-verifier)_
