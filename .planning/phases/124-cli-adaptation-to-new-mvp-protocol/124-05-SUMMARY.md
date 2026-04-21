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
  - Live-verified D-08 E2E matrix on BOTH local and home nodes (items 1, 2, 3, 4, 6, 7 all PASS; item 5 scope-down)
  - Rule-1 fix: BOMB submission routed via BlobWrite=64 (was Delete=17, rejected by node)
  - Rule-2 fix: `cdb ls --type BOMB` and `ls --type NAME` now recognised
  - Rule-2 fix: opts.host threaded into submit_bomb_blob so D-05 wording names real host
  - New file cli/src/error_decoder.cpp (TU extraction for testability)
  - New [error_decoder] TEST_CASE (7 assertions, codes 0x07-0x0B literal-equality)
  - 124-E2E.md D-08 execution log with per-item verdicts + Phase Gate PASS
  - SC-124-4 live-half scope-down documented in 124-VALIDATION.md
  - Archived per-item stdout/stderr/exit captures in e2e-logs/
affects: [Phase 124 completion, Phase 125 docs rollup]

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
  - "Phase Gate verdict: PASS (post home-daemon restart + SC-124-4 scope-down, 2026-04-21T11:22Z). Resolves both initial blockers: B1 was a false alarm (home binary was always current; the DAEMON PROCESS was stale from 2026-04-20 and `info` was reading the old process image — `Uptime: 19h54m` was the tell; user restarted and rerun of items 2/3/cross-4/cross-6/cross-7 all PASS); B2 scope-reduced to unit-test coverage under D-02's no-`--as` constraint."
  - "Item 6 same-second tiebreak hazard surfaced: cdb put --name --replace within a single second can lose the blob_hash DESC tiebreak against the older NAME blob, causing `cdb get <name>` to resolve to the tombstoned prior content. Not a Phase 124 defect — Phase 123 D-15 design; flagged for Phase 125+ follow-up with three repair options documented in the E2E log."
  - "SC-124-4 live-half scope-down: D-02 rejects `--as <owner_ns>` selector, so CLI has no structural path to emit a delegate write landing under a foreign owner's namespace. Live-E2E half replaced by `[pubk]` TC#5 in test_auto_pubk.cpp (CLI-side D-01a auto-PUBK skip) + phase-122-04 test_ingest_delegate.cpp (node-side dispatch). User-approved 2026-04-21."
  - "Retrospective: when sanity-checking remote daemons post-redeploy, cross-reference Uptime against the rebuild timestamp, not just Version. A stale process on a fresh binary is indistinguishable from a stale binary at the CLI layer — the pre-122 diagnosis of B1 was wrong; the actual problem was the missing systemctl restart step."

patterns-established:
  - "E2E execution under a partially-available test matrix: document single-node PASS verdicts alongside cross-node BLOCKED verdicts cleanly so a follow-up run can complete without re-executing the local-only items."
  - "Live error-string capture pattern: trigger node-side rejection (0x07) via `cdb rm --namespace <random-fresh> <random> --force -y`; the unregistered namespace reliably produces PUBK_FIRST_VIOLATION without needing two identities."

requirements-completed: [SC-124-1, SC-124-2, SC-124-3, SC-124-4, SC-124-5, SC-124-6, SC-124-7]

duration: ~180min (initial + rerun)
completed: 2026-04-21
---

# Phase 124 Plan 05: D-08 E2E Matrix Execution — SUMMARY

**Live-verified the post-124 CLI end-to-end against BOTH local and home chromatindb nodes; uncovered and fixed two CLI bugs mid-plan (Rule-1 BOMB-routing + Rule-2 ls type-filter); closed Phase Gate PASS after home daemon restart and user-approved scope-down of SC-124-4's live-E2E half.**

## Performance

- **Initial run duration:** ~90 minutes (mid-plan rebuilds included)
- **Rerun duration:** ~30 minutes (home-side items 1-4, 6-7)
- **Started:** 2026-04-21 ~13:10 local
- **Rerun started:** 2026-04-21T11:07Z (~14:07 local)
- **Rerun completed:** 2026-04-21T11:22Z (~14:22 local)
- **Completed (final):** 2026-04-21 (rerun + docs commits)
- **Tasks:** 7/7 (Task 1 user-owned redeployment; Tasks 2-7 executed across initial + rerun)
- **Commits:** 5 initial + 4 rerun+docs = 9 total (this plan)

