# Phase 31: Report Generation - Context

**Gathered:** 2026-03-16
**Status:** Ready for planning

<domain>
## Phase Boundary

Aggregate benchmark results from Phase 30's JSON output into a structured markdown report with hardware specs, topology description, per-scenario tables, computed analysis, and a combined JSON summary. Report generation is added to the existing `run-benchmark.sh` as a `generate_report()` function. This phase does NOT add new benchmark scenarios or modify existing measurement logic.

</domain>

<decisions>
## Implementation Decisions

### Report content & structure
- Executive summary at the top with 3-4 headline numbers: peak ingest blobs/sec, worst p99 latency, PQ-vs-trusted overhead %, late-joiner catch-up time
- Full system profile auto-detected from host: CPU model, cores, RAM, disk type, kernel version, Docker version (lscpu, free, uname, docker version)
- ASCII diagram for topology (node1 -> node2 -> node3 -> node4 chain) plus key config summary (sync interval, blob count, rate)
- Benchmark parameters section after topology: BLOB_COUNT, RATE, drain timeout, sync interval
- One table per scenario with key metrics:
  - Ingest: single table with sizes as rows (1K/100K/1M), columns = blobs/sec, MiB/sec, p50, p95, p99
  - Sync: 1-hop ms, 2-hop ms
  - Latejoin: catch-up ms
  - Trusted-vs-PQ: side-by-side columns (PQ, Trusted) plus delta % column, one row per metric
- Per-scenario resource summary after each scenario table: peak CPU% and memory per node from docker stats snapshots
- Caveats section noting: sync latency includes 10s sync_interval, single-run (no statistical significance), Docker overhead
- Report header includes timestamp, git commit hash, and version for traceability

### Report generation approach
- Add `generate_report()` function to existing `deploy/run-benchmark.sh` — single pipeline automation
- `collect_hardware_info()` runs at script start, writes `deploy/results/hardware-info.json` — available for both full runs and --report-only
- Report generated before final compose down cleanup (while containers still available)
- jq + heredoc templating for markdown generation (same toolchain as rest of script)
- Graceful handling of missing scenario JSON: include "[not run]" marker in that section, don't fail
- Combined `deploy/results/benchmark-summary.json` aggregating all scenario results + hardware info + timestamp (satisfies OBS-03)
- `--report-only` flag to regenerate report from existing JSON without re-running benchmarks

### Output format & naming
- Report written to `deploy/results/REPORT.md`
- Overwrite on each run (no timestamped directories). Git tracks history if needed.
- `deploy/results/` added to `.gitignore` — results are machine-specific
- Provenance in report header: "Generated: {date} | Commit: {hash} | chromatindb {version}"

### Analysis depth
- Computed deltas and ratios: PQ overhead % over trusted, throughput scaling across blob sizes, sync latency as multiple of sync_interval
- No pass/fail thresholds — this is the first benchmark establishing a baseline
- Executive summary highlights: peak throughput, worst latency, PQ overhead %, catch-up time

### Claude's Discretion
- Exact markdown formatting and table alignment
- Column width and number formatting (decimal places)
- Docker version info detail level
- ASCII diagram style

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `deploy/run-benchmark.sh`: 513-line script with 13 functions, jq dependency, already writes 6+ JSON result files
- Existing `capture_stats()` function writes `docker-stats-*.json` with CPU/memory per container
- `log()` function for timestamped stderr output

### Established Patterns
- jq for JSON parsing and construction throughout the script
- `$RESULTS_DIR` variable points to `deploy/results/`
- `$COMPOSE` variable for docker compose commands
- `--skip-build` flag pattern already exists (can model `--report-only` similarly)
- Per-scenario JSON output with consistent structure

### Integration Points
- `generate_report()` called in main() after all scenarios, before final `$COMPOSE down -v`
- `collect_hardware_info()` called in main() before scenarios, writes to `$RESULTS_DIR/hardware-info.json`
- `--report-only` flag parsed alongside `--skip-build` in arg parsing
- `.gitignore` needs `deploy/results/` entry added

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 31-report-generation*
*Context gathered: 2026-03-16*
