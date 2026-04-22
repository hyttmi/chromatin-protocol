---
phase: 125-docs-update-for-mvp-protocol
plan: 04
subsystem: infra
tags: [flatbuffers, schema, transport, cli-help, test-migration]

# Dependency graph
requires:
  - phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey
    provides: BlobWriteBody envelope + signer_hint (replaced Data=8 direct-write branch)
  - phase: 124-cli-adaptation-to-new-mvp-protocol
    provides: deferred D-12 string-leak target (cli/src/main.cpp:619) recorded in deferred-items.md
  - phase: 125-docs-update-for-mvp-protocol/plan-01
    provides: PROTOCOL.md row retention policy for Data=8 (marked DELETED at doc level, enum deletion deferred to this plan)
provides:
  - TransportMsgType enum with Data=8 slot deleted
  - flatc-regenerated transport_generated.h (slot 8 now "" in EnumNamesTransportMsgType)
  - cli/src/main.cpp help text free of (Phase N) string literals
  - 16 test sites migrated from TransportMsgType_Data to TransportMsgType_BlobWrite
affects: [125-05-PLAN (comment hygiene — no schema changes mid-flight), external client implementers (type 8 now returns unknown_type 0x02)]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Sparse FlatBuffers enum: leaving a numeric gap (Data=8 removed, SyncRequest=9 retained) is idiomatic — explicit numerics tolerate non-contiguous values. flatc emits empty string ('') for the gap slot in EnumNamesTransportMsgType."
    - "Test-site migration pattern: when a test uses an enum value as a generic filler for codec/framing exercises (no dispatcher-branch assertions), migrate to a live enum value rather than delete the test. Preserves regression coverage without adding branch-specific assumptions."

key-files:
  created: []
  modified:
    - cli/src/main.cpp
    - db/schemas/transport.fbs
    - db/wire/transport_generated.h
    - db/peer/message_dispatcher.cpp
    - db/tests/net/test_connection.cpp
    - db/tests/net/test_framing.cpp
    - db/tests/net/test_protocol.cpp

key-decisions:
  - "All 16 TransportMsgType_Data test sites are generic codec/connection exercises (framing round-trip, AEAD send/recv, send-queue ordering, nonce exhaustion). None assert on dispatcher-branch routing of type=8. Migrate-all, delete-none."
  - "Use flatc --cpp --gen-object-api to match the existing CMake invocation (db/CMakeLists.txt:130,142). Omitting --gen-object-api would drop TransportMessageT/BlobWriteBodyT object-API structs and break the build."
  - "Preserve surrounding Phase-122-D-07/D-08 reference in message_dispatcher.cpp comment after removing the Data=DELETED memo line — that's Plan 5's D-13 scope (strip // Phase N breadcrumbs), not Plan 4's D-11 scope (remove the now-orphaned memo only)."
  - "Rename SECTION label 'Data round-trips with large payload' -> 'BlobWrite round-trips with large payload' in test_protocol.cpp — the section name is test-discovery-visible and keeping 'Data' as a section label would falsely imply the removed enum value is still under test."

patterns-established:
  - "flatc regen discipline: schema change -> flatc --cpp --gen-object-api -> commit the regenerated header. Never hand-edit db/wire/transport_generated.h. The regen is reproducible; hand-edits are not."
  - "Post-edit test binary rebuild is the regression gate for schema changes — compile success + baseline-match assertion count confirms the change propagated safely through all consumers."

requirements-completed: [DOCS-01, DOCS-02]

# Metrics
duration: 13min
completed: 2026-04-22
---

# Phase 125 Plan 04: Pre-122 Vestige Cleanup Summary

**Deleted `TransportMsgType_Data = 8` from the schema, regenerated the FlatBuffers header via flatc, migrated 16 test sites to `TransportMsgType_BlobWrite`, stripped the `(Phase 123)` token from `cdb rm` help text, and scanned source for lingering pre-122 field references — all tests green at baseline.**

## Performance

- **Duration:** 13 min
- **Started:** 2026-04-22T03:24:55Z
- **Completed:** 2026-04-22T03:38:26Z
- **Tasks:** 3 (D-12, D-11, D-10)
- **Files modified:** 7

## Accomplishments