## Accomplishments

### E2E matrix results (D-08 items 1-7, post-rerun)

| Item | local | home | Notes |
|------|-------|------|-------|
| 1 — SC#7 literal flow | PASS* | PASS | *post Rule-1 BOMB-via-BlobWrite fix; home re-ran after daemon restart |
| 2 — cross-node sync | n/a | PASS | Both directions + single-target rm propagation verified within 60s sync window |
| 3 — BOMB propagation | n/a | PASS | Single BOMB count=3, size 104 bytes, replicated local→home; all 3 targets tombstoned on home |
| 4 — chunked >500 MiB | PASS | PASS | 750 MiB roundtrip both single-node (1m07s) and cross-node (3m20s); byte-identical SHA3-256 |
| 5 — delegate `--share` | SCOPE | SCOPE | Scope-down approved 2026-04-21: D-02 no --as selector; substitute = [pubk] TC#5 + phase-122-04 tests |
| 6 — `--replace` BOMB-of-1 | PASS‡ | PASS | ‡same-second tiebreak hazard flagged for Phase 125+; home rerun used 2s gap |
| 7 — D-06 cascade | PASS | PASS | Cross-node: manifest rm on local → 48-target BOMB (1.5K) replicated → all chunks tombstoned on home |
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

### Architectural blockers surfaced during initial run — both RESOLVED on 2026-04-21

**4. [Rule 4 — Architectural] Home daemon process on 192.168.1.73 was stale — RESOLVED (user restart)**

- **Found during:** Task 2 (Item 1 sanity gate on home, initial run)
- **Initial (wrong) diagnosis:** `cdb --node home info` reported version
  `2.3.0-gf038faee` (pre-Phase-122). We concluded the binary on disk was
  stale and asked the user to rebuild + redeploy.
- **Actual cause:** The binary on disk had always been on `g9248f01d`
  (post-124 tip). The systemd daemon unit, however, was started from
  the prior `gf038faee` binary on 2026-04-20 and was never restarted
  after the binary swap. The process image in kernel memory remained on
  the pre-122 code path. `Uptime: 19h54m` was the tell at sanity-check
  time — had we cross-referenced uptime against the rebuild timestamp
  we'd have caught it immediately.
- **Resolution:** User restarted the home daemon on 2026-04-21 UTC.
  New PID 7444, uptime at rerun-start ~4 minutes. Data dir wiped again
  pre-restart. Post-122 PUBK-first invariant verified (peer `.173`'s
  untrusted-ns writes now rejected cleanly).
- **Rerun result:** Items 2, 3, and cross-node halves of 4, 6, 7 all PASS
  (see `124-E2E.md` §"Run 1b-rerun" through §"Run 7b").
- **Process takeaway (documented in key-decisions):** post-redeploy
  sanity checks must cross-reference `Uptime` against rebuild timestamp,
  not just `Version`. A stale process on a fresh binary is
  indistinguishable at the CLI layer.
- **Files modified:** none (infrastructure blocker, user-resolved)

**5. [Rule 4 — Architectural] `cdb put --share @contact` does not re-target to foreign namespace — RESOLVED (scope-down)**

