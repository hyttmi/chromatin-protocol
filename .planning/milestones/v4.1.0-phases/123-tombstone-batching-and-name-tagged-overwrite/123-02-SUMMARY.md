---
phase: 123
plan: 02
subsystem: engine
tags: [engine, bomb, name, ingest, dispatcher]
dependency_graph:
  requires:
    - phase-122                  # post-signer_hint verify path, IngestError enum, wire-error code idiom
    - plan-123-01                # NAME + BOMB codec, is_bomb / validate_bomb_structure / extract_bomb_targets, ERROR_BOMB_* constants, make_name_blob / make_bomb_blob test helpers
  provides:
    - engine::IngestError::bomb_ttl_nonzero / bomb_malformed / bomb_delegate_not_allowed
    - Step 0e BOMB exemption (max_ttl guard)
    - Step 1.7 BOMB structural validation (pre-crypto-offload)
    - Step 2 delegate-reject extension for is_bomb
    - Step 3.5 BOMB side-effect (iterate + delete N targets)
    - Dispatcher IngestError→wire mapping at BOTH BlobWrite + Delete sites
  affects:
    - plan-123-03 (CLI put --name / rm multi-target / get by name) — CLI now receives precise wire errors
    - plan-123-04 (integration tests) — can assert dispatcher-level ERROR_BOMB_* codes
tech_stack:
  added: []
  patterns:
    - Pitfall #6 adversarial-flood defense (structural validation BEFORE crypto::offload)
    - Reuse Plan-01 helpers (no inline re-parsing; feedback_no_duplicate_code.md)
    - Post-back-to-ioc discipline preserved (Phase 121 D-07)
    - Catch2 [phase123][engine][...] subset tagging convention
key_files:
  created:
    - db/tests/engine/test_bomb_validation.cpp
    - db/tests/engine/test_bomb_side_effect.cpp
    - db/tests/engine/test_name_delegate.cpp
    - db/tests/engine/test_name_overwrite.cpp
  modified:
    - db/engine/engine.h
    - db/engine/engine.cpp
    - db/peer/message_dispatcher.cpp
    - db/CMakeLists.txt
decisions:
  - "BOMB exemption in Step 0e max_ttl guard mirrors the existing tombstone exemption (BOMB is permanent, D-13(1))"
  - "Step 1.7 placed between Step 1.5 (PUBK-first) and Step 2 (signer resolution) — runs before any crypto::offload"
  - "Step 1.7 uses wire::validate_bomb_structure (Plan 01) — no inline payload re-parsing"
  - "Step 2 delegate-reject adds is_bomb alongside existing is_tombstone / is_delegation (identical reject-list idiom)"
  - "Step 3.5 BOMB side-effect iterates wire::extract_bomb_targets and calls storage_.delete_blob_data per target on io_context thread — no additional offload (Phase 121 post-back already happened at :340)"
  - "No Storage::delete_blobs_data_batch helper added — A3 YAGNI; per-target MDBX txn pattern matches existing single-tombstone path"
  - "Dispatcher wire mapping extended at BOTH BlobWrite (~:1442) and Delete (~:391) sites via the existing else-if idiom — no new helper introduced"
metrics:
  duration: "~35m"
  completed_date: "2026-04-20"
  files_modified: 4
  files_created: 4
  tasks: 4
  commits: 4
---

# Phase 123 Plan 02: Engine BOMB Ingest Gates + Dispatcher Wire Mapping Summary

**One-liner:** Four surgical edits to `db/engine/engine.cpp` (Step 0e exemption, NEW Step 1.7 structural validation, Step 2 delegate-reject extension, NEW Step 3.5 side-effect) plus the matching `IngestError`→wire-error-code mappings at both dispatcher sites. Reuses Plan 01 helpers (`wire::is_bomb`, `wire::validate_bomb_structure`, `wire::extract_bomb_targets`); no schema changes, no new `TransportMsgType`, no duplicated codec code.

## Engine.cpp Surgical Edits (line ranges, post-edit)

