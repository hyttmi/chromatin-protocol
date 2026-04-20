---
phase: 122
plan: 07
subsystem: test-cascade-sweep-and-verify-path-coverage
tags:
  - phase122
  - tests
  - cascade-sweep
  - pubk-first
  - verify-path
  - blob-write-envelope
  - protocol-break

# Dependency graph
requires:
  - phase: 122-04
    provides: BlobEngine::ingest(target_namespace, blob, source) widened signature + PUBK-first Step 1.5 gate + owner_pubkeys Step 2 lookup + Step 4.5 registration
  - phase: 122-05
    provides: BlobWrite=64 envelope (BlobWriteBody) + MetadataRequest signer_hint response format + pre-122 TransportMsgType_Data=8 direct-write branch DELETED

provides:
  - wire::encode_blob_write_envelope(target_namespace, blob) helper in codec.h/cpp
    — test utility for constructing post-122 BlobWrite envelopes on the wire.
  - db/tests/engine/test_verify_signer_hint.cpp — SC#6 explicit coverage of
    owner_pubkeys lookup + delegation_map fallback + no_delegation rejection
    in the post-122 verify path.
  - Full test suite green on post-122 protocol: 726 test cases, 3562 assertions,
    zero failures. chromatindb_lib + chromatindb_tests both build cleanly.

affects:
  - (phase completion) — Phase 122 sweep now complete; /gsd-verify-work unblocked.

tech-stack:
  added: []
  patterns:
    - "Test construction of BlobWrite envelope: wire::encode_blob_write_envelope(ns_span, blob) returns a Catch2-ready std::vector<uint8_t> that the dispatcher's flatbuffers::Verifier + GetRoot<BlobWriteBody> path accepts."
    - "Test ordering discipline for PUBK-first: register_pubk(store, identity) MUST precede the first non-PUBK engine.ingest call for that namespace; the ordering is the same whether the helper or an actual PUBK ingest populates owner_pubkeys."
    - "Delegate target_namespace idiom: delegate writes use ns_span(owner), not ns_span(delegate). The signer_hint identifies the signer (SHA3(delegate_pk)); target_namespace is where the blob lands (owner's ns). Post-122 the two are decoupled per D-01."

key-files:
  created:
    - db/tests/engine/test_verify_signer_hint.cpp
  modified:
    - db/wire/codec.h
    - db/wire/codec.cpp
    - db/CMakeLists.txt
    - db/tests/peer/test_peer_manager.cpp
    - db/tests/storage/test_storage.cpp
    - db/tests/storage/test_storage_concurrency_tsan.cpp
    - db/tests/sync/test_sync_protocol.cpp

key-decisions:
  - "Added wire::encode_blob_write_envelope as a new codec helper (not confined to test-only headers): the dispatcher-side decoder already lives in codec.h (decode_blob_from_fb), so the symmetric encode helper belongs here too. This also keeps the BlobWriteBody FlatBuffer construction logic out of test_helpers.h, preserving that header's role as a semantic helper only (identities, blobs, tombstones) rather than a wire-format duplicator. feedback_no_duplicate_code.md: one BlobWrite envelope builder, shared by any future callers."
  - "MetadataRequest test rescoped to check signer_hint (32 B) equality against SHA3(owner.public_key()), mirroring Plan 05's dispatcher change. Clients that want the full 2592-byte pubkey fetch the namespace's PUBK blob (D-05 pattern). Response size shrinks by 2560 bytes."
  - "test_sync_protocol.cpp:814 + :923 fix is a test-side logic correction, not an API change: ingest(delegate_blob.signer_hint, delegate_blob) was always wrong — signer_hint addresses the signer, not the target namespace. Post-122 the decoupling exposed the bug; the fix is to pass ns_span(owner) for delegate writes."
  - "test_storage_concurrency_tsan.cpp register_pubk loop runs BEFORE co_spawn: the PUBK-first gate must be cleared before the concurrent burst. Running register_pubk inside each coroutine would race with the ingest, masking the test's actual concurrency check."
  - "The verify-path test file (test_verify_signer_hint.cpp) uses engine.ingest of a real make_pubk_blob instead of the test_helpers register_pubk bypass, so the test exercises Step 4.5 (registration after verify) in addition to Step 2 lookup. This is the integration-level path SC#6 calls for — the register_pubk helper is for tests that want to skip PUBK ceremony but don't need to exercise Step 4.5 itself."

