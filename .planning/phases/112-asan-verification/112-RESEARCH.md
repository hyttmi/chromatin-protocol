# Phase 112: ASAN Verification - Research

**Researched:** 2026-04-14
**Domain:** AddressSanitizer testing, LSAN suppressions, concurrent load testing, signal handling under ASAN
**Confidence:** HIGH

## Summary

Phase 112 proves the single-threaded relay (completed in Phase 111) is free of memory safety bugs under realistic concurrent HTTP load and signal handling. The relay already has a clean ASAN history for unit tests and basic smoke tests (Phase 106), but has never been validated under concurrent load with the new single-threaded + thread pool offload architecture from Phase 111. The key risk areas are: (1) the offload() / transfer-back pattern in auth verify, where coroutine state crosses between the thread pool and event loop thread, (2) shared_ptr lifetime management in ResponsePromiseMap during concurrent request/response, (3) UDS send queue and drain coroutine interaction during high-throughput forwarding, and (4) shutdown cleanup when connections are mid-flight.

The existing `tools/relay_benchmark.py` (1139 lines) is a fully functional HTTP benchmark tool with ML-DSA-87 auth flow, blob write/read, configurable concurrency (1/10/100), and multiple workload types. It can be reused directly as the ASAN load driver since ASAN instruments the relay server process, not the client. The CMake build system already supports `-DSANITIZER=asan` with proper compile and link flags. The existing `sanitizers/lsan.supp` provides suppression patterns for liboqs and Asio coroutine frame leaks that have been validated over many phases.

**Primary recommendation:** Create a bash test script that: (1) builds the relay with ASAN, (2) starts node + ASAN relay, (3) runs relay_benchmark.py at concurrency 1/10/100 with reduced iterations, (4) sends SIGHUP mid-run to verify config reload, (5) sends SIGTERM to verify clean drain, (6) parses ASAN stderr for errors, (7) produces a clear pass/fail exit code. Create a relay-specific LSAN suppression file at `relay/lsan_suppressions.txt`. Fix any bugs found.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Reuse existing `tools/relay_benchmark.py` as the load driver for ASAN testing. It already performs auth flow (challenge -> sign -> verify -> bearer token) and data operations at configurable concurrency levels. ASAN instruments the relay server process, not the client -- driver language doesn't matter.
- **D-02:** Run benchmark at 1, 10, and 100 concurrent HTTP clients against an ASAN-instrumented relay build. Parse ASAN stderr output for heap-use-after-free and other memory safety reports.
- **D-03:** Automated script that launches ASAN relay, starts benchmark load, sends SIGHUP mid-run (verify config change observed + no crash), then sends SIGTERM (verify clean drain + exit). Parse ASAN stderr for errors. Script must be repeatable and CI-friendly.
- **D-04:** SIGHUP test should verify at minimum: rate limit change takes effect, TLS cert swap works (if TLS enabled), no crash under concurrent load.
- **D-05:** SIGTERM test should verify: active connections drain, relay exits cleanly, ASAN reports no new issues during shutdown.
- **D-06:** Create an LSAN suppression file (`relay/lsan_suppressions.txt`) for known shutdown leaks from third-party libraries (liboqs global state, OpenSSL cleanup). These are accepted and documented.
- **D-07:** Any NEW leak reports (not covered by suppression file) are treated as bugs and fixed in this phase.
- **D-08:** Set `LSAN_OPTIONS=suppressions=relay/lsan_suppressions.txt` when running ASAN builds.
- **D-09:** Fix ALL ASAN-reported memory safety bugs found during testing -- in relay code OR node code (db/). No scope restriction on where fixes go.
- **D-10:** If a bug requires architectural changes beyond simple fixes, document it and discuss before proceeding. Simple bugs (use-after-free, buffer overrun, etc.) get fixed inline.

