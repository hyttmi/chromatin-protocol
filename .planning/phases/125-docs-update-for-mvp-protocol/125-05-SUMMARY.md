---
phase: 125-docs-update-for-mvp-protocol
plan: 05
subsystem: code-hygiene
tags: [comments, refactor, phase-breadcrumbs, d-13, d-14]

# Dependency graph
requires:
  - phase: 125-04
    provides: "Schema regen + pre-122 vestige cleanup; frozen baseline to run comment hygiene on"
provides:
  - "Zero `// Phase N` breadcrumb comments across cli/src/, cli/tests/, and db/ (.cpp/.h/.fbs)"
  - "D-14 hygiene baseline: no commented-out code blocks, no obvious-verb restatement comments in the surveyed high-density files"
  - "Load-bearing WHY preserved: CONC-04, AEAD nonce ordering, MDBX mmap geometry, ForceDefaults determinism, 11-step ingest pipeline markers, requirement IDs (D-03..D-15, CHUNK-0x, CONN-0x, MAINT-0x, FILT-0x, RATE-0x, PUSH-0x, ARCH-01, CORO-01, etc.)"
affects: [future-phases, contributor-onboarding]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Comment-hygiene mechanical rule: strip Phase-N token; preserve REQ-ID prefix; preserve WHY substance"
    - "Catch2 identifier carve-out: TEST_CASE names, tags ([phase122]), and SECTION() labels are NOT modified by D-13 (plan <truths>: comments, not identifiers)"

key-files:
  created: []
  modified:
    - cli/src/chunked.cpp
    - cli/src/chunked.h
    - cli/src/commands.cpp
    - cli/src/commands.h
    - cli/src/commands_internal.h
    - cli/src/connection.cpp
    - cli/src/error_decoder.cpp
    - cli/src/main.cpp
    - cli/tests/test_chunked.cpp
    - cli/tests/test_wire.cpp
    - db/config/config.h
    - db/engine/engine.cpp
    - db/engine/engine.h
    - db/main.cpp
    - db/net/connection.cpp
    - db/net/connection.h
    - db/net/framing.h
    - db/peer/blob_push_manager.cpp
    - db/peer/blob_push_manager.h
    - db/peer/connection_manager.cpp
    - db/peer/connection_manager.h
    - db/peer/message_dispatcher.h
    - db/peer/metrics_collector.h
    - db/peer/peer_types.h
    - db/peer/pex_manager.h
    - db/peer/sync_orchestrator.h
    - db/storage/storage.cpp
    - db/storage/storage.h
    - db/tests/engine/test_engine.cpp
    - db/tests/config/test_config.cpp
    - db/tests/net/test_framing.cpp
    - db/tests/peer/test_event_expiry.cpp
    - db/tests/peer/test_keepalive.cpp
    - db/tests/peer/test_list_by_magic.cpp
    - db/tests/peer/test_namespace_announce.cpp
    - db/tests/peer/test_peer_manager.cpp
    - db/tests/sync/test_sync_protocol.cpp
    - db/tests/test_helpers.h
    - db/tests/test_schema_phase122.cpp
    - db/wire/codec.h

key-decisions:
  - "Plan 05 executed in sequential (non-worktree) mode; 14 atomic `refactor(125-05): ...` commits spread across 11 subsystem batches + 1 D-14 consolidation pass."
  - "D-14 files were already at the clean baseline from earlier hygiene work (9c3d5e3d) and prior phases — zero commented-out code blocks, zero obvious-verb restatement comments in the 6 non-touched-by-9c3d5e3d files of the plan's 12-file list. Only 3 single-word field-label comments removed in cli/src/connection.cpp and db/net/connection.cpp."
  - "Test-case names and SECTION() labels containing `Phase N` tokens (e.g. `TEST_CASE(\"Phase 122 ...\"`, `SECTION(\"... (Phase 115)\")`) are retained as identifiers, consistent with the plan's <truths> directive."
  - "The `cli/src/connection.cpp` `// Pubkey` / `// Signature` labels above payload.insert() calls in encode_auth_payload are obvious-code restates of self-evident wire-format appends → removed. The neighboring `// 4-byte BE pubkey size` line is retained because it documents wire layout (load-bearing for a wire-format writer)."

patterns-established:
  - "D-13 mechanical rule now permanently applied: no `// Phase N`, `/// Phase N`, or `(Phase N)` tokens in source comments. Git log preserves archaeology; in-source phase tokens do not."
  - "Comment-hygiene pass commit-granularity: per-subsystem batch for reviewable diff history."
  - "Test-scaffolding identifier carve-out: Catch2 TEST_CASE names + SECTION() labels are identifiers (user-visible test output); D-13 leaves them alone, same as Catch2 tags."