| Edit | Location | Lines | What |
|------|----------|-------|------|
| Step 0e BOMB exemption | engine.cpp:160-172 | +1 (extended guard condition) | `!wire::is_tombstone(blob.data) && !wire::is_bomb(blob.data)` — BOMB is permanent like tombstone |
| Step 1.7 (NEW) structural validation | engine.cpp:198-219 | +22 | `ttl!=0` → `IngestError::bomb_ttl_nonzero`; `!validate_bomb_structure(data)` → `bomb_malformed`. Runs BEFORE any `crypto::offload` (Pitfall #6 adversarial-flood defense) |
| Step 2 delegate-reject extension | engine.cpp:304-311 | +8 | `if (is_delegate) { ... if (wire::is_bomb(blob.data)) reject with bomb_delegate_not_allowed; }` — mirrors existing `is_tombstone` / `is_delegation` rejects (D-12 owner-only deletion) |
| Step 3.5 (NEW branch) BOMB side-effect | engine.cpp:381-402 | +14 | `else if (wire::is_bomb(blob.data)) { for each target in extract_bomb_targets(data) storage_.delete_blob_data(ns, target); }` — runs on io_context thread after post-back (engine.cpp:340) |

Grep gates (all pass):

```bash
$ grep -c "wire::is_bomb" db/engine/engine.cpp             # 4  (Steps 0e, 1.7, 2, 3.5)
$ grep -c "!wire::is_tombstone(blob.data) && !wire::is_bomb(blob.data)" db/engine/engine.cpp   # 1  (Step 0e)
$ grep -n "bomb_ttl_nonzero\|bomb_malformed\|bomb_delegate_not_allowed" db/engine/engine.cpp   # 3 uses
$ grep -c "post.*this_coro::executor" db/engine/engine.cpp  # 6  (post-back discipline preserved, no regression)
```

## IngestError Enum Additions (`db/engine/engine.h`)

```cpp
enum class IngestError {
    // ... existing entries ...
    pubk_first_violation,
    pubk_mismatch,
    bomb_ttl_nonzero,          // Phase 123 D-13(1): BOMB with ttl != 0 (BOMB must be permanent)
    bomb_malformed,            // Phase 123 D-13(2): BOMB header structural sanity failed (size mismatch)
    bomb_delegate_not_allowed  // Phase 123 D-12: delegates cannot emit BOMB blobs
};
```

The enum was extended, not re-shaped — no dependent code (sync, dispatcher, other engines) touches `IngestError`'s numeric ordering.

## Dispatcher Wire Mapping (`db/peer/message_dispatcher.cpp`)

Both translation sites extended via the existing `else if` idiom (no new helper introduced per Phase 122 precedent):

- **Delete dispatch (~:391-404):** 3 new `else if` arms mapping the three new `IngestError` values to their wire constants from Plan 01 (`ERROR_BOMB_TTL_NONZERO=0x09`, `ERROR_BOMB_MALFORMED=0x0A`, `ERROR_BOMB_DELEGATE_NOT_ALLOWED=0x0B`).
- **BlobWrite dispatch (~:1455-1478):** Same 3 mappings, using the existing `spdlog::warn` + `record_strike_` + `send_error_response` pattern from Phase 122.

Regression check — Phase 122 mappings still present:

```bash
$ grep -c "pubk_first_violation" db/peer/message_dispatcher.cpp   # 2  (both sites)
$ grep -c "pubk_mismatch" db/peer/message_dispatcher.cpp          # 2  (both sites)
```

BOMB rejections now surface as precise wire errors instead of the catch-all `ERROR_VALIDATION_FAILED` — downstream clients (CLI, future integration tests) can distinguish ttl-nonzero vs malformed vs delegate-BOMB without parsing `error_detail` strings.

## Test Files Created

| File | TEST_CASE count | Tags | What it proves |
|------|-----------------|------|----------------|
| `db/tests/engine/test_bomb_validation.cpp` | 5 | `[bomb_ttl]`, `[bomb_sanity]`, `[bomb_accept]` (×2), `[bomb_delegate]` | Rejects ttl!=0 BOMB (T-123-03), rejects size-mismatched BOMB (T-123-02), accepts count=0 no-op (T-123-04 / A2), rejects delegate-signed BOMB via delegation_map resolution (T-123-01), accepts owner BOMB with ttl=0 + valid structure |
| `db/tests/engine/test_bomb_side_effect.cpp` | 2 | `[bomb_side_effect]` | BOMB with N=3 targets tombstones all three via Step 3.5 loop; BOMB with one absent target is accepted (D-14) and present targets still deleted |
| `db/tests/engine/test_name_delegate.cpp` | 1 | `[name_delegate]` | Delegate-signed NAME blob accepted (D-11: is_name NOT in Step 2 delegate-reject list); stored NAME's `signer_hint` equals `SHA3(delegate_pk)` for audit |
| `db/tests/engine/test_name_overwrite.cpp` | 2 | `[overwrite]`, `[overwrite][tiebreak]` | Higher `blob.timestamp` wins (D-01); identical timestamps tiebreak on `content_hash` lex-DESC (D-02). Enumeration uses `storage.get_blob_refs_since` + `ref.blob_type == NAME_MAGIC` (Storage-layer analog of the CLI's ListRequest+type_filter=NAME path) |

All four files registered in `db/CMakeLists.txt` and compile against `chromatindb_tests`.

Test construction notes:

- **Delegate BOMB test** (test_bomb_validation.cpp, `bomb_delegate_rejected`): uses the standard "register owner + ingest delegation blob + delegate signs a BOMB" sequence (not the non-existent `storage.register_delegation` API). Engine resolves signer via `storage.get_delegate_pubkey_by_hint(owner_ns, delegate_hint)` → `is_delegate=true` → trips the new Step 2 trailer arm.
- **Delegate NAME test** (test_name_delegate.cpp): same setup, but D-11 means the delegate's NAME is ACCEPTED. The stored NAME's `signer_hint` is asserted to equal `SHA3(delegate_pk)` so downstream audit tooling can identify who named what.
- **Overwrite enumeration** (test_name_overwrite.cpp): re-implements the CLI's resolution rule (`timestamp DESC, content_hash DESC`) as a Catch2 helper over `get_blob_refs_since` + `get_blob` — does NOT require the Plan 04 ListRequest+type_filter integration.

## Decisions Made

1. **BOMB exemption in Step 0e max_ttl guard mirrors the tombstone exemption.** Both are permanent-by-invariant; grace/expiry violates the delete-propagation guarantee (project memory `project_phase123_bomb_ttl_zero.md`).

2. **Step 1.7 runs BEFORE crypto::offload.** Mirrors Phase 122 Step 1.5 PUBK-first discipline. Adversarial-flood defense: malformed BOMB rejected in integer arithmetic (~nanoseconds) instead of burning ML-DSA-87 verify (~5-10ms). RESEARCH §Pitfall 2.

3. **No inline payload re-parsing.** Step 1.7 calls `wire::validate_bomb_structure(blob.data)` from Plan 01; Step 3.5 calls `wire::extract_bomb_targets(blob.data)`. Zero duplicate byte-shift code (`feedback_no_duplicate_code.md`).

4. **No `Storage::delete_blobs_data_batch` helper.** A3 recommendation: per-target MDBX txn matches the single-tombstone pattern; batch-txn optimization is YAGNI until measured on a real multi-thousand-target BOMB.

5. **Dispatcher mapping via `else if` chain, not a helper function.** Matches Phase 122 precedent verbatim; introducing a `ingest_error_to_wire_code()` helper would have churned two call sites for zero de-duplication win (2-3 extra lines each vs an indirect call).

6. **`count == 0` BOMB accepted as no-op.** Plan 01 A2 recommendation. The side-effect loop runs zero iterations; storage unchanged. Covered by the `[bomb_accept]` TEST_CASE in test_bomb_validation.cpp.

## Deviations from Plan

None. Every plan task executed as written. All acceptance-criteria grep checks pass on the post-edit tree.

### Auto-fixed Issues

None — no Rule 1/2/3 triggers. `chromatindb_lib` and `chromatindb_tests` compile cleanly on the extended enum + reshaped Step 3.5. Post-back-to-ioc count (6) preserved.

### Tooling Notes

Executed in isolated worktree at `.claude/worktrees/agent-a6592fa2`. Reset to plan-assigned base `5e1b70e0` at start (HEAD was one commit behind). All four commits use `git commit --no-verify` per plan convention. Per `feedback_delegate_tests_to_user.md`, the executor did NOT run the full Catch2 suite — compile-check only.

## Verification

```bash
# Compilation (exit 0):
cmake --build build -j$(nproc) --target chromatindb_lib chromatindb_tests  # OK

# Engine.cpp grep gates:
grep -c "wire::is_bomb" db/engine/engine.cpp                          # 4
grep -c "!wire::is_tombstone(blob.data) && !wire::is_bomb(blob.data)" db/engine/engine.cpp  # 1
grep -qE "bomb_ttl_nonzero|bomb_malformed|bomb_delegate_not_allowed" db/engine/engine.h     # OK
grep -c "post.*this_coro::executor" db/engine/engine.cpp              # 6 (no regression)

# Dispatcher grep gates:
grep -c "bomb_ttl_nonzero" db/peer/message_dispatcher.cpp              # 2 (BlobWrite + Delete)
grep -c "bomb_malformed" db/peer/message_dispatcher.cpp                # 2
grep -c "bomb_delegate_not_allowed" db/peer/message_dispatcher.cpp     # 2
grep -qE "ERROR_BOMB_TTL_NONZERO|0x09" db/peer/message_dispatcher.cpp  # OK

# Test file presence + tag coverage:
test -f db/tests/engine/test_bomb_validation.cpp       # OK
test -f db/tests/engine/test_bomb_side_effect.cpp      # OK
test -f db/tests/engine/test_name_delegate.cpp         # OK
test -f db/tests/engine/test_name_overwrite.cpp        # OK
grep -c "TEST_CASE" db/tests/engine/test_bomb_validation.cpp    # 5
grep -c "TEST_CASE" db/tests/engine/test_bomb_side_effect.cpp   # 2
grep -c "TEST_CASE" db/tests/engine/test_name_overwrite.cpp     # 2
grep -q "\[phase123\]\[engine\]\[bomb_ttl\]"      db/tests/engine/test_bomb_validation.cpp  # OK
grep -q "\[phase123\]\[engine\]\[bomb_sanity\]"   db/tests/engine/test_bomb_validation.cpp  # OK
grep -q "\[phase123\]\[engine\]\[bomb_delegate\]" db/tests/engine/test_bomb_validation.cpp  # OK
grep -q "\[phase123\]\[engine\]\[bomb_accept\]"   db/tests/engine/test_bomb_validation.cpp  # OK
grep -q "\[phase123\]\[overwrite\]\[tiebreak\]"   db/tests/engine/test_name_overwrite.cpp   # OK

# Confirmation: no schema touched, no new TransportMsgType:
git diff 5e1b70e0..HEAD -- db/schemas/      # empty
git diff 5e1b70e0..HEAD -- db/schemas/transport.fbs  # empty
```

## Hand-off to User

Per `feedback_delegate_tests_to_user.md`, the executor did NOT run the Catch2 suite. The build has been verified. **Please run the Phase 123 engine subset and paste the output:**

```bash
./build/db/chromatindb_tests '[phase123][engine]' --reporter compact
```

Expected: all new test cases pass (bomb_ttl, bomb_sanity, bomb_accept×2, bomb_delegate, bomb_side_effect×2, name_delegate, overwrite, overwrite tiebreak). Also worth a regression check against Phase 122:

```bash
./build/db/chromatindb_tests '[phase122]' --reporter compact
```

Expected: no regressions (engine Step 0e/Step 2 edits preserve the tombstone + delegation paths verbatim; dispatcher additions appended to `else if` chains without altering the existing branches).

## Commits

| Hash | Message |
|------|---------|
| `d145d664` | feat(123-02): BOMB ingest gates in engine — Step 0e exemption, Step 1.7 validation, Step 2 delegate-reject, Step 3.5 side-effect |
| `1081960b` | test(123-02): add test_bomb_validation.cpp — BOMB ingest rejections + accept paths |
| `403eaba6` | test(123-02): add BOMB side-effect, delegate NAME, NAME overwrite coverage |
| `7b05c46f` | feat(123-02): dispatcher IngestError→wire mapping for BOMB rejection codes |

## Downstream Impact

- **Plan 03 (CLI):** `cdb rm A B C` → ONE BOMB covering {A, B, C}, signed by owner. The node now enforces ttl=0 + structural sanity + owner-only at ingest; CLI can submit without client-side validation. CLI can reuse `cli::make_name_data` / `cli::make_bomb_data` from Plan 01 regardless of pre-/post-122 wire choice — the CLI's pre-122 BlobData shape does not affect codec helpers.
- **Plan 04 (integration):** can assert dispatcher-level ERROR_BOMB_* wire codes via `send_error_response` inspection.
- **Future ops tooling (999.x):** BOMB garbage-collection / tombstone-admin commands now have structured rejection codes to distinguish "sender sent bad BOMB" from "sender is unauthorized" without string-parsing.

## Threat Flags

None. All modifications are within the plan's `<threat_model>` scope (T-123-01..04, T-123-06). No new network endpoints, no new auth paths, no file-access changes, no schema changes at trust boundaries.

## Self-Check: PASSED

- Files created/modified exist on disk: FOUND (8/8 — 4 test files + engine.h + engine.cpp + message_dispatcher.cpp + CMakeLists.txt).
- Commits exist in git log:
  ```bash
  $ git log --oneline 5e1b70e0..HEAD
  7b05c46f feat(123-02): dispatcher IngestError→wire mapping for BOMB rejection codes
  403eaba6 test(123-02): add BOMB side-effect, delegate NAME, NAME overwrite coverage
  1081960b test(123-02): add test_bomb_validation.cpp — BOMB ingest rejections + accept paths
  d145d664 feat(123-02): BOMB ingest gates in engine — Step 0e exemption, Step 1.7 validation, Step 2 delegate-reject, Step 3.5 side-effect
  ```
- All grep checks pass (see Verification section above).
- `chromatindb_lib` and `chromatindb_tests` both compile (exit 0).
- No STATE.md / ROADMAP.md edits made (per executor constraints).
- No schema touched (`git diff 5e1b70e0..HEAD -- db/schemas/` is empty).
