# Phase 113: Performance Benchmarking - Research

**Researched:** 2026-04-14
**Domain:** Relay performance measurement and orchestration tooling (bash + Python)
**Confidence:** HIGH

## Summary

This phase creates a standalone shell script (`tools/relay_perf_test.sh`) that builds Release binaries, starts a local node+relay pair, runs the existing `tools/relay_benchmark.py` with all 4 PERF workloads, and captures results in `tools/benchmark_report.md`. The heavy lifting is already done: `relay_benchmark.py` (1139 lines) implements all 4 workloads with ML-DSA-87 auth, FlatBuffer blob construction, async HTTP via httpx, and markdown report generation. The shell script follows the established pattern in `tools/relay_asan_test.sh` (temp dir, config generation, node+relay startup, cleanup trap).

The main scripting work is adapting the ASAN test harness for Release builds: swap `-DSANITIZER=asan -DCMAKE_BUILD_TYPE=Debug` for `-DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF`, keep the node+relay startup/teardown pattern, configure the relay with `rate_limit_messages_per_sec=0` and `request_timeout_seconds=0`, and invoke `relay_benchmark.py` with default parameters. The script should also inject git hash and build type metadata that the Python tool does not currently capture.

**Primary recommendation:** Create `tools/relay_perf_test.sh` by adapting `tools/relay_asan_test.sh` — replace ASAN build with Release build, strip ASAN analysis sections, invoke `relay_benchmark.py` with defaults, and append git/build metadata to the report.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Create a new standalone script `tools/relay_perf_test.sh` that builds Release binaries, starts node+relay, runs `relay_benchmark.py` with all 4 workloads, and captures the report. Pattern follows `tools/relay_asan_test.sh` but optimized for performance (Release build, no sanitizer overhead).
- **D-02:** Build type is Release (`-DCMAKE_BUILD_TYPE=Release`) for production-representative numbers. No debug symbols, full optimizations.
- **D-03:** Use `relay_benchmark.py` defaults as-is: 100 iterations, 5 warmup, concurrency 1/10/100, blob sizes 1/10/50/100 MiB, 30s mixed duration. The tool is already well-tuned.
- **D-04:** Run all 4 workloads (PERF-01 through PERF-04). No skipping.
- **D-05:** Report stored at `tools/benchmark_report.md` (the default from relay_benchmark.py). Stays with tooling, easy to find.
- **D-06:** Standard metadata: git hash, build type, CPU model, OS, date. The benchmark tool already captures most of this.
- **D-07:** The script runs benchmarks and produces the report only. No pass/fail thresholds. This is a baseline measurement phase, not a regression gate.

### Claude's Discretion
- Script structure and error handling details
- Whether to add any warmup or cooldown between workloads
- Node/relay config tuning for benchmark (e.g., max_connections)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| PERF-01 | Throughput benchmark produces blobs/sec at 1, 10, and 100 concurrent HTTP clients recorded in benchmark report | `relay_benchmark.py` `run_throughput_benchmark()` handles this with configurable concurrency levels. Default `--concurrency-levels "1,10,100"` matches requirement exactly. |
| PERF-02 | Latency benchmark measures per-operation round-trip time (p50/p95/p99) through HTTP relay recorded in benchmark report | `relay_benchmark.py` `run_latency_benchmark()` measures write, read, exists, and stats latency with p50/p95/p99. Default `--skip-uds` measures relay-only path. |
| PERF-03 | Large blob benchmark measures write+read throughput at 1 MiB, 10 MiB, 50 MiB, 100 MiB with MiB/sec recorded | `relay_benchmark.py` `run_large_blob_benchmark()` with default `--blob-sizes "1048576,10485760,52428800,104857600"`. Reports write/read MiB/s per size. |
| PERF-04 | Mixed workload benchmark measures small-query latency degradation under concurrent large-blob load | `relay_benchmark.py` `run_mixed_workload_benchmark()` runs baseline (small queries only) then under-load (small + large), computes degradation percentage. Default 30s duration. |
</phase_requirements>

## Standard Stack

### Core
| Tool | Version | Purpose | Why Standard |
|------|---------|---------|--------------|
| `relay_benchmark.py` | Existing (1139 LOC) | All 4 PERF workloads | Already implements PERF-01 through PERF-04 with ML-DSA-87 auth, FlatBuffer encoding, httpx async HTTP |
| `relay_asan_test.sh` | Existing (402 LOC) | Template for orchestration script | Proven node+relay startup, config gen, cleanup trap pattern |
| CMake | 4.3.1 (installed) | Release build | Standard project build system |

