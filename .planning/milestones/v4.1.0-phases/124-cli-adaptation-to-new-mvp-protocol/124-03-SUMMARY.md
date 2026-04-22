---
phase: 124
plan: "03"
subsystem: cli
tags: [blob-migration, build_owned_blob, BlobWriteBody, signer_hint, d-03, d-04, d-04b, sc-124-2, sc-124-4, sc-124-6]

# Dependency graph
requires:
  - phase: 124
    plan: "01"
    provides: "BlobData post-122 shape; build_owned_blob + encode_blob_write_body helpers; MsgType::BlobWrite=64; MsgType::Delete=17 retained; TEMP-124 compile-fix stubs inventoried"
  - phase: 124
    plan: "02"
    provides: "pubk_presence module ready for plan 04 wiring (NOT consumed by plan 03)"
provides:
  - "All 12 blob-construction sites (9 commands.cpp + 3 chunked.cpp) route through build_owned_blob + encode_blob_write_body"
  - "Zero TEMP-124 markers remain in cli/ (phase-wide grep returns 0)"
  - "Zero blob.namespace_id / blob.pubkey.assign references remain in cli/src/"
  - "Zero MsgType::Data references remain in cli/src/"
  - "MsgType binding per RESEARCH Q3: BlobWrite=64 at all 5 owner-write sites in commands.cpp + 3 chunked CDAT/manifest sites; Delete=17 at all 4 tombstone sites in commands.cpp + 2 rm_chunked tombstone sites"
  - "build_cdat_blob_flatbuf and build_tombstone_flatbuf return semantics updated: now emit BlobWriteBody envelope bytes (not bare Blob bytes)"
affects:
  - 124-04-error-mapping-and-bomb-cascade       # plan 04 wires ensure_pubk + D-05 error decoder + D-06 BOMB cascade
  - 124-05-e2e-verification                      # blocked on plan 04

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "D-03 helper consolidation: every blob-construction site shrinks from ~11 lines of namespace_id+pubkey+sign+BlobData population to 2 lines (build_owned_blob + encode_blob_write_body)"
    - "Helper-body return-semantic change in chunked.cpp: callers unchanged but bytes returned are now BlobWriteBody-shaped"
    - "Per RESEARCH Q3, MsgType::Delete retained for tombstone paths — node emits DeleteAck for TransportMsgType_Delete regardless of BlobWriteBody-shaped payload"

key-files:
  created: []
  modified:
    - cli/src/commands.cpp
    - cli/src/chunked.cpp

key-decisions:
  - "MsgType binding followed RESEARCH Q3 verbatim: BlobWrite for OWNER-write paths (NAME/CENV/DLGT/PUBK/CDAT/CPAR); Delete for all tombstone paths (single tombstone, reshare old-blob tombstone, revoke delegation tombstone, BOMB batched tombstone, rm_chunked per-chunk + manifest tombstones)"
  - "Kept sub-repo conn.send(MsgType::Delete, ...) semantics at BOMB site (commands.cpp:538) and all 4 tombstone call sites (commands.cpp:1203, 1706, 2318; chunked.cpp:369, 422)"
  - "build_cdat_blob_flatbuf and build_tombstone_flatbuf keep std::vector<uint8_t> return type; only the semantic meaning of the bytes changes (documented in helper header comments)"
  - "cmd::publish migrated to build_owned_blob like every other owner-write site; auto-PUBK bypass for cmd::publish is plan 04's concern (this plan only does mechanical migration)"

requirements-completed:
  - SC-124-2
  - SC-124-4
  - SC-124-6

# Metrics
metrics:
  tasks_completed: 2
  files_modified: 2
  duration_minutes: 6
  completed_date: "2026-04-21"
---

# Phase 124 Plan 03: commands.cpp + chunked.cpp Site Migration — Summary

Twelve blob-construction sites migrated to build_owned_blob + encode_blob_write_body in two atomic commits; all 20 TEMP-124 compile-fix stubs from plan 01 replaced with real migrations; phase-wide invariants (zero namespace_id, zero pubkey.assign, zero MsgType::Data, zero TEMP-124) all hold; full cli_tests suite (95 cases, 197595 assertions) green; cdb binary builds cleanly.

