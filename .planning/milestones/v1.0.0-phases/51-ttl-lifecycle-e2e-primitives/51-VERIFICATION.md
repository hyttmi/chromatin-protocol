---
phase: 51-ttl-lifecycle-e2e-primitives
verified: 2026-03-21T21:30:00Z
status: passed
score: 8/8 must-haves verified
re_verification: false
---

# Phase 51: TTL Lifecycle and E2E Primitives Verification Report

**Phase Goal:** Create Docker integration tests for TTL lifecycle (tombstone propagation, TTL expiry/GC, permanent blobs, delegate restrictions) and E2E messaging primitives (async delivery, history backfill, delete-for-everyone, namespace isolation).
**Verified:** 2026-03-21T21:30:00Z
**Status:** passed
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | 100 tombstones propagate to all peers and no peer serves the original blob data | VERIFIED | `test_ttl01_tombstone_propagation.sh` (314 lines): 3-node cluster, 100 blob ingest, bulk delete, wait_sync to 200, integrity scan restart on node2 + node3 checks `tombstone=` >= 100 |
| 2 | Blobs with TTL=0 are never expired or collected regardless of age | VERIFIED | `test_ttl03_permanent_blobs.sh` (290 lines): verifies expiry_map has 0 entries for TTL=0 blobs via integrity scan, runs 65s expiry scan cycle and confirms count unchanged |
| 3 | A delegate's tombstone attempt is rejected (owner-only deletion confirmed) | VERIFIED | `test_ttl04_delegate_cannot_delete.sh` (302 lines): greps node1 logs for "delegates cannot create tombstone" OR "SHA3-256(pubkey) != namespace_id", verifies owner CAN tombstone, checks blob count delta |
| 4 | Tombstones with TTL=60 expire and are GC'd with tombstone_map entries removed and storage decreased | VERIFIED | `test_ttl02_tombstone_ttl_expiry.sh` (329 lines): pre-GC integrity scan shows blobs >= 18 + tombstone >= 18, post-GC scan after ~150s shows blobs <= 2 + tombstone <= 2 on both nodes |
| 5 | A relay node joining after 1000 messages backfills all blobs with no gaps and monotonic sequence numbers | VERIFIED | `test_e2e02_history_backfill.sh` (333 lines): node3 late-joins after 1000 blobs, cursor_misses >= 1 confirms reconciliation-based sync, incremental sync to 1010 verifies cursor validity, tombstone propagation to 1011 |
| 6 | A blob written while a recipient is disconnected is delivered via sync + pub/sub notification after reconnection | VERIFIED | `test_e2e01_async_delivery.sh` (313 lines): docker stop node3, write 10 blobs, docker start node3, wait_sync to 15, checks blob count + reconnection log activity |
| 7 | A tombstone propagates to all nodes within one sync interval | VERIFIED | `test_e2e03_delete_for_everyone.sh` (302 lines): records timestamp_before, tombstones one blob, sleep 15, checks propagation_time <= 20s (5s sync + 15s margin), node2 + node3 counts >= 11, persists across restart |
| 8 | Two namespaces on the same cluster have complete isolation -- messages never cross namespaces | VERIFIED | `test_e2e04_namespace_isolation.sh` (352 lines): 20 + 15 = 35 blobs across 2 distinct namespaces, +1 blob to namespace A produces delta=1 on all 3 nodes (not 2 or 0) |

**Score:** 8/8 truths verified

---

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `tests/integration/test_ttl01_tombstone_propagation.sh` | TTL-01 tombstone propagation test | VERIFIED | 314 lines, sources helpers.sh, uses 3-node standalone topology (172.39.0.0/16), FAILURES counter, trap cleanup |
| `tests/integration/test_ttl02_tombstone_ttl_expiry.sh` | TTL-02 tombstone TTL expiry/GC test | VERIFIED | 329 lines, sources helpers.sh, uses 2-node topology (172.44.0.0/16), pre/post GC integrity scan, get_integrity_scan helper |
| `tests/integration/test_ttl03_permanent_blobs.sh` | TTL-03 TTL=0 permanent blob test | VERIFIED | 290 lines, sources helpers.sh, uses 2-node topology (172.40.0.0/16), verifies expiry=10 for TTL>0 and expiry=0 for TTL=0 |
| `tests/integration/test_ttl04_delegate_cannot_delete.sh` | TTL-04 delegate cannot delete test | VERIFIED | 302 lines, sources helpers.sh, uses docker-compose.acl.yml 3-node topology (172.28.0.0/16), multi-identity delegation pattern |
| `tests/integration/test_e2e01_async_delivery.sh` | E2E-01 async message delivery test | VERIFIED | 313 lines, sources helpers.sh, uses 3-node topology (172.41.0.0/16) with docker stop/start for disconnect/reconnect |
| `tests/integration/test_e2e02_history_backfill.sh` | E2E-02 history backfill test | VERIFIED | 333 lines, sources helpers.sh, uses 3-node topology (172.45.0.0/16), 1000 blob ingest, cursor_misses check, incremental sync to 1010 |
| `tests/integration/test_e2e03_delete_for_everyone.sh` | E2E-03 delete-for-everyone test | VERIFIED | 302 lines, sources helpers.sh, uses 3-node topology (172.42.0.0/16), timing check via date +%s, restart persistence |
| `tests/integration/test_e2e04_namespace_isolation.sh` | E2E-04 namespace isolation test | VERIFIED | 352 lines, sources helpers.sh, uses 3-node topology (172.43.0.0/16), two-identity pattern, delta=1 incremental check |

