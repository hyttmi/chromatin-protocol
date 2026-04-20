---
phase: 122
plan: 04
subsystem: engine-verify-path
tags:
  - phase122
  - engine
  - verify-path
  - pubk-first
  - owner-pubkeys
  - delegation
  - protocol-break
  - post-back-to-ioc

# Dependency graph
requires:
  - phase: 122-01
    provides: Post-122 Blob FlatBuffer (signer_hint; no namespace_id, no pubkey)
  - phase: 122-02
    provides: Storage owner_pubkeys DBI + register/get/has_owner_pubkey + get_delegate_pubkey_by_hint + D-04 throw-on-mismatch
  - phase: 122-03
    provides: wire::BlobData post-122 shape + build_signing_input(target_namespace, ...) + wire::extract_pubk_signing_pk

provides:
  - BlobEngine::ingest(target_namespace, blob, source) widened signature
  - BlobEngine::delete_blob(target_namespace, delete_request, source) widened signature
  - IngestError::pubk_first_violation (D-03) + IngestError::pubk_mismatch (D-04)
  - ERROR_PUBK_FIRST_VIOLATION = 0x07 + ERROR_PUBK_MISMATCH = 0x08 wire codes
  - Engine Step 1.5 PUBK-first gate — single site, covers direct + sync via delegation
  - Engine Step 2 owner_pubkeys lookup + delegation fallback (no more derived_ns == blob.namespace_id)
  - Engine Step 4.5 PUBK registration with D-04 throw-catch translation
  - Storage::store_blob family widened to take target_namespace as first parameter (Rule 3 cascade fix)
  - PrecomputedBlob.target_namespace field (for atomic batch storage)
  - test_helpers.h post-122 shape: make_signed_blob / make_signed_tombstone / make_signed_delegation / make_delegate_blob + new make_pubk_blob

affects:
  - 122-05 (sync_protocol.cpp, message_dispatcher.cpp, sync_orchestrator.cpp, blob_push_manager.cpp call sites)
  - 122-06 (concurrency TSAN coverage — PUBK-first race fixture)
  - 122-07 (remaining test file schema-cascade sweep)

tech-stack:
  added: []
  patterns:
    - "PUBK-first gate: cheap integer check via storage_.has_owner_pubkey(target_namespace) + wire::is_pubkey_blob(blob.data) BEFORE any crypto::offload (Pitfall #6 DoS defense)"
    - "Fresh-namespace PUBK verify: extract embedded signing_pk from body, SHA3(embedded_sk) == target_namespace integrity check, then Signer::verify against embedded_sk"
    - "D-04 throw-on-mismatch translation: try/catch around storage_.register_owner_pubkey surfaces storage::runtime_error as IngestError::pubk_mismatch"
    - "T-122-07 integrity guard: owner-branch cross-checks SHA3(resolved_pubkey) == target_namespace to defend against owner_pubkeys DBI corruption"
    - "Rule 3 scope-expansion: Storage::store_blob widened to take target_namespace because blob.namespace_id no longer exists — plans 02/03 left the internal store_blob body broken; the verify gate for chromatindb_lib compile required this to land"

key-files:
  created: []
  modified:
    - db/engine/engine.h
    - db/engine/engine.cpp
    - db/peer/error_codes.h
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/tests/test_helpers.h

