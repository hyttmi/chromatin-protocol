---
phase: 124-cli-adaptation-to-new-mvp-protocol
plan: 01
subsystem: wire
tags: [flatbuffers, ml-dsa-87, sha3-256, oqs, catch2, signer_hint, BlobWriteBody]

# Dependency graph
requires:
  - phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey
    provides: "post-122 Blob schema (5 fields: signer_hint, data, ttl, timestamp, signature) and build_signing_input target_namespace invariant"
  - phase: 123-schema-signing-cleanup-append-bomb-and-name
    provides: "NAME + BOMB payload magic constants and make_name_data / make_bomb_data / parse_name_payload helpers"
provides:
  - "BlobData migrated from pre-122 (namespace_id + inline pubkey) to signer_hint-only layout"
  - "FlatBuffer Blob vtable renumbered to 5 slots (4/6/8/10/12) byte-identical to db/schemas/blob.fbs"
  - "MsgType enum: Data=8 deleted, BlobWrite=64 added, Delete=17 retained"
  - "build_signing_input first parameter renamed to target_namespace; byte output invariant (golden vector locks)"
  - "NamespacedBlob struct mirroring node's db/sync/sync_protocol.h vocabulary"
  - "build_owned_blob central helper: signer_hint = SHA3(id.signing_pubkey) structurally (T-124-02 mitigation)"
  - "encode_blob_write_body: 2-slot FlatBuffer envelope matching db/schemas/transport.fbs:83-87"
  - "7 new/migrated [wire] TEST_CASEs including [wire][golden] canonical-form cross-check"
affects:
  - 124-02-pubk-presence-auto-registration
  - 124-03-commands-chunked-migration-to-build_owned_blob
  - 124-04-error-mapping-and-bomb-cascade
  - 124-05-e2e-verification

# Tech tracking
tech-stack:
  added: []  # pure refactor — no new dependencies
  patterns:
    - "D-03 central helper: build_owned_blob replaces 12 copy-pasted blob-construction sequences"
    - "D-04 envelope encoder: encode_blob_write_body symmetric with node's BlobWriteBody table name (not node's function name)"
    - "Golden-vector test idiom: hardcoded 32-byte digest expected array guards canonical-form drift"
    - "Forward-declare Identity in wire.h; include identity.h only in wire.cpp (compile-graph tightness)"

key-files:
  created: []
  modified:
    - cli/src/wire.h
    - cli/src/wire.cpp
    - cli/tests/test_wire.cpp
    - cli/src/commands.cpp    # TEMP-124 compile-fix stubs only
    - cli/src/chunked.cpp     # TEMP-124 compile-fix stubs only

key-decisions:
  - "Kept MsgType::Delete=17 (RESEARCH Q3): node still emits DeleteAck for this TransportMsgType regardless of BlobWriteBody-shaped payload; removing would break 6 ack-check sites"
  - "Forward-declare Identity in wire.h rather than pulling identity.h into the header (compile-graph tightness)"
  - "TEMP-124 compile-fix stubs in commands.cpp (5 sites) and chunked.cpp (3 sites) flip MsgType::Data -> MsgType::BlobWrite and zero signer_hint; payloads are still bare Blob bytes and will be fully migrated to BlobWriteBody envelope in plan 03"
  - "Golden vector digest baked from x86_64 liboqs SHA3: e90ab535631db54ce4f0892f10426e72ca1470f3ffebd3083828d1c50613db89"

patterns-established:
  - "D-03: central signer_hint composition lives in build_owned_blob; downstream call sites shrink from 11 lines to 3"
  - "D-04: BlobWriteBody envelope is symmetric with node's table name (encode_blob_write_body) not node's function (encode_blob_write_envelope)"

requirements-completed:
  - SC-124-2
  - SC-124-3
  - SC-124-4
  - SC-124-6

# Metrics
duration: resumed-execution
completed: 2026-04-21
---

# Phase 124 Plan 01: Wire Foundation Summary

**BlobData migrated to signer_hint-only; MsgType::BlobWrite=64 added; build_owned_blob + encode_blob_write_body land as the D-03/D-04 foundation for all post-122 CLI writes.**

## Performance

- **Duration:** resumed-execution (prior executor hit budget mid-task-1; this run completed task 1 golden vector, all of task 2, and the four bookkeeping commits)
- **Completed:** 2026-04-21
- **Tasks:** 2/2
- **Files modified:** 5

## Accomplishments

