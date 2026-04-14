# Phase 113: Performance Benchmarking - Context

**Gathered:** 2026-04-14
**Status:** Ready for planning

<domain>
## Phase Boundary

Run all 4 benchmark workloads (throughput, latency, large blob, mixed) against a live relay and record baseline performance numbers. This phase produces tooling (benchmark runner script) and data (benchmark report), not relay code changes.

</domain>

<decisions>
## Implementation Decisions

### Test Environment
- **D-01:** Create a new standalone script `tools/relay_perf_test.sh` that builds Release binaries, starts node+relay, runs `relay_benchmark.py` with all 4 workloads, and captures the report. Pattern follows `tools/relay_asan_test.sh` but optimized for performance (Release build, no sanitizer overhead).
- **D-02:** Build type is Release (`-DCMAKE_BUILD_TYPE=Release`) for production-representative numbers. No debug symbols, full optimizations.

### Benchmark Parameters
- **D-03:** Use `relay_benchmark.py` defaults as-is: 100 iterations, 5 warmup, concurrency 1/10/100, blob sizes 1/10/50/100 MiB, 30s mixed duration. The tool is already well-tuned.
- **D-04:** Run all 4 workloads (PERF-01 through PERF-04). No skipping.

### Report Format
- **D-05:** Report stored at `tools/benchmark_report.md` (the default from relay_benchmark.py). Stays with tooling, easy to find.
- **D-06:** Standard metadata: git hash, build type, CPU model, OS, date. The benchmark tool already captures most of this.

### Automation Script
- **D-07:** The script runs benchmarks and produces the report only. No pass/fail thresholds. This is a baseline measurement phase, not a regression gate. Thresholds can be added later when we have historical data.

### Claude's Discretion
- Script structure and error handling details
- Whether to add any warmup or cooldown between workloads
- Node/relay config tuning for benchmark (e.g., max_connections)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Requirements
- `.planning/REQUIREMENTS.md` -- PERF-01 (throughput), PERF-02 (latency), PERF-03 (large blob), PERF-04 (mixed workload)

### Benchmark tool
- `tools/relay_benchmark.py` -- Existing HTTP benchmark with all 4 workloads implemented. CLI args at lines 1019-1066. Main entry at line 1069.

### Prior phase tooling (pattern to follow)
- `tools/relay_asan_test.sh` -- ASAN test harness from Phase 112. Same node+relay startup pattern, temp dir, cleanup trap, config generation.

### Relay config
- `relay/config/relay_config.h` -- Config fields. Need rate_limit_messages_per_sec=0 and request_timeout_seconds=0 for benchmarking.

### Node config
- `db/config/config.h` lines 40-55 -- Node config fields for uds_path
- `tests/integration/configs/node1.json` -- Example node config

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `tools/relay_benchmark.py`: Complete benchmark tool with all 4 PERF workloads, ML-DSA-87 auth, configurable CLI. 1139 lines.
- `tools/relay_asan_test.sh`: Node+relay startup pattern, temp dir management, cleanup trap, config generation. Direct template for the perf script.

### Established Patterns
- Build: `cmake -B build-dir -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF` + `cmake --build build-dir --target chromatindb chromatindb_relay -j$(nproc)`
- Node startup: ephemeral bind port, absolute UDS path, wait for socket file
- Relay startup: connect to UDS, wait for TCP/HTTP accept
- Cleanup: trap EXIT, kill PIDs, remove temp dir

### Integration Points
- Relay needs running chromatindb node on UDS for full data path (auth + write + read)
- relay_benchmark.py connects via HTTP to relay, which forwards to node via UDS
- Report output goes to --output path (default: tools/benchmark_report.md)

</code_context>

<specifics>
## Specific Ideas

No specific requirements -- use standard approaches. The benchmark tool already handles the heavy lifting; the script just needs to orchestrate build + start + run + capture.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope.

</deferred>

---

*Phase: 113-performance-benchmarking*
*Context gathered: 2026-04-14*
