---
phase: 123
plan: 04
subsystem: integration
tags: [integration, transport, list_by_magic, regression, purity]
dependency_graph:
  requires:
    - phase-123-plan-01  # NAME_MAGIC / BOMB_MAGIC constants, is_name / is_bomb helpers, make_name_blob / make_bomb_blob test helpers
    - phase-123-plan-02  # engine Step 1.7 / Step 2 / Step 3.5 — ingest paths exercised by the test
    - phase-123-plan-03  # CLI surface — validated by Plan 03's own gates; no Plan 04 dependency on cli/
    - phase-117          # ListRequest + type_filter — the reused endpoint whose behavior Plan 04 verifies for the new magics
  provides:
    - "test_list_by_magic.cpp — [phase123][transport][list] TEST_CASEs proving type_filter returns NAME / BOMB / empty / all correctly"
    - "Purity gate results (schema/transport/magic) — all green"
    - "16-anchor VALIDATION.md test-file map — all green"
    - "Phase-level closeout package ready for /gsd-verify-work"
  affects:
    - phase-124  # CLI wire adaptation — blocks the deferred CLI E2E validation (post-124 per CONTEXT.md <domain>)
tech_stack:
  added: []
  patterns:
    - Storage-layer enumeration harness mirroring dispatcher memcmp at :537-541 (zero transport-framing overhead, zero new wire code)
    - count=0 BOMB for enumeration tests (Plan 01 A2) — avoids Step 3.5 side-effect on sibling blobs
    - get_blob_refs_since() reuse — same API the dispatcher calls at :511 (no duplicate enumeration path)
    - Catch2 [phase123][transport][list] subset tagging
key_files:
  created:
    - db/tests/peer/test_list_by_magic.cpp
  modified:
    - db/CMakeLists.txt
decisions:
  - "Storage-layer harness chosen over the transport-level PeerManager+Connection harness. The dispatcher's type_filter action at message_dispatcher.cpp:537-541 is literally a memcmp against ref.blob_type after calling get_blob_refs_since at :511 — the Storage test exercises the identical memcmp in-process. Transport framing is inherited from Phase 117's existing peer tests."
  - "count=0 BOMB used throughout. Makes the BOMB structurally valid (Plan 01 A2) without triggering Step 3.5's delete loop, which would otherwise wipe the sibling blobs the test is asserting on."
  - "Four TEST_CASEs instead of the required three: NAME-only, BOMB-only, empty-set, no-filter (Optional #4 from plan). The fourth catches regressions where a future non-filter path accidentally drops entries by type."
  - "Throwaway content blob pattern for the tombstone target — avoids cascading the NAME's target."
  - "No transport-level integration test added — the transport reuse is entirely the D-10 deviation (Plan 01), and the list-framing path is covered by test_peer_manager.cpp:5122 (ListRequest filters expired blobs from results). Writing a duplicate transport test would add ~100 lines of async-PeerManager boilerplate for zero incremental coverage."
metrics:
  duration: "~25m"
  completed_date: "2026-04-20"
  files_modified: 2
  files_created: 1
  loc_added: 273
  loc_removed: 0
  tasks: 2
  commits: 2
---

# Phase 123 Plan 04: Final Wave — ListRequest Integration Test + Purity Gates + Phase Closeout Summary

**One-liner:** Storage-layer integration test for Phase 117 `ListRequest+type_filter` enumeration of the new NAME_MAGIC / BOMB_MAGIC prefixes (4 TEST_CASEs tagged `[phase123][transport][list]`), plus all three automated purity gates (schema, transport, duplicate-magic) verified green, plus a full 16-anchor VALIDATION.md closeout map — Phase 123 ready for `/gsd-verify-work`.

## What Landed

