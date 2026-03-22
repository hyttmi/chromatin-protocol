# Phase 54: Deferred Items

## Pre-existing: is_blob_expired microsecond division bug

**Found during:** 54-02 Task 1 (test timestamp updates)

**Issue:** `SyncProtocol::is_blob_expired()` in `db/sync/sync_protocol.cpp` divides `blob.timestamp` by 1000000, treating it as microseconds. However, timestamps are actually Unix seconds (as documented in PROTOCOL.md and BlobData struct). This causes all blobs with real timestamps to appear expired when compared against the real system clock.

**Impact:** The "SyncProtocol tracks quota_exceeded_count in SyncStats" test in `db/tests/peer/test_peer_manager.cpp` was already failing before plan 54-02 changes. Sync expiry filtering may incorrectly skip valid blobs in production when using real system clock (not the test clock with small values).

**Suggested fix:** Remove the `/1000000` division in `is_blob_expired()`. The formula should be `(blob.timestamp + blob.ttl) <= now`, not `(blob.timestamp / 1000000 + blob.ttl) <= now`.

**Files:** `db/sync/sync_protocol.cpp` line 22
