# Deferred Items - Phase 19

## Pre-existing Test Failure

**Test 260: PeerManager storage full signaling** - SEGFAULT in first section ("Data to full node sends StorageFull and sets peer_is_full") at test_peer_manager.cpp:1211. This failure exists before any Phase 19 changes and is not caused by the version bump. 283/284 tests pass consistently.

Likely cause: Use-after-free or dangling reference during async peer manager stop/restart cycle in the test fixture (lines 1259-1279 create new PeerManagers reusing the same io_context and storage objects).