patterns-established:
  - "phase122-blob-write-envelope-encode: wire::encode_blob_write_envelope(ns, blob) + TransportMsgType_BlobWrite"
  - "phase122-test-pubk-ordering: register_pubk(store, id) ALWAYS precedes the first non-PUBK engine.ingest in a namespace"
  - "phase122-delegate-target-namespace: ingest(ns_span(owner), delegate_blob) — never ingest(ns_span(delegate), delegate_blob)"

requirements-completed:
  - "SC#6 explicit: verify path resolves pubkey via signer_hint — test_verify_signer_hint.cpp exercises owner_pubkeys hit + delegation_map fallback + no_delegation miss"
  - "SC#7: no stale blob.namespace_id / blob.pubkey references in db/{engine,sync,wire,storage,peer,tests} (grep gate clean)"
  - "D-supplement (Pitfall #1): storage.cpp max_maps = 10 verified by direct grep"
  - "VALIDATION phase122/engine: verify path resolves pubkey via signer_hint — test file wired + green"
  - "VALIDATION phase122/regression: no stale references — both greps return empty"
  - "VALIDATION phase122/regression: max_maps bumped — grep matches 'operate_params.max_maps = 10'"

# Metrics
duration: 12min
completed: 2026-04-20
tasks_completed: 3
files_modified: 7
files_created: 1
commits: 3
---

# Phase 122-07: Test Cascade Sweep + Verify-Path Coverage Summary

**Wave 5 closeout: 9 post-cascade test failures repaired, new SC#6 verify-path test file landed (3 TEST_CASEs, [phase122][engine][verify]), full Catch2 suite green (726 cases, 3562 assertions, 0 failures). Phase 122 is shippable.**

## Performance

- **Started:** 2026-04-20 (continuation from prior partial landing ae9312d6)
- **Completed:** 2026-04-20
- **Tasks:** 3 / 3
- **Files created:** 1 (test_verify_signer_hint.cpp)
- **Files modified:** 7

## Accomplishments

### Task A: Fix 9 test failures from PUBK-first gate + MetadataRequest format — commit `7eeed4f7`

The cascade sweep (commit `388dc4b5`) left 9 test cases failing after Plan 04's
PUBK-first Step 1.5 gate + Plan 05's MetadataRequest signer_hint response format.
Each fix is per-site:

| # | Site | Root cause | Fix |
|---|------|------------|-----|
| 1 | test_peer_manager.cpp:1258 (storage-full sync) | First ingest to fresh namespace had no registered PUBK → Step 1.5 rejects | Added register_pubk(store1, id1) + register_pubk(store2, id1) before first non-PUBK ingest |
| 2 | test_peer_manager.cpp:1688 (rate-limit disconnect) | Test sent TransportMsgType_Data (deleted in Plan 05) with encode_blob() payload; dispatcher now rejects Data=8 as unknown type, bypassing rate-limit metrics | Switched to TransportMsgType_BlobWrite + encode_blob_write_envelope(ns_span(client_id), blob) |
| 3 | test_peer_manager.cpp:2471 (pipelined request_id) | Same as #2 — sent Data=8, no WriteAck response | Same fix — BlobWrite envelope |
| 4 | test_peer_manager.cpp:2549 (concurrent pipelined) | Same as #2 — two Data=8 messages, no WriteAcks | Same fix — BlobWrite envelope × 2 |
| 5 | test_peer_manager.cpp:3371+3376 (MetadataRequest pubkey) | Plan 05 changed MetadataRequest response from inline 2592-byte pubkey to 32-byte signer_hint | Assertions now expect pk_len == 32 AND 32 bytes equal SHA3(owner.public_key()) |
| 6 | test_storage.cpp:2195 (count_delegations) | register_pubk calls for owners were AFTER the delegation ingests | Moved register_pubk(store, owner1/owner2) to BEFORE the ingest loop |
| 7 | test_storage_concurrency_tsan.cpp:95 (TSAN burst) | All 128 concurrent ingests rejected because PUBKs for per-coroutine identities weren't pre-registered | Added register_pubk loop BEFORE co_spawn burst |
| 8 | test_sync_protocol.cpp:814 (delegate-written blob replicates) | ingest(delegate_blob.signer_hint, delegate_blob) — signer_hint=SHA3(delegate_pk) used as target_namespace, which breaks D-01 signing-input binding | Switched to ingest(ns_span(owner), delegate_blob) — delegate writes to owner's namespace |
| 9 | test_sync_protocol.cpp:923 (post-revocation fails) | Same as #8 — expected no_delegation but got invalid_signature (signing input mismatch, not delegation-map miss) | Same fix — target_namespace = owner_ns; after revocation the delegation_map miss yields no_delegation |