## Performance

- **Duration:** 5m 30s (sequential executor, master branch)
- **Completed:** 2026-04-21
- **Tasks:** 2/2
- **Files modified:** 2 (cli/src/commands.cpp, cli/src/chunked.cpp)

## The 12 Migration Sites

| # | File                  | Function                          | Pre-Edit Line Anchor | MsgType Change   | `X_data`                                | `ttl` |
|---|-----------------------|-----------------------------------|----------------------|------------------|-----------------------------------------|-------|
| 1 | cli/src/commands.cpp  | `submit_name_blob`                | ~513                 | (→ BlobWrite)    | `make_name_data(name_bytes, target_hash)` | caller-supplied |
| 2 | cli/src/commands.cpp  | `submit_bomb_blob`                | ~547                 | Delete (KEEP)    | `bomb_data` (caller-built)              | 0     |
| 3 | cli/src/commands.cpp  | `cmd::put` pipeline loop body     | ~715                 | (→ BlobWrite)    | `envelope::encrypt(...)` CENV           | user / default |
| 4 | cli/src/commands.cpp  | `cmd::rm` single-tombstone path   | ~1232                | Delete (KEEP)    | `make_tombstone_data(target_hash)`      | 0     |
| 5 | cli/src/commands.cpp  | `cmd::reshare` new CENV blob      | ~1702                | (→ BlobWrite)    | `envelope::encrypt(...)` re-encrypted   | ttl   |
| 6 | cli/src/commands.cpp  | `cmd::reshare` old-blob tombstone | ~1748                | Delete (KEEP)    | `make_tombstone_data(old_hash)`         | 0     |
| 7 | cli/src/commands.cpp  | `cmd::delegate` loop body         | ~2254                | (→ BlobWrite)    | `make_delegation_data(delegate_pk)`     | 0     |
| 8 | cli/src/commands.cpp  | `cmd::revoke` loop body           | ~2386                | Delete (KEEP)    | `make_tombstone_data(delegation_blob_hash)` | 0 |
| 9 | cli/src/commands.cpp  | `cmd::publish` (PUBK writer)      | ~2520                | (→ BlobWrite)    | `make_pubkey_data(signing_pk, kem_pk)`  | 0     |
| 10 | cli/src/chunked.cpp  | `build_cdat_blob_flatbuf` body    | ~94                  | (helper; caller sends BlobWrite) | `[CDAT magic:4][CENV(...)]`        | ttl   |
| 11 | cli/src/chunked.cpp  | `build_tombstone_flatbuf` body    | ~115                 | (helper; caller sends Delete)    | `make_tombstone_data(target)`       | 0     |
| 12 | cli/src/chunked.cpp  | `put_chunked` manifest emit       | ~318                 | (→ BlobWrite)    | `[CPAR magic:4][CENV(manifest_bytes)]`  | ttl   |

**MsgType binding matches RESEARCH Q3 table verbatim.** Sites 1/3/5/7/9 in commands.cpp = 5 BlobWrite writes. Sites 2/4/6/8 = 4 Delete tombstones (node emits DeleteAck for these — ack semantics independent of payload shape). In chunked.cpp, CDAT chunks + manifest ride BlobWrite; rm_chunked's per-chunk + manifest tombstones retain Delete.

## Accomplishments

- **commands.cpp:** All 9 RESEARCH Q1 sites migrated; `submit_name_blob` and `submit_bomb_blob` helpers shrink to inline `build_owned_blob` + `encode_blob_write_body` calls; `cmd::put` pipeline loop unchanged in flow but blob construction now one helper call; `cmd::reshare` uses `new_ns_blob` / `del_ns_blob` pair for the put-then-delete double-write; `cmd::delegate` / `cmd::revoke` loop bodies now 2 lines of construction each.
- **chunked.cpp:** `build_cdat_blob_flatbuf` shrinks from 18 lines to 8 (kept the CDAT-magic prefix-concat); `build_tombstone_flatbuf` shrinks from 13 lines to 5; manifest emit in `put_chunked` collapses the 10-line `BlobData mb{}` + `encode_blob(mb)` block to a 2-liner; all retry / recover paths inherit the new send semantics unchanged.
- **Zero TEMP-124 markers** across `cli/` — plan 01 handoff debt fully retired.
- **cdb binary builds cleanly** — phase 124's midpoint goal reached: CLI emits the post-122 wire format on every path.

