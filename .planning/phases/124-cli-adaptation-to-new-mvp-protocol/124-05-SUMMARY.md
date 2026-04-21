---
phase: 124-cli-adaptation-to-new-mvp-protocol
plan: 05
subsystem: cli
tags: [cdb, bomb, blobwrite, error-decoder, e2e, delegate, rule-1-fix, rule-2-fix]

requires:
  - phase: 124-cli-adaptation-to-new-mvp-protocol plan 04
    provides: ensure_pubk wired everywhere, D-05 error decoder exported, D-06 cascade in cmd::rm_batch
  - phase: 123-tombstone-batching-and-name-tagged-overwrite
    provides: BOMB semantics, NAME pointer blobs, --replace
  - phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey
    provides: post-122 Blob schema, BlobWrite=64 envelope, signer_hint model
provides:
  - Live-verified D-08 E2E matrix on local node (items 1, 4, 6, 7; 0x07 trigger; full unit suite green)
  - Rule-1 fix: BOMB submission routed via BlobWrite=64 (was Delete=17, rejected by node)
  - Rule-2 fix: `cdb ls --type BOMB` and `ls --type NAME` now recognised
  - Rule-2 fix: opts.host threaded into submit_bomb_blob so D-05 wording names real host
  - New file cli/src/error_decoder.cpp (TU extraction for testability)
  - New [error_decoder] TEST_CASE (7 assertions, codes 0x07-0x0B literal-equality)
  - 124-E2E.md D-08 execution log with per-item verdicts + Phase Gate FAIL
affects: [Phase 124 completion (blocked by home redeploy + D-02 scope call), Phase 125 docs rollup]

tech-stack:
  added: []
  patterns: ["TU extraction for test linkage (error_decoder.cpp) — mirrors Plan 04's commands_internal.h approach"]

key-files:
  created:
    - cli/src/error_decoder.cpp
    - .planning/phases/124-cli-adaptation-to-new-mvp-protocol/124-E2E.md
    - .planning/phases/124-cli-adaptation-to-new-mvp-protocol/124-05-SUMMARY.md
  modified:
    - cli/src/commands.cpp (Rule-1 + Rule-2 fixes + decoder extraction)
    - cli/src/main.cpp (ls --type help strings updated for NAME/BOMB)
    - cli/CMakeLists.txt (add error_decoder.cpp to cdb target)
    - cli/tests/CMakeLists.txt (add error_decoder.cpp to cli_tests)
    - cli/tests/test_wire.cpp (new [error_decoder] TEST_CASE)

key-decisions:
  - "Rule-1 bug fix mid-plan: submit_bomb_blob was sending BOMBs via MsgType::Delete=17. The node's Delete handler routes to engine.delete_blob() which only accepts 36-byte tombstone format. BOMBs (8+32N bytes) were being rejected with 'delete request data must be tombstone format'. Changed to MsgType::BlobWrite=64 + WriteAck — engine.ingest() recognises BOMB magic via the standard ingest path. Pre-existing Phase 123 latent defect; never caught because earlier phases had no live-E2E."
  - "Rule-2 UX fix: `cdb ls --type BOMB` and `ls --type NAME` were unrecognised. Node's ListRequest type_filter already honours both magics. Added to the CLI filter switch + help strings."
  - "Extracted decode_error_response to cli/src/error_decoder.cpp so cli_tests can link it without pulling in commands.cpp's asio/spdlog/json dependencies. Pure linkage refactor — production binary unchanged."
  - "Phase Gate verdict: FAIL. Two blockers: (B1) home node at 192.168.1.73 is running 2.3.0-gf038faee — 23 commits pre-Phase-122 — despite user's Task-1 approval; (B2) `cdb put --share @contact` does not re-target to a foreign namespace, blocking plan Task 5's required-minimum delegate scenario."
  - "Item 6 same-second tiebreak hazard surfaced: cdb put --name --replace within a single second can lose the blob_hash DESC tiebreak against the older NAME blob, causing `cdb get <name>` to resolve to the tombstoned prior content. Not a Phase 124 defect — Phase 123 D-15 design; flagged for Phase 125+ follow-up with three repair options documented in the E2E log."

patterns-established:
  - "E2E execution under a partially-available test matrix: document single-node PASS verdicts alongside cross-node BLOCKED verdicts cleanly so a follow-up run can complete without re-executing the local-only items."
  - "Live error-string capture pattern: trigger node-side rejection (0x07) via `cdb rm --namespace <random-fresh> <random> --force -y`; the unregistered namespace reliably produces PUBK_FIRST_VIOLATION without needing two identities."

