# Phase 21: Test 260 SEGFAULT Fix - Context

**Gathered:** 2026-03-13
**Status:** Ready for planning

<domain>
## Phase Boundary

Fix test 260 (PeerManager storage full signaling E2E) use-after-free in test fixture lifecycle. Production code is correct — this is a test infrastructure bug only. Success = all tests pass with zero segfaults.

</domain>

<decisions>
## Implementation Decisions

### Scope
- Strictly test 260 SEGFAULT fix — no other audit tech debt bundled
- Other v0.4.0 tech debt items (stale comment, missing log counters, timer inconsistency) remain on backlog as independent items

### Fix strategy
- Fix lifetime management in the existing test fixture — keep the E2E test pattern, fix object lifetimes so io_context is fully drained before destructors run
- Do not restructure the test away from the restart/E2E cycle pattern

### Production code policy
- If root cause analysis reveals a real shutdown ordering issue in PeerManager::stop() that only manifests in tests, fix it in production code — correctness first
- If the bug is purely test fixture scoping, keep the fix test-only

### Verification standard
- Run full suite under AddressSanitizer to prove no hidden memory errors beyond the segfault
- Run fixed test multiple times to confirm deterministic, not timing-lucky
- "All tests pass" is the standard — exact count (284) may differ if tests were added/removed since the audit; zero failures and zero segfaults is what matters

### Claude's Discretion
- Port handling in storage full tests (hardcoded vs ephemeral) — Claude assesses during implementation
- Whether to add permanent CMake sanitizer option vs one-time verification — Claude decides based on effort
- Specific lifetime fix approach (RAII ordering, explicit drain, scope restructuring)

</decisions>

<specifics>
## Specific Ideas

No specific requirements — the fix is defined by the root cause. The audit clearly identifies the bug location: `test_peer_manager.cpp:1211`, use-after-free in test fixture restart cycle.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `TempDir` RAII wrapper (test_peer_manager.cpp:23-41): manages temp directory lifecycle
- `make_signed_blob()` / `make_signed_tombstone()`: test blob builders already used across all peer tests

### Established Patterns
- Test fixture creates PeerManager pair, runs ioc.run_for(), asserts, then stops — consistent pattern across all E2E tests in the file
- PeerManager::stop() cancels timers and sets stopping_ flag; io_context drain follows
- Hardcoded port pairs per test section (14300/14301, 14308/14309, etc.)

### Integration Points
- Test 260 is SECTION "Data to full node sends StorageFull and sets peer_is_full" at line 1211
- Depends on: Storage, BlobEngine (with max_storage_bytes=1), PeerManager, AccessControl, io_context
- The use-after-free likely involves PeerManager/Engine/Storage destruction order relative to pending io_context handlers

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 21-test-260-segfault-fix*
*Context gathered: 2026-03-13*
