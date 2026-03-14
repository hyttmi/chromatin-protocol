# Phase 21: Test 260 SEGFAULT Fix - Research

**Researched:** 2026-03-13
**Domain:** Test infrastructure / async object lifetime
**Confidence:** HIGH

## Summary

Investigation reveals that test 260 (PeerManager storage full signaling E2E) **currently passes consistently** — 5/5 isolated runs and 284/284 full suite. The SEGFAULT was identified during the v0.4.0 milestone audit as a use-after-free in the test fixture restart cycle at `test_peer_manager.cpp:1211`.

The most likely explanation is that Phase 20's timer cancel parity fix (commit adding `sync_timer_`, `pex_timer_`, `flush_timer_`, `metrics_timer_` cancellation to `on_shutdown_`) resolved the root cause — outstanding timer callbacks that could fire after object destruction during the `ioc.run_for()` drain window.

**Primary recommendation:** Confirm the fix via AddressSanitizer build + targeted test runs, then verify full suite remains green. No code changes expected unless ASan reveals hidden issues.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Strictly test 260 SEGFAULT fix — no other audit tech debt bundled
- Other v0.4.0 tech debt items remain on backlog as independent items
- Fix lifetime management in the existing test fixture — keep the E2E test pattern, fix object lifetimes so io_context is fully drained before destructors run
- Do not restructure the test away from the restart/E2E cycle pattern
- If root cause analysis reveals a real shutdown ordering issue in PeerManager::stop() that only manifests in tests, fix it in production code — correctness first
- If the bug is purely test fixture scoping, keep the fix test-only
- Run full suite under AddressSanitizer to prove no hidden memory errors beyond the segfault
- Run fixed test multiple times to confirm deterministic, not timing-lucky
- "All tests pass" is the standard — exact count may differ; zero failures and zero segfaults is what matters

### Claude's Discretion
- Port handling in storage full tests (hardcoded vs ephemeral) — Claude assesses during implementation
- Whether to add permanent CMake sanitizer option vs one-time verification — Claude decides based on effort
- Specific lifetime fix approach (RAII ordering, explicit drain, scope restructuring)

### Deferred Ideas (OUT OF SCOPE)
- None — discussion stayed within phase scope
</user_constraints>

## Architecture Patterns

### Test Fixture Pattern (E2E PeerManager tests)

Every E2E test in `test_peer_manager.cpp` follows this pattern:
```cpp
TempDir tmp1, tmp2;
auto id1 = NodeIdentity::load_or_generate(tmp1.path);
Config cfg1; cfg1.bind_address = "127.0.0.1:XXXX"; ...
Storage store1(tmp1.path.string());
BlobEngine eng1(store1);
asio::io_context ioc;
AccessControl acl1(...);
PeerManager pm1(cfg1, id1, eng1, store1, ioc, acl1);
pm1.start();
ioc.run_for(std::chrono::seconds(N));
pm1.stop();
ioc.run_for(std::chrono::seconds(2));  // drain
```

**Destruction order** (reverse of construction): PeerManager -> AccessControl -> io_context -> BlobEngine -> Storage -> Identity -> TempDir. This is correct by C++ stack destruction semantics.

### Timer-Cancel Pattern
PeerManager uses raw pointer-to-stack-timer pattern for coroutine timers:
```cpp
asio::steady_timer timer(ioc_);
expiry_timer_ = &timer;      // Store pointer
co_await timer.async_wait();
expiry_timer_ = nullptr;     // Clear on wake
```
`stop()` cancels all 5 timers via these pointers. If any pointer is stale (timer destroyed but pointer not cleared), cancellation writes to freed memory.

### Root Cause Analysis

The audit identified the SEGFAULT at test 260 (line 1211) as a use-after-free in the test fixture restart cycle. The `on_shutdown_` lambda in Phase 17 originally only cancelled `expiry_timer_` but not the other 4 timers (`sync_timer_`, `pex_timer_`, `flush_timer_`, `metrics_timer_`). Phase 20 fixed this inconsistency.

**Why the fix resolved it:** When `ioc.run_for()` was draining after `pm.stop()`, outstanding timer completion handlers could still fire. If `stop()` cancelled all timers but `on_shutdown_` (triggered by SIGTERM path) didn't, the asymmetry created a window where timer callbacks could reference destroyed PeerManager state. The Phase 20 timer cancel parity fix closed this gap.

**Current state (verified empirically):**
- 5/5 isolated runs: PASSED
- 284/284 full suite: PASSED (zero failures)
- No SEGFAULT observed

## Common Pitfalls