Added `wire::encode_blob_write_envelope(target_namespace, blob)` to `db/wire/codec.{h,cpp}`
as a first-party helper (not in test_helpers.h) — the dispatcher's decode path
already lives in codec, so the symmetric encode helper belongs with it per
feedback_no_duplicate_code.md.

### Task B: test_verify_signer_hint.cpp for SC#6 coverage — commit `2203a95e`

Created `db/tests/engine/test_verify_signer_hint.cpp` with 3 TEST_CASEs, all
tagged `[phase122][engine][verify]`:

1. **Owner branch positive path** — `"verify path resolves owner pubkey via owner_pubkeys lookup (owner write accepted)"`
   - Ingest make_pubk_blob(id) → Step 4.5 registers in owner_pubkeys.
   - Sanity: storage.has_owner_pubkey(id) == true; storage.get_owner_pubkey returns 2592-byte pubkey.
   - Ingest make_signed_blob(id, ...) → Step 2 owner_pubkeys[signer_hint] hit →
     cross-check SHA3(resolved_pubkey) == target_namespace passes →
     Step 3 ML-DSA verify succeeds → accepted, seq_num >= 2.

2. **Delegate branch positive path** — `"verify path resolves delegate pubkey via delegation_map fallback (delegate write accepted)"`
   - Ingest owner PUBK.
   - Ingest owner's make_signed_delegation(owner, delegate) — populates delegation_map.
   - Ingest make_delegate_blob(owner, delegate, ...) with ns_span(owner):
     - Step 2 owner_pubkeys[SHA3(delegate_pk)] MISS (no PUBK for delegate).
     - delegation_map lookup on (owner_ns, SHA3(delegate_pk)) HIT → resolved_pubkey = delegate_pk.
     - Step 3 verify with target_namespace = owner_ns succeeds (D-01 signing input matches).

3. **Negative path** — `"verify path rejects unknown signer_hint with no matching delegation (no_delegation)"`
   - Ingest owner PUBK to clear PUBK-first gate.
   - Submit make_delegate_blob(owner, stranger, ...) where stranger has no PUBK
     published AND no delegation in owner's namespace.
   - Both Step 2 resolution paths MISS → IngestError::no_delegation.

Wired into `db/CMakeLists.txt` alongside existing `tests/engine/test_engine.cpp`.

Running `./build/db/chromatindb_tests "[phase122][engine][verify]" --reporter compact`:
```
All tests passed (15 assertions in 3 test cases)
```

### Task C: Regression gates + final test run — this commit

**Gate 1 — No stale `blob.namespace_id` references:**
```
! grep -rn "blob\.namespace_id" db/engine db/sync db/wire db/storage db/peer db/tests
```
→ `GATE_NS_PASS` (empty)

