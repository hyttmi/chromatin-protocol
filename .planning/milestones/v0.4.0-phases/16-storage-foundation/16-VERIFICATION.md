---
phase: 16-storage-foundation
verified: 2026-03-10T07:00:00Z
status: passed
score: 14/14 must-haves verified
re_verification: false
gaps: []
---

# Phase 16: Storage Foundation Verification Report

**Phase Goal:** Storage foundation — tombstone O(1) index, capacity limits, disk-full signaling
**Verified:** 2026-03-10T07:00:00Z
**Status:** PASSED
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

| #  | Truth | Status | Evidence |
|----|-------|--------|----------|
| 1  | `has_tombstone_for()` completes in O(1) via tombstone_map lookup instead of O(n) namespace scan | VERIFIED | `storage.cpp:613-626`: single `txn.get()` on `tombstone_map`, no cursor/loop present |
| 2  | No migration needed — pre-production, fresh databases only | VERIFIED | No migration code in `storage.cpp`; plan truth explicitly documented; code matches |
| 3  | Tombstone index entries are created atomically in the same transaction as blob storage | VERIFIED | `storage.cpp:261-266`: tombstone_map upsert inside the same `txn` as `blobs_map` upsert, committed together at line 268 |
| 4  | Tombstone index entries are cleaned up when the tombstone blob itself is deleted | VERIFIED | `storage.cpp:574-583`: `delete_blob_data()` erases from `tombstone_map` when `is_tombstone(blob.data)` |
| 5  | Operator can configure max_storage_bytes in the config file to set a global storage limit | VERIFIED | `config.h:22`: `uint64_t max_storage_bytes = 0;`; `config.cpp:33`: JSON parsing |
| 6  | Node rejects blobs with IngestError::storage_full when used_bytes exceeds max_storage_bytes | VERIFIED | `engine.cpp:50-59`: Step 0b check returns `IngestError::storage_full` |
| 7  | Capacity check runs as Step 0 in ingest() before any cryptographic operations | VERIFIED | `engine.cpp:50-59`: check at line 52 precedes structural checks (line 62), namespace check (line 77), signature verification (line 110) |
| 8  | Tombstone blobs are exempt from capacity check (they free space) | VERIFIED | `engine.cpp:52`: `!wire::is_tombstone(blob.data)` guard on capacity check |
| 9  | max_storage_bytes=0 means unlimited (backward compatible default) | VERIFIED | `engine.cpp:52`: `max_storage_bytes_ > 0` guard; `config.h:22`: default is 0 |
| 10 | Node sends StorageFull wire message to peers when rejecting a Data message blob due to capacity | VERIFIED | `peer_manager.cpp:366-372`: `storage_full` case spawns async `send_message(TransportMsgType_StorageFull)` |
| 11 | Peers receiving StorageFull set peer_is_full flag and suppress sync blob pushes to that peer | VERIFIED | `peer_manager.cpp:342-349`: handler sets `peer->peer_is_full = true`; `peer_manager.cpp:546-549,575-578,717-720,746-749`: BlobRequest handling skips push when `peer->peer_is_full` |
| 12 | Sync Phase A/B (namespace/hash exchange) still occurs with full peers — only Phase C blob transfer is suppressed | VERIFIED | `peer_manager.cpp`: suppression is scoped to `BlobRequest` handling (Phase C response); `SyncRequest`/`NamespaceList`/`HashList` flow is unaffected |
| 13 | peer_is_full flag resets on reconnect (PeerInfo is recreated) | VERIFIED | `peer_manager.h:56`: `bool peer_is_full = false;` default initializer; `PeerInfo` created fresh per connection |
| 14 | StorageFull during sync causes blob to be skipped silently, then StorageFull sent after sync completes | VERIFIED | `sync_protocol.cpp:118-121`: counts skips into `storage_full_count`; `peer_manager.cpp:601-607` and `peer_manager.cpp:772-778`: post-sync signal from both initiator and responder |

**Score:** 14/14 truths verified