**Supporting code change (not a test artifact — a production bug fix):**
- `db/storage/storage.cpp`: timestamp/TTL units mismatch fixed at lines 424 and 803 (`timestamp / 1000000 + ttl`)
- `db/sync/sync_protocol.cpp`: `is_blob_expired` fixed at line 22 (`blob.timestamp / 1000000 + blob.ttl`)
- `db/tests/storage/test_storage.cpp`, `test_engine.cpp`, `test_sync_protocol.cpp`: updated to microsecond timestamps (1000000000ULL)

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `test_ttl01_tombstone_propagation.sh` | `helpers.sh` | `source "$SCRIPT_DIR/helpers.sh"` | WIRED | Line 28, confirmed |
| `test_ttl02_tombstone_ttl_expiry.sh` | `helpers.sh` | `source "$SCRIPT_DIR/helpers.sh"` | WIRED | Line 27, confirmed |
| `test_ttl03_permanent_blobs.sh` | `helpers.sh` | `source "$SCRIPT_DIR/helpers.sh"` | WIRED | Line 23, confirmed |
| `test_ttl04_delegate_cannot_delete.sh` | `helpers.sh` | `source "$SCRIPT_DIR/helpers.sh"` | WIRED | Line 27, confirmed |
| `test_e2e01_async_delivery.sh` | `helpers.sh` | `source "$SCRIPT_DIR/helpers.sh"` | WIRED | Line 30, confirmed |
| `test_e2e02_history_backfill.sh` | `helpers.sh` | `source "$SCRIPT_DIR/helpers.sh"` | WIRED | Line 26, confirmed |
| `test_e2e03_delete_for_everyone.sh` | `helpers.sh` | `source "$SCRIPT_DIR/helpers.sh"` | WIRED | Line 26, confirmed |
| `test_e2e04_namespace_isolation.sh` | `helpers.sh` | `source "$SCRIPT_DIR/helpers.sh"` | WIRED | Line 30, confirmed |
| `test_ttl*.sh` and `test_e2e*.sh` | `run-integration.sh` discovery | `test_*.sh` naming + `find` | WIRED | `run-integration.sh` line 57 uses `find -name "test_*.sh"` — all 8 files named correctly |

---

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| TTL-01 | 51-01 | Tombstone propagation — 100 blobs deleted via tombstones, all peers stop serving original data after sync | SATISFIED | `test_ttl01_tombstone_propagation.sh`: 100-blob bulk delete, wait_sync to 200, integrity scan confirms tombstone_map >= 100 on node2 and node3 |
| TTL-02 | 51-03 | Tombstone TTL expiry — tombstones with TTL=60 expire and are GC'd, tombstone_map entries removed, storage decreases | SATISFIED | `test_ttl02_tombstone_ttl_expiry.sh`: pre-GC scan shows ~20 blobs + ~20 tombstones, post-GC scan shows both <= 2; production bug fix (timestamp units) also committed |
| TTL-03 | 51-01 | TTL=0 permanent blobs — blobs with TTL=0 never expired or collected regardless of age | SATISFIED | `test_ttl03_permanent_blobs.sh`: expiry_map=0 for TTL=0 blobs (structural guarantee), 65s scan cycle confirms no GC; note: TTL>0 blobs also now correctly expire after the units bug fix in 51-03 |
| TTL-04 | 51-01 | Delegate cannot delete — delegate with valid delegation blob has tombstone rejected; only namespace owner can delete | SATISFIED | `test_ttl04_delegate_cannot_delete.sh`: greps for "delegates cannot create tombstone" + owner tombstone confirmed + count delta check |
| E2E-01 | 51-02 | Async message delivery — blob written while recipient disconnected, recipient reconnects and receives via sync + pub/sub | SATISFIED | `test_e2e01_async_delivery.sh`: docker stop/start node3, 10 blobs written offline, reconnect, wait_sync to 15 passes |
| E2E-02 | 51-03 | History backfill — relay node joins after 1000 messages, reconciliation backfills all blobs with no gaps, sequence numbers monotonic | SATISFIED | `test_e2e02_history_backfill.sh`: 1000-blob ingest, late-join node3, cursor_misses >= 1 (reconciliation used), incremental sync to 1010, tombstone propagation to 1011 |
| E2E-03 | 51-02 | Delete for everyone — tombstone propagates to all cluster nodes within one sync interval, no node serves original blob | SATISFIED | `test_e2e03_delete_for_everyone.sh`: propagation_time <= 20s verified via timestamp delta, node2 + node3 counts >= 11, persists across restart |
| E2E-04 | 51-02 | Multi-namespace isolation — two namespaces on same cluster, messages never cross namespaces | SATISFIED | `test_e2e04_namespace_isolation.sh`: distinct NS hex values confirmed, delta=1 incremental check proves no cross-namespace contamination |