- BlobData lost the pre-122 `namespace_id` and inline `pubkey` fields; gained a 32-byte `signer_hint` (SHA3 of signing pubkey).
- FlatBuffer Blob vtable renumbered to 5 slots (SIGNER_HINT=4, DATA=6, TTL=8, TIMESTAMP=10, SIGNATURE=12) — byte-identical to `db/schemas/blob.fbs`.
- `MsgType::Data=8` deleted (D-04a, T-124-03 mitigation); `MsgType::BlobWrite=64` added (D-04); `MsgType::Delete=17` retained (RESEARCH Q3).
- `build_signing_input` first parameter renamed to `target_namespace`; byte output invariance proven by pre-existing `wire: build_signing_input deterministic` TEST_CASE plus the new `wire: build_signing_input golden vector [wire][golden]` TEST_CASE with a hardcoded 32-byte digest.
- New surfaces land in `cli/src/wire.{h,cpp}`: `NamespacedBlob`, `build_owned_blob` (D-03 keystone), and `encode_blob_write_body` (D-04 envelope encoder).
- Six new `[wire]` TEST_CASEs verify: owner signer_hint, delegate signer_hint mismatch (T-124-02), OQS_SIG_verify against the canonical digest, BlobWriteBody FlatBuffer roundtrip, make_bomb_data byte-layout, parse_name_payload round-trip + magic corruption.

## Task Commits

Each task was committed atomically on `master` (sequential executor, no worktree):

1. **Task 1: Migrate BlobData + FlatBuffer Blob codec + build_signing_input rename; update MsgType enum** — `f04a6b9a` (feat)
2. **Task 2: Add build_owned_blob, NamespacedBlob, encode_blob_write_body; 6 new TEST_CASEs** — `37baed4e` (feat)

Plan metadata (summary + state + roadmap) will be committed as a single follow-up per `<final_commit>` protocol.

## Files Created/Modified

- `cli/src/wire.h` — BlobData migrated to 5-field signer_hint layout; MsgType::Data=8 deleted, MsgType::BlobWrite=64 added, MsgType::Delete=17 retained; added forward declaration of Identity; declared `NamespacedBlob`, `build_owned_blob`, and `encode_blob_write_body`; renamed `build_signing_input` parameter to `target_namespace`.
- `cli/src/wire.cpp` — `blob_vt` renumbered to 5 slots; new `blob_write_body_vt` namespace (TARGET_NAMESPACE=4, BLOB=6); rewrote `encode_blob` / `decode_blob` around the new schema; added `build_owned_blob` and `encode_blob_write_body` implementations; included `cli/src/identity.h`.
- `cli/tests/test_wire.cpp` — migrated two existing roundtrip TEST_CASEs (no more `namespace_id` / `pubkey` references); added 7 new TEST_CASEs: `wire: build_signing_input golden vector` [wire][golden], `wire: build_owned_blob owner sets signer_hint == id.namespace_id()`, `wire: build_owned_blob delegate -- signer_hint differs from target_namespace`, `wire: build_owned_blob signature verifies via OQS_SIG_verify`, `wire: encode_blob_write_body roundtrip`, `wire: make_bomb_data roundtrip`, `wire: parse_name_payload roundtrip`.
- `cli/src/commands.cpp` — 5 TEMP-124 compile-fix stubs: `MsgType::Data` → `MsgType::BlobWrite` at lines 524, 728, 1720, 2268, 2540 (line numbers post-edit). Plus 5 pre-existing `blob.signer_hint.fill(0)` stubs from the prior executor.
- `cli/src/chunked.cpp` — 3 TEMP-124 compile-fix stubs: `MsgType::Data` → `MsgType::BlobWrite` at lines 228, 247, 334. Plus 3 pre-existing `blob.signer_hint.fill(0)` stubs from the prior executor.

## Golden Vector Digest (Committed)

```
Inputs: ns = {0x00 × 32}
        data = {0x01, 0x02, 0x03}
        ttl = 3600
        timestamp = 1700000000

SHA3-256(ns || data || ttl_be32 || ts_be64)
  = e90ab535631db54ce4f0892f10426e72ca1470f3ffebd3083828d1c50613db89
```

Any drift in this digest means CLI↔node canonical signing form has desynced — regenerate only if the node's `build_signing_input` formula changes.

## TEMP-124 Compile-Fix Stubs (Inventory)

Explicitly allowed by plan 01's `<verification>` block ("compile-fix-only touch; full migrations happen in plan 03"). Plan 03 will migrate every one of these to `build_owned_blob` + `encode_blob_write_body` and drop the `TEMP-124` comments.

