---
phase: 13-namespace-delegation
status: passed
verified: 2026-03-08
---

# Phase 13: Namespace Delegation -- Verification

## Phase Goal
Namespace owners can grant write access to other pubkeys via signed delegation blobs, enabling multi-writer namespaces.

## Success Criteria Verification

### 1. Namespace owner can create a signed delegation blob that names a delegate pubkey, and the node stores it in the owner's namespace
**Status: VERIFIED**
- `wire::make_delegation_data()` creates DELEGATION_MAGIC (0xDE1E6A7E) + 2592-byte delegate pubkey (`db/wire/codec.h:37-38`, `db/wire/codec.cpp`)
- `wire::is_delegation()` validates exactly DELEGATION_DATA_SIZE bytes with correct magic prefix (`db/wire/codec.cpp`)
- Owner creates delegation blob with TTL=0 (permanent), signs with canonical form SHA3-256(namespace||data||ttl||timestamp)
- `BlobEngine::ingest()` accepts delegation blobs from namespace owners through normal pipeline -- no special code path needed (`db/engine/engine.cpp:40-153`)
- `Storage::store_blob()` indexes delegation in delegation_map sub-database with key [namespace:32][SHA3-256(delegate_pubkey):32] (`db/storage/storage.cpp`)
- 6 engine tests verify delegation blob creation: owner creates, stores, deduplicates, different timestamps produce different blobs
- 3 codec tests verify format: correct magic, size validation, pubkey extraction

### 2. A delegate can write blobs to a namespace they have been delegated to, signing with their own key, and the node accepts the write after verifying a valid delegation blob exists
**Status: VERIFIED**
- `BlobEngine::ingest()` Step 2 checks ownership first (cheap SHA3-256 compare), then delegation second via `storage_.has_valid_delegation()` (`db/engine/engine.cpp:65-93`)
- Delegates sign with their own key using the same canonical form; signature verified in Step 3 against delegate's pubkey
- Delegates cannot create delegation blobs (guard at line 81-85) or tombstone blobs (guard at line 88-92)
- `IngestError::no_delegation` returned when pubkey has neither ownership nor valid delegation (`db/engine/engine.h`)
- 10 delegate write tests: acceptance, rejection without delegation, delegation-blob restriction, tombstone restriction, multiple delegates, persistence after revocation

### 3. Delegation blobs replicate to peers via the existing sync protocol like any other blob
**Status: VERIFIED**
- Delegation blobs are structurally regular blobs -- no special sync logic needed
- Sync integration test "delegation blobs replicate via sync" confirms: owner creates delegation on node A, sync transfers it to node B, node B's storage has valid delegation indexed (`tests/sync/test_sync_protocol.cpp`)
- Sync integration test "delegate-written blobs replicate via sync" confirms: delegate writes on node A after delegation, sync transfers both delegation and delegate blob to node B (`tests/sync/test_sync_protocol.cpp`)

### 4. Owner can revoke a delegation by tombstoning the delegation blob, and the node rejects subsequent writes from that delegate
**Status: VERIFIED**
- Tombstoning a delegation blob removes it from delegation_map via `Storage::delete_blob_data()` cleanup (`db/storage/storage.cpp`)
- After revocation, `has_valid_delegation()` returns false, delegate writes rejected with `IngestError::no_delegation`
- Re-delegation works: new delegation blob has new timestamp -> new content hash -> not blocked by old tombstone
- Delegate's previously-written blobs persist after revocation (no cascade delete)
- Sync integration test "delegation revocation propagates via sync" confirms: revocation on node A propagates to node B, node B rejects delegate writes after sync (`tests/sync/test_sync_protocol.cpp`)

### 5. Delegation verification on the write hot-path is efficient (indexed lookup, not storage scan)
**Status: VERIFIED**
- delegation_map sub-database in MDBX provides O(1) btree lookup (`db/storage/storage.cpp`)
- Index key is [namespace:32][SHA3-256(delegate_pubkey):32] = 64 bytes, fixed-size, compact
- `has_valid_delegation()` performs single `txn.get()` on delegation_map -- no scan (`db/storage/storage.cpp`)
- Contrast with `has_tombstone_for()` which uses O(n) namespace scan (acceptable because deletion is rare; delegation check is on every write)

## Requirement Coverage

| Requirement | Plan | Verified |
|-------------|------|----------|
| DELEG-01: Owner creates signed delegation blob granting write access | 13-01 | Yes |
| DELEG-02: Node accepts delegate writes after verifying delegation | 13-02 | Yes |
| DELEG-03: Delegation blobs replicate via sync protocol | 13-01, 13-02 | Yes |
| DELEG-04: Owner revokes delegation by tombstoning delegation blob | 13-02 | Yes |

## Test Summary

- **Total tests:** 244 (216 original + 28 new delegation tests)
- **All pass:** Yes
- **Breakdown:** 9 storage tests (3 codec + 6 index), 6 engine creation tests, 10 engine write tests, 3 sync integration tests
- **3 existing tests updated:** namespace_mismatch -> no_delegation (correct behavioral change from delegation bypass)

## Conclusion

Phase 13 goal achieved. All 4 DELEG requirements verified. Namespace owners can grant, verify, and revoke write access via delegation blobs. The delegation index provides O(1) hot-path verification. Delegation, delegate writes, and revocation all replicate correctly via sync.