### Claude's Discretion
- Script language for the automated ASAN test harness (bash, Python, or mixed)
- Whether relay_benchmark.py needs minor modifications for ASAN testing (e.g., shorter runs, different workload mix)
- Suppression file format and granularity (function-level vs module-level suppressions)
- Plan decomposition -- how many plans to split this into

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| VER-02 | Relay runs ASAN-clean under benchmark tool at 1, 10, and 100 concurrent HTTP clients with zero heap-use-after-free or data race reports | Benchmark tool exists at tools/relay_benchmark.py with configurable concurrency. CMake SANITIZER=asan flag already wired. ASAN test script orchestrates build + run + parse. |
| VER-03 | Relay handles SIGHUP config reload and SIGTERM graceful shutdown correctly under single-threaded model | SIGHUP handler at relay_main.cpp:462+ reloads rate_limit, TLS, ACL, max_connections, max_blob_size, request_timeout. SIGTERM handler at relay_main.cpp:436+ stops acceptor, cancels promises, 2s drain timer, ioc.stop(). Test script sends signals mid-load and parses ASAN output. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| GCC ASAN | 15.2.1 (system) | Memory error detection | System compiler, `-fsanitize=address` already in CMakeLists.txt |
| LSAN (bundled with ASAN) | 15.2.1 | Leak detection at exit | Enabled by default with ASAN; controlled via `LSAN_OPTIONS` |
| Python 3 + httpx | 3.14.4 / 0.28.1 | HTTP load driver | relay_benchmark.py already uses these; all deps available |
| liboqs-python | system | ML-DSA-87 auth in benchmark | Already installed, benchmark tool depends on it |
| flatbuffers (Python) | system | Blob building in benchmark | Already installed, benchmark tool depends on it |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| bash | system | Test harness script | Orchestrates ASAN build + run + signal + parse |
| jq | system (if available) | Optional JSON config manipulation | For modifying relay config between SIGHUP tests |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Python benchmark tool | C++ stress tool | Unnecessary -- ASAN instruments server, not client |
| Bash test harness | Python test harness | Bash is simpler for process management, signals, stderr capture |
| Custom leak checker | Valgrind | ASAN is faster and already integrated via CMake |

**Installation:**
No new packages needed. All dependencies are already installed.

## Architecture Patterns

### Recommended Test Script Structure
```
tools/
  relay_asan_test.sh          # Main ASAN test harness (new)
relay/
  lsan_suppressions.txt       # Relay-specific LSAN suppressions (new)
```

### Pattern 1: ASAN Build + Run + Parse
**What:** Build relay with `-DSANITIZER=asan`, run under load, capture stderr, parse for ASAN error markers.
**When to use:** Every ASAN test run.
**Example:**
```bash
# Build
cmake -B build-asan -DSANITIZER=asan -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan --target chromatindb chromatindb_relay -j$(nproc)

# Run relay with ASAN
export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:log_path=asan_relay"
export LSAN_OPTIONS="suppressions=relay/lsan_suppressions.txt"
./build-asan/chromatindb_relay --config relay_test.json 2>asan_stderr.log &
RELAY_PID=$!

# Run load
python3 tools/relay_benchmark.py --relay-url http://127.0.0.1:4201 \
  --concurrency-levels "1,10,100" --iterations 20 --blob-sizes ""

# Parse ASAN output
if grep -qE 'ERROR: (Address|Leak)Sanitizer' asan_stderr.log; then
  echo "FAIL: ASAN errors detected"
  exit 1
fi
```

### Pattern 2: Signal Testing Under Load
**What:** Send SIGHUP/SIGTERM to ASAN-instrumented relay while load is running. Verify behavior and no ASAN errors.
**When to use:** VER-03 verification.
**Example:**
```bash
# Start load in background
python3 tools/relay_benchmark.py --concurrency-levels "10" --iterations 50 &
BENCH_PID=$!
sleep 2  # Let load establish

# SIGHUP: modify config, send signal, verify change
sed -i 's/"rate_limit_messages_per_sec": 0/"rate_limit_messages_per_sec": 999/' relay_test.json
kill -HUP $RELAY_PID
sleep 1  # Let reload complete
# Verify relay logs show "rate_limit reloaded: 999"

# Wait for benchmark to finish
wait $BENCH_PID

# SIGTERM: send signal, verify clean exit
kill -TERM $RELAY_PID
wait $RELAY_PID
EXIT_CODE=$?
# Parse asan_stderr.log for errors
```

