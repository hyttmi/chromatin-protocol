---
phase: 46-sanitizers-bug-fix
verified: 2026-03-20T19:45:53Z
status: human_needed
score: 7/8 must-haves verified
human_verification:
  - test: "Run 50-run TSAN reliability: ./scripts/run-e2e-reliability.sh build-tsan 50 \"TSAN_OPTIONS=suppressions=sanitizers/tsan.supp:halt_on_error=1\""
    expected: "50/50 passes (all [daemon][e2e] tests pass on every run)"
    why_human: "SUMMARY documents 50/50 release build and full TSAN suite (469 tests) but does not confirm 50/50 TSAN E2E reliability run. Plan required 50/50 under TSAN. Cannot verify without executing the binary."
  - test: "Run 50-run ASAN reliability: ./scripts/run-e2e-reliability.sh build-asan 50 \"ASAN_OPTIONS=suppressions=sanitizers/asan.supp:detect_leaks=1:halt_on_error=1\""
    expected: "50/50 passes"
    why_human: "SUMMARY documents 10/10 PEX ASAN passes but not a full 50-run ASAN E2E reliability run. Plan required 50/50 under ASAN."
---

# Phase 46: Sanitizers & Bug Fix Verification Report

**Phase Goal:** All existing unit tests pass under memory, thread, and UB sanitizers with zero findings, and the pre-existing PEX coroutine SIGSEGV is resolved
**Verified:** 2026-03-20T19:45:53Z
**Status:** human_needed
**Re-verification:** No -- initial verification

## Goal Achievement

### Observable Truths

The ROADMAP defines 4 success criteria for Phase 46. The plans (46-01, 46-02) define 8 must-have truths total. Verification covers all.

#### From ROADMAP Success Criteria

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| SC-1 | `cmake --build` with `-fsanitize=address` succeeds and full suite runs with zero ASAN findings | VERIFIED | Build commits 9561649 + 0c65bd9; SANITIZER=asan CMake path confirmed in CMakeLists.txt:31-34; LSAN/ASAN suppression files exist; 469-test suite documented as passing in SUMMARY |
| SC-2 | `cmake --build` with `-fsanitize=thread` succeeds and full suite runs with zero data race reports | VERIFIED | Build infrastructure confirmed in CMakeLists.txt:35-38; TSAN fix in a1cc446 (recv_sync_msg executor transfer); tsan.supp populated with Asio suppression; SUMMARY states "zero data race reports in db/ code" |
| SC-3 | `cmake --build` with `-fsanitize=undefined` succeeds and full suite runs with zero UB findings | VERIFIED | Build infrastructure confirmed in CMakeLists.txt:39-50; kdf.cpp nonnull fix (dummy byte); CMakeLists.txt per-target mdbx alignment exclusion; ubsan.supp has documentation; SUMMARY states "zero UB findings in db/ code" |
| SC-4 | "three nodes: peer discovery via PEX" (test_daemon.cpp:296) passes reliably without SIGSEGV across 10 consecutive runs | VERIFIED | Test exists at test_daemon.cpp:296. Root cause (concurrent SyncRejected write causing AEAD nonce desync) fixed in 0c65bd9; peer_manager.cpp:456 shows silent drop when syncing=true; SUMMARY documents 10/10 ASAN consecutive passes |

