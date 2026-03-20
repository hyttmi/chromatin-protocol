# Deferred Items - Phase 44

## Pre-existing Test Failure

**Test:** "three nodes: peer discovery via PEX" (db/tests/test_daemon.cpp:296)
**Failure:** SIGSEGV during test execution (sometimes SIGABRT with fmt "negative value" assertion)
**Confirmed pre-existing:** Fails on master branch before any Phase 44 changes
**Root cause (suspected):** Coroutine lifetime issue during test teardown -- PeerManager/Server destroyed while reconnect_loop or other coroutines are still pending in io_context
**Scope:** v1.0.0 integration test phase (Docker-based system tests)