requirements-completed:
  - DOCS-01
  - DOCS-02
  - DOCS-03
  - DOCS-04

# Metrics
duration: 7min (executor session)
completed: 2026-04-22
---

# Phase 125 Plan 05: Code-Comment Hygiene (D-13 + D-14) Summary

**313 Phase-N breadcrumb comments stripped across 49 files, and the 12 highest-density comment files surveyed per-comment for obvious-code and commented-out-code pruning. Zero `// Phase N` hits remain in source; all load-bearing WHY comments (CONC-04, AEAD nonce ordering, MDBX mmap geometry, ForceDefaults determinism, 11-step ingest pipeline, 20+ requirement-ID references) preserved intact.**

## Performance

- **Duration:** ~7 min (executor session); plan-total ~30 min (cumulative across 14 commits over two sessions)
- **Started:** 2026-04-22T05:38:43Z (executor session start)
- **Completed:** 2026-04-22T05:45:09Z
- **Tasks:** 3 (D-13 strip, D-14 hygiene, compile-check gate)
- **Files modified:** 40 (diff `6fb2ac5c..HEAD`: 67 files touched, 429 insertions / 439 deletions — net -10 lines; comment content net reduction larger)
- **Commits:** 14 atomic `refactor(125-05): ...` commits (8 new in this session + 6 from prior run) + this metadata commit

## Accomplishments

- D-13 complete: zero `// Phase N` / `/// Phase N` comments remain in `cli/src/`, `cli/tests/`, or any `db/` subtree across `.cpp`, `.h`, `.fbs` files.
- D-14 pass performed on all 12 high-density files the plan enumerated; the 6 not already cleaned by commit `9c3d5e3d` were found to already be at the clean baseline (zero commented-out code blocks, zero obvious-verb restatement comments, zero TODO/FIXME cruft).
- Every requirement-ID annotation (D-03..D-15, CHUNK-0x, CONN-0x, MAINT-0x, FILT-0x, RATE-0x, PUSH-0x, ARCH-01, CORO-01, CONC-04, etc.) preserved after phase-token stripping.
- Every load-bearing WHY comment preserved — preservation grep confirms: `CONC-04` in `db/engine/engine.cpp`, `AEAD nonce` in `db/net/connection.cpp`, `used_data_bytes`/`mmap` in `db/storage/storage.cpp`, `ForceDefaults` in `cli/src/wire.cpp`.
- Both binary targets (`cdb` + `chromatindb`) compile cleanly post-hygiene — no accidental code deletion from comment stripping.

## Task Commits

Fourteen atomic commits:

**D-13 strip batches (11 commits, ordered oldest → newest):**
1. `e6fd6ba7` — D-13 strip Phase-N tokens from db/tests/storage/
2. `ea9d4484` — D-13 strip Phase-N tokens from db/tests/ (engine/peer/net/sync/wire/config + top-level)
3. `7e444cc7` — D-13 strip Phase-N prefixes from db/wire/ and db/schemas/
4. `1059e66c` — D-13 strip Phase-N prefixes from db/storage/ and db/engine/
5. `6c3bcefc` — D-13 strip Phase-N prefixes from db/net/, db/sync/, db/peer/
6. `f8d82690` — D-13 strip Phase-N prefixes from cli/src/
7. `cc40b94a` — **[this session]** D-13 remainder strips in cli/src/
8. `83d6270e` — **[this session]** D-13 remainder strips in cli/tests/
9. `bd4e5e88` — **[this session]** D-13 remainder strips in db/ engine/storage/config/wire
10. `3cb42fcd` — **[this session]** D-13 remainder strips in db/peer/ headers
11. `36ab19a5` — **[this session]** D-13 final strips in db/ main/net/peer source
12. `46e232f1` — **[this session]** D-13 final strips across db/tests/ and cli/tests/

**D-14 hygiene-pass commits (2 commits):**
13. `9c3d5e3d` — D-14 hygiene pass on sync_protocol / codec / wire / engine / commands
14. `12c6edd5` — **[this session]** D-14 hygiene pass on cli connection.cpp + db/net/connection.cpp

**Plan metadata commit:** appended at end of plan finalization.

## Files Created/Modified

All 40 files modified were edits-only; no files created, no files deleted.

### Before/after Phase-N hit counts

Initial research (§3a) counted 313 `// Phase [0-9]+` occurrences across 49 files.

