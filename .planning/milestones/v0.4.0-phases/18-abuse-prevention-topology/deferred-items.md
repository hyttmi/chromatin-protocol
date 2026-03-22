# Deferred Items - Phase 18

## Pre-existing: SIGSEGV in [storage-full] test

**Discovered during:** 18-03 execution (verification step)
**File:** tests/peer/test_peer_manager.cpp:1211
**Test:** "PeerManager storage full signaling" SECTION "Data to full node sends StorageFull and sets peer_is_full"
**Issue:** Segmentation fault (SIGSEGV) during test execution. Pre-existing failure, not caused by Phase 18 changes.
**Impact:** 1 of 28 peer tests fails. All other tests including all ratelimit and nsfilter tests pass.