### Supporting
| Tool | Version | Purpose | When to Use |
|------|---------|---------|-------------|
| Python 3 | 3.14.4 (installed) | Run benchmark tool | Always -- benchmark is Python |
| httpx | 0.28.1 (installed) | Async HTTP client | Used by relay_benchmark.py |
| flatbuffers | installed | FlatBuffer blob encoding | Used by relay_benchmark.py |
| liboqs-python (oqs) | installed | ML-DSA-87 identity/signing | Used by relay_benchmark.py |

**Installation:** All dependencies already installed. No new packages required.

## Architecture Patterns

### Script Structure (adapt from relay_asan_test.sh)

```
tools/relay_perf_test.sh
  1. Setup (SCRIPT_DIR, REPO_ROOT, BUILD_DIR, WORK_DIR)
  2. Parse --skip-build flag
  3. Cleanup trap (EXIT handler kills PIDs, removes temp dir)
  4. Build: cmake Release + chromatindb + chromatindb_relay
  5. Generate node.json and relay.json in WORK_DIR
  6. Start node, wait for UDS socket
  7. Start relay, wait for TCP accept
  8. Run relay_benchmark.py with defaults
  9. Append git/build metadata to report
  10. Print summary
```

### Key Config Differences from ASAN Script

| Setting | ASAN Script | Perf Script | Why |
|---------|-------------|-------------|-----|
| `CMAKE_BUILD_TYPE` | Debug | Release | Production-representative numbers (D-02) |
| `-DSANITIZER` | asan | (omitted) | No instrumentation overhead |
| `-DBUILD_TESTING` | OFF | OFF | Don't build tests |
| `BUILD_DIR` | build-asan | build-release | Separate directory |
| `rate_limit_messages_per_sec` | 0 | 0 | Same -- no artificial limits |
| `request_timeout_seconds` | 0 | 0 | Same -- no timeouts during benchmark |
| `max_blob_size_bytes` | 0 | 0 | Same -- no size limit (100 MiB blobs in PERF-03) |
| `max_connections` | 1024 | 1024 | Default is sufficient for 100 concurrent clients |
| Benchmark iterations | 20 (reduced for ASAN) | 100 (default) | Full measurement |
| Blob sizes | "" (skipped) | default (1/10/50/100 MiB) | Full PERF-03 |
| Mixed duration | 10s (reduced) | 30s (default) | Full PERF-04 |

### relay_benchmark.py Invocation

The script should invoke with all defaults, only overriding `--relay-url` and `--output`:

```bash
python3 "$REPO_ROOT/tools/relay_benchmark.py" \
    --relay-url "http://127.0.0.1:$RELAY_PORT" \
    --output "$REPO_ROOT/tools/benchmark_report.md"
```

All other flags use defaults from the tool:
- `--warmup 5`
- `--iterations 100`
- `--concurrency-levels "1,10,100"`
- `--blob-sizes "1048576,10485760,52428800,104857600"`
- `--mixed-duration 30`
- `--skip-uds` (default: true)

### Metadata Gap: Git Hash and Build Type

`relay_benchmark.py` captures `hardware` (platform + processor) and `CPUs` but does NOT capture git hash or build type. The script should append this metadata to the report after the benchmark completes:

```bash
GIT_HASH=$(git -C "$REPO_ROOT" rev-parse --short HEAD)
echo "" >> "$REPORT_PATH"
echo "---" >> "$REPORT_PATH"
echo "**Git hash:** $GIT_HASH" >> "$REPORT_PATH"
echo "**Build type:** Release" >> "$REPORT_PATH"
```

### Pattern: No ASAN Analysis Section

The ASAN script has sections for SIGHUP test, SIGTERM test, and ASAN output parsing. The perf script omits all of these -- it only builds, starts, runs benchmark, and captures the report (D-07: no pass/fail thresholds).

### Pattern: Node Port Selection

The ASAN script uses a hardcoded high port (4290/4291) to avoid conflicts. The perf script should follow the same pattern with different ports (e.g., 4280/4281) to allow parallel execution, or reuse the same ports since they are unlikely to conflict.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Benchmark measurement | Custom timing/stats | `relay_benchmark.py` | 1139 lines of tested benchmark code with percentiles, report generation |
| Node+relay startup | Custom orchestration | Pattern from `relay_asan_test.sh` | Proven temp dir, config gen, UDS wait, TCP wait, cleanup trap |
| FlatBuffer blob encoding | Manual binary packing | `relay_benchmark.py` `build_blob()` | Correct VTable layout, ML-DSA-87 signing, SHA3-256 digest |
| ML-DSA-87 auth flow | Custom auth implementation | `relay_benchmark.py` `authenticate()` | Challenge-response with nonce signing, token bearer |
| Percentile calculation | Custom stats | `relay_benchmark.py` `compute_percentiles()` | Handles edge cases, sorted array approach |

