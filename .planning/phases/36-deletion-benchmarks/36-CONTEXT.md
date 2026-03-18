# Phase 36: Deletion Benchmarks - Context

**Gathered:** 2026-03-18
**Status:** Ready for planning

<domain>
## Phase Boundary

Tombstone creation, sync propagation, and garbage collection performance are measured and baselined in the existing Docker benchmark suite. Extends the loadgen with delete capability and adds three new benchmark scenarios to `deploy/run-benchmark.sh`.

</domain>

<decisions>
## Implementation Decisions

### Tombstone creation method
- Extend `chromatindb_loadgen` with `--delete` mode that creates tombstones for previously-written blobs
- Loadgen outputs blob hashes as JSON array in stdout (added to existing stats output) during write runs
- Delete mode reads hashes from stdin (`--hashes-from stdin`)
- Loadgen owns key management — reuses the same keypair from the write phase (via `--identity-file` or same seed)
- Delete throughput stats use the same JSON output format as write stats (`blobs_per_sec`, `latency_ms.p50/p95/p99`, `scenario: "delete"`)

### GC measurement approach
- Use existing SIGUSR1 metrics pattern: poll blob count until it drops to expected value after TTL expiry
- Blobs use TTL=30 seconds for the GC scenario
- Measure GC convergence across all 3 nodes (per-node GC timing reported)
- Use default expiry scan interval (no benchmark-specific override) — results reflect production behavior

### Benchmark parameters
- Same BLOB_COUNT=200 and RATE=50 as existing ingest scenarios
- Test tombstone creation with all three blob sizes (1 KiB, 100 KiB, 1 MiB)
- Each tombstone scenario gets a fresh topology reset (separate `down -v`, `up -d`) — no state leakage between scenarios

### Report integration
- Three new sections in REPORT.md: "Tombstone Creation", "Tombstone Sync Propagation", "Tombstone GC/Expiry"
- Each follows existing table format (metrics table + resource usage)
- Executive Summary gets 3 new rows: "Peak tombstone throughput" (blobs/sec), "Tombstone sync (2-hop)" (ms), "GC reclamation (worst)" (ms)
- benchmark-summary.json: flat keys under `scenarios{}` — `tombstone_creation_1k`, `tombstone_creation_100k`, `tombstone_creation_1m`, `tombstone_sync`, `tombstone_gc`

### Claude's Discretion
- Loadgen internal implementation details (how identity is persisted between write/delete runs)
- Exact SIGUSR1 polling interval for GC detection
- Docker stats capture points within tombstone scenarios
- Error handling for edge cases (e.g., TTL expires before sync completes)

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `deploy/run-benchmark.sh`: Scenario orchestration framework with `reset_topology()`, `capture_stats()`, `wait_for_convergence()`, `run_loadgen()`, `get_blob_count()`, `generate_report()`
- `loadgen/loadgen_main.cpp`: Load generator binary with JSON output, rate limiting, blob creation
- `deploy/docker-compose.yml`: 3-node chain topology + node4 late-joiner profile
- `deploy/results/benchmark-summary.json`: Aggregated JSON results

### Established Patterns
- Each scenario is a `run_scenario_*()` function in `run-benchmark.sh`
- `run_loadgen()` wraps Docker exec of chromatindb_loadgen
- `wait_for_convergence()` polls SIGUSR1 metrics for blob counts
- `format_ingest_row()` and `format_resource_table()` build markdown tables from JSON
- `generate_report()` assembles all results into `REPORT.md` and `benchmark-summary.json`

### Integration Points
- New `run_scenario_tombstone_*()` functions added to `run-benchmark.sh`
- `main()` calls new scenarios after existing ones
- `generate_report()` extended with tombstone sections
- `loadgen_main.cpp` extended with `--delete` mode and blob hash output

</code_context>

<specifics>
## Specific Ideas

- Tombstone creation throughput should be directly comparable to write throughput (same format, same parameters)
- GC latency is measured per-node to expose propagation effects (node1 wrote the data, node3 got it via 2-hop sync — GC timing may differ)
- The 30s TTL was chosen to keep total benchmark runtime reasonable while being long enough to not race with write/delete/sync phases

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 36-deletion-benchmarks*
*Context gathered: 2026-03-18*