## Task Commits

| # | Hash       | Type       | Subject                                                                          |
|---|------------|------------|----------------------------------------------------------------------------------|
| 1 | `03a9912b` | refactor   | `refactor(124-03): migrate 9 commands.cpp blob-construction sites to build_owned_blob` |
| 2 | `9293ae6e` | refactor   | `refactor(124-03): migrate 3 chunked.cpp blob-construction sites to build_owned_blob`  |

Plan metadata (summary + state + roadmap) will be committed as a single follow-up per `<final_commit>` protocol.

## Grep Verification (Phase-Wide Invariants)

```
$ grep -c "TEMP-124" cli/src/commands.cpp
0
$ grep -c "TEMP-124" cli/src/chunked.cpp
0
$ grep -rn "TEMP-124" cli/
(no output — 0 matches)

$ grep -cE "blob\.namespace_id\s*=|blob\.namespace_id\.data\(\)" cli/src/commands.cpp
0
$ grep -cE "blob\.pubkey\.assign|blob\.pubkey\.data\(\)"         cli/src/commands.cpp
0
$ grep -cE "blob\.namespace_id\s*=|blob\.namespace_id\.data\(\)" cli/src/chunked.cpp
0
$ grep -cE "blob\.pubkey\.assign|blob\.pubkey\.data\(\)"         cli/src/chunked.cpp
0
$ grep -rnE "blob\.namespace_id\s*=|blob\.pubkey\.assign" cli/src/
(no output — phase-wide zero)

$ grep -cE "MsgType::Data" cli/src/commands.cpp
0
$ grep -cE "MsgType::Data" cli/src/chunked.cpp
0
$ grep -rn "MsgType::Data" cli/src/
(no output — phase-wide zero)

$ grep -c "build_owned_blob"        cli/src/commands.cpp
9
$ grep -c "encode_blob_write_body"  cli/src/commands.cpp
9
$ grep -c "build_owned_blob"        cli/src/chunked.cpp
3
$ grep -c "encode_blob_write_body"  cli/src/chunked.cpp
3

$ grep -cE "MsgType::BlobWrite" cli/src/commands.cpp
5     # sites 1, 3, 5, 7, 9
$ grep -cE "MsgType::Delete"    cli/src/commands.cpp
8     # 4 sends (sites 2, 4, 6, 8) + 4 DeleteAck refs
$ grep -cE "MsgType::BlobWrite" cli/src/chunked.cpp
4     # 3 sends (CDAT phase A, retry, manifest) + 1 comment banner
$ grep -cE "MsgType::Delete"    cli/src/chunked.cpp
5     # 2 sends (rm_chunked per-chunk + manifest) + 3 DeleteAck refs + comment
```

## Test Suite Results

```
$ cmake --build build/cli/tests -j$(nproc) --target cli_tests
…
[100%] Built target cli_tests

$ ./build/cli/tests/cli_tests "[wire]"   | tail -1
All tests passed (568 assertions in 23 test cases)

$ ./build/cli/tests/cli_tests "[pubk]"   | tail -1
All tests passed (49 assertions in 7 test cases)

$ ./build/cli/tests/cli_tests "[chunked]" | tail -1
All tests passed (196829 assertions in 28 test cases)

$ ./build/cli/tests/cli_tests             | tail -1
All tests passed (197595 assertions in 95 test cases)

$ cmake --build build/cli -j$(nproc) --target cdb
…
[100%] Built target cdb
```

No regressions. Same exact test counts as plan 02 handoff (95 cases, 197595 assertions), because plan 03 is a pure refactor — no test changes, no behavior changes, just the wire format bytes that `cli_tests` doesn't exercise against a real node.