**Key insight:** This phase is orchestration, not implementation. The benchmark tool and startup patterns exist. The only new code is a ~150-line shell script that glues them together with Release build settings.

## Common Pitfalls

### Pitfall 1: Large Blob Memory on Dev Machine
**What goes wrong:** 100 MiB blob test requires building the FlatBuffer in memory (100 MiB data + 8 KB header + ML-DSA-87 signature). The Python process and the relay both hold the blob in memory during transfer.
**Why it happens:** `build_blob()` creates the full FlatBuffer in memory; httpx sends it as `content=blob_bytes`; relay receives the full body before forwarding to node.
**How to avoid:** Ensure the machine has sufficient RAM (500+ MiB free should be ample). The default `max_blob_size_bytes=0` (unlimited) in relay config is correct -- do not set a limit.
**Warning signs:** httpx timeout errors, relay crashes on large POST bodies, out-of-memory kills.

### Pitfall 2: Relay Port Conflict
**What goes wrong:** If another relay or process is already using port 4291 (or whatever port the script uses), the relay fails to bind.
**Why it happens:** The ASAN script uses hardcoded ports 4290/4291.
**How to avoid:** Use unique ports (e.g., 4280/4281) or check port availability before starting. The ports only need to avoid conflicting with a simultaneously running ASAN test.
**Warning signs:** "Address already in use" in relay stderr, script fails at "waiting for relay to accept connections".

### Pitfall 3: Benchmark Tool Missing Dependencies
**What goes wrong:** `relay_benchmark.py` requires `httpx`, `flatbuffers`, and `oqs` Python packages.
**Why it happens:** These are not stdlib; they must be pip-installed or system-packaged.
**How to avoid:** Verified all three are installed on this machine (httpx 0.28.1, flatbuffers, oqs). The script could add a dependency check (`python3 -c "import httpx, flatbuffers, oqs"`) as an early guard.
**Warning signs:** ImportError at script start with helpful message (the benchmark tool already prints missing deps).

### Pitfall 4: Node Not Ready When Relay Starts
**What goes wrong:** Relay tries to connect to UDS before the node has created the socket file, failing the TrustedHello handshake.
**Why it happens:** Node startup includes MDBX database initialization which can take a moment.
**How to avoid:** Wait for UDS socket file to exist (same `while [ ! -S ... ]` loop from ASAN script) before starting relay.
**Warning signs:** Relay logs "UDS connection failed" repeatedly, eventually connects but with wasted time.

### Pitfall 5: Benchmark Takes Too Long
**What goes wrong:** With 100 iterations at 100 concurrency, plus 4 large blob sizes (100 MiB x signing is ~2s), plus 60s mixed workload (30s baseline + 30s load), total runtime can be 10-20 minutes.
**Why it happens:** ML-DSA-87 signing is CPU-intensive (~100ms per sign); 100 MiB blob signing is ~2s; 100 concurrent clients each do 100 iterations.
**How to avoid:** This is expected and correct for a baseline measurement. The script should not add a global timeout. The ASAN script completed with reduced parameters; full parameters will simply take longer.
**Warning signs:** Not a pitfall per se -- just set expectations. Estimate 10-20 minutes total runtime.

### Pitfall 6: SIGTERM Cleanup Race
**What goes wrong:** If the benchmark Python process is still running when the script is interrupted, orphan node/relay processes remain.
**Why it happens:** The cleanup trap kills node/relay PIDs but the Python benchmark might spawn subprocesses.
**How to avoid:** The `relay_benchmark.py` tool uses `asyncio.run()` with a single event loop -- no subprocesses. The cleanup trap pattern from ASAN script (kill relay, kill node, rm temp dir) is sufficient. Add the benchmark PID to cleanup if running via `&` and `wait`.
**Warning signs:** Zombie chromatindb/chromatindb_relay processes after interrupted script runs.

## Code Examples

### Relay Config for Benchmarking (from ASAN script, verified against relay_config.h)

```json
{
  "bind_address": "127.0.0.1",
  "bind_port": 4281,
  "uds_path": "/tmp/relay_perf_test.XXXXXX/chromatindb.sock",
  "identity_key_path": "/tmp/relay_perf_test.XXXXXX/relay.key",
  "log_level": "info",
  "rate_limit_messages_per_sec": 0,
  "request_timeout_seconds": 0,
  "max_blob_size_bytes": 0,
  "max_connections": 1024
}
```

