---
phase: 23-ttl-flexibility
verified: 2026-03-14T17:00:00Z
status: passed
score: 6/6 must-haves verified
re_verification: false
---

# Phase 23: TTL Flexibility Verification Report

**Phase Goal:** Writers control blob lifetime via TTL in signed data (no hardcoded constant), and expired tombstones are garbage-collected
**Verified:** 2026-03-14T17:00:00Z
**Status:** passed
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| #   | Truth                                                                                        | Status     | Evidence                                                                                                              |
| --- | -------------------------------------------------------------------------------------------- | ---------- | --------------------------------------------------------------------------------------------------------------------- |
| 1   | BLOB_TTL_SECONDS constexpr no longer exists (writer-controlled TTL replaces it)              | VERIFIED   | `db/config/config.h` has no BLOB_TTL_SECONDS; `grep -r BLOB_TTL_SECONDS db/ tests/` returns zero matches           |
| 2   | run_expiry_scan() reads blob data before deleting from blobs_map                             | VERIFIED   | Lines 661-665 of `db/storage/storage.cpp`: `txn.get(impl_->blobs_map, ...)` then `wire::decode_blob(...)` before erase |
| 3   | run_expiry_scan() detects expired tombstones via is_tombstone() and cleans tombstone_map     | VERIFIED   | Lines 666-674 of `db/storage/storage.cpp`: `wire::is_tombstone(decoded.data)` → `txn.erase(impl_->tombstone_map, ...)` |
| 4   | Tombstones with TTL>0 get expiry entries via existing store_blob() code path (no change)    | VERIFIED   | Lines 240-247 of `db/storage/storage.cpp`: `if (blob.ttl > 0)` creates expiry entry for any blob including tombstones |
| 5   | Tombstones with TTL=0 remain permanent (existing behavior preserved)                         | VERIFIED   | Test #139 "Storage tombstone with TTL=0 is never expired" passes; no expiry entry written for TTL=0                  |
| 6   | Regular blob expiry is unaffected by the scan reorder                                        | VERIFIED   | Test #140 "Storage regular blob expiry unaffected by tombstone scan" passes; blobs_map erase still occurs             |

**Score:** 6/6 truths verified

### Required Artifacts

| Artifact                          | Expected                                                           | Status     | Details                                                                                     |
| --------------------------------- | ------------------------------------------------------------------ | ---------- | ------------------------------------------------------------------------------------------- |
| `db/config/config.h`              | BLOB_TTL_SECONDS removed                                           | VERIFIED   | File contains only Config struct + function declarations. No BLOB_TTL_SECONDS anywhere.     |
| `db/storage/storage.cpp`          | run_expiry_scan() reads blob before delete, cleans tombstone_map   | VERIFIED   | Lines 659-682: read-before-delete pattern with `is_tombstone` check and `tombstone_map` erase |
| `tests/config/test_config.cpp`    | BLOB_TTL_SECONDS test removed                                      | VERIFIED   | File has no mention of BLOB_TTL_SECONDS (315 lines, clean)                                 |
| `tests/storage/test_storage.cpp`  | Tombstone expiry tests present                                     | VERIFIED   | Three new test cases at lines 1161-1254: TTL>0 expiry, TTL=0 permanent, regular blob regression |

### Key Link Verification

| From                                      | To                              | Via                                          | Status   | Details                                                                                       |
| ----------------------------------------- | ------------------------------- | -------------------------------------------- | -------- | --------------------------------------------------------------------------------------------- |
| `db/storage/storage.cpp run_expiry_scan()` | `db/wire/codec.h`               | `decode_blob()` and `is_tombstone()` to detect tombstones during expiry | WIRED    | Line 664: `wire::decode_blob(...)`, line 666: `wire::is_tombstone(decoded.data)` — both called |
| `db/storage/storage.cpp run_expiry_scan()` | `db/storage/storage.cpp store_blob()` | Expiry entries created for tombstones with TTL>0 consumed by scan | WIRED    | `store_blob()` lines 240-247 create expiry entry for any TTL>0 blob; `run_expiry_scan()` lines 634-692 consumes via `expiry_map` cursor |

### Requirements Coverage

| Requirement | Source Plan | Description                                                                                        | Status    | Evidence                                                                                     |
| ----------- | ----------- | -------------------------------------------------------------------------------------------------- | --------- | -------------------------------------------------------------------------------------------- |
| TTL-01      | 23-01-PLAN  | Blob TTL is set by the writer (included in signed blob data), not a hardcoded constant             | SATISFIED | BLOB_TTL_SECONDS removed from `db/config/config.h`; no constexpr anywhere in `db/` or `tests/` |
| TTL-03      | 23-01-PLAN  | TTL=0 remains valid and means permanent (no expiry)                                                | SATISFIED | Test #105 "Storage TTL=0 blobs are never purged" + Test #139 "Storage tombstone with TTL=0 is never expired" — both pass |
| TTL-04      | 23-01-PLAN  | Tombstone TTL is writer-controlled (set in signed tombstone data) — tombstones with TTL>0 expire naturally | SATISFIED | Test #138 "Storage tombstone with TTL>0 is expired by expiry scan" passes; tombstone stored with ttl=3600 expires at clock+3601 |
| TTL-05      | 23-01-PLAN  | Expired tombstones are garbage collected by the existing expiry scan, including tombstone_map cleanup | SATISFIED | Test #138 verifies `has_tombstone_for` returns false after `run_expiry_scan()`; `tombstone_map` erase confirmed at storage.cpp line 671 |

All four requirement IDs declared in the PLAN frontmatter (`requirements: [TTL-01, TTL-03, TTL-04, TTL-05]`) are accounted for. There is no TTL-02 requirement in REQUIREMENTS.md; the numbering gap is intentional (TTL-02 is absent from the requirements list, not an orphan).

Requirements marked `[x]` in REQUIREMENTS.md for all four IDs. No orphaned requirements for Phase 23 found.

### Anti-Patterns Found

None detected. Scanned `db/config/config.h`, `db/storage/storage.cpp`, `tests/config/test_config.cpp`, `tests/storage/test_storage.cpp`:
- No TODO/FIXME/PLACEHOLDER comments
- No empty or stub implementations
- No hardcoded fallback TTL values introduced
- No `return null` / `return {}` stubs

### Human Verification Required

None. All observable truths are verifiable programmatically:
- Constant removal: grep-confirmed zero matches
- Logic correctness: covered by Catch2 unit tests with injectable clock
- Test counts: 286 total, 5 TTL/tombstone-expiry tests all pass

### Build and Test Verification

- Build: clean, zero errors, zero warnings (cmake --build confirms `[100%] Built target chromatindb_tests`)
- Commit `cc05dca` exists and is the implementation commit
- Test run: 5/5 targeted TTL+tombstone tests pass; 286/286 total tests confirmed in suite
- BLOB_TTL_SECONDS: zero references in `db/` and `tests/` directories (only in archived milestone planning docs)

### Gaps Summary

No gaps. All six must-have truths verified, all four artifacts pass all three levels (exists, substantive, wired), both key links confirmed wired, all four requirements satisfied with test evidence.

---

_Verified: 2026-03-14T17:00:00Z_
_Verifier: Claude (gsd-verifier)_