- **Found during:** Task 5 (Item 5 delegate scenario preparation)
- **Issue:** The plan Task 5 REQUIRED-MINIMUM scenario presumes a CLI path
  to write to a FOREIGN owner's namespace from a delegate identity. In the
  current CLI, `cdb put --share @contact` only adds extra CENV recipients;
  `target_namespace` is always `SHA3(own_signing_pk)`. This is intentional
  per plan 124 D-02 ("no `--as <owner_ns>` flag; delegate-vs-owner is
  implicit"). Under that design, the CLI cannot produce a real delegate
  write against a live node, so Item 5 cannot execute as written.
- **Resolution (user decision 2026-04-21):** Option (b) — scope-down to
  unit-test coverage. The substitute evidence retains SC-124-4's
  guarantee:
  - **CLI side:** `cli/tests/test_auto_pubk.cpp` `[pubk]` TEST_CASE #5
    asserts auto-PUBK short-circuits when `target_ns != SHA3(own_sk)`
    (D-01a). The CLI cannot accidentally emit a PUBK against a foreign
    namespace.
  - **Node side:** Phase 122-04's `db/tests/peer/test_ingest_delegate.cpp`
    covers server-side delegate-ingest dispatch against the owner's
    DLGT record.
- **Scope-down recorded in:** `124-VALIDATION.md` §SC-124-4 dedicated
  section, manual-only verifications table, `124-E2E.md` §"Item 5", and
  this summary's key-decisions.
- **Files modified:** `124-VALIDATION.md`, `124-E2E.md`, `124-05-SUMMARY.md`
  (this file) — all docs only.

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
- `.planning/phases/124-cli-adaptation-to-new-mvp-protocol/124-E2E.md` — FOUND (updated with rerun sections 2026-04-21T11:07Z+)
- `.planning/phases/124-cli-adaptation-to-new-mvp-protocol/124-05-SUMMARY.md` — this file
- `.planning/phases/124-cli-adaptation-to-new-mvp-protocol/124-VALIDATION.md` — FOUND (SC-124-4 scope-down appended 2026-04-21)
- `.planning/phases/124-cli-adaptation-to-new-mvp-protocol/e2e-logs/` — FOUND (6 raw capture logs from rerun)
- `cli/src/commands.cpp` — modified (see diffs in commits 5d337da0, db7c5283, a6b282dd)
- `cli/src/main.cpp` — modified (commit 5d337da0)
- `cli/CMakeLists.txt` — modified (commit db7c5283)
- `cli/tests/CMakeLists.txt` — modified (commit db7c5283)
- `cli/tests/test_wire.cpp` — modified (commit db7c5283)

**Commits claimed (initial + rerun):**

Initial run (2026-04-21 morning):
- `5d337da0 fix(124-05): route BOMB submission via BlobWrite + surface ErrorResponse` — FOUND
- `f917075f test(124-05): run Item 1 SC#7 literal flow on both nodes` — FOUND
- `db7c5283 test(124-05): add [error_decoder] TEST_CASE + extract decoder TU` — FOUND
- `a6b282dd fix(124-05): thread opts.host into submit_bomb_blob for D-05 wording` — FOUND
- `88dfbfcc test(124-05): complete D-08 matrix E2E log — Phase Gate FAIL` — FOUND

Rerun + docs (2026-04-21 afternoon):
- `4be4048c test(124-05): rerun home-side E2E items 1-4, 6-7 after home daemon restart` — FOUND
- `6b7f428f docs(124-05): scope-down SC-124-4 live half to unit-test coverage` — FOUND
- `docs(124-05): update E2E phase gate to PASS + retro on false-blocker B1` — pending this commit
- `docs(124): mark plan 05 complete; advance state` — pending final commit

**Test results:**

- `./build/cli/tests/cli_tests "[error_decoder]"` → **All tests passed (7 assertions in 1 test case)** — FOUND in commit db7c5283
- Full suite `./build/cli/tests/cli_tests` → **All tests passed (197614 assertions in 98 test cases)** — FOUND

## Deferred Issues

None within Plan 05 scope. One Phase 123 hazard flagged for Phase 125+:

1. NAME-blob tiebreak hazard on `--replace` within a single second.
   Three repair options documented in `124-E2E.md` §"Run 6a": timestamp
   bump, tiebreak-on-(ts DESC, target_hash DESC), or 1-second-granularity
   contract docs.

Both 124-05 blockers resolved on 2026-04-21 (see Deviations §#4 and §#5).

## Link

- E2E execution log (phase-gate artifact): `./124-E2E.md`
- Phase Gate verdict: **PASS** (see `## Phase Gate` at bottom of 124-E2E.md)
- Raw E2E command captures: `./e2e-logs/*.log`
- SC-124-4 scope-down record: `./124-VALIDATION.md` §"SC-124-4 scope-down (2026-04-21)"
