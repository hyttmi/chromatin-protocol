# Deferred Items - Phase 115

## Pre-existing Test Failure

**File:** db/tests/peer/test_peer_manager.cpp:2740
**Issue:** NodeInfoRequest test expects 38 supported types but node reports 39.
**Cause:** Not related to Phase 115 changes. A prior phase likely added a new type without updating this test.
**Impact:** One test assertion failure (CHECK, not REQUIRE -- does not block other tests).