After Plan 05: repo-wide grep `// Phase [0-9]+|/// Phase [0-9]+` in `cli/src/ cli/tests/ db/ *.{cpp,h,fbs}` returns **0 hits**. Final-absence verification command (plan <verify>):

```bash
! grep -rEq "// Phase [0-9]+|/// Phase [0-9]+" cli/src/ cli/tests/ db/ --include="*.cpp" --include="*.h" --include="*.fbs"
# → exit 0 (PASS)
```

### D-14 per-file audit (plan's 12 high-density files)

| File | D-14 status | Notes |
|---|---|---|
| `cli/src/commands.cpp` | Cleaned in 9c3d5e3d | Dropped one navigational `// Decrypt` + GSD plan-breadcrumb tokens. |
| `cli/src/chunked.cpp` | Clean baseline | Every comment load-bearing (REQ-IDs, retry-backoff timing rationale, ML-DSA-87 non-determinism note, UnlinkGuard WR-03 rationale). |
| `cli/src/connection.cpp` | 3 restates removed (12c6edd5) | `// Pubkey`, `// Signature` in encode_auth_payload (obvious-code field labels). |
| `cli/src/wire.cpp` | Cleaned in 9c3d5e3d | Section-header Phase-N strip; ForceDefaults determinism note preserved. |
| `cli/src/wire.h` | Cleaned in 9c3d5e3d | REQ-IDs (D-01, D-03, D-05, A4) preserved. |
| `db/engine/engine.cpp` | Cleaned in 9c3d5e3d + bd4e5e88 | Step 0a/0b/0c/0d/0e + Step 1/1.5/1.7/2/4.5 annotations preserved (11-step pipeline structure). |
| `db/storage/storage.cpp` | Cleaned in 1059e66c + bd4e5e88 | `// Operate parameters`, `// Encryption helpers`, `// Seq helpers` retained as section dividers (not obvious-code restates). MDBX mmap quirk notes preserved. |
| `db/storage/storage.h` | Cleaned in 1059e66c + bd4e5e88 | Public doc comments: REQ-IDs (D-03) preserved, phase tokens removed. |
| `db/peer/message_dispatcher.cpp` | Already clean | One candidate (`// check in non-raw mode` at line 545) is load-bearing (explains secondary expiry check). |
| `db/net/connection.cpp` | 1 restate removed (12c6edd5) | `// Read response` above `co_await recv_raw()` (function name is the doc). AEAD nonce-order invariants preserved. |
| `db/sync/sync_protocol.cpp` | Cleaned in 9c3d5e3d | 3 `// Read [ns:32B]` obvious-restatement comments removed. Pitfall #3, SYNC-03 MVCC notes preserved. |
| `db/wire/codec.cpp` | Cleaned in 9c3d5e3d | `(Phase 123 D-03)` / `(Phase 123 D-05)` → `(D-03)` / `(D-05)`. |

### Examples of retained load-bearing WHY (audit trail, 10 samples)

1. **`db/engine/engine.cpp:172`** — `// Step 1: Structural checks (no inline pubkey, only signature)` — 11-step ingest pipeline marker; step numbers are structural.
2. **`db/engine/engine.cpp:175`** — `// Step 1.5 (D-03): PUBK-first invariant. Runs BEFORE any crypto::offload — the adversarial-flood defense (Pitfall #6): no registered owner ... => reject without burning ML-DSA-87 verify CPU.` — protocol rejection reason + adversarial-flood defense rationale.
3. **`db/engine/engine.cpp:197`** — `// Step 1.7 (D-13): BOMB structural validation. Runs BEFORE crypto::offload — Pitfall #6 adversarial-flood defense (mirrors Step 1.5 PUBK-first discipline).` — cross-reference to Pitfall invariant.
4. **`cli/src/chunked.cpp:587`** — `// Two-phase pipeline. rid_to_chunk_index binds rid to chunk_index at send time; retries allocate a fresh rid and resend the same chunk_hash — ML-DSA-87 non-determinism on the server side is not an issue for reads (we are not re-signing; we are re-requesting).` — crypto non-determinism boundary explanation.
5. **`cli/src/chunked.cpp:612`** — `// D-15: up to RETRY_ATTEMPTS (=3) retries per chunk with backoff 250/1000/4000 ms. Post-index (not pre-increment) so the first retry uses RETRY_BACKOFF_MS[0]=250ms — matches put_chunked. See 119-REVIEW.md §WR-02.` — non-obvious timing detail + review doc anchor.
6. **`db/storage/storage.cpp:176`** — `operate_params.max_maps = 10;  // 9 named sub-databases + 1 default (owner_pubkeys added post-122)` — geometry constraint with evolution note.
7. **`db/engine/engine.h:38-42`** — `pubk_first_violation, ///< D-03: first write to namespace was non-PUBK.` + siblings — REQ-ID annotations on error-code enum values.
8. **`db/net/connection.h:204`** — `// Send queue (PUSH-04)` — requirement-ID anchor for per-connection send-queue invariant.
9. **`cli/src/wire.cpp` (retained)** — ForceDefaults determinism notes + coroutine-frame value-copy lifetime workarounds.
10. **`cli/src/commands.cpp:573`** — Rule-1 fix rationale: "BOMBs are structurally regular blobs, so MsgType=BlobWrite not Delete" — load-bearing protocol-dispatcher rule (retained with phase prefix stripped).