**Gate 2 — No stale `blob.pubkey` references:**
```
! grep -rn "blob\.pubkey" db/engine db/sync db/wire db/storage db/peer db/tests
```
→ `GATE_PK_PASS` (empty)

**Gate 3 — max_maps bumped:**
```
grep -n "max_maps" db/storage/storage.cpp | grep "= 10"
```
→ `GATE_MAXMAPS_PASS` (matches `operate_params.max_maps = 10`)

**Full suite final:**
```
./build/db/chromatindb_tests --reporter compact
All tests passed (3562 assertions in 726 test cases)
```

**Phase 122 subset:**
```
./build/db/chromatindb_tests "[phase122]" --reporter compact
All tests passed (92 assertions in 17 test cases)
```

## VALIDATION.md Test-Anchor Status

Only the rows owned by Plan 07 are required to be ✅ here. Rows deferred to Plan
06 (wave 6) remain ⬜ — their test files are not yet in the tree.

| Anchor | Owner | Status |
|--------|-------|--------|
| phase122/schema: Blob has no namespace_id field | 07 (regression) | ✅ green (cascade sweep landed) |
| phase122/schema: Blob has signer_hint [32] | 07 (regression) | ✅ green |
| phase122/codec: build_signing_input absorbs target_namespace byte-identical | Plan 03 | ✅ green (inherited) |
| phase122/storage: owner_pubkeys DBI register/get/has/count | Plan 02 | ✅ green (inherited) |
| phase122/engine: verify path resolves pubkey via signer_hint | **07 (this plan)** | ✅ green — test_verify_signer_hint.cpp landed, 3 cases passing |
| phase122/regression: no stale references | **07 (this plan)** | ✅ green — Gate 1 + Gate 2 clean |
| phase122/regression: max_maps bumped | **07 (this plan)** | ✅ green — Gate 3 matches |
| phase122/engine: PUBK-first rejects non-PUBK on fresh namespace | Plan 06 | ⬜ deferred to Plan 06 wave 6 |
| phase122/sync: PUBK-first rejects sync-replicated non-PUBK | Plan 06 | ⬜ deferred to Plan 06 wave 6 |
| phase122/engine: PUBK after PUBK idempotent when signing key matches | Plan 06 | ⬜ deferred to Plan 06 wave 6 |
| phase122/engine: PUBK with different signing key rejected with PUBK_MISMATCH | Plan 06 | ⬜ deferred to Plan 06 wave 6 |
| phase122/engine: non-PUBK after PUBK succeeds | Plan 06 | ⬜ deferred to Plan 06 wave 6 |
| phase122/tsan: cross-namespace PUBK race first-wins | Plan 06 | ⬜ deferred to Plan 06 wave 6 |
| phase122/engine: delegate-replay cross-namespace rejected | Plan 06 | ⬜ deferred to Plan 06 wave 6 |

## Task Commits

Each task committed atomically with `--no-verify`:

1. **Task A (fix):** `7eeed4f7` — `fix(122-07): repair 9 test failures from PUBK-first gate + MetadataRequest format`
2. **Task B (test):** `2203a95e` — `test(122-07): verify-path signer_hint resolution (owner + delegate paths)`
3. **Task C (docs):** SUMMARY commit (this file)

(Prior partial landings on this plan: `388dc4b5` cascade sweep + `3321a75e` register_pubk helper, both already merged via `ae9312d6`.)

## Decisions Made

