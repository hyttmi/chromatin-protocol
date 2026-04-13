# Phase 110: Performance Benchmarking - Context

**Gathered:** 2026-04-13
**Status:** Ready for planning

<domain>
## Phase Boundary

Build a Python benchmark tool that measures relay performance across 4 workload types: throughput (concurrent clients), latency overhead (relay vs UDS), large blob transfer, and mixed workload. Results recorded in a markdown report. All benchmarks run on local dev laptop (node on UDS, relay on localhost).

</domain>

<decisions>
## Implementation Decisions

### Tooling
- **D-01:** Python benchmark tool at `tools/relay_benchmark.py`. Uses `websockets` library + `asyncio` for concurrent client simulation. No PQ crypto needed — connects through the relay's WebSocket JSON interface (relay handles auth via ML-DSA-87 challenge-response).
- **D-02:** Dependencies: `websockets`, `liboqs-python` (for ML-DSA-87 challenge-response auth signing). No other deps. Not pip-installable — standalone script.
- **D-03:** Identity: benchmark tool needs an ML-DSA-87 keypair for relay auth. Generate ephemeral keypair at startup or load from a test key file.

### Test Environment
- **D-04:** Local dev laptop only. Node running on UDS, relay on localhost. Simulates concurrent WebSocket connections with asyncio tasks.
- **D-05:** Concurrency levels: 1, 10, 100 clients (per PERF-01 success criteria).

### Benchmark Workloads
- **D-06:** PERF-01 (Throughput): Each client sends N small Data(8) blobs (e.g., 1 KB), measure total messages/sec at each concurrency level.
- **D-07:** PERF-02 (Latency overhead): Same operation (e.g., ReadRequest) through relay vs direct UDS. Measure per-operation time delta, report overhead percentage.
- **D-08:** PERF-03 (Large blobs): Write+read blobs at 1 MiB, 10 MiB, 50 MiB, 100 MiB. Report MiB/sec throughput for each size.
- **D-09:** PERF-04 (Mixed workload): Run concurrent small metadata queries (StatsRequest, ExistsRequest) alongside large blob transfers. Report whether small-message p99 latency degrades under large-blob load.

### Report Format
- **D-10:** Markdown report at `tools/benchmark_report.md`. Tables with: msgs/sec, p50/p95/p99 latency in ms, MiB/sec for large blobs. Human-readable.
- **D-11:** Report includes: test date, hardware info (CPU, RAM), relay config, node config, and raw numbers per workload.

### Metrics
- **D-12:** Latency: measure round-trip time (send request → receive response) per operation. Report p50, p95, p99.
- **D-13:** Throughput: total operations / wall-clock time. Report per concurrency level.
- **D-14:** Large blob: total bytes transferred / wall-clock time. Report MiB/sec.

### Claude's Discretion
- Warm-up period before measuring (recommended: discard first N operations)
- Number of iterations per benchmark (enough for stable p99)
- Whether to include relay /metrics scrape in the report for cross-reference
- UDS direct benchmark implementation (raw TCP to UDS socket with AEAD, reuse relay_test_helpers pattern or Python equivalent)

</decisions>

<canonical_refs>
## Canonical References

### Existing test infrastructure
- `tools/relay_smoke_test.cpp` — C++ tool that connects to relay via raw TCP, does auth, sends requests. Pattern reference but Python benchmark won't reuse this code.
- `tools/relay_test_helpers.h` — C++ helpers for auth payload, signing input. Python needs equivalent logic.

### Relay auth protocol
- `db/PROTOCOL.md` — Full wire protocol including relay challenge-response auth
- `relay/core/authenticator.h/cpp` — Challenge-response flow: relay sends challenge, client signs with ML-DSA-87, relay verifies

### Relay config (for report context)
- `relay/config/relay_config.h` — Config fields that affect performance (max_connections, rate_limit, max_send_queue)

### Requirements
- `PERF-01` through `PERF-04` in REQUIREMENTS.md

</canonical_refs>

<code_context>
## Existing Code Insights

### Relay Auth Flow (Python client must implement)
1. Connect WebSocket to relay
2. Receive JSON `{"type": "auth_challenge", "challenge": "<hex>"}`
3. Sign challenge with ML-DSA-87: `signature = sign(bytes.fromhex(challenge))`
4. Send JSON `{"type": "auth_response", "public_key": "<hex>", "signature": "<hex>"}`
5. Receive JSON `{"type": "auth_success", "namespace": "<hex>"}`
6. Session authenticated — send Data/Read/List/etc. as JSON

### No SDK Dependency
Old Python SDK was deleted in Phase 100. Benchmark implements minimal auth + JSON message sending directly. Much simpler than the full SDK.

</code_context>

<specifics>
## Specific Ideas

- asyncio.gather() for concurrent client simulation — each "client" is a coroutine holding one WebSocket connection
- time.perf_counter_ns() for high-resolution latency measurement
- For UDS direct comparison (PERF-02): either skip it (relay overhead is the interesting metric) or implement minimal Python UDS+AEAD client

</specifics>

<deferred>
## Deferred Ideas

- Automated regression benchmarks (CI integration) — separate phase
- KVM swarm benchmarks — only if local numbers look suspicious
- Grafana dashboard for benchmark results — overkill for pre-MVP

</deferred>

---

*Phase: 110-performance-benchmarking*
*Context gathered: 2026-04-13*