### Examples of removed comments (5 samples)

1. **`cli/src/connection.cpp:70-74`** — `// Pubkey` / `// Signature` above `payload.insert(..., signing_pubkey.begin()...)` / `...signature.begin()...`. Obvious-code restate; wire-format WHY still documented by the surrounding `// 4-byte BE pubkey size` preamble and the `encode_auth_payload` function name.
2. **`db/net/connection.cpp:284`** — `// Read response` above `auto response = co_await recv_raw();`. Function call is self-documenting.
3. **`db/main.cpp:465`** — `// Write pidfile for peer management subcommands (Phase 118)` → `// Write pidfile for peer management subcommands`. Phase-token removal (retained subcommand WHY).
4. **`db/net/framing.h:21`** — `// Matches the database chunk size from Phase 999.8.` → `// Matches the node's internal chunk granularity.` — rephrased to drop GSD-phase reference.
5. **`db/tests/sync/test_sync_protocol.cpp:286`** — `// diff_hashes removed in Phase 39 -- replaced by reconciliation module` → `// diff_hashes removed -- replaced by reconciliation module`. Preserved the evolutionary-history WHY; dropped the phase breadcrumb.

## Decisions Made

- **Test-scaffolding identifier carve-out:** Catch2 `TEST_CASE` names (e.g. `TEST_CASE("Phase 122 BlobData ...")`), tags (`[phase122]`), and `SECTION()` labels (e.g. `SECTION("MAX_BLOB_DATA_SIZE is 500 MiB (Phase 115)")`) are identifiers / user-visible test output, NOT comments. D-13 scope per plan `<truths>` explicitly excludes identifiers. Leaving them intact preserves the test suite's public API and CI-output continuity.
- **Section-divider vs obvious-code:** Short 1-3 word comments like `// Constants`, `// Public API`, `// Encryption helpers` function as outline structure; they aid navigation through 2000-line files. Retained as a CLAUDE.md-compatible judgment (they describe a block, not the next line).
- **D-14 discipline under time pressure:** The 12-file plan list was fully surveyed; 6 files were already clean from commit 9c3d5e3d, the remaining 6 surveyed clean at the baseline level (0 commented-out code, 0 restatement comments). No "sprint the last 4 files at degraded quality" scenario hit.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Pending D-13 strips in working tree at plan start**
- **Found during:** Plan startup environment scan
- **Issue:** 22 files had uncommitted D-13 strip edits from a prior agent session (orchestrator's note mentioned `cli/src/chunked.cpp` and `db/peer/peer_manager.cpp` as linter-touched, and 20 more turned out to need the same treatment). These prevented a clean-working-tree start.
- **Fix:** Audited every pending diff (inspected via `git diff` per-file) — all were valid D-13 transformations preserving REQ-IDs. Committed in 4 per-subsystem batches (`cc40b94a`, `83d6270e`, `bd4e5e88`, `3cb42fcd`) to match the plan's "per-subsystem commit strategy for reviewable history".
- **Files modified:** 22 (itemized in batch commit bodies)
- **Verification:** Post-commit `git status --short` clean; repo-wide `// Phase N` grep returns 0.
- **Committed in:** `cc40b94a`, `83d6270e`, `bd4e5e88`, `3cb42fcd`

**2. [Rule 3 - Blocking] Test-scaffolding identifier collision with plan verify command**
- **Found during:** Final D-13 verification grep
- **Issue:** Plan's verify command `! grep -rEq '"[^"]*\\(Phase [0-9]+\\)[^"]*"' cli/src/ db/` matched `db/tests/net/test_framing.cpp:187: SECTION("MAX_BLOB_DATA_SIZE is 500 MiB (Phase 115)")`. A SECTION label is a Catch2 identifier (user-visible test output), not a help-text or error-message string.
- **Fix:** Classified as an intentional survivor per plan `<truths>`: "Test-case names ... are NOT modified — D-13 is scoped to comments, not identifiers". SECTION labels are in the same identifier family as TEST_CASE names. Left untouched.
- **Files modified:** none (intentional non-edit)
- **Verification:** Cross-referenced against plan `<success_criteria>` line: "Catch2 `TEST_CASE` names and tags (e.g. `[phase122]`) are NOT modified (D-13 is comment scope, not identifier scope)". SECTION labels covered by same carve-out.
- **Committed in:** documented in this SUMMARY rather than as a code commit.

**Total deviations:** 2 auto-fixed (1 blocking pickup of prior-session work, 1 verification-grep scope clarification).
**Impact on plan:** Both deviations were non-code-modifying interpretations of in-progress or edge-case scope. No scope creep — strictly within plan's stated truths/success criteria.

## TDD Gate Compliance

Plan 05 is `type: execute` (not `type: tdd`). No RED/GREEN/REFACTOR gate applies.

## Issues Encountered

None.

## Test Gate Results

**Baseline tests assumed green — user verifies post-plan.**

Per `DO_NOT_RUN_TESTS` directive in the executor prompt (inherited from MEMORY `feedback_delegate_tests_to_user.md` + `feedback_no_test_automation_in_executor_prompts.md`), the executor did NOT run `cli_tests` or `chromatindb_tests`. Plan 04's baseline was captured as: `cli_tests` 197614 assertions / 98 cases; `chromatindb_tests [peer]` 506 / 77. Plan 05 is a pure comment-edit + trivial field-label-removal pass; no code paths changed.

**Compile-check gate (executor-performed, ONE command per target, no test binaries invoked):**
```bash
cmake --build /home/mika/dev/chromatin-protocol/cli/build --target cdb -j$(nproc)
# → [100%] Built target cdb

cmake --build /home/mika/dev/chromatin-protocol/build --target chromatindb -j$(nproc)
# → [100%] Built target chromatindb
```
Both exit 0; no warnings, no errors. This rules out the primary D-14 risk (silent code deletion via accidental comment-strip), since any such deletion would surface as a compile error at link or use site.

**User action requested:** Please run `./build/cli/tests/cli_tests` and `./build/db/db/chromatindb_tests` locally. Expected baselines preserved (197614/98 CLI; 506/77 [peer]).

## Preservation Grep Confirmation

All load-bearing WHY invariants still present after the pass:

```
$ grep -q "CONC-04" db/engine/engine.cpp && echo PRESENT
PRESENT
$ grep -qi "AEAD nonce" db/net/connection.cpp && echo PRESENT
PRESENT
$ grep -qi "used_data_bytes\|mmap" db/storage/storage.cpp && echo PRESENT
PRESENT
$ grep -q "ForceDefaults" cli/src/wire.cpp && echo PRESENT
PRESENT
```

## User Setup Required

None - no external service configuration required.

## Deferred Issues

None. Scope discipline held: D-14 files confined to the plan's 12-file list; no `125-05-DEFERRED.md` written (not needed — scope completed in-budget).

## Next Phase Readiness

- Phase 125 complete: all 5 plans (01 PROTOCOL rewrite, 02 README/cli/README, 03 ARCHITECTURE, 04 pre-122 vestige cleanup + schema regen, 05 comment hygiene) now have SUMMARY.md and are committed to master.
- Source tree is now in post-v4.1.0 MVP-docs shape: public docs describe the shipping wire/storage/architecture surface, code comments follow CLAUDE.md "default to no comments" policy, and `git log` is the sole archaeology source for phase-provenance.
- Ready for Phase 126+ (per ROADMAP — next milestone work).

## Self-Check

Verified:
- [x] All 14 `refactor(125-05): ...` commits present in `git log --oneline --grep="125-05"`.
- [x] This file (`125-05-SUMMARY.md`) at the documented path.
- [x] `git status --short` shows only pre-existing untracked files (other phases' PATTERNS.md, docs/index.html, .claude/) — nothing from this plan.
- [x] Repo-wide `// Phase N` primary-absence grep returns empty.
- [x] Preservation greps (CONC-04, AEAD nonce, MDBX mmap, ForceDefaults) all PRESENT.
- [x] Both `cdb` and `chromatindb` targets build cleanly (one `cmake --build` invocation per target; no test binaries executed).
- [x] No files in `key-files.created`; 40 files in `key-files.modified` match the `git diff --name-only 6fb2ac5c..HEAD | grep -vE '^\.(planning|claude)'` output.

## Self-Check: PASSED

---
*Phase: 125-docs-update-for-mvp-protocol*
*Completed: 2026-04-22*