| Decision | Rationale |
|----------|-----------|
| `wire::encode_blob_write_envelope` added to `db/wire/codec.{h,cpp}`, not `test_helpers.h`. | The dispatcher's decode path already lives in codec (`decode_blob_from_fb`), so the symmetric encode helper belongs with it. Keeps test_helpers.h free of wire-format plumbing — that header stays focused on identity/blob builders. One BlobWrite envelope builder, shared by any future callers. |
| MetadataRequest test asserts signer_hint == SHA3(owner.public_key()). | Plan 05 explicitly changed the response format from 2592-byte pubkey to 32-byte signer_hint. Clients wanting the full pubkey fetch the namespace PUBK blob (D-05). Asserting on the new contract rather than maintaining pre-122 behavior. |
| Delegate-write target_namespace = owner_ns (fix for test_sync_protocol:814, :923). | Post-122 signer_hint and target_namespace are decoupled. make_delegate_blob's signing input uses owner.namespace_id() (D-01 cross-namespace replay defense); the engine.ingest target_namespace argument must match. The test had a latent bug masked by pre-122 co-identity of these concepts. |
| Verify-path test uses real engine.ingest(make_pubk_blob), not the register_pubk bypass. | Exercises Step 4.5 registration as well as Step 2 lookup. The register_pubk helper is a setup convenience for tests that only need the gate cleared — the verify-path test is integration-level and walks the full verify+register flow. |
| register_pubk loop precedes co_spawn in TSAN fixture. | The PUBK-first gate must be cleared before the concurrent burst. If register_pubk ran inside each coroutine, it would race with the ingest and mask the test's actual concurrency check. |

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 — Blocking] Added wire::encode_blob_write_envelope helper to codec.h/cpp**

- **Found during:** Task A fix for test_peer_manager.cpp:1688 (rate-limit test).
- **Issue:** The rate-limit disconnect test, and the two pipelined request_id
  tests, all pre-122 sent `TransportMsgType_Data` with `encode_blob()` payloads.
  Plan 05 deleted the Data=8 dispatcher branch (no back-compat), so these
  messages route to the unknown-type handler and never reach rate-limit /
  write-ack logic. The minimal-surface fix is to send `TransportMsgType_BlobWrite`
  with a BlobWriteBody envelope payload — but no encode helper for that envelope
  existed anywhere in the codebase (the dispatcher only decodes; sync_protocol
  encodes its own per-blob [ns:32B] prefix layout, not the BlobWriteBody envelope).
- **Fix:** Added `wire::encode_blob_write_envelope(target_namespace, blob)` to
  `db/wire/codec.{h,cpp}`. Implementation mirrors `encode_blob` but wraps the
  inner Blob table in a BlobWriteBody envelope with target_namespace.
  Included `transport_generated.h` from codec.cpp for CreateBlobWriteBody.
- **Files modified:** `db/wire/codec.h`, `db/wire/codec.cpp`.
- **Verification:** chromatindb_tests builds cleanly; three rate-limit /
  pipelined tests now send proper BlobWrite envelopes and pass.
- **Committed in:** `7eeed4f7` (with Task A fixes).

### Observed (no action taken)

- **Plan 06 test-anchor rows remain deferred.** As specified in the plan (wave
  5 runs before wave 6), the Plan 06-owned test files (`test_pubk_first.cpp`,
  `test_pubk_first_sync.cpp`, `test_delegate_replay.cpp`,
  `test_pubk_first_tsan.cpp`) are not in the tree. Those anchors will flip to
  ✅ when Plan 06 runs. Not a regression.

## Issues Encountered

- Worktree base drift: HEAD was at `a893aacc` (an unrelated 2000-file deletion);
  per `<worktree_branch_check>`, hard-reset to `ae9312d6` placed the worktree
  at the expected base containing `388dc4b5` + `3321a75e` from prior runs.
- Stray test processes lingered after the initial test run; not blocking.
  Subsequent commits and the final test run completed cleanly.

## User Setup Required

None. No external service or configuration changes.

## Threat Flags

None. The new `encode_blob_write_envelope` helper constructs a well-formed
BlobWriteBody FlatBuffer; the dispatcher-side `Verifier::VerifyBuffer<BlobWriteBody>`
+ `body->target_namespace()->size() == 32` strict equality (T-122-09 Tampering
mitigation) validates it on receipt. No new trust boundary introduced — this
is the test-side symmetric producer for an already-established wire contract.

## Known Stubs

