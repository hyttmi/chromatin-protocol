# Phase 110: Performance Benchmarking - Context

**Gathered:** 2026-04-13
**Status:** Ready for planning

<domain>
## Phase Boundary

Build a Python benchmark tool that measures HTTP relay performance across 4 workload types: throughput (concurrent clients), latency overhead (relay vs UDS), large blob transfer (raw binary, no base64), and mixed workload. Results recorded in a markdown report. All benchmarks run on local dev laptop.

</domain>

<decisions>
## Implementation Decisions

### Tooling
- **D-01:** Python benchmark tool at `tools/relay_benchmark.py`. Uses `requests` or `httpx` for HTTP, `sseclient-py` or raw HTTP for SSE. asyncio + httpx for concurrent client simulation.
- **D-02:** Auth flow: POST /auth/challenge → sign nonce with ML-DSA-87 (liboqs-python) → POST /auth/verify → bearer token for all subsequent requests.
- **D-03:** Ephemeral ML-DSA-87 keypair generated at startup for auth.

### Test Environment
- **D-04:** Local dev laptop only. Node on UDS, relay on localhost HTTP.
- **D-05:** Concurrency levels: 1, 10, 100 clients (per PERF-01).

### Benchmark Workloads
- **D-06:** PERF-01 (Throughput): Each client sends N small POST /blob requests (1 KB raw binary body), measure total blobs/sec at each concurrency level.
- **D-07:** PERF-02 (Latency overhead): Same operation (GET /blob/{ns}/{hash}) through HTTP relay vs direct UDS. Measure per-operation time, report relay overhead percentage.
- **D-08:** PERF-03 (Large blobs): POST /blob + GET /blob at 1 MiB, 10 MiB, 50 MiB, 100 MiB. Raw binary bodies — no base64 overhead. Report MiB/sec write and read throughput.
- **D-09:** PERF-04 (Mixed workload): Concurrent GET /stats + GET /exists (small JSON) alongside POST /blob (large binary). Report whether small-query p99 latency degrades under large-blob load.

### Report Format
- **D-10:** Markdown report at `tools/benchmark_report.md`. Tables with: ops/sec, p50/p95/p99 latency in ms, MiB/sec for large blobs.
- **D-11:** Report includes: test date, hardware info, relay config, node config, raw numbers.

### HTTP-Specific Considerations
- **D-12:** Raw binary POST /blob body (application/octet-stream) — no JSON wrapping, no base64. This is the whole point of the HTTP transport.
- **D-13:** Raw binary GET /blob response — read directly as bytes.
- **D-14:** HTTP keep-alive for throughput tests — reuse connections to avoid TCP handshake per request.
- **D-15:** Disable relay rate limiter and request timeout for benchmarks (rate_limit_messages_per_sec=0, request_timeout_seconds=0 in relay config).

### Claude's Discretion
- Warm-up period before measuring
- Number of iterations per benchmark
- Whether to include /metrics scrape in report
- UDS direct benchmark implementation approach (Python FlatBuffers + AEAD or skip PERF-02)

</decisions>

<canonical_refs>
## Canonical References

### HTTP API (benchmark targets)
- `relay/http/handlers_data.cpp` — POST /blob, GET /blob/{ns}/{hash}, DELETE /blob/{ns}/{hash}, POST /batch/read
- `relay/http/handlers_query.cpp` — GET /list, /stats, /exists, /node-info, etc.
- `relay/http/http_router.cpp` — Auth routes: POST /auth/challenge, POST /auth/verify

### Relay config
- `relay/config/relay_config.h` — rate_limit_messages_per_sec, request_timeout_seconds, max_blob_size_bytes

### Requirements
- PERF-01 through PERF-04 in REQUIREMENTS.md

</canonical_refs>

<code_context>
## Existing Code Insights

### HTTP API Surface (for benchmark client)
Auth: POST /auth/challenge → POST /auth/verify → Bearer token
Data: POST /blob (binary), GET /blob/{ns}/{hash} (binary), DELETE /blob/{ns}/{hash} (binary)
Query: GET /list/{ns}, GET /stats/{ns}, GET /exists/{ns}/{hash}, GET /node-info, etc.
SSE: GET /events?token=<token>

### No Base64 Overhead
POST /blob accepts raw FlatBuffer binary body. GET /blob returns raw binary. This eliminates the base64 problem that originally blocked benchmarking.

</code_context>

<specifics>
## Specific Ideas

- httpx with async client for concurrent HTTP requests — cleaner than aiohttp
- time.perf_counter_ns() for latency measurement
- PERF-02 UDS direct: could be complex (Python FlatBuffers + AEAD). If too much work, measure relay-only and note "UDS baseline deferred"

</specifics>

<deferred>
## Deferred Ideas

- CI automated regression benchmarks
- KVM swarm benchmarks
- Grafana dashboard

</deferred>

---

*Phase: 110-performance-benchmarking*
*Context gathered: 2026-04-13*