- **D-12 (Task 1):** `cli/src/main.cpp:619` user-visible help text no longer contains `(Phase 123)` — the only pre-existing phase-number leak in user strings, deferred from Phase 124.
- **D-11 (Task 2):** `TransportMsgType_Data = 8` deleted from `db/schemas/transport.fbs`; `db/wire/transport_generated.h` regenerated via `flatc --cpp --gen-object-api` (not hand-edited); all 16 test-site references migrated to `TransportMsgType_BlobWrite`; stale dispatcher-memo line at `db/peer/message_dispatcher.cpp:1388` removed.
- **D-10 (Task 3):** Source-tree scan for pre-122 field references (`BlobData.namespace_id`, `BlobData.pubkey`, old signing-input shape, `MsgType::Data`) returned zero hits across all `*.cpp`/`*.h` files in `cli/src/` and `db/`. No fixes needed — the scan is the safety-net confirmation that Phases 122+123+124 left no C++ source-level residue.
- **Regression gate:** Full CLI suite green (197614 assertions / 98 cases — baseline match). Full node `[peer]` suite green (506 / 77 cases — baseline match). Zero regressions from the schema change.

## Task Commits

Each task was committed atomically:

1. **Task 1: D-12 — strip `(Phase 123)` from cdb rm help text** — `29d7b6a2` (fix)
2. **Task 2: D-11 — delete Data=8 schema + regen + test migration + memo removal** — `aa9f4400` (refactor)
3. **Task 3: D-10 — scan for pre-122 field references** — no source changes (scan returned zero hits; verification recorded in this summary)

**Plan metadata commit:** pending (will be `docs(125-04): plan summary` + state advance)

## Files Created/Modified

- `cli/src/main.cpp` — 1 line: removed `(Phase 123)` token from cdb rm help text
- `db/schemas/transport.fbs` — 1 line: removed `Data = 8,` row from `TransportMsgType` enum (sparse numeric gap now exists at slot 8)
- `db/wire/transport_generated.h` — flatc-regenerated; 6-line diff showing only the expected three spots (enum row, `EnumValuesTransportMsgType[65]` → `[64]`, `EnumNamesTransportMsgType` slot 8 `"Data"` → `""`). No hand edits.
- `db/peer/message_dispatcher.cpp` — 1 line: removed the stale `// ... TransportMsgType_Data direct-write branch was DELETED — no backward compat.` comment (the enum value it memoed is now gone, so the comment has no referent)
- `db/tests/net/test_connection.cpp` — 11 sites migrated to `TransportMsgType_BlobWrite` (send/recv exercises, send-queue ordering, nonce-exhaustion test, lightweight handshake integration)
- `db/tests/net/test_framing.cpp` — 2 sites migrated (codec integration round-trip test)
- `db/tests/net/test_protocol.cpp` — 3 sites migrated (codec round-trip, truncated-buffer rejection); SECTION label renamed `"Data round-trips with large payload"` → `"BlobWrite round-trips with large payload"` to keep test discovery output consistent with the current wire vocabulary

## D-11 Test-Site Inventory (16 sites, all migrate)

Per-site decision log. Per-site rule from the plan: migrate if the test uses `TransportMsgType_Data` as a filler for codec/framing/send-queue behavior; delete only if the test specifically asserts dispatcher-branch routing of type=8. All 16 sites fell into the first category.

| File | Line | Context | Decision | Rationale |
|------|------|---------|----------|-----------|
| db/tests/net/test_connection.cpp | 55 | `on_message` callback: `if (type == TransportMsgType_Data) ...` | migrate | Callback filters incoming message type; Data was filler. BlobWrite works identically. |
| db/tests/net/test_connection.cpp | 166 | Same pattern (handshake-integration test) | migrate | Same as above. |
| db/tests/net/test_connection.cpp | 200 | `init_conn->send_message(TransportMsgType_Data, payload)` | migrate | Encrypted send-path exercise; type is framing filler. |
| db/tests/net/test_connection.cpp | 308 | `on_message` filter (lightweight handshake test) | migrate | Same as 55. |
| db/tests/net/test_connection.cpp | 340 | `send_message(TransportMsgType_Data, ...)` (lightweight path) | migrate | Same as 200. |
| db/tests/net/test_connection.cpp | 498 | `on_message` filter (send-queue multi-concurrent test) | migrate | Counts received messages for NUM_MESSAGES assertion. |
| db/tests/net/test_connection.cpp | 524 | `conn->send_message(TransportMsgType_Data, payload)` in loop | migrate | Multi-concurrent send exercise. |
| db/tests/net/test_connection.cpp | 582 | `send_message` after `conn->close()` — expects `false` | migrate | Post-close send failure test; type is filler. |
| db/tests/net/test_connection.cpp | 646 | `send_message` after Ping/Pong (send-queue ordering test) | migrate | Verifies Pong went through queue without nonce desync. |
| db/tests/net/test_connection.cpp | 930 | `send_message` before counter forced to exhaustion | migrate | Normal-send baseline in nonce-exhaustion test. |
| db/tests/net/test_connection.cpp | 938 | `send_message` after counter forced to `1ULL << 63` | migrate | Nonce-exhaustion failure assertion. |
| db/tests/net/test_framing.cpp | 222 | `TransportCodec::encode(TransportMsgType_Data, payload)` | migrate | Codec + frame round-trip integration test. |
| db/tests/net/test_framing.cpp | 234 | `REQUIRE(decoded->type == TransportMsgType_Data)` | migrate | Paired with line 222; same exercise. |
| db/tests/net/test_protocol.cpp | 73 | Codec large-payload (64 KB) encode | migrate | Codec round-trip exercise. |
| db/tests/net/test_protocol.cpp | 76 | Paired decode-type assertion for line 73 | migrate | Same exercise. |
| db/tests/net/test_protocol.cpp | 265 | Encode inside "truncated buffer" decode-rejection test | migrate | Any valid type works for the truncation exercise. |