## Deviations from Plan

None. Plan 03's tasks were mechanical refactor specs; every site was migrated exactly as the BEFORE→AFTER snippets in PATTERNS Shared Pattern A prescribed, and every MsgType preserved per RESEARCH Q3's binding rule. No Rule 1/2/3 auto-fixes were needed — the post-plan-01 TEMP-124 stubs compiled cleanly, so the migration could proceed site-by-site without intermediate repair work. No Rule 4 architectural decisions were required.

## Authentication Gates

None. Plan 03 is build-only; no network interaction, no identity management beyond what already runs in the unit test harness.

## Decisions Made

- **MsgType binding per RESEARCH Q3 verbatim.** Not reinterpreted. The decision was "decided once, applied twelve times":
  - OWNER writes (NAME blob, CENV user data, new-CENV in reshare, DLGT delegation, PUBK publish, CDAT chunks, CPAR manifest) → `MsgType::BlobWrite`.
  - TOMBSTONES (single-target `cmd::rm`, BOMB batched tombstone, reshare's old-blob tombstone, revoke's delegation tombstone, rm_chunked per-chunk + manifest tombstones) → `MsgType::Delete`.
- **Helper return-type semantics documented inline.** `build_cdat_blob_flatbuf` and `build_tombstone_flatbuf` keep `std::vector<uint8_t>` return types — callers didn't need to change — but the comment above each now reads "Post-124: returns BlobWriteBody envelope bytes". Documented without breaking the caller contract.
- **`cmd::publish` migrated mechanically like every other site.** The plan explicitly called out that `cmd::publish` is the PUBK writer and gets its auto-PUBK bypass in plan 04 — not here. Plan 03 only did the blob-construction migration. Plan 04 will add the successful-WriteAck cache-seed for the `pubk_cache` that `ensure_pubk` consults.
- **Retained both retry-path BlobWrite calls in chunked.cpp's `put_chunked` Phase A loop.** The retry after a failed send_async re-runs `build_cdat_blob_flatbuf` (which regenerates a fresh ML-DSA-87 signature — they're non-deterministic) and re-sends under `MsgType::BlobWrite`. Same mechanism as before, just the helper return is now a BlobWriteBody envelope.

## Issues Encountered

- A leftover `/tmp/chunked_overwrite_guard_*` fixture file from an earlier test run caused an assertion in the first attempt at the full suite (harmless — actually the assertion is a test case checking the CLI refuses to overwrite without `--force`). Cleaned with `rm -f /tmp/chunked_overwrite_guard_*` before the clean run. Not a regression — same state as the plan 02 SUMMARY noted.

## D-XX Traceability

- **D-03 (central helper consumption):** 12 sites now call `build_owned_blob` — one per site, no copy-pasted sign-and-populate sequence anywhere in the file tree.
- **D-03b (FlatBuffer emission):** `encode_blob_write_body` is the only path that produces bytes for `conn.send*` on a blob-write path. `encode_blob` remains exported in wire.cpp (for symmetry with `decode_blob` which parses `ReadResponse` payloads) but is never called from the new send paths — plan 03 leaves it untouched per plan spec.
- **D-04 (BlobWrite envelope at all writes):** Every owner-write site emits `MsgType::BlobWrite=64`; the grep count is exactly 5 in commands.cpp and 3 in chunked.cpp.
- **D-04a (MsgType::Data=8 fully eliminated):** `grep -rn "MsgType::Data" cli/src/` returns 0 lines phase-wide, closing out T-124-03 (old-format blob acceptance in CLI).
- **D-04b (tombstone envelope unification):** Tombstone paths use the SAME `encode_blob_write_body` envelope — the ONLY distinguishing bit is the `MsgType` byte, which picks `DeleteAck` vs `WriteAck` reply routing on the node side (Q3 evidence).
- **T-124-03 (Tampering — old-format blob):** MITIGATED. Zero `MsgType::Data`, zero `blob.namespace_id`, zero `blob.pubkey.assign` across the CLI. Old format cannot be emitted.
- **T-124-03b (Tampering — residual `encode_blob(blob)` send-path usage):** MITIGATED. `grep -c "encode_blob(" cli/src/commands.cpp` and `cli/src/chunked.cpp` both return 0 post-migration.
- **T-124-site-mix (Confused Deputy — MsgType mix-up):** MITIGATED structurally. Binding decided in RESEARCH Q3, applied verbatim, grep-verified at commit time.
- **T-124-delegate-helper:** Structural mitigation comes from the helper itself (from plan 01) — `build_owned_blob` structurally forces `signer_hint = SHA3(id.signing_pubkey())` regardless of `target_namespace`, so a delegate migration site cannot accidentally spoof the owner's signer_hint. Confirmed by existence of plan 01's `wire: build_owned_blob delegate — signer_hint != target_namespace` TEST_CASE.
- **T-124-chunked-orphan (Availability — orphan chunks):** DEFERRED (per plan). Plan 04's D-06 cascade closes this; plan 03 only changed the wire format of the existing rm_chunked cascade.

