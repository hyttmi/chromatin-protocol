---
phase: 34-sync-resumption
verified: 2026-03-18T05:30:00Z
status: passed
score: 11/11 must-haves verified
re_verification:
  previous_status: gaps_found
  previous_score: 9/11
  gaps_closed:
    - "Tombstone propagates between two connected nodes via sync within 15 seconds (ctest #213 passes)"
    - "Tombstone ingest triggers notification with is_tombstone=true via sync (ctest #210 passes)"
    - "All 337 tests pass with zero regressions"
  gaps_remaining: []
  regressions: []
---

# Phase 34: Sync Resumption Verification Report

**Phase Goal:** Cursor-based sync resumption — skip unchanged namespaces to reduce sync bandwidth and CPU usage while preserving correctness.
**Verified:** 2026-03-18T05:30:00Z
**Status:** PASSED
**Re-verification:** Yes — after gap closure (Plan 03)

## Goal Achievement

### Observable Truths

| #  | Truth | Status | Evidence |
|----|-------|--------|----------|
| 1  | Cursor CRUD (get/set/delete) work correctly for per-peer per-namespace entries | VERIFIED | `test_storage.cpp` lines 1446-1679: 13 cursor test cases pass, 14 cursor-named tests pass |
| 2  | Cursors persist across Storage reopen (libmdbx sub-database survives restart) | VERIFIED | `test_storage.cpp` restart persistence test passes; cursor_map created with max_maps=7 |
| 3  | Round counters can be bulk-reset to zero for all cursors (SIGHUP support) | VERIFIED | `reset_all_round_counters()` in storage.cpp; test_sync_protocol.cpp line 1051; peer_manager.cpp line 1308 on SIGHUP |
| 4  | Startup cleanup scan deletes cursors for unknown peer hashes | VERIFIED | `cleanup_stale_cursors()` in storage.cpp; startup code in peer_manager.cpp lines 157-176 |
| 5  | Config fields full_resync_interval and cursor_stale_seconds parse from JSON with defaults | VERIFIED | config.h lines 25-26 (defaults 10, 3600); config.cpp lines 37-38; 5 config tests pass |
| 6  | After a full sync round completes, a subsequent round with no new blobs exchanges zero hashes for unchanged namespaces | VERIFIED | cursor_skip_namespaces in Phase C of run_sync_with_peer and handle_sync_as_responder; cursor_hits metric incremented; tested in test_sync_protocol.cpp |
| 7  | A periodic full hash-diff resync triggers every Nth round (configurable, default 10) | VERIFIED | check_full_resync() in peer_manager.cpp lines 595-605; tested in test_sync_protocol.cpp lines 931-986 |
| 8  | SIGHUP forces full resync on next round without wiping cursor seq_nums | VERIFIED | reload_config() calls reset_all_round_counters() (line 1308); preserves seq_num and last_sync_timestamp |
| 9  | Time gap exceeding cursor_stale_seconds forces full resync | VERIFIED | check_full_resync() lines 601-603; tested in test_sync_protocol.cpp lines 959-986 |
| 10 | Cursor mismatch (remote seq < stored cursor) resets that namespace's cursor only | VERIFIED | peer_manager.cpp lines 711-715: delete_sync_cursor for mismatched namespace only; test_sync_protocol.cpp line 988 |
| 11 | Sync cursors survive node restart and resume where they left off — tombstone propagation correct | VERIFIED | ctest #210 and #213 both PASS (25s each); root-cause fix in storage.cpp commit ff6a70e |