requirements-completed: [SC-124-5, SC-124-6]

duration: ~90min
completed: 2026-04-21
---

# Phase 124 Plan 05: D-08 E2E Matrix Execution — SUMMARY

**Live-verified the post-124 CLI end-to-end against the local chromatindb node; uncovered and fixed two CLI bugs mid-plan (Rule-1 BOMB-routing + Rule-2 ls type-filter); documented a Phase Gate FAIL blocked on home-node redeployment and a CLI design constraint for the delegate scenario.**

## Performance

- **Duration:** ~90 minutes (mid-plan rebuilds included)
- **Started:** 2026-04-21 ~13:10 local
- **Completed:** 2026-04-21 ~14:40 local
- **Tasks:** 6/7 (Task 1 was user-owned redeployment, approved at start; Tasks 2-7 executed)
- **Commits:** 5 (this plan)

## Accomplishments

### E2E matrix results (D-08 items 1-7)

| Item | local | home | Notes |
|------|-------|------|-------|
| 1 — SC#7 literal flow | PASS* | FAIL# | *post Rule-1 BOMB-via-BlobWrite fix; #stale home binary |
| 2 — cross-node sync | n/a | BLOCKED | Home binary stale |
| 3 — BOMB propagation | n/a | BLOCKED | Home binary stale |
| 4 — chunked >500 MiB | PASS | BLOCKED | 750 MiB local roundtrip; hash-identical |
| 5 — delegate `--share` | n/a | BLOCKED | CLI design constraint + stale home |
| 6 — `--replace` BOMB-of-1 | PASS‡ | BLOCKED | ‡same-second tiebreak hazard flagged |
| 7 — D-06 cascade | PASS | BLOCKED | 48-target BOMB (1 manifest + 47 chunks) |
| — Live 0x07 trigger | PASS | n/a | D-05 string verbatim from decoder |
| — `[error_decoder]` unit TEST_CASE | PASS | n/a | 7 assertions; codes 0x07-0x0B |
| — Full CLI test suite | PASS | n/a | 98 cases / 197614 assertions |

### Code changes (all CLI-side; no protocol/schema changes)

1. **cli/src/commands.cpp `submit_bomb_blob`** — Rule-1 auto-fix:
   - Changed MsgType from `Delete=17` → `BlobWrite=64`. BOMBs are structural
     blobs (8+32N data bytes), not tombstones (36 bytes); the Delete handler
     enforced tombstone format and rejected all BOMB submissions.
   - Added ErrorResponse decode + stderr print (previously the node's
     rejection reason was silently dropped).
   - Subsequent refinement: threaded `opts.host` through as default-arg so
     the D-05 wording names `127.0.0.1` rather than the placeholder "node".

2. **cli/src/commands.cpp `cmd::ls` + cli/src/main.cpp** — Rule-2 auto-fix:
   - Added `NAME` and `BOMB` to the recognised `--type` filter switch and
     to the help strings in both `cdb ls --help` and the inline error
     string. Node side already honours the magics (see
     `db/tests/peer/test_list_by_magic.cpp`).

3. **cli/src/error_decoder.cpp (new)** — TU extraction:
   - Moved `decode_error_response` body out of commands.cpp into its own
     translation unit so cli_tests (which does not link commands.cpp) can
     call it. Declaration stays in commands_internal.h. Production
     behaviour is byte-identical.

4. **cli/tests/test_wire.cpp [error_decoder] TEST_CASE** — SC-124-7 gate:
   - 7 literal-equality REQUIRE assertions: 5 for codes 0x07/0x08/0x09/0x0A/0x0B
     against the D-05 expected strings, plus 2 defensive branches (short-read,
     unknown-code). Lowercase hex tiebreak verified (`abababababababab`).

### Live-triggered 0x07 (SC-124-7 partial)

Triggered `ERROR_PUBK_FIRST_VIOLATION` on the local node by running
`cdb rm --namespace <random-fresh-ns> <random-hash> --force -y`. Captured
exact D-05 wording:

> `Error: namespace not yet initialized on node 127.0.0.1. Auto-PUBK failed; try running 'cdb publish' first.`

Byte-identical to the `[error_decoder]` TEST_CASE's REQUIRE literal (modulo
the live-triggered host name being `127.0.0.1` vs the unit test's
`192.168.1.73`). No phase-number / internal-token leak. Node journalctl
corroborated with `Ingest rejected: PUBK-first violation (ns 9bbe... has no
registered owner)`.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 — Bug] BOMB submission went to MsgType::Delete instead of MsgType::BlobWrite**