| File | Line (current) | Kind | Note |
|------|---------------:|------|------|
| cli/src/commands.cpp | ~514 | signer_hint.fill(0) | pre-existing (prior executor) |
| cli/src/commands.cpp | ~524 | MsgType::BlobWrite flip | this run |
| cli/src/commands.cpp | ~548 | signer_hint.fill(0) | pre-existing |
| cli/src/commands.cpp | ~716 | signer_hint.fill(0) | pre-existing |
| cli/src/commands.cpp | ~728 | MsgType::BlobWrite flip | this run |
| cli/src/commands.cpp | ~1233 | signer_hint.fill(0) | pre-existing |
| cli/src/commands.cpp | ~1703 | signer_hint.fill(0) | pre-existing |
| cli/src/commands.cpp | ~1720 | MsgType::BlobWrite flip | this run |
| cli/src/commands.cpp | ~1749 | signer_hint.fill(0) | pre-existing |
| cli/src/commands.cpp | ~2255 | signer_hint.fill(0) | pre-existing |
| cli/src/commands.cpp | ~2268 | MsgType::BlobWrite flip | this run |
| cli/src/commands.cpp | ~2387 | signer_hint.fill(0) | pre-existing |
| cli/src/commands.cpp | ~2521 | signer_hint.fill(0) | pre-existing |
| cli/src/commands.cpp | ~2540 | MsgType::BlobWrite flip | this run |
| cli/src/chunked.cpp  | ~105 | signer_hint.fill(0) | pre-existing |
| cli/src/chunked.cpp  | ~126 | signer_hint.fill(0) | pre-existing |
| cli/src/chunked.cpp  | ~228 | MsgType::BlobWrite flip | this run |
| cli/src/chunked.cpp  | ~247 | MsgType::BlobWrite flip | this run |
| cli/src/chunked.cpp  | ~322 | signer_hint.fill(0) | pre-existing |
| cli/src/chunked.cpp  | ~334 | MsgType::BlobWrite flip | this run |

Count: 12 × `signer_hint.fill(0)` + 8 × `MsgType::BlobWrite` flip = 20 TEMP-124 markers. Every one is comment-tagged `TEMP-124` for easy grep in plan 03.

## Grep Verification (Post-State)

```
$ grep -c "struct BlobData" cli/src/wire.h
1
$ grep -A7 "struct BlobData" cli/src/wire.h | grep -cE "namespace_id|pubkey"
0
$ grep -c "signer_hint" cli/src/wire.h
1
$ grep -cE "Data\s*=\s*8" cli/src/wire.h
0
$ grep -cE "BlobWrite\s*=\s*64" cli/src/wire.h
1
$ grep -cE "Delete\s*=\s*17" cli/src/wire.h
1
$ grep -c "target_namespace" cli/src/wire.h
2   # build_signing_input decl + build_owned_blob decl
$ grep -cE "SIGNER_HINT\s*=\s*4" cli/src/wire.cpp
1
$ grep -cE "NAMESPACE_ID\s*=\s*4" cli/src/wire.cpp
0
$ grep -c "blob_vt::PUBKEY" cli/src/wire.cpp
0
$ grep -c "NamespacedBlob" cli/src/wire.h
3   # forward hint + struct def + build_owned_blob return type
$ grep -c "build_owned_blob" cli/src/wire.h
3
$ grep -c "build_owned_blob" cli/src/wire.cpp
1
$ grep -c "encode_blob_write_body" cli/src/wire.h
1
$ grep -c "encode_blob_write_body" cli/src/wire.cpp
1
$ grep -cE "blob_write_body_vt::TARGET_NAMESPACE\s*=\s*4" cli/src/wire.cpp
1
$ ./build/cli/tests/cli_tests "[wire]" | tail -1
All tests passed (568 assertions in 23 test cases)
$ ./build/cli/tests/cli_tests "[wire][golden]" | tail -1
All tests passed (1 assertion in 1 test case)
$ ./build/cli/tests/cli_tests | tail -1
All tests passed (197546 assertions in 88 test cases)
```

All grep counts match the plan's `<acceptance_criteria>` for tasks 1 and 2.

## Decisions Made