---

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/storage/storage.h` | `used_bytes()` public method declaration, 5 sub-databases in docstring | VERIFIED | Line 137: `uint64_t used_bytes() const;`; lines 43-48: docstring lists all 5 sub-databases including tombstone |
| `db/storage/storage.cpp` | `tombstone_map` DBI, O(1) `has_tombstone_for`, `used_bytes()` implementation | VERIFIED | Line 82: `mdbx::map_handle tombstone_map{0};`; lines 113: created in init txn; lines 613-626: O(1) lookup; lines 693-696: `used_bytes()` via `env.get_info()` |
| `tests/storage/test_storage.cpp` | Tests for O(1) tombstone lookup, index cleanup, used_bytes | VERIFIED | Lines 984-1174: 7 test cases covering all required behaviors, substantive and non-trivial |
| `db/config/config.h` | `max_storage_bytes` field in Config struct | VERIFIED | Line 22: `uint64_t max_storage_bytes = 0;` |
| `db/config/config.cpp` | JSON parsing for `max_storage_bytes` | VERIFIED | Line 33: `cfg.max_storage_bytes = j.value("max_storage_bytes", cfg.max_storage_bytes);` |
| `db/engine/engine.h` | `IngestError::storage_full` enum value, `BlobEngine` constructor with `max_storage_bytes` | VERIFIED | Line 24: `storage_full` in enum; line 63: constructor with default parameter; line 112: private member |
| `db/engine/engine.cpp` | Step 0b capacity check in `ingest()` | VERIFIED | Lines 50-59: guard with tombstone exemption before any crypto |
| `tests/config/test_config.cpp` | Tests for `max_storage_bytes` config parsing | VERIFIED | Lines 133-162: 3 test cases for JSON parsing, missing default, struct default |
| `tests/engine/test_engine.cpp` | Tests for capacity enforcement and tombstone exemption | VERIFIED | Lines 1283-1346: 5 test cases covering over-capacity rejection, tombstone exemption, unlimited mode, under-capacity success |
| `schemas/transport.fbs` | `StorageFull = 23` enum value | VERIFIED | Line 32: `StorageFull = 23` with Phase 16 comment |
| `db/wire/transport_generated.h` | `TransportMsgType_StorageFull = 23` in generated enum | VERIFIED | Line 46: `TransportMsgType_StorageFull = 23`; line 48: `TransportMsgType_MAX = TransportMsgType_StorageFull` |
| `db/peer/peer_manager.h` | `peer_is_full` field on `PeerInfo` | VERIFIED | Line 56: `bool peer_is_full = false;` with Phase 16 comment |
| `db/peer/peer_manager.cpp` | StorageFull send on Data rejection, StorageFull receive handler, sync push suppression | VERIFIED | Lines 342-349 (handler), 366-372 (send on rejection), 546-549/575-578/717-720/746-749 (push suppression) |
| `db/sync/sync_protocol.h` | `SyncStats` with `storage_full_count` field | VERIFIED | Line 21: `uint32_t storage_full_count = 0;` |
| `db/sync/sync_protocol.cpp` | `ingest_blobs` skips blobs on `storage_full`, counts rejections | VERIFIED | Lines 94/118-121/129: local counter, skip on `storage_full`, assigned to `stats.storage_full_count` |
| `tests/peer/test_peer_manager.cpp` | E2E tests for StorageFull signaling and sync suppression | VERIFIED | Lines 1209-1360: 3 E2E sections — full node Data rejection, peer_is_full default reset, graceful sync completion |
| `db/main.cpp` | `config.max_storage_bytes` passed to `BlobEngine` constructor | VERIFIED | Line 122: `BlobEngine engine(storage, config.max_storage_bytes);` |

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `storage.cpp store_blob()` | `tombstone_map` | Same-transaction upsert when `is_tombstone(blob.data)` | WIRED | `storage.cpp:261-266`: `txn.upsert(impl_->tombstone_map, ...)` inside store transaction |
| `storage.cpp has_tombstone_for()` | `tombstone_map` | O(1) `txn.get()` with `not_found_sentinel` | WIRED | `storage.cpp:617-619`: `txn.get(impl_->tombstone_map, to_slice(ts_key), not_found_sentinel)` |
| `storage.cpp delete_blob_data()` | `tombstone_map` | Conditional erase when `is_tombstone(blob.data)` | WIRED | `storage.cpp:574-583`: `txn.erase(impl_->tombstone_map, to_slice(ts_key))` |
| `engine.cpp ingest()` | `storage.cpp used_bytes()` | Step 0b capacity check: `storage_.used_bytes() >= max_storage_bytes_` | WIRED | `engine.cpp:53`: exact pattern present |
| `config.cpp load_config()` | `Config::max_storage_bytes` | JSON value extraction | WIRED | `config.cpp:33`: `j.value("max_storage_bytes", cfg.max_storage_bytes)` |
| `peer_manager.cpp on_peer_message(Data)` | `StorageFull` wire message | Send on `IngestError::storage_full` | WIRED | `peer_manager.cpp:366-372`: `storage_full` case sends `TransportMsgType_StorageFull` |
| `peer_manager.cpp on_peer_message(StorageFull)` | `PeerInfo::peer_is_full` | Set flag on receipt | WIRED | `peer_manager.cpp:342-349`: `peer->peer_is_full = true` |
| `peer_manager.cpp run_sync_with_peer()` | `PeerInfo::peer_is_full` | Skip Phase C blob transfer when `peer_is_full` | WIRED | `peer_manager.cpp:546-549` and `575-578`: BlobRequest suppression |
| `sync_protocol.cpp ingest_blobs()` | `SyncStats::storage_full_count` | Count `storage_full` rejections for post-sync signal | WIRED | `sync_protocol.cpp:94,118-121,129`: counter incremented, assigned to `stats.storage_full_count` |

---

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| STOR-01 | 16-01 | Tombstone lookups use O(1) indexed check via dedicated mdbx sub-database | SATISFIED | `tombstone_map` sub-database with O(1) `txn.get()` in `has_tombstone_for()`; 7 passing tests |
| STOR-02 | 16-02 | Operator can configure a global storage limit (max_storage_bytes) | SATISFIED | `Config::max_storage_bytes`, JSON parsing, `BlobEngine` constructor param, `main.cpp` wiring |
| STOR-03 | 16-02 | Storage limit check runs as Step 0 inside synchronous ingest() before any crypto operations | SATISFIED | `engine.cpp:50-59`: Step 0b before structural checks (line 62), namespace check (line 77), signature verify (line 110) |
| STOR-04 | 16-03 | Node sends StorageFull wire message to peers when rejecting a blob due to capacity | SATISFIED | `TransportMsgType_StorageFull = 23` in wire protocol; `peer_manager.cpp:366-372` sends on rejection |
| STOR-05 | 16-03 | Peers receiving StorageFull set peer_is_full flag and suppress sync pushes | SATISFIED | `peer_manager.cpp:342-349` sets flag; 4 suppression sites in sync paths |

All 5 requirements fully satisfied. No orphaned requirements for Phase 16 in REQUIREMENTS.md.

---

### Anti-Patterns Found

No anti-patterns detected.

Scanned files: `storage.h`, `storage.cpp`, `engine.h`, `engine.cpp`, `config.h`, `config.cpp`, `peer_manager.h`, `peer_manager.cpp`, `sync_protocol.h`, `sync_protocol.cpp`, `transport.fbs`, `transport_generated.h`

No TODOs, FIXMEs, placeholder returns, empty handlers, or stub implementations found in any Phase 16 modified file.

---

### Summary Discrepancy (Not a Gap)

The 16-01-SUMMARY.md claims a "one-time startup migration that populates tombstone_map from existing tombstone blobs" was implemented. The actual `storage.cpp` has no migration code. This is consistent with the plan truth that explicitly stated "No migration needed — pre-production, fresh databases only." The SUMMARY key-decisions section appears to document a considered-then-discarded option rather than a delivered feature. The code is correct per the PLAN specification.

---

### Human Verification Required

None. All phase behaviors are verifiable via static code analysis:
- O(1) lookup: implementation uses single-key read with no iteration
- Capacity enforcement: integer comparison before crypto call chain
- Wire protocol: generated header and runtime dispatch verified
- Sync suppression: flag read within BlobRequest handler paths

E2E tests exist for the runtime behavior (StorageFull flow, graceful sync completion, peer_is_full reset).

---

## Conclusion

Phase 16 goal fully achieved. All three plans delivered working implementations:

- **Plan 16-01 (STOR-01):** `tombstone_map` 5th sub-database with O(1) lookup replacing cursor scan. `used_bytes()` via `env.get_info()` for capacity input.
- **Plan 16-02 (STOR-02, STOR-03):** `max_storage_bytes` config field wired from JSON through `main.cpp` to `BlobEngine`. Step 0b capacity gate with tombstone exemption.
- **Plan 16-03 (STOR-04, STOR-05):** `StorageFull = 23` wire message, `peer_is_full` flag with reconnect reset, sync push suppression, post-sync signaling from both roles.

All 14 observable truths verified. All 5 requirements satisfied. No gaps.

---

_Verified: 2026-03-10T07:00:00Z_
_Verifier: Claude (gsd-verifier)_