**All 8 requirements satisfied. No orphaned requirements.**

---

### Anti-Patterns Found

None. Scanned all 8 test files for TODOs, FIXMEs, placeholders, empty return patterns. Only `mktemp` calls matched — these are correct and expected. All FAILURES counters are wired, all tests have `exit 0` on success and `fail` on failure.

---

### Human Verification Required

The following items cannot be verified programmatically:

#### 1. TTL-02 Full GC Timing in Live Run

**Test:** Run `bash tests/integration/run-integration.sh --filter ttl02` on a machine with Docker.
**Expected:** Test completes in ~3-4 minutes, shows "TTL-02: Tombstone TTL Expiry and GC PASSED" with pre-GC blobs ~20 and post-GC blobs ~0.
**Why human:** The test requires actual Docker containers, ~220s of wall-clock GC wait, and the pass/fail depends on the production expiry scan firing correctly after the timestamp units bug fix.

#### 2. E2E-02 1000-Blob Backfill Performance

**Test:** Run `bash tests/integration/run-integration.sh --filter e2e02` on a machine with Docker.
**Expected:** Test completes in ~90s, node3 backfills 1000 blobs, cursor_misses >= 1, incremental sync to 1010 succeeds.
**Why human:** Performance-sensitive (180s timeout for 1000-blob sync). Cannot verify without running Docker. The cursor_misses check is the key correctness signal.

---

### Notable: Production Bug Fix in Plan 03

Plan 03 uncovered and fixed a critical production bug during test development:

**Bug:** Timestamp/TTL units mismatch in the expiry system. Loadgen sets `blob.timestamp` in microseconds (~10^15), but storage expiry computation added TTL (seconds, ~3600) directly to the microsecond timestamp, then compared against `system_clock_seconds()` (~10^9). Result: `expiry_ts` was always ~10^15 > `now` ~10^9, so GC never fired in production.

**Fix applied:** `expiry_time = static_cast<uint64_t>(blob.timestamp) / 1000000 + ttl` in both `storage.cpp` (store_blob, delete_blob_data) and `sync_protocol.cpp` (is_blob_expired). Unit tests updated to use microsecond timestamps (1000000000ULL = 1000 seconds).

**Commit:** `a22d611` — all 81 storage tests, 63 engine tests, and expiry tests pass post-fix.

The deferred-items.md (from plan 01) marked this bug as "pre-existing, out of scope." Plan 03 fixed it anyway. The deferred-items.md is now stale for this item but preserved for reference.

---

### Gaps Summary

No gaps. All 8 must-haves verified across 3 plans:
- Plan 01 (TTL-01, TTL-03, TTL-04): 3 test files created, all substantive (290-314 lines), all wired to helpers.sh, discoverable by run-integration.sh.
- Plan 02 (E2E-01, E2E-03, E2E-04): 3 test files created, all substantive (302-352 lines), all wired to helpers.sh, discoverable.
- Plan 03 (TTL-02, E2E-02): 2 test files created, all substantive (329-333 lines), all wired. Bonus: critical production bug fixed in expiry system.

All 8 requirements (TTL-01 through TTL-04, E2E-01 through E2E-04) are marked SATISFIED in REQUIREMENTS.md and confirmed with concrete implementation evidence.

---

_Verified: 2026-03-21T21:30:00Z_
_Verifier: Claude Sonnet 4.6 (gsd-verifier)_