**Total:** 16 sites migrated, 0 deleted. Case count preserved (98 CLI / 77 peer baseline).

## D-11 flatc Regeneration Diff

`git diff --stat db/wire/transport_generated.h` after the regen:

```
 db/wire/transport_generated.h | 6 ++----
 1 file changed, 2 insertions(+), 4 deletions(-)
```

The diff shape (verified via `git diff db/wire/transport_generated.h`):
1. Enum row removed: `-  TransportMsgType_Data = 8,`
2. Array length change: `-inline const TransportMsgType (&EnumValuesTransportMsgType())[65] {` → `+...[64] {`
3. Value list entry removed: `-    TransportMsgType_Data,`
4. Name table slot blanked: `-    "Data",` → `+    "",`

All other file content is byte-identical to the previous generated header. This confirms the regen is mechanical and the schema change propagated cleanly.

**flatc command used (matches db/CMakeLists.txt:130,142):**
```
./build/_deps/flatbuffers-build/flatc --cpp --gen-object-api -o db/wire db/schemas/transport.fbs
```

## D-10 Scan Findings (Task 3)

Four patterns scanned across `cli/src/` + `db/` (all `*.cpp`/`*.h` files):

| Pattern | Hits in source | Disposition |
|---------|---------------|-------------|
| `BlobData\.namespace_id\b` | 0 | Clean — the rename completed in Phase 122. The only comment that still references `namespace_id` (at `cli/src/wire.h:158-165`) correctly uses the current `target_namespace` parameter name and documents the pre-rename byte-identity invariant. |
| `BlobData\.pubkey\b` OR `per-blob pubkey` OR `2592-byte embedded` | 0 | Clean — the embedded per-blob pubkey was replaced by `signer_hint` in Phase 122 and no comment-level residue remains. |
| `namespace_id \|\| data \|\| ttl \|\| timestamp` (old signing shape) | 0 | Clean — all occurrences are in `.planning/` historical artifacts only, not in shipping source. |
| `MsgType::Data` OR `type = Data` OR `Data = 8` | 0 in source | Clean — all occurrences are in `.planning/` historical artifacts, `db/PROTOCOL.md:310` (which documents the deletion for external implementers — intentional), and this summary. |

**Outcome:** No source edits required for Task 3. The research §3c hypothesis ("the only D-10 residue is a documentation-style comment at `wire.h:158-165` whose phase-prefix is Plan 5's D-13 scope") held exactly — Plan 4 had no surprise fix work.

Legitimate storage-key names (`namespace_id` used as a variable or DBI column name) and the `Identity::namespace_id()` method are not touched — those are the current API surface, not pre-122 residue. Per the plan's D-10 scope note: "the method `namespace_id()` is fine."

## Decisions Made

- **Migrate-all for Task 2 Step D:** Every one of the 16 test sites proved to be a generic codec/framing exercise. None asserted on dispatcher-branch routing of type=8 (which would have been dead-code coverage by definition, since the Data=8 dispatcher branch was removed in Phase 122). No test cases deleted; assertion-count baseline preserved exactly.
- **`--gen-object-api` preserved in regen invocation:** The CMake build uses `flatc --cpp --gen-object-api` (db/CMakeLists.txt:130 for blob.fbs, :142 for transport.fbs). Regenerating without that flag would drop the `TransportMessageT`/`BlobWriteBodyT` object-API unpacked structs and break the build. Matched the invocation exactly.
- **SECTION label renamed in test_protocol.cpp:** `"Data round-trips with large payload"` → `"BlobWrite round-trips with large payload"`. Section names show up in Catch2's `--list-tests` output and in failure messages. Keeping "Data" as a section label would reintroduce a wire-vocabulary reference to a removed enum value at the test-discovery layer.
- **Preserve `Phase 122 D-07/D-08` in remaining comment at message_dispatcher.cpp:1386:** Plan 4's D-11 scope is the DELETED-memo line only. Plan 5's D-13 sweep will strip `// Phase N` historical breadcrumbs project-wide. Keeping the narrow scope lets Plan 5 do its sweep cleanly without ambiguity about what Plan 4 already touched.