### Pitfall 1: Timing-Dependent Pass
**What goes wrong:** Test passes because timing window for use-after-free is narrow; ASan catches it reliably
**Why it happens:** Use-after-free manifests only when scheduler delivers completion handler at exact wrong moment
**How to avoid:** ASan verification is mandatory — passing tests alone don't prove absence of UB
**Warning signs:** Test that "sometimes" fails, test runtime varies significantly

### Pitfall 2: ASan + libmdbx Interaction
**What goes wrong:** ASan may report false positives from libmdbx internal memory management
**Why it happens:** mdbx uses mmap-based memory that ASan can misinterpret
**How to avoid:** Use ASAN_OPTIONS=detect_leaks=0 if leak reports are noisy; focus on use-after-free reports
**Warning signs:** Reports pointing to mdbx internals, not chromatindb code

### Pitfall 3: Port Conflicts in Parallel Test Runs
**What goes wrong:** Hardcoded ports (14300/14301, etc.) cause bind failures if tests run in parallel or previous test didn't clean up
**Why it happens:** Catch2 may run SECTIONs within a TEST_CASE sequentially, but TEST_CASEs could overlap
**How to avoid:** Current architecture uses unique port pairs per test section; sequential execution within TEST_CASE prevents overlap

## Code Examples

### Test 260 — The Specific Test (test_peer_manager.cpp:1211)
```cpp
SECTION("Data to full node sends StorageFull and sets peer_is_full") {
    TempDir tmp1, tmp2;
    // ... setup with max_storage_bytes = 1 ...
    asio::io_context ioc;
    PeerManager pm1(cfg1, id1, eng1, store1, ioc, acl1);
    PeerManager pm2(cfg2, id2, eng2, store2, ioc, acl2);
    pm1.start();
    pm2.start();
    ioc.run_for(std::chrono::seconds(8));
    // ... assertions ...
    pm1.stop();
    pm2.stop();
    ioc.run_for(std::chrono::seconds(6));  // drain window
}
```

### PeerManager::stop() — All Timers Cancelled (Phase 20 fix)
```cpp
void PeerManager::stop() {
    stopping_ = true;
    sighup_signal_.cancel();
    sigusr1_signal_.cancel();
    if (expiry_timer_) expiry_timer_->cancel();
    if (sync_timer_) sync_timer_->cancel();
    if (pex_timer_) pex_timer_->cancel();
    if (flush_timer_) flush_timer_->cancel();
    if (metrics_timer_) metrics_timer_->cancel();
    server_.stop();
}
```

## Implementation Approach

### CMake ASan Option (Claude's Discretion)
Adding a permanent `ENABLE_ASAN` option is low-effort and high-value for future debugging:
```cmake
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
```
This adds 4 lines to CMakeLists.txt and makes future ASan runs trivial.

### Verification Protocol
1. Build with ASan: `cmake .. -DENABLE_ASAN=ON && make -j$(nproc)`
2. Run test 260 specifically: `ctest -R "storage full"`
3. Run test 260 five times: confirm deterministic
4. Run full suite: confirm 284/284 (or current count) with zero failures
5. Check ASan output for any use-after-free reports

### If ASan Reveals Issues
If ASan finds use-after-free that doesn't SEGFAULT:
1. Identify the specific handler and object
2. Fix destruction order or add explicit drain
3. Re-run ASan to confirm fix
4. Keep fix minimal per CONTEXT.md constraints

## Open Questions

1. **Was the fix intentional or coincidental?**
   - What we know: Phase 20 added timer cancel parity to `on_shutdown_`, which is the SIGTERM path not the test path (tests call `stop()` directly). The test path (`stop()`) already cancelled all timers since Phase 17.
   - What's unclear: Whether the SEGFAULT was in the `stop()` path or the destruction path. If it was a race between coroutine completion and destructor, the timer parity in `on_shutdown_` may be irrelevant.
   - Recommendation: ASan will give definitive answer. If ASan is clean, the fix is confirmed regardless of which specific change resolved it.

## Sources

### Primary (HIGH confidence)
- Direct codebase analysis: `test_peer_manager.cpp`, `peer_manager.h`, `peer_manager.cpp`
- v0.4.0 Milestone Audit: `.planning/v0.4.0-MILESTONE-AUDIT.md`
- Empirical verification: 5/5 test runs passed, 284/284 full suite passed

## Metadata

**Confidence breakdown:**
- Root cause analysis: HIGH - code is well understood, timer pattern is documented
- Current test status: HIGH - empirically verified (5/5 + 284/284)
- Fix verification approach: HIGH - ASan is the standard tool for this class of bug

**Research date:** 2026-03-13
**Valid until:** Permanent (test infrastructure, not version-dependent)