### Pattern 3: LSAN Suppression File Format
**What:** Function-level suppressions for known third-party library leaks.
**When to use:** Relay LSAN suppression file.
**Example:**
```
# relay/lsan_suppressions.txt
# Relay-specific LeakSanitizer suppressions for accepted shutdown leaks.
# Usage: LSAN_OPTIONS=suppressions=relay/lsan_suppressions.txt

# liboqs: ML-DSA-87 global state not freed at process exit.
leak:OQS_SIG_new
leak:OQS_MEM_malloc

# OpenSSL: global SSL library state allocated once, not freed at exit.
leak:OPENSSL_init_ssl
leak:SSL_CTX_new

# Asio: coroutine frame allocations from in-flight coroutines at shutdown.
leak:awaitable_handler
leak:awaitable_frame
```

### Anti-Patterns to Avoid
- **Running ASAN with `halt_on_error=1` during load testing:** A single error would terminate the relay before collecting all error reports. Use `halt_on_error=0` to continue and capture all errors.
- **Parsing ASAN output with simple grep:** ASAN error reports are multi-line. Match on `ERROR: AddressSanitizer` and `ERROR: LeakSanitizer` header lines, not individual report lines.
- **Skipping the transfer-back check after offload():** The offload() pattern resumes on the thread pool thread. If any code path accesses shared state (TokenStore, ChallengeStore) without the `co_await asio::post(ioc, asio::use_awaitable)` transfer-back, ASAN may not catch it (single-threaded model) but TSAN would. Worth a manual review during testing.
- **Using `--blob-sizes` in ASAN benchmark:** Large blob tests (1-100 MiB) are for PERF-03 (Phase 113), not ASAN. Under ASAN the 3-5x slowdown makes large blobs painfully slow. Use `--blob-sizes ""` to skip them.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| HTTP load generation | Custom C++ stress client | `tools/relay_benchmark.py` | Already handles auth flow, blob build, concurrency, error counting. ASAN instruments the server, client language irrelevant. |
| LSAN suppression format | Custom leak filter | LSAN suppression file syntax | Standard `leak:function_name` format, well documented, compiler-native |
| ASAN output parsing | Full ASAN report parser | Simple grep for `ERROR:` lines | ASAN summary lines are sufficient for pass/fail. Full reports are in the log for manual inspection. |
| Build configuration | Custom compiler flags | `cmake -DSANITIZER=asan` | Already wired in root CMakeLists.txt with correct flags |
| Process lifecycle management | Custom C++ harness | Bash `kill -HUP`/`kill -TERM` | Native signal delivery, simpler than any IPC mechanism |

**Key insight:** The relay benchmark tool already exists and covers the full auth + data path. The only new code needed is the orchestration script and suppression file. This is a testing phase, not a feature phase.

## Common Pitfalls

### Pitfall 1: ASAN Slowdown Causes Benchmark Timeouts
**What goes wrong:** ASAN instrumentation adds 2-5x runtime overhead. At 100 concurrent clients, the benchmark's HTTP timeouts (120s in httpx.AsyncClient) may be too short, causing spurious failures.
**Why it happens:** Each ML-DSA-87 verify takes ~10ms normally but ~30-50ms under ASAN. With 100 clients queuing on the single event loop, latency compounds.
**How to avoid:** Reduce benchmark iterations for ASAN runs (e.g., 20 iterations instead of 100). Set relay config `request_timeout_seconds=0` (disabled) and `rate_limit_messages_per_sec=0` (disabled). Use `httpx.Timeout(300.0)` if modifying the benchmark.
**Warning signs:** Benchmark errors > 0, "connection reset" errors, relay log shows dropped connections.

### Pitfall 2: Node Must Be Running for Full Data Path
**What goes wrong:** The benchmark writes blobs via POST /blob, which forwards to the node via UDS. Without a running chromatindb node, the UDS multiplexer has no connection and all data operations fail with 502.
**Why it happens:** Relay is a gateway, not standalone. Auth-only testing (challenge + verify) works without node, but data operations require the full pipeline.
**How to avoid:** Start a chromatindb node with a test config before launching the relay. The ASAN test script must manage both processes.
**Warning signs:** All blob operations return 502 or connection refused. Relay log shows "UDS not connected".

