---
status: complete
phase: 12-blob-deletion
source: [12-01-SUMMARY.md, 12-02-SUMMARY.md]
started: 2026-03-08T00:00:00Z
updated: 2026-03-08T00:01:00Z
---

## Current Test

[testing complete]

## Tests

### 1. Full test suite passes
expected: Running `ctest --test-dir build --output-on-failure` shows all 216 tests passing (791+ assertions). No failures or crashes.
result: pass

### 2. Owner can delete own blob via engine
expected: Engine::delete_blob() with owner's signing key succeeds, blob is removed from storage, tombstone is stored. Verified by tombstone engine tests passing.
result: skipped

### 3. Non-owner deletion rejected
expected: Engine::delete_blob() with a different key than the namespace owner returns an error. The blob remains in storage. Verified by engine test coverage.
result: skipped

### 4. Tombstone blocks future blob ingestion
expected: After a blob is tombstoned, attempting to ingest a new blob with the same hash is rejected with IngestError::tombstoned. Verified by engine tests.
result: skipped

### 5. DELETE wire protocol works end-to-end
expected: Sending a DELETE message to a peer causes the peer to delete the target blob, store the tombstone, and reply with DeleteAck. Verified by peer_manager integration test.
result: skipped

### 6. Tombstone propagation via sync
expected: When node A deletes a blob, the tombstone syncs to node B via the standard sync protocol. Node B deletes the target blob and stores the tombstone. Verified by sync integration test with staggered intervals.
result: skipped

### 7. Clean build with stripped liboqs
expected: A clean CMake configure + build succeeds with only ML-DSA and ML-KEM algorithms enabled. All other KEM/SIG families (BIKE, FrodoKEM, HQC, Kyber, NTRUPrime, Classic McEliece, Dilithium, Falcon, SPHINCS+, MAYO, CROSS, SLH-DSA) are disabled. Binary runs correctly.
result: pass

## Summary

total: 7
passed: 2
issues: 0
pending: 0
skipped: 5

## Gaps

[none yet]