Key fields for benchmarking:
- `rate_limit_messages_per_sec: 0` -- disables rate limiting (relay_config.h line 23)
- `request_timeout_seconds: 0` -- disables request timeouts (relay_config.h line 24)
- `max_blob_size_bytes: 0` -- no blob size limit for 100 MiB tests (relay_config.h line 25)
- `max_connections: 1024` -- sufficient for 100 concurrent benchmark clients (relay_config.h line 20)

### Node Config for Benchmarking

```json
{
  "bind_address": "127.0.0.1:4280",
  "uds_path": "/tmp/relay_perf_test.XXXXXX/chromatindb.sock",
  "log_level": "info"
}
```

### Build Command (Release)

```bash
cmake -B "$BUILD_DIR" -S "$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF
cmake --build "$BUILD_DIR" --target chromatindb chromatindb_relay -j"$(nproc)"
```

### UDS Socket Wait Loop (from relay_asan_test.sh)

```bash
TIMEOUT=30
while [ ! -S "$WORK_DIR/chromatindb.sock" ] && [ "$TIMEOUT" -gt 0 ]; do
    sleep 1
    TIMEOUT=$((TIMEOUT - 1))
    if ! kill -0 "$NODE_PID" 2>/dev/null; then
        fail "Node startup: process exited"
        exit 1
    fi
done
```

### TCP Port Wait Loop (from relay_asan_test.sh)

```bash
TIMEOUT=15
while ! bash -c "echo > /dev/tcp/127.0.0.1/$RELAY_PORT" 2>/dev/null && [ "$TIMEOUT" -gt 0 ]; do
    sleep 1
    TIMEOUT=$((TIMEOUT - 1))
    if ! kill -0 "$RELAY_PID" 2>/dev/null; then
        fail "Relay startup: process exited"
        exit 1
    fi
done
```

### Git Hash Capture

```bash
GIT_HASH=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| ASAN build for testing | Release build for benchmarking | Phase 113 | 2-10x faster execution, production-representative numbers |
| Reduced iterations (20) | Full iterations (100) | Phase 113 | Statistically meaningful results |
| No large blob testing | 1/10/50/100 MiB blobs | Phase 113 | PERF-03 coverage |
| 10s mixed workload | 30s mixed workload | Phase 113 | PERF-04 stability |

## Open Questions

1. **Port allocation strategy**
   - What we know: ASAN script uses 4290/4291. Perf script needs different ports if both might run simultaneously.
   - What's unclear: Whether parallel execution is a use case.
   - Recommendation: Use 4280/4281 as a simple convention. Not worth dynamic port allocation for a developer tool.

2. **Report metadata injection**
   - What we know: `relay_benchmark.py` does not capture git hash or build type. D-06 says "The benchmark tool already captures most of this."
   - What's unclear: Whether to modify the Python tool or append from the shell script.
   - Recommendation: Append from the shell script (simpler, no tool modification needed, keeps benchmark tool reusable as-is).

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| Python 3 | relay_benchmark.py | Yes | 3.14.4 | -- |
| httpx | relay_benchmark.py | Yes | 0.28.1 | -- |
| flatbuffers | relay_benchmark.py | Yes | installed | -- |
| liboqs-python (oqs) | relay_benchmark.py | Yes | installed | -- |
| CMake | Release build | Yes | 4.3.1 | -- |
| nproc | Parallel build | Yes | 12 cores | -- |
| git | Hash capture | Yes | installed | -- |

**Missing dependencies with no fallback:** None.
**Missing dependencies with fallback:** None.

## Sources

### Primary (HIGH confidence)
- `tools/relay_benchmark.py` -- Full source read, all 4 PERF workloads verified, CLI args at lines 998-1066
- `tools/relay_asan_test.sh` -- Full source read, startup/cleanup pattern verified
- `relay/config/relay_config.h` -- All config fields verified (rate_limit, request_timeout, max_blob_size, max_connections)
- `tests/integration/configs/node1.json` -- Node config structure verified
- `.planning/phases/113-performance-benchmarking/113-CONTEXT.md` -- All 7 locked decisions

### Secondary (MEDIUM confidence)
- Environment availability verified by running `python3 -c "import httpx, flatbuffers, oqs"`, `cmake --version`, `nproc`

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all tools exist and are verified installed
- Architecture: HIGH -- direct adaptation of proven ASAN test pattern
- Pitfalls: HIGH -- identified from reading both scripts and config headers

**Research date:** 2026-04-14
**Valid until:** 2026-05-14 (stable -- no fast-moving dependencies)