**Score:** 11/11 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/storage/storage.h` | SyncCursor struct, cursor CRUD API, updated delete_blob_data doc | VERIFIED | SyncCursor struct, 7 cursor methods; doc comment updated to reflect zero-hash sentinel behavior |
| `db/storage/storage.cpp` | cursor_map sub-db, CRUD, zero-hash sentinel in delete_blob_data, filter in get_hashes_by_namespace | VERIFIED | commit ff6a70e: zero_hash sentinel on deletion (line 718-720); memcmp filter in get_hashes_by_namespace (line 566) |
| `db/config/config.h` | full_resync_interval and cursor_stale_seconds fields | VERIFIED | Lines 25-26 with defaults (10, 3600) |
| `db/config/config.cpp` | JSON parsing for new config fields | VERIFIED | Lines 37-38 using j.value() pattern |
| `db/tests/storage/test_storage.cpp` | Cursor CRUD tests, persistence tests, cleanup scan tests | VERIFIED | Lines 1446-1679: 13 cursor test cases, all pass |
| `db/peer/peer_manager.h` | cursor_hits, cursor_misses, full_resyncs in NodeMetrics; FullResyncReason enum; check_full_resync | VERIFIED | Lines 72-74 (metrics), line 35 (pubkey_hash), line 247 (enum), line 248 (check_full_resync), lines 281-282 (config members) |
| `db/peer/peer_manager.cpp` | Cursor-aware sync logic, SIGHUP reset, startup cleanup | VERIFIED | Lines 668-871 (initiator), 943-1135 (responder); tombstone regression fixed at storage layer |
| `db/tests/sync/test_sync_protocol.cpp` | Cursor integration tests | VERIFIED | Lines 828-1124: 8 cursor test cases; all pass |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `db/storage/storage.cpp` | libmdbx cursor_map | `txn.create_map("cursor")` | VERIFIED | cursor_map handle created; CRUD uses impl_->cursor_map |
| `db/config/config.cpp` | `db/config/config.h` | `j.value("full_resync_interval")` / `j.value("cursor_stale_seconds")` | VERIFIED | Lines 37-38 parse both fields |
| `db/peer/peer_manager.cpp` | `db/storage/storage.h` | `storage_.get_sync_cursor() / set_sync_cursor()` | VERIFIED | Lines 683, 704, 865, 870 (initiator); lines 958, 978, 1122, 1127 (responder) |
| `db/peer/peer_manager.cpp` | `db/config/config.h` | `config_.full_resync_interval / cursor_stale_seconds` | VERIFIED | Lines 92-93 (constructor init), lines 1305-1306 (reload_config), lines 597, 601 (check_full_resync) |
| `db/peer/peer_manager.cpp (reload_config)` | `db/storage/storage.h` | `storage_.reset_all_round_counters()` | VERIFIED | Line 1308 in reload_config(); called on SIGHUP |
| `db/storage/storage.cpp (delete_blob_data)` | seq_map monotonicity | zero-hash sentinel upsert | VERIFIED | commit ff6a70e lines 718-720: upsert preserves seq slot; get_hashes_by_namespace filters zero entries at line 566 |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| SYNC-01 | 34-01, 34-02, 34-03 | Per-peer per-namespace seq_num cursors tracked during sync rounds | SATISFIED | SyncCursor struct; cursor update after each successful sync round in both sync functions; seq_num monotonicity guaranteed by zero-hash sentinel fix |
| SYNC-02 | 34-02, 34-03 | Hash-list exchange skipped for namespaces where remote seq_num equals cursor | SATISFIED | cursor_skip_namespaces in Phase C correctly skips only when cursor.seq_num == pns.latest_seq_num; tombstone seq_num is now always > prior blob seq_num |
| SYNC-03 | 34-01 | Sync cursors persisted across node restarts via dedicated libmdbx sub-database | SATISFIED | cursor_map as 6th MDBX sub-database; restart persistence tested; 337/337 E2E tests pass |
| SYNC-04 | 34-02 | Periodic full hash-diff resync as fallback (configurable interval, default every 10th round) | SATISFIED | check_full_resync() with full_resync_interval_ (default 10) and cursor_stale_seconds_ (default 3600); full_resyncs metric counted |

All 4 SYNC requirements fully satisfied. No orphaned requirements.

### Test Results

| Test Suite | Pre-Phase 34 | Post-Plan 02 | Post-Plan 03 (final) |
|------------|-------------|--------------|----------------------|
| ctest total | 313/313 | 335/337 | 337/337 |
| cursor tests (by name) | N/A | 14/14 | 14/14 |
| ctest #210 (tombstone notification) | PASS | FAIL (SIGSEGV) | PASS |
| ctest #213 (tombstone propagation) | PASS | FAIL (SIGSEGV) | PASS |

### Anti-Patterns Found

None. No TODO/FIXME/HACK/placeholder comments in any modified files (verified on storage.cpp and storage.h).

### Gap Closure Analysis

**Gap from initial verification:** ctest #210 and #213 failed with SIGSEGV after tombstone sync assertion failure.

**Root cause:** `delete_blob_data` in `storage.cpp` erased seq_map entries when a blob was deleted. This allowed `next_seq_num()` to recycle the same sequence number for the tombstone replacing the deleted blob. The cursor comparison (`cursor.seq_num == pns.latest_seq_num`) saw an identical seq_num and classified the namespace as a cursor HIT, causing the tombstone to be skipped in Phase C.

**Fix (commit ff6a70e, 2 files changed, 19 insertions, 7 deletions):**

1. `db/storage/storage.cpp`: `delete_blob_data` writes a zero-hash sentinel (32 zero bytes) into the seq_map entry instead of erasing it. This preserves the seq_num slot so the tombstone receives seq=N+1 rather than reusing seq=N.

2. `db/storage/storage.cpp`: `get_hashes_by_namespace` filters zero-hash sentinels via a 32-byte memcmp. Ghost entries are never included in sync hash lists sent to peers.

3. `db/storage/storage.h`: Doc comment on `delete_blob_data` updated to reflect sentinel behavior.

**Fix location:** Storage layer (root cause) rather than peer_manager.cpp (symptom). Any future consumer of seq_num monotonicity benefits automatically.

**No regressions:** 337/337 tests pass. `list_namespaces` is unaffected — it reads seq_map keys for seq_num extraction, not values, so zero-hash sentinels in values do not affect namespace listing.

---

_Verified: 2026-03-18T05:30:00Z_
_Verifier: Claude (gsd-verifier)_