#### From Plan Must-Haves (46-01)

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| T-1 | cmake -DSANITIZER=asan builds and full suite runs with zero ASAN findings in db/ code | VERIFIED | CMakeLists.txt:28-34 has SANITIZER enum; 469 tests documented passing under ASAN |
| T-2 | cmake -DSANITIZER=tsan and cmake -DSANITIZER=ubsan accepted by CMake | VERIFIED | CMakeLists.txt:35-50 has both branches with proper flags |
| T-3 | Third-party sanitizer findings suppressed via suppression files, not by modifying third-party code | VERIFIED | sanitizers/*.supp files contain only third-party suppressions; db/ code was fixed at root cause |
| T-4 | ENABLE_ASAN removed and replaced by SANITIZER enum | VERIFIED | Commit 9561649; grep confirms no ENABLE_ASAN in CMakeLists.txt; set(SANITIZER at line 28 |

#### From Plan Must-Haves (46-02)

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| T-5 | PEX test (test_daemon.cpp:296) passes reliably without SIGSEGV -- coroutine lifetime during teardown properly managed | VERIFIED | Root cause identified as AEAD nonce desync (not coroutine scope per se); fix in peer_manager.cpp:456 (silent drop); 10/10 ASAN passes documented |
| T-6 | Full suite passes under TSAN with zero data race reports in db/ code | VERIFIED | recv_sync_msg executor transfer fix in a1cc446; co_await asio::post(ioc_) at line 693 in peer_manager.cpp before sync_inbox access |
| T-7 | Full suite passes under UBSAN with zero UB findings in db/ code | VERIFIED | kdf.cpp dummy byte fix; CMakeLists.txt nonnull-attribute exclusion and per-target alignment suppression |
| T-8 | All [daemon][e2e] tests pass 50 consecutive runs without failure | UNCERTAIN | SUMMARY documents 50/50 release build only. TSAN and ASAN 50-run executions not confirmed. See Human Verification. |

**Score:** 7/8 truths verified (T-8 needs human confirmation for TSAN and ASAN configurations)

---

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `CMakeLists.txt` | SANITIZER enum option (asan/tsan/ubsan) replacing ENABLE_ASAN | VERIFIED | Lines 28-50: set(SANITIZER...), all three branches, FATAL_ERROR for unknown values; UBSAN nonnull-attribute exclusion and per-target mdbx alignment at lines 40-52, 151-152 |
| `sanitizers/asan.supp` | ASAN suppression file | VERIFIED | Exists; comment-only (no third-party ASAN findings needed suppression) |
| `sanitizers/tsan.supp` | TSAN suppression file | VERIFIED | Exists; contains Asio posix_event::~posix_event suppression with explanation |
| `sanitizers/ubsan.supp` | UBSAN suppression file | VERIFIED | Exists; documents libmdbx alignment approach (suppressed via CMake per-target, not runtime file) |
| `sanitizers/lsan.supp` | LSAN suppression file (created as needed) | VERIFIED | Exists; liboqs OQS_SIG_new, asio awaitable_handler/frame suppressions |
| `db/peer/peer_manager.cpp` | Fixed coroutine teardown (PEX SIGSEGV root cause) + executor transfer | VERIFIED | Silent drop when syncing at line 456; co_await asio::post(ioc_) at line 693 |
| `db/net/server.h` | Coroutine params by value | VERIFIED | connect_to_peer(std::string address) and reconnect_loop(std::string address) both by value |
| `db/net/server.cpp` | Coroutine params by value | VERIFIED | reconnect_loop(std::string address) -- by value, commit 0c65bd9 |
| `db/storage/storage.cpp` | ASAN geometry reduction | VERIFIED | Lines 169-174: __SANITIZE_ADDRESS__ conditional reducing size_upper to 1 GiB |
| `db/crypto/kdf.cpp` | UBSAN nonnull fix | VERIFIED | Lines 15-21: dummy byte instead of nullptr for empty salt |
| `db/crypto/thread_pool.h` | offload() docs + removed caller_ex | VERIFIED | Lines 30-31: caller must transfer back; caller_ex removed from signature |
| `db/engine/engine.cpp` | Updated offload() call sites | VERIFIED | All offload() calls use two-argument form (no stale caller_ex) |
| `db/net/connection.cpp` | Updated offload() call sites | VERIFIED | Same: stale caller_ex parameter removed in commit a1cc446 |
| `scripts/run-e2e-reliability.sh` | 50-run reliability validation script | VERIFIED | Exists, executable (-rwxr-xr-x), filters TAG="[daemon][e2e]", accepts build_dir/runs/env_opts parameters |

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `CMakeLists.txt` | `sanitizers/*.supp` | ASAN_OPTIONS/TSAN_OPTIONS env vars documented in supp file headers | WIRED | Suppression files document usage via env var; CMakeLists.txt activates sanitizer flags; the connection is env-var-driven at test runtime (not CMake wiring), consistent with plan design |
| `db/peer/peer_manager.cpp` | `db/tests/test_daemon.cpp` | PeerManager::stop() teardown sequence -- stopping_ flag checked in pex_timer_loop | WIRED | stopping_ checked at peer_manager.cpp:2021,2028; PEX test at test_daemon.cpp:296 calls pm_a.stop() etc; silent drop at line 456 prevents nonce desync |
| `scripts/run-e2e-reliability.sh` | `build-tsan/chromatindb_tests` | Catch2 CLI with tag filter [daemon][e2e] | WIRED | Script at line 21: TAG="[daemon][e2e]"; line 35 executes binary with $TAG; binary path lookup handles both build/db/ and build/ layouts |

---

### Requirements Coverage

All four requirement IDs claimed by this phase are accounted for in REQUIREMENTS.md:

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| SAN-01 | 46-01 | Full Catch2 suite passes under ASAN with zero findings | SATISFIED | CMakeLists.txt SANITIZER=asan path; 469 tests documented passing; REQUIREMENTS.md marked [x] |
| SAN-02 | 46-02 | Full Catch2 suite passes under TSAN with zero data races | SATISFIED | recv_sync_msg executor transfer fix; tsan.supp with Asio suppression; 469 tests documented clean; REQUIREMENTS.md marked [x] |
| SAN-03 | 46-02 | Full Catch2 suite passes under UBSAN with zero findings | SATISFIED | kdf.cpp nonnull fix; CMakeLists nonnull-attribute exclusion + mdbx alignment target flag; 469 tests documented clean; REQUIREMENTS.md marked [x] |
| FIX-01 | 46-02 | PEX test SIGSEGV fixed -- coroutine lifetime during teardown resolved | SATISFIED | AEAD nonce desync root cause fixed in peer_manager.cpp:456; 10/10 ASAN consecutive passes documented; REQUIREMENTS.md marked [x] |

No orphaned requirements: REQUIREMENTS.md maps SAN-01, SAN-02, SAN-03, FIX-01 to Phase 46. No additional Phase 46 requirements exist in the traceability table.

---

### Anti-Patterns Found

Scanned all files modified in this phase: CMakeLists.txt, db/peer/peer_manager.cpp, db/net/server.cpp, db/net/server.h, db/storage/storage.cpp, db/crypto/kdf.cpp, db/crypto/thread_pool.h, db/engine/engine.cpp, db/net/connection.cpp, sanitizers/*.supp, scripts/run-e2e-reliability.sh.

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| 46-02-SUMMARY.md | Issues section | "Pre-existing full-suite hang in release build" when running all 469 tests in single invocation | Info | Logged as pre-existing, out-of-scope for this phase. Sanitizer builds run normally. Not a blocker. |

No TODO/FIXME/PLACEHOLDER/HACK comments found in any modified source files. No stub implementations. No empty handlers. No return null/empty implementations in phase-critical paths.

---

### Human Verification Required

#### 1. TSAN 50-Run E2E Reliability

**Test:** From the project root, with build-tsan compiled:
```
./scripts/run-e2e-reliability.sh build-tsan 50 "TSAN_OPTIONS=suppressions=sanitizers/tsan.supp:halt_on_error=1"
```
**Expected:** "Passed: 50/50" and "All runs passed." with exit code 0. Note: each run takes ~30 seconds under TSAN, so this is approximately a 25-minute validation.

**Why human:** The SUMMARY (line 73) explicitly says "50/50 release build passed" but does not claim 50/50 TSAN. Line 127 mentions "Release 50/50 and TSAN full suite (469 tests) both pass cleanly" -- "full suite" refers to running all 469 tests once, not 50 repeated E2E runs. The plan's must_have truth T-8 requires 50/50 across multiple build configs. Cannot verify execution happened without running it.

#### 2. ASAN 50-Run E2E Reliability

**Test:** From the project root, with build-asan compiled:
```
./scripts/run-e2e-reliability.sh build-asan 50 "ASAN_OPTIONS=suppressions=sanitizers/asan.supp:detect_leaks=1:halt_on_error=1"
```
**Expected:** "Passed: 50/50" and "All runs passed." with exit code 0.

**Why human:** The SUMMARY documents 10/10 consecutive ASAN passes for the specific PEX test (line 72) but does not document a 50-run E2E reliability pass under ASAN for all [daemon][e2e] tests. The plan required 50/50 ASAN.

---

### Gaps Summary

No hard gaps blocking the phase goal. The ROADMAP's 4 success criteria are all verified. The uncertainty is at the plan-level must-have T-8 (50-run reliability under TSAN and ASAN configurations, not just release build). Since TSAN and UBSAN full suites (469 tests) passed and the release build showed 50/50, the underlying code correctness is established -- but the mechanical 50-run validation under sanitizer configs was documented incompletely.

The two human verification items are confirmations of already-fixed code against a timing/reliability bar, not indicators of missing implementation.

---

## Summary of Verified Implementation

The following real bugs were discovered and fixed by this phase (all verified in code):

1. **Coroutine parameter lifetime (stack-use-after-scope):** `reconnect_loop` and `connect_to_peer` changed from `const std::string&` to `std::string` by value in server.h and server.cpp.

2. **MDBX_TOO_LARGE under ASAN:** `#if defined(__SANITIZE_ADDRESS__)` conditional in storage.cpp reduces `size_upper` from 64 GiB to 1 GiB.

3. **AEAD nonce desync from concurrent SyncRejected writes (root cause of PEX SIGSEGV):** peer_manager.cpp silently drops incoming SyncRequest when `peer->syncing` is true instead of spawning a detached write coroutine.

4. **TSAN data race in recv_sync_msg:** `co_await asio::post(ioc_, asio::use_awaitable)` added at top of `recv_sync_msg` to transfer back to io_context thread before accessing `sync_inbox`.

5. **UBSAN nonnull-attribute in KDF extract:** kdf.cpp passes `&dummy` (zero byte) instead of nullptr when salt is empty.

---

_Verified: 2026-03-20T19:45:53Z_
_Verifier: Claude (gsd-verifier)_
