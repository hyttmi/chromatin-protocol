# Phase 112: ASAN Verification - Context

**Gathered:** 2026-04-14
**Status:** Ready for planning

<domain>
## Phase Boundary

Prove the single-threaded relay (from Phase 111) is free of memory safety and concurrency bugs under realistic concurrent HTTP load and signal handling. Run ASAN at 1, 10, and 100 concurrent clients. Verify SIGHUP config reload and SIGTERM graceful shutdown. Fix any issues found. Create LSAN suppression file for accepted shutdown leaks.

</domain>

<decisions>
## Implementation Decisions

### Load Driver
- **D-01:** Reuse existing `tools/relay_benchmark.py` as the load driver for ASAN testing. It already performs auth flow (challenge → sign → verify → bearer token) and data operations at configurable concurrency levels. ASAN instruments the relay server process, not the client — driver language doesn't matter.
- **D-02:** Run benchmark at 1, 10, and 100 concurrent HTTP clients against an ASAN-instrumented relay build. Parse ASAN stderr output for heap-use-after-free and other memory safety reports.

### Signal Testing
- **D-03:** Automated script that launches ASAN relay, starts benchmark load, sends SIGHUP mid-run (verify config change observed + no crash), then sends SIGTERM (verify clean drain + exit). Parse ASAN stderr for errors. Script must be repeatable and CI-friendly.
- **D-04:** SIGHUP test should verify at minimum: rate limit change takes effect, TLS cert swap works (if TLS enabled), no crash under concurrent load.
- **D-05:** SIGTERM test should verify: active connections drain, relay exits cleanly, ASAN reports no new issues during shutdown.

### ASAN Leak Policy
- **D-06:** Create an LSAN suppression file (`relay/lsan_suppressions.txt`) for known shutdown leaks from third-party libraries (liboqs global state, OpenSSL cleanup). These are accepted and documented.
- **D-07:** Any NEW leak reports (not covered by suppression file) are treated as bugs and fixed in this phase.
- **D-08:** Set `LSAN_OPTIONS=suppressions=relay/lsan_suppressions.txt` when running ASAN builds.

### Fix Strategy
- **D-09:** Fix ALL ASAN-reported memory safety bugs found during testing — in relay code OR node code (db/). No scope restriction on where fixes go.
- **D-10:** If a bug requires architectural changes beyond simple fixes, document it and discuss before proceeding. Simple bugs (use-after-free, buffer overrun, etc.) get fixed inline.

### Claude's Discretion
- Script language for the automated ASAN test harness (bash, Python, or mixed)
- Whether relay_benchmark.py needs minor modifications for ASAN testing (e.g., shorter runs, different workload mix)
- Suppression file format and granularity (function-level vs module-level suppressions)
- Plan decomposition — how many plans to split this into

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Requirements
- `.planning/REQUIREMENTS.md` — VER-02 (ASAN-clean at concurrent load), VER-03 (SIGHUP/SIGTERM under single-threaded model)

### Phase 111 context (predecessor)
- `.planning/phases/111-single-threaded-rewrite/111-CONTEXT.md` — Single-threaded model decisions, offload pattern, strand/mutex removal

### Phase 999.10 context (why we're here)
- `.planning/phases/999.10-relay-thread-safety-overhaul-for-multi-threaded-http/999.10-CONTEXT.md` — Root cause analysis of ASAN failures that led to single-threaded rewrite

### Build system
- `CMakeLists.txt` lines 26-50 — SANITIZER=asan flag, compile/link options
- `relay/CMakeLists.txt` — Relay standalone build (inherits sanitizer flags from root)

### Load driver
- `tools/relay_benchmark.py` — Existing HTTP benchmark tool with auth flow + data ops (reuse for ASAN load)

### Relay signal handling
- `relay/relay_main.cpp` — SIGHUP coroutine (line 462+), SIGTERM handler (line 436+)

### Relay code under test
- `relay/http/http_router.cpp` — HTTP dispatch, offload() for ML-DSA-87 verify
- `relay/core/authenticator.cpp` — ML-DSA-87 verify (offloaded to thread pool)
- `relay/core/uds_multiplexer.cpp` — UDS connection, send queue, read loop
- `relay/util/thread_pool.h` — offload() template (copied from node in Phase 111)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `tools/relay_benchmark.py`: Full HTTP benchmark with ML-DSA-87 auth, blob write/read, configurable concurrency. Covers auth flow + data operations needed for ASAN load testing.
- `CMakeLists.txt` SANITIZER flag: Already wired for asan/tsan/ubsan with proper compile and link options.
- `relay/relay_main.cpp` signal handlers: SIGHUP and SIGTERM coroutines already implemented on event loop.

### Established Patterns
- ASAN builds: `cmake -DSANITIZER=asan ..` from root CMakeLists.txt. Relay inherits flags.
- Phase 106 established "shutdown leaks only" as accepted baseline — this phase formalizes that with a suppression file.
- Node (db/) is ASAN/TSAN/UBSAN clean with 647 tests. Same bar for relay.

### Integration Points
- Relay needs a running chromatindb node on UDS for full data path testing (auth + write + read).
- SIGHUP reload touches: rate_limit_rate, request_timeout, max_blob_size, TLS context, authenticator ACL.
- SIGTERM shutdown path: stop acceptor → stop metrics → 5s drain timer → close all connections → ioc.stop().

</code_context>

<specifics>
## Specific Ideas

- The automated ASAN test script should produce a clear pass/fail result parseable by CI.
- Suppression file should be minimal — only suppress what's genuinely third-party cleanup that can't be fixed.
- The benchmark tool may need rate_limit_messages_per_sec=0 and request_timeout_seconds=0 in relay config for stress testing (already documented in relay_benchmark.py header).

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 112-asan-verification*
*Context gathered: 2026-04-14*