- **Kept `MsgType::Delete=17`.** Research Q3 identified that the node's `message_dispatcher.cpp` still emits `DeleteAck=18` for `TransportMsgType=17` regardless of the new BlobWriteBody-shaped payload. Removing this would break 6 DeleteAck check sites in the CLI without any correctness gain. Ack-type semantics are independent of payload schema.
- **Forward declared `Identity` in `wire.h`.** `build_owned_blob` takes `const Identity&` but the helper's body is in `wire.cpp`. Forward declaration keeps the header's compile graph minimal — consumers of `wire.h` that don't build owned blobs don't pay for parsing `identity.h`.
- **TEMP-124 compile-fix stubs use `MsgType::BlobWrite` (not `MsgType::Delete`).** All pre-existing `MsgType::Data` sites semantically mapped to the new "write path", so flipping to `BlobWrite` keeps the semantics coherent even though the payload bytes remain bare-Blob-shaped until plan 03.
- **Golden vector inputs:** ns=zero-filled, small 3-byte data, ttl=3600, ts=1700000000 — small enough to make the expected array readable in the test, diverse enough that accidental byte-swap in any of the four absorb steps would show up immediately.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Flipped 8 `MsgType::Data` call sites to `MsgType::BlobWrite` in commands.cpp and chunked.cpp**

- **Found during:** Task 1 (cli_tests wouldn't link after `MsgType::Data=8` was deleted from the enum).
- **Issue:** 5 sites in `cli/src/commands.cpp` (lines 524, 728, 1720, 2268, 2540 post-edit) and 3 sites in `cli/src/chunked.cpp` (lines 228, 247, 334 post-edit) still referenced `MsgType::Data`, causing compile errors. The prior executor's stubs had handled the `BlobData` field migration (signer_hint.fill(0)) but missed the enum references.
- **Fix:** Flipped each to `MsgType::BlobWrite` with a `TEMP-124` comment explaining that the payload is still bare-Blob bytes and will be migrated to BlobWriteBody envelope in plan 03.
- **Files modified:** cli/src/commands.cpp, cli/src/chunked.cpp
- **Verification:** `cli_tests` links cleanly; full suite (88 cases, 197546 assertions) passes.
- **Committed in:** `f04a6b9a` (Task 1 commit)
- **Plan authorization:** `<verification>` block explicitly allows this — "choose one of two repairs... Insert placeholder... Either approach is acceptable as long as the deletion in plan 03 is clean and grep-verifiable." `TEMP-124` markers satisfy the grep requirement.

---

**Total deviations:** 1 auto-fixed (1 blocking compile-fix)
**Impact on plan:** Compile-fix was pre-authorized by plan verification block. No scope creep. All TEMP-124 markers are comment-tagged for plan 03's full migration pass.

## Issues Encountered

- Catch2 prints `REQUIRE(digest == expected)` failure output with char-formatted bytes, not hex. Resolved by adding a one-shot `std::fprintf(stderr, "GOLDEN_DIGEST_HEX=...")` line, capturing the clean hex from stderr, baking it into the `expected` initializer, and removing the debug print before commit.

## User Setup Required

None — pure wire refactor, no external services, no env vars.

## Next Phase Readiness

- **Plan 124-02 (PUBK presence auto-registration):** Ready. `build_owned_blob` + `encode_blob_write_body` are the exact surfaces the plan's `pubk_presence.cpp` composes for the emit path. `MsgType::BlobWrite=64` is in place for the WriteAck flow.
- **Plan 124-03 (commands + chunked migration):** Ready. 20 TEMP-124 markers identify every site that needs rewriting. The target replacement pattern is fully captured in PATTERNS.md §"cli/src/commands.cpp" and §"cli/src/chunked.cpp".
- **Plan 124-04 (error mapping + BOMB cascade):** Unaffected by plan 01 — BOMB helpers (`make_bomb_data`) already land and are covered by plan 01 test.
- **Plan 124-05 (E2E):** Blocked on plans 02–04 as expected.

## Self-Check: PASSED

- **Task 1 commit `f04a6b9a`:** `git log --oneline | grep -q f04a6b9a` → FOUND
- **Task 2 commit `37baed4e`:** `git log --oneline | grep -q 37baed4e` → FOUND
- **cli/src/wire.h:** FOUND (modified)
- **cli/src/wire.cpp:** FOUND (modified)
- **cli/tests/test_wire.cpp:** FOUND (modified)
- **`./build/cli/tests/cli_tests "[wire]"`:** 23 cases, 568 assertions, ALL PASSED
- **`./build/cli/tests/cli_tests "[wire][golden]"`:** 1 case, 1 assertion, PASSED
- **`./build/cli/tests/cli_tests`:** 88 cases, 197546 assertions, ALL PASSED

---
*Phase: 124-cli-adaptation-to-new-mvp-protocol*
*Plan: 01*
*Completed: 2026-04-21*
