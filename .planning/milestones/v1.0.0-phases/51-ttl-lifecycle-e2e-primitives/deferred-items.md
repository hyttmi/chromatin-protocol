# Phase 51 Deferred Items

## Timestamp/TTL Units Mismatch (Pre-existing)

**Found during:** 51-01 Task 1 (TTL-03 permanent blob test)

**Issue:** The `run_expiry_scan()` in `storage.cpp` compares `expiry_time` (computed as `blob.timestamp + blob.ttl`) against `system_clock_seconds()`. However, loadgen sets `blob.timestamp` as microseconds (via `std::chrono::microseconds`), while the clock returns seconds. This means `expiry_time` is always a microsecond-scale number (~10^15) while `now` is a seconds-scale number (~10^9), so `expiry_ts > now` is always true and blobs with TTL>0 are never actually expired by the GC scan.

**Impact:** Unit tests pass because they use consistent second-based timestamps. Real integration testing reveals the mismatch. TTL=0 blobs are correctly exempt (no expiry entry created), but TTL>0 blobs never expire in practice.

**Scope:** Pre-existing, not caused by this plan's changes. Out of scope per deviation rules.

**Suggested fix:** Either normalize timestamps to seconds in storage (divide by 1,000,000), or change `system_clock_seconds()` to return microseconds, or convert the blob timestamp before computing expiry_time.