key-decisions:
  - "PUBK-first check lives in ONE PLACE (engine.cpp Step 1.5). sync_protocol.cpp does NOT duplicate the check — it delegates via engine_.ingest (SyncProtocol::ingest_blobs per-blob call), so the single site covers both paths (feedback_no_duplicate_code.md, Pitfall #2)."
  - "Step 4.5 registration fires AFTER successful signature verify: defense-in-depth against a corrupted PUBK body registering a bogus signing_pk in owner_pubkeys. The fresh-namespace PUBK path in Step 2 still does the SHA3(embedded_sk) == target_namespace integrity check before verify, so both gates must pass for registration to occur."
  - "delete_blob does NOT run a PUBK-first gate: tombstones are never PUBKs, and a delete to a namespace without a registered owner falls through to no_delegation rejection. Mirror the ingest-path delegate-tombstone rule (delegates cannot delete)."
  - "Rule 3 — Storage::store_blob family widened to take target_namespace: plan 04 scope said files_modified = [engine.h, engine.cpp, error_codes.h, test_helpers.h], but plans 02/03 left storage.cpp's internal store_blob body reading blob.namespace_id (no longer a field post-122). The verify gate `cmake --build build --target chromatindb_lib` is unachievable without this fix, so it lands here. PrecomputedBlob grew a target_namespace field to match."
  - "Fresh-namespace PUBK verify branch is explicit (not folded into register_owner_pubkey): at Step 2, if owner_pubkeys[signer_hint] misses AND blob is PUBK, verify SHA3(embedded_sk) == target_namespace, set resolved_pubkey = embedded_sk, is_owner = true. Signature is then verified against embedded_sk. This makes the registration-after-verify flow explicit and defends against a PUBK claiming a namespace it does not hash to (D-03 would catch, but this is tighter)."

patterns-established:
  - "phase122-pubk-first-gate: storage_.has_owner_pubkey(target_namespace) + wire::is_pubkey_blob(blob.data) BEFORE any offload"
  - "phase122-resolve-signer: owner_pubkeys[signer_hint] -> integrity-check -> is_owner; else delegation_map[ns || signer_hint] -> is_delegate; else no_delegation"
  - "phase122-pubk-register: Step 4.5 try { register } catch(...) { return IngestResult::rejection(IngestError::pubk_mismatch, ...) }"
  - "phase121-post-back-to-ioc preserved at all 6 crypto::offload sites in new engine.cpp code"

requirements-completed:
  - "SC#4 (engine layer): PUBK-first invariant enforced in BlobEngine::ingest (Step 1.5)"
  - "SC#5 (engine layer): PUBK ingest triggers storage_.register_owner_pubkey (Step 4.5)"
  - "SC#6: derived_ns == blob.namespace_id check removed; verify path resolves pubkey via owner_pubkeys + delegation lookup"
  - "D-03: PUBK-first at node protocol level (engine.cpp); sync inherits via delegation"
  - "D-04: register throws on mismatch; engine catches and emits pubk_mismatch"
  - "D-09: full owner / delegate verify flow per spec"
  - "D-12 (foundation): make_pubk_blob helper enables Plan 06 coverage of (a)/(b)/(c)/(d)/(e)/(f)"
  - "D-13 (foundation): target_namespace-as-first-sponge-input preserved in Step 3 verify offload"

# Metrics
duration: 12min
completed: 2026-04-20
tasks_completed: 3
files_modified: 6
commits: 3
---

# Phase 122-04: Engine Verify Path Refactor (PUBK-first + owner_pubkeys + Step 4.5) Summary

**Keystone plan landed: BlobEngine::ingest + delete_blob now take target_namespace separately; Step 1.5 enforces the PUBK-first invariant BEFORE any crypto::offload (adversarial-flood defense); Step 2 resolves signing pubkey via owner_pubkeys (owner) or delegation_map (delegate); Step 4.5 registers PUBK signing pubkeys in owner_pubkeys after successful verify with D-04 mismatch -> IngestError::pubk_mismatch translation. test_helpers.h migrated to the post-122 BlobData shape with a new make_pubk_blob helper.**

## Performance

- **Duration:** ~12 minutes
- **Started:** 2026-04-20T07:36:56Z
- **Completed:** 2026-04-20 (this commit)
- **Tasks:** 3 / 3
- **Files modified:** 6 (engine.h, engine.cpp, error_codes.h, storage.h, storage.cpp, test_helpers.h)

## Accomplishments

### Task 1: engine.h API widening + IngestError / wire-error constants — commit `2de56983`

- **IngestError enum additive changes** (no reorder, no removals):
  - `pubk_first_violation` (D-03: first write to namespace was non-PUBK)
  - `pubk_mismatch` (D-04: incoming PUBK signing pubkey differs from registered owner)
  - `namespace_mismatch` doc-comment updated to post-122 semantics
    (`SHA3(resolved_pubkey) != target_namespace`).
- **BlobEngine::ingest** gains `std::span<const uint8_t, 32> target_namespace`
  as new first parameter (transport envelope carries it per D-07).