- **Found during:** Task 2 (Item 1 first attempt, batched rm)
- **Issue:** `submit_bomb_blob` in cli/src/commands.cpp built a BOMB blob
  (data = [BOMB_MAGIC:4][count:4BE][hash:32]*N, 8+32N bytes) and sent it via
  `MsgType::Delete=17`. Node's Delete handler routes to `engine.delete_blob()`
  which only accepts 36-byte tombstone format; every BOMB was rejected with
  "delete request data must be tombstone format" (captured in node journalctl).
  The CLI's error path silently dropped the rejection detail and just printed
  "Error: BOMB submission failed". Pre-existing latent bug from Phase 123-03
  when `submit_bomb_blob` was introduced; never caught because no live-E2E
  ran before Phase 124 Plan 05.
- **Fix:** Changed MsgType to `BlobWrite=64`, which routes to
  `engine.ingest()`. That path dispatches on the 4-byte magic and recognises
  BOMB as a batch-tombstone type (see
  `db/peer/message_dispatcher.cpp:1472-1486` — the node already emits
  ERROR_BOMB_TTL_NONZERO/BOMB_MALFORMED/BOMB_DELEGATE_NOT_ALLOWED from the
  BlobWrite path). Also added ErrorResponse decoding inside
  `submit_bomb_blob` so callers see the node's reason rather than a
  generic failure.
- **Files modified:** cli/src/commands.cpp
- **Commit:** 5d337da0
- **Follow-up Rule-2 refinement (commit a6b282dd):** threaded `opts.host`
  through `submit_bomb_blob` as a default-arg parameter so the D-05 error
  wording names the real host rather than the literal string "node".

**2. [Rule 2 — Missing critical functionality] `ls --type BOMB` and `ls --type NAME` unrecognised**

- **Found during:** Task 2 (Item 1 first attempt, BOMB count verification)
- **Issue:** `cdb ls --type BOMB` and `cdb ls --type NAME` errored with
  "Error: unknown type 'BOMB'. Known: CENV, PUBK, TOMB, DLGT, CDAT, CPAR".
  The node's ListRequest type_filter already honours both 4-byte magics
  (see `db/tests/peer/test_list_by_magic.cpp`). Without filter support, the
  D-08 matrix cells that read "`ls --type BOMB` shows N entry" cannot be
  verified from the CLI side.
- **Fix:** Added `NAME` → `NAME_MAGIC_CLI` and `BOMB` → `BOMB_MAGIC_CLI`
  branches to the type-filter switch in cmd::ls, plus updated the help
  strings in main.cpp.
- **Files modified:** cli/src/commands.cpp, cli/src/main.cpp
- **Commit:** 5d337da0 (same commit as Rule-1 fix above)

### Findings / hazards flagged (no fix in scope)

**3. [Flag — Phase 123 D-15 hazard] Same-second `--replace` tiebreak**

- **Found during:** Task 6 (Item 6 first attempt)
- **Issue:** `cdb put --name X` immediately followed by `cdb put --name X
  --replace` emits two NAME blobs with identical timestamps (1-second
  resolution). `pick_name_winner` tiebreaks on NAME-blob-hash DESC; when the
  older NAME's hash happens to be larger, the RESOLUTION picks the older
  pointer → which now targets the tombstoned content → `cdb get <name>`
  returns "blob not found". Reproducible with no sleep; fixed with 2-second
  sleep between the two puts.
- **Scope:** NOT a Phase 124 defect — Phase 123 D-15 design. Flagged in
  124-E2E.md §"Run 6a" for Phase 125 follow-up with three repair options:
  (a) bump NAME timestamp to `max(seen_latest + 1, now)` in `--replace`
  path; (b) tiebreak on `(timestamp DESC, target_hash DESC)` instead of
  NAME-blob-hash; (c) document the 1-second granularity contract in
  PROTOCOL.md.
- **Files modified:** none (flagged, not fixed)

### Architectural blockers surfaced (Rule 4 — user decision required)

**4. [Rule 4 — Architectural] Home node at 192.168.1.73 is on pre-Phase-122 binary**

- **Found during:** Task 2 (Item 1 sanity gate on home)
- **Issue:** Despite user's Task-1 approval stating "home node at
  192.168.1.73 pulled & recompiled", `cdb --node home info` reports version
  `2.3.0-gf038faee` which precedes Phase 122 by 23 commits. The post-124
  CLI cannot speak to a pre-122 node (`BlobWrite=64` + post-122 Blob schema
  unsupported). `cdb --node home publish` returns "Error: bad response".