### Pitfall 3: ASAN Shutdown Leaks vs Real Leaks
**What goes wrong:** ASAN/LSAN reports leaks from liboqs global state, OpenSSL library init, and Asio coroutine frames that were in-flight at SIGTERM. These look like bugs but are accepted shutdown leaks.
**Why it happens:** Third-party libraries allocate global state once and rely on process exit to free it. Coroutines suspended at SIGTERM have their frames leaked by design (Asio's detached spawn).
**How to avoid:** Use the LSAN suppression file (D-06/D-08). Review each LSAN report against the suppression list before declaring failure. Only NEW, un-suppressed leaks are bugs (D-07).
**Warning signs:** LSAN reports on exit from `OQS_SIG_new`, `OPENSSL_init_ssl`, `awaitable_handler` -- these are expected. Reports from relay/ functions are real bugs.

### Pitfall 4: SIGHUP Config File Race
**What goes wrong:** If the test script modifies the config file and sends SIGHUP before the file write is flushed, the relay reads a partially written config and logs a parse error.
**Why it happens:** `sed -i` and `echo >` are not atomic with respect to the SIGHUP signal delivery.
**How to avoid:** Write the new config to a temp file, then `mv` (atomic on same filesystem) before sending SIGHUP. Or add a small sleep after write.
**Warning signs:** Relay logs "SIGHUP config reload failed: parse error".

### Pitfall 5: ASAN build must include both chromatindb and chromatindb_relay
**What goes wrong:** Building only `chromatindb_relay` target produces an ASAN-instrumented relay but the node binary (`chromatindb`) is built without ASAN. When the node crashes, the test misattributes it to the relay.
**Why it happens:** The test needs both processes. If the node isn't ASAN-instrumented too, node bugs are invisible.
**How to avoid:** Build both targets: `cmake --build build-asan --target chromatindb chromatindb_relay`. Per D-09, bugs in either relay or node code are in scope.
**Warning signs:** Node crashes with SIGSEGV but no ASAN report. Only relay ASAN output is captured.

## Code Examples

### Relay ASAN Build Command
```bash
# Source: CMakeLists.txt lines 31-34
cmake -B build-asan \
  -DSANITIZER=asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=OFF
cmake --build build-asan --target chromatindb chromatindb_relay -j$(nproc)
```

### ASAN Environment Variables
```bash
# Source: GCC ASAN documentation
export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:print_stats=0:log_path=/dev/stderr"
export LSAN_OPTIONS="suppressions=relay/lsan_suppressions.txt"
```
Note: `log_path=/dev/stderr` keeps all ASAN output on stderr for easy capture. `halt_on_error=0` allows the process to continue after detecting an error, collecting all reports before exit.

### Relay Test Config (Permissive for Stress Testing)
```json
{
  "bind_address": "127.0.0.1",
  "bind_port": 4201,
  "uds_path": "/tmp/chromatindb_asan_test.sock",
  "identity_key_path": "/tmp/asan_test_relay.key",
  "log_level": "info",
  "rate_limit_messages_per_sec": 0,
  "request_timeout_seconds": 0,
  "max_blob_size_bytes": 0,
  "max_connections": 1024
}
```

### Benchmark Command for ASAN (Reduced Load)
```bash
# Skip large blobs (too slow under ASAN), fewer iterations
python3 tools/relay_benchmark.py \
  --relay-url http://127.0.0.1:4201 \
  --concurrency-levels "1,10,100" \
  --iterations 20 \
  --warmup 3 \
  --blob-sizes "" \
  --output /dev/null \
  --mixed-duration 10
```

### ASAN Error Detection Pattern
```bash
# Parse relay stderr for ASAN error markers
ASAN_ERRORS=0
if grep -qE 'ERROR: AddressSanitizer' "$RELAY_STDERR"; then
    echo "FAIL: AddressSanitizer detected memory errors"
    grep -A 20 'ERROR: AddressSanitizer' "$RELAY_STDERR"
    ASAN_ERRORS=1
fi

# LeakSanitizer runs at exit after SIGTERM
if grep -qE 'ERROR: LeakSanitizer' "$RELAY_STDERR"; then
    # Check if leaks are ONLY suppressed ones
    UNSUPPRESSED=$(grep -c 'SUMMARY: AddressSanitizer.*leaked' "$RELAY_STDERR" || true)
    if [ "$UNSUPPRESSED" -gt 0 ]; then
        echo "WARNING: LeakSanitizer found un-suppressed leaks"
        grep -A 30 'ERROR: LeakSanitizer' "$RELAY_STDERR"
        ASAN_ERRORS=1
    fi
fi
```

### SIGHUP Verification Pattern
```bash
# Write new config with changed rate limit
jq '.rate_limit_messages_per_sec = 999' "$CONFIG_FILE" > "$CONFIG_FILE.tmp"
mv "$CONFIG_FILE.tmp" "$CONFIG_FILE"

# Send SIGHUP
kill -HUP "$RELAY_PID"
sleep 1

# Verify rate limit change took effect
if grep -q "rate_limit reloaded: 999" "$RELAY_STDERR"; then
    echo "PASS: SIGHUP rate_limit change observed"
else
    echo "FAIL: SIGHUP rate_limit change not observed"
fi
```

### SIGTERM Verification Pattern
```bash
# Send SIGTERM
kill -TERM "$RELAY_PID"

# Wait for clean exit (max 10 seconds)
TIMEOUT=10
while kill -0 "$RELAY_PID" 2>/dev/null && [ "$TIMEOUT" -gt 0 ]; do
    sleep 1
    TIMEOUT=$((TIMEOUT - 1))
done

if kill -0 "$RELAY_PID" 2>/dev/null; then
    echo "FAIL: Relay did not exit within 10 seconds"
    kill -9 "$RELAY_PID"
else
    echo "PASS: Relay exited cleanly after SIGTERM"
fi

# Verify ASAN clean on exit
if ! grep -qE 'ERROR: AddressSanitizer' "$RELAY_STDERR"; then
    echo "PASS: No ASAN errors during shutdown"
fi
```

## ASAN-Specific Technical Details

### What ASAN Catches
| Error Class | Detection | Relevance to Relay |
|-------------|-----------|-------------------|
| heap-use-after-free | Quarantined memory access | Coroutine frames referencing destroyed objects after co_await |
| heap-buffer-overflow | Redzone canary trip | FlatBuffer/binary parsing bounds errors in translator |
| stack-use-after-return | Stack frame tracking | Lambda captures of local variables in coroutines (Phase 106 FIX-02 already addressed std::visit) |
| use-after-scope | Scope tracking | Local variable lifetime in async handlers |
| double-free | Redzone pattern check | shared_ptr misuse in ResponsePromiseMap |
| memory leak (LSAN) | Exit-time reachability | Third-party library global state (suppressed) vs relay code leaks (bugs) |

### What ASAN Does NOT Catch (Important)
| Error Class | Why Not | Mitigation |
|-------------|---------|------------|
| Data races | Need TSAN | Single-threaded model eliminates races by design; offload() is the only cross-thread boundary |
| Uninitialized reads | Need MSAN | Not in scope for this phase |
| Integer overflow | Need UBSAN | Already verified in prior phases |

### ASAN Performance Impact
ASAN typically adds 2-5x overhead due to:
- Shadow memory mapping (1/8th of address space)
- Redzone insertion around allocations
- Quarantine for freed memory
- Stack instrumentation

For the relay under ASAN:
- ML-DSA-87 verify: ~10ms normal -> ~25-40ms under ASAN (CPU-bound, offloaded)
- HTTP request/response: ~1ms normal -> ~3-5ms under ASAN
- At concurrency=100 with single event loop: queue depth increases significantly
- Recommendation: 20 iterations per client (not 100) to keep total runtime under 5 minutes

## Existing Suppression Patterns

The project has existing suppression files at `sanitizers/`:
- `sanitizers/lsan.supp` -- liboqs (OQS_SIG_new, OQS_MEM_malloc), Asio (basic_resolver_results, resolve_query_op, awaitable_handler, awaitable_frame)
- `sanitizers/asan.supp` -- empty (no ASAN suppressions needed for db/)

The new `relay/lsan_suppressions.txt` should carry forward the relevant patterns and add relay-specific ones (OpenSSL TLS context if leaked at shutdown).

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Bash test script + Python benchmark tool |
| Config file | tools/relay_asan_test.sh (new, Wave 0) |
| Quick run command | `bash tools/relay_asan_test.sh` |
| Full suite command | `bash tools/relay_asan_test.sh` (same -- single script) |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| VER-02 | ASAN-clean at 1/10/100 concurrent clients | integration | `bash tools/relay_asan_test.sh` | Wave 0 |
| VER-03 | SIGHUP config reload + SIGTERM drain under ASAN | integration | `bash tools/relay_asan_test.sh` (includes signal tests) | Wave 0 |

### Sampling Rate
- **Per task commit:** Run full ASAN test script after each fix
- **Per wave merge:** Full ASAN test script must pass
- **Phase gate:** ASAN test script exits 0 with all checks passed

### Wave 0 Gaps
- [ ] `tools/relay_asan_test.sh` -- Main ASAN test harness script
- [ ] `relay/lsan_suppressions.txt` -- Relay-specific LSAN suppression file
- [ ] Relay test config template for ASAN runs (JSON, permissive settings)

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| GCC with ASAN | ASAN build | Yes | 15.2.1 | -- |
| CMake | Build system | Yes | 4.3.1 | -- |
| Python 3 | Benchmark tool | Yes | 3.14.4 | -- |
| httpx (Python) | Benchmark HTTP client | Yes | 0.28.1 | -- |
| liboqs-python | Benchmark ML-DSA-87 auth | Yes | available | -- |
| flatbuffers (Python) | Benchmark blob building | Yes | available | -- |
| jq | Config manipulation in test script | Check needed | -- | sed/Python fallback |
| chromatindb node binary | Full data path testing | Build from source | -- | -- |

**Missing dependencies with no fallback:**
- None -- all dependencies available.

**Missing dependencies with fallback:**
- jq: If not installed, use sed or Python one-liners for JSON config modification in the test script.

## Open Questions

1. **jq availability**
   - What we know: jq is commonly installed on Linux but not guaranteed.
   - What's unclear: Whether it's installed on the dev machine.
   - Recommendation: Check `command -v jq` in the test script. Fall back to Python one-liner for JSON modification if absent.

2. **Relay_benchmark.py iteration count for ASAN**
   - What we know: 100 iterations at 100 clients under ASAN is 10,000 operations with 3-5x slowdown. Could take 10+ minutes.
   - What's unclear: Exact ASAN overhead for this specific workload.
   - Recommendation: Start with 20 iterations, adjust based on runtime. The goal is memory safety proof, not performance measurement.

3. **Node ASAN build necessity**
   - What we know: D-09 says fix bugs in relay OR node code. D-02 says ASAN instruments the relay process.
   - What's unclear: Whether the node should also be ASAN-instrumented for this phase.
   - Recommendation: Build both with ASAN since both processes share the same CMake configuration and D-09 puts node bugs in scope. The node is already ASAN-clean (647 tests pass under ASAN), so any new findings would be integration-specific.

## Sources

### Primary (HIGH confidence)
- `CMakeLists.txt` lines 31-50 -- SANITIZER=asan build flag configuration (verified in codebase)
- `sanitizers/lsan.supp` -- existing LSAN suppression patterns (verified in codebase)
- `tools/relay_benchmark.py` -- full benchmark tool with auth + data operations (1139 lines, verified in codebase)
- `relay/relay_main.cpp` lines 436-538 -- SIGHUP/SIGTERM signal handlers (verified in codebase)
- `relay/http/http_router.cpp` lines 306-310 -- offload() + transfer-back pattern (verified in codebase)
- Phase 111 verification report -- confirms 377 relay tests pass, single-threaded model complete (verified in codebase)
- Phase 106 verification report -- confirms relay ASAN clean for basic smoke test (verified in codebase)

### Secondary (MEDIUM confidence)
- GCC ASAN documentation -- ASAN_OPTIONS, LSAN_OPTIONS environment variables (well-documented, stable)

### Tertiary (LOW confidence)
- None.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all tools exist and are verified available on the system
- Architecture: HIGH -- reusing existing benchmark tool; only new code is the orchestration script
- Pitfalls: HIGH -- based on extensive prior ASAN experience in this project (Phase 46, 106, 107)

**Research date:** 2026-04-14
**Valid until:** 2026-05-14 (stable domain, no fast-moving dependencies)