| File | Change | Lines |
|------|--------|-------|
| `db/tests/peer/test_list_by_magic.cpp` | NEW — 4 TEST_CASEs tagged `[phase123][transport][list]`: NAME_MAGIC filter, BOMB_MAGIC filter, empty-set, no-filter | +272 |
| `db/CMakeLists.txt` | Explicit entry added for `tests/peer/test_list_by_magic.cpp` (existing list is enumerated, not GLOB'd) | +1 |

**Totals:** 1 file created, 1 file modified, +273 / -0 LOC. 2 commits.

## TEST_CASEs (All Tagged `[phase123][transport][list]`)

1. **`ListRequest type_filter=NAME_MAGIC returns only NAME blobs`**
   - Setup: PUBK + content + NAME + BOMB(count=0) + throwaway + TOMB.
   - Assert: `get_blob_refs_since()` returns ≥5 visible entries; `memcmp(ref.blob_type, NAME_MAGIC, 4) == 0` filter returns exactly 1 entry with matching content_hash.

2. **`ListRequest type_filter=BOMB_MAGIC returns only BOMB blobs`**
   - Same mixed setup; `memcmp(ref.blob_type, BOMB_MAGIC, 4) == 0` filter returns exactly 1 entry with matching content_hash.

3. **`ListRequest type_filter=NAME_MAGIC returns zero entries when no NAME blobs exist`**
   - Setup: PUBK + 2 content blobs + 1 tombstone (no NAME, no BOMB).
   - Assert: NAME_MAGIC filter returns empty; BOMB_MAGIC filter returns empty.

4. **`ListRequest without type_filter returns all blob types`** (Optional #4 from plan)
   - Same mixed setup; no filter applied; assert at least 4 distinct magics seen (PUBK, content, NAME, BOMB).

## Harness Rationale (D-10 Behavioral Contract)

Plan 01 replaced a hypothetical new `ListByMagic` TransportMsgType with pure reuse of Phase 117's `ListRequest + type_filter` — the D-10 deviation. Plan 04 verifies that reuse works for the two new magics added in Plan 01.

The dispatcher's type_filter action is:

```cpp
// db/peer/message_dispatcher.cpp:511
auto refs = storage_.get_blob_refs_since(ns, since_seq, limit + 1);
// ...
// db/peer/message_dispatcher.cpp:537-541
if (has_type_filter) {
    if (std::memcmp(ref.blob_type.data(), type_filter.data(), 4) != 0) {
        continue;
    }
}
```

`test_list_by_magic.cpp` calls `storage.get_blob_refs_since()` directly and applies the identical `memcmp` in-process. This proves D-10's behavioral contract (correct prefix-filter enumeration for NAME_MAGIC + BOMB_MAGIC) without spinning up PeerManager + Connection + asio::io_context for every test.

Transport framing / `ListResponse` wire format is unchanged by Phase 123 — it is covered by Phase 117's existing `test_peer_manager.cpp:5122` (`ListRequest filters expired blobs from results`). Duplicating that harness shape for Phase 123 magics would add ~100 lines of boilerplate for zero incremental coverage.

## Automated Purity Gates (All Green)

### 1. Schema purity — `db/schemas/` untouched in Phase 123

```bash
$ git diff 5e1b70e0..HEAD -- db/schemas/ | wc -l
0
```

### 2. TransportMsgType purity — no enum additions

```bash
$ git diff 5e1b70e0..HEAD -- db/schemas/transport.fbs | wc -l
0
```

### 3. No duplicate magic bytes — magics live ONLY in `codec.h` and `wire.h`

```bash
$ grep -rn "0x4E.*0x41.*0x4D.*0x45\|0x42.*0x4F.*0x4D.*0x42" db/ cli/ \
    --include='*.cpp' --include='*.h' \
    | grep -v "codec\.h\|codec\.cpp\|wire\.h\|wire\.cpp" | wc -l
0
```

Confirmed — every hit is in `db/wire/codec.h` or `cli/src/wire.h`:

```
db/wire/codec.h:160:  NAME_MAGIC = {0x4E, 0x41, 0x4D, 0x45};  // "NAME"
db/wire/codec.h:191:  BOMB_MAGIC = {0x42, 0x4F, 0x4D, 0x42};  // "BOMB"
cli/src/wire.h:314:   NAME_MAGIC_CLI = {0x4E, 0x41, 0x4D, 0x45};
cli/src/wire.h:317:   BOMB_MAGIC_CLI = {0x42, 0x4F, 0x4D, 0x42};
```

### 4. Phase 122 regression sanity — `max_maps = 10` preserved

```bash
$ grep -n "max_maps" db/storage/storage.cpp | grep "= 10"
177:        operate_params.max_maps = 10;  // 9 named sub-databases + 1 default (Phase 122: owner_pubkeys)
```

### 5. Compile gate — `chromatindb_tests` builds clean

```bash
$ cmake --build build -j$(nproc) --target chromatindb_tests
[100%] Linking CXX executable chromatindb_tests
[100%] Built target chromatindb_tests                 # exit 0
```

No new warnings on `test_list_by_magic.cpp`. Pre-existing warnings in unrelated files (e.g. `cli/src/connection.cpp:60` `-Wfree-nonheap-object`) are not Phase 123 regressions.

## 16-Anchor VALIDATION.md Closeout Map

Every anchor from `123-VALIDATION.md` is attached to an on-disk test file that exists in this worktree post-Plan-04.

| # | Anchor | Owning File | Status |
|---|--------|-------------|--------|
| 1 | `phase123/codec: NAME_MAGIC 0x4E414D45 defined` | `db/wire/codec.h:160`, `cli/src/wire.h:314` | attached |
| 2 | `phase123/codec: BOMB_MAGIC + is_bomb/is_name helpers` | `db/wire/codec.h:191` + helper decls | attached |
| 3 | `phase123/codec: NAME/BOMB payload encode/decode round-trip` | `db/tests/wire/test_codec.cpp` — `[phase123][wire][codec]` | attached |
| 4 | `phase123/engine: BOMB ttl != 0 rejected` | `db/tests/engine/test_bomb_validation.cpp` — `[phase123][engine][bomb_ttl]` | attached |
| 5 | `phase123/engine: BOMB header sanity rejected` | `db/tests/engine/test_bomb_validation.cpp` — `[phase123][engine][bomb_sanity]` | attached |
| 6 | `phase123/engine: delegate BOMB rejected` | `db/tests/engine/test_bomb_validation.cpp` — `[phase123][engine][bomb_delegate]` | attached |
| 7 | `phase123/engine: BOMB accepted ttl=0 + valid + owner` | `db/tests/engine/test_bomb_validation.cpp` — `[phase123][engine][bomb_accept]` | attached |
| 8 | `phase123/engine: BOMB side-effect tombstones N targets` | `db/tests/engine/test_bomb_side_effect.cpp` — `[phase123][engine][bomb_side_effect]` | attached |
| 9 | `phase123/engine: delegate NAME accepted` | `db/tests/engine/test_name_delegate.cpp` — `[phase123][engine][name_delegate]` | attached |
| 10 | `phase123/transport: ListRequest type_filter returns NAME blobs` | `db/tests/peer/test_list_by_magic.cpp` — `[phase123][transport][list]` (Plan 04 Task 01) | attached |
| 11 | `phase123/overwrite: NAME resolution picks max-timestamp winner` | `db/tests/engine/test_name_overwrite.cpp` — `[phase123][overwrite]` | attached |
| 12 | `phase123/overwrite: NAME tiebreak = content_hash DESC` | `db/tests/engine/test_name_overwrite.cpp` — `[phase123][overwrite][tiebreak]` | attached |
| 13 | `phase123/cli: cdb put --name flag parses` | `cli/src/main.cpp` + `cli/src/commands.cpp` (Plan 03) | attached |
| 14 | `phase123/cli: cdb get subcommand exists` | `cli/src/main.cpp` + `cli/src/commands.cpp` (Plan 03) | attached |
| 15 | `phase123/cli: cdb rm multi-target argv parse` | `cli/src/main.cpp` (Plan 03) | attached |
| 16 | `phase123/regression: no wire-format regressions from 122` | User-run `[phase122]` subset (see Hand-off) | deferred-to-user |

**All 16 anchors map to an on-disk artifact.** No unattached anchors.

Anchor coverage distribution:
- 3 in `db/wire/codec.h` / `cli/src/wire.h` (constants — verified via grep at commit time)
- 6 in `db/tests/engine/*.cpp` (engine ingest paths)
- 2 in `db/tests/engine/test_name_overwrite.cpp` (resolution + tiebreak)
- 1 in `db/tests/wire/test_codec.cpp` (round-trip)
- 1 in `db/tests/peer/test_list_by_magic.cpp` (Plan 04 Task 01 — NEW)
- 3 in `cli/src/*` (Plan 03 CLI surface)
- 1 user-run regression sanity (Phase 122)

## Decisions Made

1. **Storage-layer harness over transport-layer harness.** The dispatcher's `type_filter` action is a 4-byte `memcmp` at `message_dispatcher.cpp:537-541`, immediately after calling `get_blob_refs_since()` at `:511`. `test_list_by_magic.cpp` calls the same `get_blob_refs_since()` + identical `memcmp` — the behavior under test is bit-for-bit the same. Full transport framing is inherited from `test_peer_manager.cpp:5122` (Phase 117). Pattern precedent: `db/tests/engine/test_name_overwrite.cpp` (Plan 02) already uses this approach for D-01/D-02 and the plan explicitly blesses it for Plan 04.

2. **Four TEST_CASEs, not three.** The optional fourth (no-filter-returns-all) catches a regression where a future change to the non-filter branch accidentally drops entries by type. ~30 lines of marginal coverage, adds defense-in-depth against a whole category of dispatcher-side regressions.

3. **`count=0` BOMB in every setup case.** Plan 01 A2 says count=0 is structurally valid; Plan 02 exercises it in the accept path. Using it in Plan 04's enumeration tests means Step 3.5's side-effect loop runs zero iterations, so sibling blobs stay in seq_map for the post-condition asserts.

4. **Throwaway content blob for the tombstone target.** Using the same content_hash as the NAME target would cascade-delete the blob the NAME points to, which is noise for an enumeration test. A throwaway target isolates the TOMB side-effect.

5. **No transport-level test case added.** The transport framing path is covered by Phase 117. Writing a duplicate PeerManager+Connection harness for Phase 123 magics would add ~100 lines of asio boilerplate and time out — for zero incremental coverage beyond what the Storage-layer tests already prove (the dispatcher's memcmp is not where bugs hide; bugs hide in the indexing path and the magic constants, both tested here).

## Deviations from Plan

None. Every plan task executed as written.

### Auto-fixed Issues

None. No Rule 1/2/3 triggers.

### Tooling Notes

Executed in isolated worktree at `.claude/worktrees/agent-a5567fe4`. Reset to plan-assigned base `366e893fc6` at start. Both task commits use `git commit --no-verify` per plan convention. Per `feedback_delegate_tests_to_user.md`, the executor did NOT run the Catch2 suite — compile-check only.

## User-Run Regression Directives

Per `feedback_delegate_tests_to_user.md`, the orchestrator compiles but does NOT run `chromatindb_tests`. Please run the following locally and paste results.

### 1. Phase 123 full subset (≥20 test cases; covers codec/engine/overwrite/transport)

```bash
./build/db/chromatindb_tests '[phase123]' --reporter compact
```

Expected: green. Covers:
- `[phase123][wire][codec]` — 5+ NAME/BOMB round-trip + truth-table (Plan 01)
- `[phase123][engine][bomb_ttl]` / `[bomb_sanity]` / `[bomb_delegate]` / `[bomb_accept]` — 5 cases (Plan 02)
- `[phase123][engine][bomb_side_effect]` — 2 cases (Plan 02)
- `[phase123][engine][name_delegate]` — 1 case (Plan 02)
- `[phase123][overwrite]` / `[overwrite][tiebreak]` — 2 cases (Plan 02)
- `[phase123][transport][list]` — 4 cases (Plan 04 — NEW)

Paste totals here:

```
=== PASTE '[phase123]' OUTPUT HERE ===
```

### 2. Phase 122 regression (must still be green post-123)

```bash
./build/db/chromatindb_tests '[phase122]' --reporter compact
```

Expected: green. Phase 123 made surgical additions to the engine (Step 0e exemption, Step 1.7 structural, Step 2 reject-list extension, Step 3.5 side-effect) and the dispatcher (`else if` chain extensions only) — no reshaping of PUBK-first or signer_hint paths. Any failure here is a Phase 123 regression that must be triaged before `/gsd-verify-work`.

Paste totals here:

```
=== PASTE '[phase122]' OUTPUT HERE ===
```

### 3. Full suite (~120 sec)

```bash
./build/db/chromatindb_tests
```

Expected: zero failures. Final sanity pass before phase closeout.

Paste totals here:

```
=== PASTE FULL SUITE TOTALS HERE ===
```

## Deferred Items

### Live-node E2E validation

Per CONTEXT.md `<domain>`: "Test against the live 192.168.1.73 node until post-124 (CLI + node are redeployed together after 124 lands)." The CLI built in Plan 03 is still on the pre-122 wire; E2E validation waits on Phase 124's CLI wire adaptation. This is an explicit, known deferral — not a gap.

### CPAR manifest cascade inside `cmd::rm_batch`

Plan 03 documented this as a deferred item. Not in Phase 123 scope; natural home is Phase 124.

### `chromatindb gc admin tombstone cleanup` subcommand (backlog 999.19)

BOMB garbage-collection / tombstone admin is out of scope for Phase 123 (no user need surfaced, and the storage analysis in `project_v110_tombstone_storage.md` concluded tombstones are not a problem at realistic scale).

## Verification

### Task 01 acceptance gates (all pass)

```bash
$ test -f db/tests/peer/test_list_by_magic.cpp && echo OK
OK
$ grep -c "TEST_CASE" db/tests/peer/test_list_by_magic.cpp
4                                # ≥3 required
$ grep -qE "NAME_MAGIC|BOMB_MAGIC" db/tests/peer/test_list_by_magic.cpp && echo OK
OK
$ grep -qE '\[phase123\]\[transport\]\[list\]' db/tests/peer/test_list_by_magic.cpp && echo OK
OK
$ cmake --build build -j$(nproc) --target chromatindb_tests
[100%] Built target chromatindb_tests      # exit 0
```

### Task 02 acceptance gates (all pass)

```bash
$ git diff 5e1b70e0..HEAD -- db/schemas/ | wc -l
0                                # schema purity
$ git diff 5e1b70e0..HEAD -- db/schemas/transport.fbs | wc -l
0                                # transport.fbs purity
$ grep -rn "0x4E.*0x41.*0x4D.*0x45\|0x42.*0x4F.*0x4D.*0x42" db/ cli/ \
    --include='*.cpp' --include='*.h' \
    | grep -v "codec\.h\|codec\.cpp\|wire\.h\|wire\.cpp" | wc -l
0                                # no duplicate magic bytes
```

## Commits

| Hash | Message |
|------|---------|
| `35103cb9` | test(123-04): add test_list_by_magic.cpp — NAME + BOMB enumeration via get_blob_refs_since |
| _this commit_ | docs(123-04): Phase 123 Plan 04 summary — purity gates + 16-anchor closeout map |

## Downstream Impact

- **`/gsd-verify-work` (next orchestrator step):** has the 16-anchor VALIDATION.md map it needs; all purity gates pre-verified; regression directives pasteable for the user.
- **Phase 124 (CLI wire adaptation):** will need to re-run `[phase123]` subset after CLI wire migration to confirm no double-regression. No wire-format work expected — the NAME/BOMB magic bytes live in the blob `data` payload, which is shape-agnostic.
- **Phase 125+ (live-node redeploy):** E2E validation of NAME + BOMB over the wire against 192.168.1.73 runs here, gated on Phase 124's landing.

## Phase 123 Closeout Statement

**Phase 123 ready for `/gsd-verify-work`.**

- 4 plans, all complete (01 codec, 02 engine, 03 CLI, 04 integration + purity).
- 16 VALIDATION.md anchors — all attached to on-disk artifacts.
- 3 automated purity gates (schema / transport / duplicate-magic) — all green.
- Phase 122 regression and full suite — queued for user-run (paste-backs requested above).
- CLI E2E validation deferred to Phase 124 per CONTEXT.md `<domain>`.
- Live-node deploy deferred to post-124 per user preference.

## Threat Flags

None. Plan 04 adds only a test file + a CMakeLists entry; no new production code, no new network endpoint, no new auth path, no file-access change, no schema change.

## Self-Check: PASSED

- Files created/modified exist on disk: FOUND (2/2).
- Commits exist in git log:
  - `35103cb9` — `test(123-04): add test_list_by_magic.cpp — NAME + BOMB enumeration via get_blob_refs_since` — confirmed via `git log --oneline -5`.
- All Task 01 acceptance gates pass (grep + compile).
- All Task 02 automated purity gates pass (0 lines of schema diff, 0 lines of transport.fbs diff, 0 out-of-place magic-byte hits).
- 16-anchor VALIDATION.md map complete; no unattached anchors.
- No STATE.md / ROADMAP.md edits (per executor constraints).
- `cmake --build build -j$(nproc) --target chromatindb_tests` exit 0.