- **Decision needed:** User rebuilds `chromatindb` from master (post-commit
  `a6b282dd`) and redeploys to 192.168.1.73 with data dir wiped, then
  restarts the daemon. Items 2, 3, and cross-node halves of 4, 6, 7 can
  then be completed in a ~5-minute follow-up run.
- **Files modified:** none (documented as blocker)

**5. [Rule 4 — Architectural] `cdb put --share @contact` does not re-target to foreign namespace**

- **Found during:** Task 5 (Item 5 delegate scenario preparation)
- **Issue:** The plan Task 5 REQUIRED-MINIMUM scenario presumes a CLI path
  to write to a FOREIGN owner's namespace from a delegate identity. In the
  current CLI, `cdb put --share @contact` only adds extra CENV recipients;
  `target_namespace` is always `SHA3(own_signing_pk)`. This is intentional
  per plan 124 D-02 ("no `--as <owner_ns>` flag; delegate-vs-owner is
  implicit"). Under that design, the CLI cannot produce a real delegate
  write against a live node, so Item 5 cannot execute as written.
- **Decision needed:** Either (a) relax D-02 and add an owner-ns selector
  for delegate writes (architectural change — requires user approval); or
  (b) scope-down SC-124-4's live-E2E half and close on existing unit-test
  coverage (`[pubk]` TEST_CASE #5 in test_auto_pubk.cpp already asserts
  delegate writes skip auto-PUBK; Phase 122-04's node-side tests cover
  delegate ingest).
- **Files modified:** none (documented as blocker)

### Auth gates

None encountered. The live infra at 127.0.0.1 + 192.168.1.73 is PQ-secured
but the handshake is key-material-based (no credential entry); both nodes
accepted the CLI's identity without extra user steps.

## Threat Surface Scan

No new security-relevant surface introduced. Changes are:
- Wire routing correction (BOMB via BlobWrite) — **reduces** mistake surface
  by ending in the correct ingest pipeline.
- Help-string updates for `ls --type`.
- TU extraction for testability (error_decoder.cpp).
- Unit test additions.
- E2E log + plan summary.

## Self-Check: PASSED

**Files claimed created/modified (per above):**

- `cli/src/error_decoder.cpp` — FOUND (57 lines)
- `.planning/phases/124-cli-adaptation-to-new-mvp-protocol/124-E2E.md` — FOUND
- `.planning/phases/124-cli-adaptation-to-new-mvp-protocol/124-05-SUMMARY.md` — this file
- `cli/src/commands.cpp` — modified (see diffs in commits 5d337da0, db7c5283, a6b282dd)
- `cli/src/main.cpp` — modified (commit 5d337da0)
- `cli/CMakeLists.txt` — modified (commit db7c5283)
- `cli/tests/CMakeLists.txt` — modified (commit db7c5283)
- `cli/tests/test_wire.cpp` — modified (commit db7c5283)

**Commits claimed:**

- `5d337da0 fix(124-05): route BOMB submission via BlobWrite + surface ErrorResponse` — FOUND
- `f917075f test(124-05): run Item 1 SC#7 literal flow on both nodes` — FOUND
- `db7c5283 test(124-05): add [error_decoder] TEST_CASE + extract decoder TU` — FOUND
- `a6b282dd fix(124-05): thread opts.host into submit_bomb_blob for D-05 wording` — FOUND
- `88dfbfcc test(124-05): complete D-08 matrix E2E log — Phase Gate FAIL` — FOUND

**Test results:**

- `./build/cli/tests/cli_tests "[error_decoder]"` → **All tests passed (7 assertions in 1 test case)** — FOUND in commit db7c5283
- Full suite `./build/cli/tests/cli_tests` → **All tests passed (197614 assertions in 98 test cases)** — FOUND

## Deferred Issues

None within Plan 05 scope. Two blockers routed upward:

1. Home-node redeployment (infrastructure — user owns per `feedback_delegate_tests_to_user.md`).
2. Plan Task 5 delegate scenario scope-call (user + Phase 125 planner).

One Phase 123 hazard flagged for Phase 125+:

3. NAME-blob tiebreak hazard on `--replace` within a single second.

## Link

- E2E execution log (phase-gate artifact): `./124-E2E.md`
- Phase Gate verdict: **FAIL** (see `## Phase Gate` at bottom of 124-E2E.md)
