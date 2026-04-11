# Phase 106: Bug Fixes - Context

**Gathered:** 2026-04-11
**Status:** Ready for planning

<domain>
## Phase Boundary

Fix all known relay bugs: compound response translation failures (FIX-01) and coroutine safety issues (FIX-02). Includes a broader coroutine safety sweep across relay/ (fix) and db/ (read-only audit). Establish ASAN/TSAN/UBSAN clean baselines with a smoke test.

</domain>

<decisions>
## Implementation Decisions

### FIX-01: Compound Translation Verification
- **D-01:** Verification uses both captured binary fixtures AND live node testing. Fixtures for reproducible unit tests, live tests for catching real-world drift.
- **D-02:** Fix bugs in place but add consistent bounds checking to all 10 compound decoders (some currently lack overflow guards). Not a full rewrite.
- **D-03:** Binary fixtures stored in `relay/tests/fixtures/` for large responses, inline hex in test code for small/simple types.
- **D-04:** Build a reusable UDS tap tool that connects to the node's UDS, sends each request type, and dumps raw binary responses to files. Reusable for Phase 107 message type verification.

### FIX-02: Coroutine Safety Sweep
- **D-05:** Broader sweep beyond std::visit — covers all four categories: lambda captures in coroutines, shared_ptr lifetimes across co_await, container invalidation across suspension points, and strand confinement.
- **D-06:** Sweep both relay/ AND db/, but only fix issues in relay/. db/ findings documented in a separate DB-COROUTINE-FINDINGS.md for user's later manual sweep.
- **D-07:** db/ audit focuses on three areas: peer/ decomposed code (Phase 96), sync protocol (Phase A/B/C), and connection lifecycle (on_peer_connected, keepalive, disconnect).
- **D-08:** Fix all issues found in relay/ within this phase (no deferral). Clean foundation before E2E testing in Phases 107-108.
- **D-09:** Documentation: code comments at each fix site explaining the coroutine safety issue, PLUS a COROUTINE-AUDIT.md summary for the phase record. Separate DB-COROUTINE-FINDINGS.md for db/ read-only findings.

### ASAN Test Workflow
- **D-10:** All three sanitizers: ASAN+UBSAN in one build, TSAN in a separate build. Run both.
- **D-11:** Write a minimal smoke test in Phase 106 that exercises key paths (write, read, subscribe, compound responses) through a live relay+node. Run under all sanitizers. Phase 107 extends this to all 38 types.
- **D-12:** CMake sanitizer preset already exists — use it. No new build infrastructure needed.

### Claude's Discretion
- Specific bounds-check patterns for compound decoders (cursor vs manual offset tracking)
- Smoke test framework choice (standalone script vs Catch2 integration test)
- COROUTINE-AUDIT.md format and severity rating scheme

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Relay Translation Code
- `relay/translate/translator.cpp` -- All 10 compound decode helpers (lines 421-755) and binary_to_json dispatcher
- `relay/translate/translator.h` -- Public API
- `relay/translate/json_schema.cpp` -- Schema registry with is_compound flags for all 12 types
- `relay/translate/json_schema.h` -- FieldSpec/MessageSchema definitions

### Coroutine Patterns (relay/)
- `relay/ws/ws_session.cpp` -- Only std::visit site (line 751, synchronous/safe), async_read/async_write get_if pattern (lines 238-263), all coroutine entry points
- `relay/ws/ws_session.h` -- Stream variant type definition, session lifecycle
- `relay/core/uds_multiplexer.cpp` -- UDS coroutine paths
- `relay/core/request_router.cpp` -- Request routing with pending map
- `relay/core/subscription_tracker.cpp` -- Subscription fan-out paths

### Coroutine Patterns (db/ -- read-only audit)
- `db/peer/connection_manager.h` / `.cpp` -- Connection lifecycle, keepalive, dedup
- `db/peer/sync_orchestrator.h` / `.cpp` -- Sync protocol, Phase A/B/C, cursor management
- `db/peer/message_dispatcher.h` / `.cpp` -- Message routing, query handlers with co_await
- `db/peer/blob_push_manager.h` / `.cpp` -- BlobNotify/BlobFetch pending fetches
- `db/peer/pex_manager.h` / `.cpp` -- PEX protocol coroutines

### Prior Art
- Commit `16e6caf` -- Previous ASAN fix: stack-use-after-return in WsSession::async_read/async_write (replaced std::visit with get_if/get)

### Test Infrastructure
- `relay/tests/test_translator.cpp` -- Existing translator tests (161+ tests)
- `relay/tests/` -- Full relay test suite (205 tests, 2378 assertions)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `relay/ws/ws_session.cpp:238-263` -- Exemplary get_if/get pattern for variant access in coroutines (template for any new fixes)
- `relay/tests/test_translator.cpp` -- Comprehensive translator test suite to extend with fixture-based tests
- CMake sanitizer preset -- existing build configuration for ASAN/TSAN/UBSAN

### Established Patterns
- Variant access in coroutines: `std::get_if` for first branch, `std::get` for known-safe fallback
- Compound decoders: static helper functions with manual offset tracking and bounds checks
- Test structure: Catch2 TEST_CASE with SECTION-based organization

### Integration Points
- UDS tap tool connects to same socket path as relay's UdsMultiplexer
- Smoke test exercises the relay's WebSocket endpoint (same as Phase 107 E2E tests)
- Binary fixtures feed into test_translator.cpp alongside existing inline test data

</code_context>

<specifics>
## Specific Ideas

- UDS tap tool should be reusable for Phase 107's all-38-types verification
- db/ audit focuses specifically on peer/ decomposition (Phase 96), sync protocol, and connection lifecycle -- these are the areas with most coroutine complexity
- DB-COROUTINE-FINDINGS.md is separate from the relay audit doc so user can reference it independently during their later db/ sweep

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 106-bug-fixes*
*Context gathered: 2026-04-11*