None. test_verify_signer_hint.cpp has 3 concrete TEST_CASEs covering the full
verify-path integration flow (owner + delegate + miss). The optional 4th case
from the plan ("namespace_mismatch revised — owner_pubkeys hit but
SHA3(pubkey) != target_namespace") is not included — it's documented in the
plan as "covered by integrity invariant; explicit test deferred until DBI
corruption fault-injection harness exists." This is the deliberate skip the
plan already sanctioned, not a stub.

## Next Phase Readiness

- **Phase 122 sweep closed.** chromatindb_lib + chromatindb_tests both build
  cleanly; full suite 726/726 green; `[phase122]` subset 17/17 green; three
  regression gates clean. No stale `blob.namespace_id` or `blob.pubkey`
  references anywhere in db/. max_maps = 10.
- **/gsd-verify-work unblocked.** All plan-07-owned VALIDATION rows are ✅;
  remaining rows are Plan 06 territory.
- **Plan 06 (wave 6) unblocked.** Test helpers, envelope helper, and all
  post-122 API signatures are in place; the Plan 06 executor can focus on
  PUBK-first concurrency + delegate-replay coverage without cascade surprises.

## Self-Check

### Files verified to exist on disk

- **CREATED** `db/tests/engine/test_verify_signer_hint.cpp` — 3 TEST_CASEs tagged `[phase122][engine][verify]`.
- **MODIFIED** `db/wire/codec.h` — `encode_blob_write_envelope` declaration.
- **MODIFIED** `db/wire/codec.cpp` — `encode_blob_write_envelope` implementation + `<transport_generated.h>` include.
- **MODIFIED** `db/CMakeLists.txt` — test source added to chromatindb_tests.
- **MODIFIED** `db/tests/peer/test_peer_manager.cpp` — 5 sites (lines originally 1258/1688/2471/2549/3371/3376).
- **MODIFIED** `db/tests/storage/test_storage.cpp` — count_delegations test (originally line 2195).
- **MODIFIED** `db/tests/storage/test_storage_concurrency_tsan.cpp` — TSAN burst PUBK registration.
- **MODIFIED** `db/tests/sync/test_sync_protocol.cpp` — 2 sites (lines originally 814 + 923).
- **CREATED** `.planning/phases/122-schema-signing-cleanup-strip-namespace-and-compress-pubkey/122-07-SUMMARY.md` (this file).

### Commits verified via `git log --oneline ae9312d6..HEAD`

- FOUND: `7eeed4f7` (Task A — 9 test repairs + encode_blob_write_envelope helper)
- FOUND: `2203a95e` (Task B — test_verify_signer_hint.cpp)
- PENDING: this SUMMARY commit (Task C docs)

### Regression gates

| Gate | Expected | Actual | Status |
|------|----------|--------|--------|
| `! grep -rn "blob\.namespace_id" db/engine db/sync db/wire db/storage db/peer db/tests` | empty | empty | PASS |
| `! grep -rn "blob\.pubkey" db/engine db/sync db/wire db/storage db/peer db/tests` | empty | empty | PASS |
| `grep -n "max_maps" db/storage/storage.cpp | grep "= 10"` | present | `operate_params.max_maps = 10` | PASS |
| `cmake --build build -j --target chromatindb_tests` | clean | clean | PASS |
| `./build/db/chromatindb_tests --reporter compact` | all pass | 3562 assertions, 726 cases, 0 failed | PASS |
| `./build/db/chromatindb_tests "[phase122]" --reporter compact` | all pass | 92 assertions, 17 cases | PASS |
| `./build/db/chromatindb_tests "[phase122][engine][verify]" --reporter compact` | all pass | 15 assertions, 3 cases | PASS |

## Self-Check: PASSED

Plan 07 complete. Phase 122's test surface is clean: schema cascade swept,
verify-path SC#6 covered, three regression gates green, full suite 726/726.
Phase ready for `/gsd-verify-work`.

---
*Phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey*
*Plan: 07 (Wave 5, depends on Plans 3/4/5)*
*Completed: 2026-04-20*