## Deviations from Plan

None — plan executed exactly as written. All three tasks completed in order (D-12 → D-11 → D-10) with no auto-fix deviations, no checkpoint pauses, no authentication gates, and no blocking issues. The research §3c site inventory (11 + 2 + 3 = 16 sites) matched the actual source exactly, and every site fell into the "migrate" category per the per-site decision rule.

## Issues Encountered

None. One minor note: the plan's interfaces block specified `flatc --cpp` but the CMake build uses `flatc --cpp --gen-object-api`. Matching the CMake invocation (decision recorded above) was the correct call; using the plan's reduced flag set would have broken the object-API unpacked structs.

## Test Run Results

Full Catch2 suite executed locally (infra reachable per feedback_self_verify_checkpoints.md — project laptop runs both test binaries natively; no delegation necessary).

**CLI tests** (`./cli/build/tests/cli_tests --reporter compact`):
```
RNG seed: 3560404285
All tests passed (197614 assertions in 98 test cases)
```
Baseline: ≥ 197614 / 98. Result: **exact match, zero regressions.**

**Node `[peer]` tests** (`./build/db/db/chromatindb_tests "[peer]" --reporter compact`):
```
All tests passed (506 assertions in 77 test cases)
```
Baseline: ≥ 506 / 77. Result: **exact match, zero regressions.**

Both test binaries rebuilt from source after the schema edits (`cmake --build cli/build --target cli_tests -j12` and `cmake --build build/db/db --target chromatindb_tests -j12`) — zero compile errors, zero warnings introduced. The flatc regeneration propagated cleanly through `chromatindb_lib` and all three `db/tests/net/` translation units that reference `TransportMsgType_BlobWrite`.

## User Setup Required

None — no external service configuration, no binary re-deploy needed. The schema change is internal (CLI and node still exchange `BlobWrite=64` for owner writes and `Delete=17` for tombstones; the removed `Data=8` had no live emitters).

## Next Phase Readiness

- Plan 125-05 ready to execute — its scope (D-13 strip `// Phase N` breadcrumbs, D-14 over-comment prune) now runs on a source tree that has no structural pre-122 residue. The comment-hygiene pass will touch surface-level text only, not wire-format or test infrastructure.
- External client implementers (future): `TransportMsgType = 8` is now a schema-level hole. A peer that sends `type=8` will be routed to the generic unknown-type error path (`ErrorResponse` with code `unknown_type = 0x02`) — the behavior documented in `db/PROTOCOL.md:310` is preserved even though the enum label no longer exists.

## Self-Check: PASSED

Verified all claims:

**Commit hashes exist:**
- `git log --oneline --all | grep -q "29d7b6a2"` → FOUND (Task 1 D-12)
- `git log --oneline --all | grep -q "aa9f4400"` → FOUND (Task 2 D-11)

**Files modified exist:**
- `cli/src/main.cpp` — FOUND
- `db/schemas/transport.fbs` — FOUND
- `db/wire/transport_generated.h` — FOUND
- `db/peer/message_dispatcher.cpp` — FOUND
- `db/tests/net/test_connection.cpp` — FOUND
- `db/tests/net/test_framing.cpp` — FOUND
- `db/tests/net/test_protocol.cpp` — FOUND

**Verify gates pass (from plan):**
- D-12: no `(Phase N)` string literals in `cli/src/` — PASS
- D-11 schema: no `Data = 8` in transport.fbs — PASS
- D-11 generated header: no `TransportMsgType_Data` in transport_generated.h — PASS
- D-11 memo: no "TransportMsgType_Data direct-write branch was DELETED" in message_dispatcher.cpp — PASS
- D-11 tests: no `TransportMsgType_Data` in db/tests/net/ — PASS
- D-10 scan: zero hits for all four patterns across cli/src/ + db/ `*.cpp`/`*.h` — PASS

**Test baselines met:**
- cli_tests: 197614 / 98 (≥ 197614 / 98) — PASS
- chromatindb_tests [peer]: 506 / 77 (≥ 506 / 77) — PASS

---

*Phase: 125-docs-update-for-mvp-protocol*
*Completed: 2026-04-22*