- **BlobEngine::delete_blob** similarly widened; the structural doc-comment
  rewritten to reflect Phase 122 flow (signer_hint, no inline pubkey,
  target_namespace threaded through signing sponge).
- **Class-level BlobEngine doxygen** rewritten: "structural -> PUBK-first
  gate -> owner_pubkeys lookup (owner) OR delegation_map lookup (delegate)
  -> sig verify".
- **error_codes.h** gains `ERROR_PUBK_FIRST_VIOLATION = 0x07` and
  `ERROR_PUBK_MISMATCH = 0x08` with matching `error_code_string` /
  `error_code_name` arms.

### Task 2: engine.cpp verify + delete path refactor — commit `7ea12c41`

- **Step 0-0e (timestamp / TTL / capacity / max_ttl)**: unchanged, pass-through.
- **Step 1 (structural)**: dropped the pre-122 pubkey-size check (no inline
  pubkey post-schema-change); empty-signature check preserved.
- **Step 1.5 (NEW — PUBK-first gate, D-03)**: runs BEFORE any `crypto::offload`.
  ```
  if (!storage_.has_owner_pubkey(target_namespace)) {
      if (!wire::is_pubkey_blob(blob.data)) {
          co_return IngestResult::rejection(IngestError::pubk_first_violation, ...);
      }
      // else: PUBK is allowed to register a new namespace — fall through.
  }
  ```
  This is the adversarial-flood defense (Pitfall #6): non-PUBK writes to
  unregistered namespaces are rejected without burning ML-DSA-87 verify CPU.
  Sync path inherits the check for free because `SyncProtocol::ingest_blobs`
  delegates per-blob to `engine_.ingest` — single site, per
  `feedback_no_duplicate_code.md` (Pitfall #2).
- **Step 2 (REWRITTEN — owner_pubkeys + delegation resolution, D-09)**:
  `derived_ns = sha3(blob.pubkey)` + `is_owner = (derived_ns == blob.namespace_id)`
  deleted entirely. Replaced with:
  - Lookup `storage_.get_owner_pubkey(blob.signer_hint)`; on hit, compute
    `SHA3(resolved_pubkey)` (offloaded) + cross-check equals `target_namespace`
    (T-122-07 integrity guard against owner_pubkeys DBI corruption).
  - **Fresh-namespace PUBK branch**: if owner_pubkeys misses AND blob is PUBK,
    extract embedded signing_pk via `wire::extract_pubk_signing_pk(blob.data)`,
    verify `SHA3(embedded_sk) == target_namespace` (offloaded), set
    `resolved_pubkey = embedded_sk`, `is_owner = true`.
  - Otherwise fall through to `storage_.get_delegate_pubkey_by_hint(target_namespace, signer_hint)`
    (Plan 02 helper) for delegate resolution.
  - If neither path resolves: `IngestError::no_delegation`.
  - Post-back-to-ioc after every `crypto::offload` before any `storage_` call
    (Phase 121 STORAGE_THREAD_CHECK discipline).
- **Step 2a quota / 2.5 dedup**: unchanged structurally; uses `target_namespace`
  in place of `blob.namespace_id` throughout.
- **Step 3 (REWRITTEN — verify with target_namespace + resolved_pubkey)**:
  ```
  auto si = wire::build_signing_input(target_namespace, blob.data, blob.ttl, blob.timestamp);
  bool ok = crypto::Signer::verify(si, blob.signature, resolved_pubkey);
  ```
  target_namespace is the first sponge input, which is the D-13 cross-namespace
  replay defense: a delegate signing for namespace N_A submitted with
  `target_namespace = N_B` produces a different signing_input digest, so verify
  fails.
- **Step 3.5 tombstone handling**: unchanged structurally; uses `target_namespace`.
- **Step 4.5 (NEW — PUBK registration, D-04)**: after verify passes and
  tombstone-block check, if `wire::is_pubkey_blob(blob.data)`:
  ```
  try {
      storage_.register_owner_pubkey(blob.signer_hint, wire::extract_pubk_signing_pk(blob.data));
  } catch (const std::exception& e) {
      co_return IngestResult::rejection(IngestError::pubk_mismatch, ...);
  }
  ```
  Idempotent-match returns silently from Storage; mismatched bytes throw (Plan 02
  contract) and are translated to wire-level `ERROR_PUBK_MISMATCH` by the
  dispatcher (Plan 05's scope).
- **Step 4 store_blob**: unchanged conceptually; now passes target_namespace
  as first argument to the widened `storage_.store_blob(target_namespace, blob, ...)`.
- **delete_blob** mirrors the refactor:
  - Drop pre-122 pubkey-size check.
  - owner_pubkeys lookup with integrity cross-check (same Step 2 pattern).
  - No PUBK-first gate (tombstones are never PUBKs; missing-owner falls through
    to `no_delegation`).
  - Explicit delegate-tombstone rejection when delegation_map resolves the
    signer_hint (mirrors ingest-path rule).
  - build_signing_input with target_namespace; Signer::verify with
    resolved_pubkey.
  - Store call updated to `storage_.store_blob(target_namespace, delete_request)`.

### Task 3: test_helpers.h cascade — commit `c905063a`

- `make_signed_blob`: emits `BlobData` with `signer_hint = SHA3(id.public_key())`,
  no inline pubkey, no namespace_id. Signs `build_signing_input(id.namespace_id(), ...)`.
- `make_signed_tombstone`: same shape.
- `make_signed_delegation`: same shape.
- `make_delegate_blob`: signer_hint = SHA3(delegate.public_key()); target_namespace
  for signing input = owner.namespace_id() (D-01 cross-namespace replay defense
  lives at this boundary).
- `make_pubk_blob` (NEW): builds 4164-byte PUBK body
  `[magic:4][signing_pk:2592][kem_pk:1568]`; `signer_hint = SHA3(signing_pk)`;
  `ttl = 0` (permanent); signs against `id.namespace_id()`. `kem_pk` is
  optional — `std::nullopt` zero-fills the KEM portion for tests that don't
  exercise KEM, pass an explicit 1568-byte value for KEM-rotation tests (D-12d).

## Task Commits

Each task committed atomically (single-repo, `--no-verify`):

1. **Task 1 (feat):** `2de56983` — `feat(122-04): widen engine.h API + add IngestError / wire-error PUBK codes`
2. **Task 2 (feat):** `7ea12c41` — `feat(122-04): refactor engine.cpp verify path + thread target_namespace via storage`
3. **Task 3 (test):** `c905063a` — `test(122-04): update test_helpers.h to post-122 BlobData shape + add make_pubk_blob`

## Decisions Made

| Decision                                                                                        | Rationale                                                                                                                                                                                                                                                                                                |
| ----------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| PUBK-first gate lives ONCE in engine.cpp (Step 1.5); sync_protocol.cpp does NOT duplicate.      | `feedback_no_duplicate_code.md` + Pitfall #2. `SyncProtocol::ingest_blobs` already delegates per-blob to `engine_.ingest` (see RESEARCH.md insight). Verified: `! grep -q has_owner_pubkey db/sync/sync_protocol.cpp` passes.                                                                             |
| Fresh-namespace PUBK verify is explicit in Step 2 (not folded into register_owner_pubkey).      | Defence-in-depth: verify SHA3(embedded_sk) == target_namespace BEFORE verifying the signature AND BEFORE registering. A PUBK claiming a namespace it does not hash to is caught by Step 2's integrity check, independent of Step 1.5's has_owner_pubkey gate.                                            |
| Step 4.5 fires AFTER signature verify, not before.                                              | Registration must only happen for blobs that verify. Verifying AFTER extraction from a PUBK body does not leak information — the embedded signing_pk is what the blob was supposedly signed by. Catching Storage's throw on mismatch for D-04 translation is the idiomatic boundary.                       |
| delete_blob has NO PUBK-first gate.                                                             | Tombstones are never PUBKs. A delete to a namespace without a registered owner_pubkey falls through to `get_delegate_pubkey_by_hint` (which also misses) → `no_delegation` rejection. No semantic gap.                                                                                                    |
| Delegate-tombstone explicitly rejected in delete_blob (mirror ingest-path rule).                | Ingest rejects delegates that attempt `is_tombstone(blob.data)`. delete_blob must mirror this — else a delegate with a valid delegation could issue tombstones via the delete path, bypassing the owner-only rule.                                                                                        |
| [Rule 3] Storage::store_blob family widened to take target_namespace + PrecomputedBlob field.   | Plans 02/03 left storage.cpp's internal `store_blob` body reading `blob.namespace_id`, which no longer exists post-schema-change. Plan 04's verify gate requires chromatindb_lib to compile. Threading target_namespace through is the minimum viable fix; PrecomputedBlob grew a target_namespace field. |
| Alias `error_code_name(code)` added beside `error_code_string(code)`.                           | Plan verify gate used both spellings; codebase uses `_name` convention in other subsystems. Both resolve to the same switch.                                                                                                                                                                              |
| Hash-offload offloads use `std::span<const uint8_t>(arr.data(), arr.size())` explicit construct. | crypto::sha3_256 takes a dynamic-extent span. Explicit span avoids implicit-array-decay gotchas on some GCC versions and matches the existing code style in engine.cpp.                                                                                                                                  |

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 — Blocking] Widened Storage::store_blob family to take target_namespace**

- **Found during:** Task 2 verification (chromatindb_lib build gate).
- **Issue:** Plan 02 landed the Storage owner_pubkeys API + D-10 parameter
  rename but left the internal `store_blob` body reading `blob.namespace_id`
  throughout (lines 369, 383, 388, 423, 457, 458, 476, 477, 485, 495, 501
  + matching sites in `store_blobs_atomic`). The post-122 `wire::BlobData`
  struct has no `namespace_id` field (Plan 03). Plan 04's verify gate —
  `cmake --build build --target chromatindb_lib ... grep 'Built target'` — is
  unachievable without this cascade fix.
- **Fix:** Widened all three `Storage::store_blob` overloads to take
  `std::span<const uint8_t, 32> target_namespace` as the first parameter;
  replaced all 11 `blob.namespace_id` references inside `store_blob` +
  6 `pb.blob.namespace_id` references inside `store_blobs_atomic` with
  `target_namespace` / `pb.target_namespace`. Added `target_namespace` field
  to `PrecomputedBlob` so batch storage callers carry the ns per blob.
  Updated both engine.cpp call sites (`storage_.store_blob(target_namespace, ...)`
  in ingest, and `storage_.store_blob(target_namespace, delete_request)` in
  delete_blob).
- **Files modified:** `db/storage/storage.h`, `db/storage/storage.cpp`, plus
  the existing in-scope `db/engine/engine.cpp` and `db/engine/engine.h`.
- **Verification:** `engine.cpp.o` and `storage.cpp.o` both build cleanly
  (`build/db/CMakeFiles/chromatindb_lib.dir/engine/engine.cpp.o` + storage
  sibling exist). Downstream Plan 05 cascade targets (sync_protocol,
  blob_push_manager, message_dispatcher) still break — those are Plan 05's
  scope and were expected to break.
- **Committed in:** `7ea12c41` (with Task 2).

**2. [Rule 2 — Missing critical functionality] Added QuotaExceeded / CapacityExceeded branches to delete_blob's StoreResult switch**

- **Found during:** Task 2 (delete_blob refactor).
- **Issue:** The pre-122 delete_blob switch on `store_result.status` only
  handled Stored / Duplicate / Error. The widened `store_blob(target_namespace, blob)`
  overload delegates to the full-limits overload via `store_blob(target_namespace, blob, hash, encoded, 0, 0, 0)`, so CapacityExceeded / QuotaExceeded cannot
  fire on the zero-limit path today. But leaving the switch non-exhaustive would
  produce `-Wswitch` warnings if the surface ever widens. Added explicit
  branches returning `IngestError::storage_full` / `IngestError::quota_exceeded`
  for defensive exhaustiveness.
- **Files modified:** `db/engine/engine.cpp`.
- **Verification:** Exhaustive switch; no compiler warnings on
  `build/db/CMakeFiles/chromatindb_lib.dir/engine/engine.cpp.o`.
- **Committed in:** `7ea12c41` (with Task 2).

**3. [Rule 3 — Blocking] Added `<optional>` to test_helpers.h includes**

- **Found during:** Task 3 (adding `std::optional<std::array<uint8_t, 1568>>` default arg to `make_pubk_blob`).
- **Issue:** Previous `test_helpers.h` did not include `<optional>` — the new
  `make_pubk_blob` signature requires it.
- **Fix:** Added `#include <optional>` alongside existing `#include <random>`.
- **Files modified:** `db/tests/test_helpers.h`.
- **Verification:** Standalone smoke compile of `test_helpers.h` is clean.
- **Committed in:** `c905063a` (with Task 3).

### Observed (no action taken)

- **Full chromatindb_lib link still fails on Plan 05 cascade targets.** Expected;
  Plan 02/03 SUMMARYs explicitly document this as the intentional coordination
  signal. Post-Plan-05 landing, the link should close. After-task verification
  has instead relied on per-TU build (`engine.cpp.o` + `storage.cpp.o` produced
  successfully).
- **blob_push_manager.cpp is not listed in Plan 05's files_modified but breaks
  on the engine.ingest signature change.** Surfaced as a grep of Plan 05's
  frontmatter — Plan 05 will discover and fix it via its own compiler-guided
  sweep (Pitfall #5 "Update all callers atomically"). Documented here so Plan 05's
  executor doesn't spend time chasing the symptom.

## Issues Encountered

- Worktree base drift on startup: HEAD was at `a893aacc` (a 2000-file deletion
  from a different branch's history). Per `<worktree_branch_check>`, hard-reset
  to `762f13e6` placed the worktree at the expected base. Post-reset,
  `git log --oneline -3` confirmed the expected history (`762f13e6` →
  `81f62dc3` → `88a0972c`).
- Initial CMake configure took ~193s (asio + Catch2 + flatbuffers FetchContent
  on a fresh worktree).

## User Setup Required

None — no external service configuration required.

## Threat Flags

None new beyond the threat register already documented in the plan. No
files outside the planned set (plus the Storage fix from the Rule 3 deviation)
were created or modified.

## Known Stubs

None. The fresh-namespace PUBK verify branch in Step 2 is a complete path
(not a stub) — it validates SHA3(embedded_sk) == target_namespace before
verifying and before registering. Plan 06 will add test coverage for the
PUBK-first invariant and D-04 mismatch on top of this foundation.

## Next Phase Readiness

- **Plan 05 (sync_protocol / message_dispatcher / sync_orchestrator):** unblocked
  on engine.h surface. Compiler will guide the cascade: `engine_.ingest(blob, conn)`
  call sites in `sync_protocol.cpp:97`, `blob_push_manager.cpp:191`,
  `message_dispatcher.cpp` (Data + Delete branches) all need to be updated to
  pass `target_namespace` as a new first argument. The pre-122 `Data=8` dispatcher
  branch must be replaced with `BlobWrite=64` decoding `BlobWriteBody` (extract
  target_namespace + blob from envelope, pass both into `engine_.ingest`).
  Error-code mapping: `IngestError::pubk_first_violation` ↔ `ERROR_PUBK_FIRST_VIOLATION`;
  `IngestError::pubk_mismatch` ↔ `ERROR_PUBK_MISMATCH`.
- **Plan 06 (PUBK-first concurrency TSAN coverage):** unblocked. `make_pubk_blob`
  and the widened ingest/delete signatures are ready; the 6-scenario D-12
  coverage (a-f) is mechanically expressible with the Task 3 helpers.
- **Plan 07 (test sweep):** unblocked — compile errors in test_engine.cpp /
  test_sync_protocol.cpp / test_storage.cpp will list every stale `blob.namespace_id`
  / `blob.pubkey` / `engine.ingest(blob, ...)` call site.
- **Not blocking anyone on this plan's content** — all new contracts (engine API,
  PUBK-first gate, owner_pubkeys lookup, Step 4.5 registration, test helpers) are
  complete and compile cleanly.

## Self-Check

### Files verified to exist on disk

- **MODIFIED** `db/engine/engine.h` — IngestError + ingest + delete_blob widened; class doxygen rewritten.
- **MODIFIED** `db/engine/engine.cpp` — Step 1.5 / Step 2 / Step 3 / Step 4.5 / delete_blob refactored; all 6 crypto::offload sites paired with post-back-to-ioc.
- **MODIFIED** `db/peer/error_codes.h` — two new constants + switch arms; `error_code_name` alias added.
- **MODIFIED** `db/storage/storage.h` — store_blob triplet widened; PrecomputedBlob.target_namespace added.
- **MODIFIED** `db/storage/storage.cpp` — store_blob internal uses replaced; store_blobs_atomic uses pb.target_namespace.
- **MODIFIED** `db/tests/test_helpers.h` — 4 existing helpers reshaped; `make_pubk_blob` added; `<optional>` include added.
- **CREATED** `.planning/phases/122-schema-signing-cleanup-strip-namespace-and-compress-pubkey/122-04-SUMMARY.md` (this file).

### Commits verified via `git log --oneline 762f13e6..HEAD`

- FOUND: `2de56983` (Task 1)
- FOUND: `7ea12c41` (Task 2)
- FOUND: `c905063a` (Task 3)

### Plan automated gates

| Gate                                                                          | Expected | Actual | Status |
|-------------------------------------------------------------------------------|----------|--------|--------|
| `grep -q "pubk_first_violation" db/engine/engine.h`                           | present  | present| PASS   |
| `grep -q "pubk_mismatch" db/engine/engine.h`                                  | present  | present| PASS   |
| `grep -q "std::span<const uint8_t, 32> target_namespace" db/engine/engine.h`  | present  | present| PASS   |
| `grep -q "ERROR_PUBK_FIRST_VIOLATION = 0x07" db/peer/error_codes.h`           | present  | present| PASS   |
| `grep -q "ERROR_PUBK_MISMATCH        = 0x08" db/peer/error_codes.h`           | present  | present| PASS   |
| `grep -q '"pubk_first_violation"' db/peer/error_codes.h`                      | present  | present| PASS   |
| `! grep -qE "blob\.namespace_id\|blob\.pubkey" db/engine/engine.cpp`          | empty    | empty  | PASS   |
| `grep -q "has_owner_pubkey(target_namespace)" db/engine/engine.cpp`           | present  | present| PASS   |
| `grep -q "is_pubkey_blob(blob.data)" db/engine/engine.cpp`                    | present  | present| PASS   |
| `grep -q "register_owner_pubkey(blob.signer_hint" db/engine/engine.cpp`       | present  | present| PASS   |
| `grep -q "extract_pubk_signing_pk(blob.data)" db/engine/engine.cpp`           | present  | present| PASS   |
| `! grep -qE "blob\.namespace_id\|blob\.pubkey" db/tests/test_helpers.h`       | empty    | empty  | PASS   |
| `grep -q "make_pubk_blob" db/tests/test_helpers.h`                            | present  | present| PASS   |
| `grep -q "PUBKEY_DATA_SIZE" db/tests/test_helpers.h`                          | present  | present| PASS   |
| `grep -q "blob.signer_hint\|tombstone.signer_hint" db/tests/test_helpers.h`   | present  | present| PASS   |
| `grep -q "PUBKEY_MAGIC" db/tests/test_helpers.h`                              | present  | present| PASS   |
| Sync path has no duplicate PUBK-first check: `! grep -q "has_owner_pubkey" db/sync/sync_protocol.cpp` | empty | empty | PASS |
| engine.cpp.o builds cleanly                                                   | present  | present| PASS   |
| storage.cpp.o builds cleanly                                                  | present  | present| PASS   |

### Plan-level verify gate (relaxed)

- `cmake --build build --target chromatindb_lib ... grep "Built target chromatindb_lib"`:
  **FAILS** on Plan 05 cascade targets (`sync_protocol.cpp`, `message_dispatcher.cpp`,
  `blob_push_manager.cpp`). This was anticipated by plans 02/03 as the
  intentional coordination signal (see their SUMMARYs). Plan 04's in-scope
  TUs (`engine.cpp` + `storage.cpp`) both build cleanly in isolation.

## Self-Check: PASSED

Plan 04 objective — the keystone engine verify-path refactor — is complete.
Downstream compile cascade is Plan 05's territory as designed.

---
*Phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey*
*Plan: 04 (Wave 3, depends on Plans 1/2/3)*
*Completed: 2026-04-20*