## Known Stubs

None introduced by plan 03. All TEMP-124 compile-fix stubs from plan 01 are now replaced with real migrations. No UI-rendering stubs, no placeholder strings, no unwired components introduced.

## Threat Flags

None. No new security-relevant surface introduced beyond the threat register already covered by the plan's `<threat_model>` block. The migration closes T-124-03 / T-124-03b / T-124-site-mix via structural grep invariants that plan 01 + plan 03 together enforce.

## Handoff to Plan 04

Plan 04's scope (per 124-CONTEXT.md D-05/D-06 and 124-PATTERNS.md §commands.cpp Pattern 3/4) is:
1. Wire `ensure_pubk(id, conn, ns_span, rid_counter)` into every owner-write command flow — `cmd::put`, `cmd::delegate`, `cmd::reshare` (on `conn2`), `cmd::rm`, `cmd::rm_batch`, `cmd::revoke`, and `chunked::put_chunked`. Bypass for `cmd::publish` (it IS the PUBK writer; manually cache-seed on WriteAck).
2. Add D-05 `decode_error_response` file-local helper near `find_pubkey_blob` in commands.cpp; route every `ErrorResponse` handling path through it. Never leaks `PUBK_FIRST_VIOLATION`, `PUBK_MISMATCH`, or phase numbers.
3. Add D-06 BOMB cascade in `cmd::rm_batch`: classify each target via ExistsRequest/ReadRequest, collect CPAR manifest chunk hashes, BOMB manifest + chunks together in one batched tombstone.

All plan-03 surfaces are in place to receive these additions without further refactoring.

## Self-Check: PASSED

- **Task 1 commit `03a9912b`:** `git log --oneline | grep -q 03a9912b` → FOUND
- **Task 2 commit `9293ae6e`:** `git log --oneline | grep -q 9293ae6e` → FOUND
- **cli/src/commands.cpp:** FOUND (modified — diff: 30 insertions, 123 deletions)
- **cli/src/chunked.cpp:** FOUND (modified — diff: 12 insertions, 39 deletions)
- **`./build/cli/tests/cli_tests "[wire]"`:** 23 cases, 568 assertions, ALL PASSED
- **`./build/cli/tests/cli_tests "[pubk]"`:** 7 cases, 49 assertions, ALL PASSED
- **`./build/cli/tests/cli_tests "[chunked]"`:** 28 cases, 196829 assertions, ALL PASSED
- **`./build/cli/tests/cli_tests`:** 95 cases, 197595 assertions, ALL PASSED
- **`cdb` binary:** builds cleanly (`cmake --build build/cli -j$(nproc) --target cdb` → exit 0)
- **Phase-wide invariants:**
  - `grep -rn "TEMP-124" cli/` → 0 matches
  - `grep -rnE "blob\.namespace_id\s*=|blob\.pubkey\.assign" cli/src/` → 0 matches
  - `grep -rn "MsgType::Data" cli/src/` → 0 matches

---
*Phase: 124-cli-adaptation-to-new-mvp-protocol*
*Plan: 03*
*Completed: 2026-04-21*
