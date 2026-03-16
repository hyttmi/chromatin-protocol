# Phase 31: Report Generation - Research

**Researched:** 2026-03-16
**Domain:** Bash scripting, jq JSON processing, markdown report generation
**Confidence:** HIGH

## Summary

Phase 31 adds report generation to the existing `deploy/run-benchmark.sh` script. The input is a set of well-structured JSON files produced by Phase 30's benchmark scenarios (loadgen output, docker stats snapshots). The output is a markdown report (`REPORT.md`) and a combined JSON summary (`benchmark-summary.json`). The entire implementation uses the existing toolchain: bash, jq, and heredoc templating. No new dependencies are required.

This is a straightforward data aggregation and formatting task. The JSON schemas are fixed and known (loadgen's `stats_to_json`, the sync/latejoin/trusted-vs-pq result builders, and docker stats captures). The main complexity is in the jq expressions for extracting, computing deltas, and formatting values, and in the heredoc markdown templating that produces readable tables.

**Primary recommendation:** Implement as two new functions (`collect_hardware_info()` and `generate_report()`) added to `run-benchmark.sh`, plus a `--report-only` flag for regeneration from existing JSON. Use jq for all JSON manipulation and computation. Use heredocs for markdown output.

<user_constraints>

## User Constraints (from CONTEXT.md)

### Locked Decisions
- Executive summary at top with 4 headline numbers: peak ingest blobs/sec, worst p99 latency, PQ-vs-trusted overhead %, late-joiner catch-up time
- Hardware auto-detection from host: CPU model, cores, RAM, disk type, kernel version, Docker version (lscpu, free, uname, docker version)
- ASCII diagram for topology (node1 -> node2 -> node3 -> node4 chain) plus key config summary
- Benchmark parameters section: BLOB_COUNT, RATE, drain timeout, sync interval
- One table per scenario with specific column layouts (see CONTEXT.md for exact schemas)
- Per-scenario resource summary from docker stats snapshots: peak CPU% and memory per node
- Caveats section noting: sync latency includes 10s sync_interval, single-run (no statistical significance), Docker overhead
- Report header: timestamp, git commit hash, version
- `generate_report()` function added to existing `deploy/run-benchmark.sh`
- `collect_hardware_info()` at script start, writes `deploy/results/hardware-info.json`
- Report generated before final compose down (while containers still available)
- jq + heredoc templating (same toolchain as rest of script)
- Graceful handling of missing scenario JSON: "[not run]" marker, don't fail
- Combined `deploy/results/benchmark-summary.json` aggregating all results
- `--report-only` flag to regenerate from existing JSON without re-running
- Report written to `deploy/results/REPORT.md`, overwrite on each run
- `deploy/results/` added to `.gitignore`
- Provenance: "Generated: {date} | Commit: {hash} | chromatindb {version}"
- Computed deltas: PQ overhead %, throughput scaling across blob sizes, sync latency as multiple of sync_interval
- No pass/fail thresholds (first benchmark = establishing baseline)
- Executive summary highlights: peak throughput, worst latency, PQ overhead %, catch-up time

### Claude's Discretion
- Exact markdown formatting and table alignment
- Column width and number formatting (decimal places)
- Docker version info detail level
- ASCII diagram style

### Deferred Ideas (OUT OF SCOPE)
None

</user_constraints>

<phase_requirements>

## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| OBS-02 | Benchmark results report generated as structured markdown with hardware specs, topology, per-scenario tables, and analysis | `generate_report()` function consumes scenario JSON files, hardware-info.json, and docker-stats JSON to produce `REPORT.md` with all required sections |
| OBS-03 | Machine-readable JSON results output for each scenario alongside markdown report | `benchmark-summary.json` aggregates all scenario results + hardware info + metadata; individual scenario JSONs already exist from Phase 30 |

</phase_requirements>

## Standard Stack

### Core
| Tool | Version | Purpose | Why Standard |
|------|---------|---------|--------------|
| bash | 5.x | Script shell | Already used throughout run-benchmark.sh |
| jq | 1.6+ | JSON parsing, computation, construction | Already a dependency; handles all JSON operations |

### Supporting
| Tool | Purpose | When to Use |
|------|---------|-------------|
| lscpu | CPU model, cores | Hardware info collection |
| free | RAM total | Hardware info collection |
| uname -r | Kernel version | Hardware info collection |
| docker version | Docker engine version | Hardware info collection |
| lsblk | Disk type (SSD/HDD/NVMe) | Hardware info collection |
| git rev-parse | Commit hash for provenance | Report header |
| date | Timestamp for provenance | Report header |

### Alternatives Considered
None -- the toolchain is locked by user decision. jq + heredoc templating matches the rest of the script.

## Architecture Patterns

### Function Integration Points in run-benchmark.sh

```
main()
  parse args (add --report-only)
  mkdir -p "$RESULTS_DIR"
  check_deps
  collect_hardware_info()           # NEW: writes hardware-info.json
  if --report-only: skip to report
  build_image
  run_scenario_ingest
  run_scenario_sync
  run_scenario_latejoin
  run_scenario_trusted_vs_pq
  generate_report()                 # NEW: produces REPORT.md + benchmark-summary.json
  $COMPOSE down -v
```

### Pattern 1: Graceful Missing-File Handling
**What:** Each report section checks if its source JSON exists before processing. Missing files produce "[not run]" markers instead of errors.
**When to use:** Every scenario section in the report.
**Example:**
```bash
if [[ -f "$RESULTS_DIR/scenario-ingest-1k.json" ]]; then
    # extract and format data with jq
else
    echo "| 1 KiB | [not run] | [not run] | [not run] | [not run] | [not run] |"
fi
```

### Pattern 2: jq Computation for Derived Metrics
**What:** Use jq arithmetic for computing deltas and ratios rather than bc or awk.
**When to use:** PQ overhead %, throughput scaling, sync-interval multiples.
**Example:**
```bash
# PQ overhead % = ((pq - trusted) / trusted) * 100
overhead=$(jq -n --argjson pq "$pq_p50" --argjson tr "$trusted_p50" \
    '(($pq - $tr) / $tr * 100) | . * 10 | round / 10')
```

### Pattern 3: Heredoc Markdown Templating
**What:** Use bash heredocs with variable interpolation for the markdown report body.
**When to use:** The main `generate_report()` function.
**Example:**
```bash
cat > "$RESULTS_DIR/REPORT.md" <<EOF
# chromatindb Benchmark Report

**Generated:** ${timestamp} | **Commit:** ${commit} | **Version:** ${version}

## Executive Summary

| Metric | Value |
|--------|-------|
| Peak ingest throughput | ${peak_blobs_sec} blobs/sec |
...
EOF
```

### Pattern 4: --report-only Flag
**What:** Model after existing `--skip-build` pattern. Skip build + scenarios, jump directly to report generation from existing JSON.
**When to use:** Regenerating reports after tweaking formatting without re-running benchmarks.
**Example:**
```bash
for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=true ;;
        --report-only) REPORT_ONLY=true ;;
        *) log "Unknown argument: $arg"; exit 1 ;;
    esac
done
```

### Anti-Patterns to Avoid
- **Inline awk/sed for JSON parsing:** Use jq exclusively. The script already uses jq for all JSON operations. Mixing tools creates maintenance burden.
- **Failing on missing scenario files:** The report must be resilient. Use `[not run]` markers, not `set -e` failures.
- **Hardcoded version strings:** Use `git describe --tags --always 2>/dev/null || echo "dev"` for version, not a hardcoded string.
- **Complex printf formatting:** Use heredocs for the report body. printf is fine for individual table rows but heredocs are more readable for multi-line blocks.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| JSON parsing | grep/sed/awk on JSON | jq | Proper JSON handling, type safety, arithmetic |
| Percentile formatting | Manual rounding logic | jq `round` or `*10\|round/10` | Consistent decimal precision |
| Hardware detection | Manual /proc parsing | lscpu, free -h, uname -r | Standardized, portable output |
| Markdown table alignment | Manual space padding | Pipe-delimited markdown (renderer handles alignment) | Simpler, less fragile |

**Key insight:** jq can do arithmetic, string formatting, and conditional logic. Use it for all JSON-to-value extraction AND for computed metrics. Don't shell out to bc or awk.

## Common Pitfalls

### Pitfall 1: Docker Stats Percentage String Parsing
**What goes wrong:** `docker stats` outputs CPU as "12.34%" (string with percent sign). If you pass this directly to jq arithmetic, it fails.
**Why it happens:** The `capture_stats()` function stores the raw docker stats format strings in JSON.
**How to avoid:** Strip the "%" suffix before arithmetic: `jq -r '.cpu | rtrimstr("%") | tonumber'`
**Warning signs:** jq errors about "string cannot be converted to number"

### Pitfall 2: Memory Usage Format Parsing
**What goes wrong:** `docker stats` memory is "45.2MiB / 512MiB" -- two values with units in a single string.
**Why it happens:** Docker stats `{{.MemUsage}}` concatenates usage and limit.
**How to avoid:** Split on " / " and parse the first part. For peak memory, just display the raw string or extract the usage portion: `jq -r '.mem_usage | split(" / ")[0]'`
**Warning signs:** Unexpected strings in memory columns

### Pitfall 3: jq Division by Zero
**What goes wrong:** Computing PQ overhead % when trusted baseline is 0 (e.g., trusted p50 = 0.0ms).
**Why it happens:** Very fast operations can report 0ms latency at the resolution available.
**How to avoid:** Guard with jq conditional: `if $tr == 0 then "N/A" else ...`
**Warning signs:** jq "nan" or "inf" in output

### Pitfall 4: Missing git in Docker Container
**What goes wrong:** `git rev-parse HEAD` fails if report is generated inside a container or in a non-git context.
**Why it happens:** The script runs on the host, but worth guarding.
**How to avoid:** `git rev-parse --short HEAD 2>/dev/null || echo "unknown"`
**Warning signs:** Error messages in report header

### Pitfall 5: Heredoc Variable Expansion with Special Characters
**What goes wrong:** If a jq-extracted value contains characters special to bash (backticks, $, etc.), heredoc expansion corrupts the output.
**Why it happens:** Unquoted heredoc (`<<EOF`) expands variables.
**How to avoid:** All values come from jq (numeric or controlled strings). This is safe for unquoted heredocs. But if embedding raw JSON, use quoted heredoc (`<<'EOF'`) for that section.
**Warning signs:** Garbled output in specific report sections

### Pitfall 6: Stale Docker Stats After Compose Down
**What goes wrong:** Trying to capture final docker stats after `$COMPOSE down -v` returns empty results.
**Why it happens:** The CONTEXT.md specifies report generation before final compose down, but this is easy to forget.
**How to avoid:** Ensure `generate_report()` is called before the `$COMPOSE down -v` in main(). The docker stats JSON files are already captured during scenarios, so the report function reads from files, not live containers. Hardware info from `docker version` should be captured early.
**Warning signs:** Empty resource summary tables

## Code Examples

### Hardware Info Collection
```bash
collect_hardware_info() {
    log "Collecting hardware info..."
    local cpu_model cores ram_total kernel docker_ver disk_type

    cpu_model=$(lscpu | grep 'Model name' | sed 's/.*:\s*//')
    cores=$(nproc)
    ram_total=$(free -h | awk '/^Mem:/ {print $2}')
    kernel=$(uname -r)
    docker_ver=$(docker version --format '{{.Server.Version}}' 2>/dev/null || echo "unknown")
    disk_type=$(lsblk -d -o NAME,ROTA 2>/dev/null | awk 'NR>1 && $2=="0" {print "SSD"; exit} NR>1 && $2=="1" {print "HDD"; exit}' || echo "unknown")

    jq -n \
        --arg cpu "$cpu_model" \
        --arg cores "$cores" \
        --arg ram "$ram_total" \
        --arg kernel "$kernel" \
        --arg docker "$docker_ver" \
        --arg disk "$disk_type" \
        '{cpu_model: $cpu, cores: ($cores | tonumber), ram_total: $ram, kernel: $kernel, docker_version: $docker, disk_type: $disk}' \
        > "$RESULTS_DIR/hardware-info.json"

    log "Hardware info written: $RESULTS_DIR/hardware-info.json"
}
```

### Peak Docker Stats Extraction
```bash
# Extract peak CPU% across all docker-stats files for a scenario
get_peak_stats() {
    local scenario="$1"
    local container="$2"
    local stat_files=("$RESULTS_DIR"/docker-stats-*-"${scenario}".json)

    if [[ ! -f "${stat_files[0]}" ]]; then
        echo "N/A"
        return
    fi

    # Find max CPU% for this container across pre/post snapshots
    jq -r --arg c "$container" \
        '[.[] | select(.container == $c) | .cpu | rtrimstr("%") | tonumber] | max // "N/A"' \
        "${stat_files[@]}" 2>/dev/null || echo "N/A"
}
```

### PQ Overhead Computation
```bash
# Compute PQ overhead % for a metric
compute_overhead() {
    local pq_val="$1"
    local trusted_val="$2"
    jq -n --argjson pq "$pq_val" --argjson tr "$trusted_val" \
        'if $tr == 0 then "N/A"
         else (($pq - $tr) / $tr * 100 | . * 10 | round / 10 | tostring) + "%"
         end' -r
}
```

### Ingest Table Row
```bash
format_ingest_row() {
    local label="$1"
    local file="$RESULTS_DIR/scenario-ingest-${label}.json"

    if [[ ! -f "$file" ]]; then
        echo "| ${label} | [not run] | [not run] | [not run] | [not run] | [not run] |"
        return
    fi

    local blobs_sec mib_sec p50 p95 p99
    blobs_sec=$(jq -r '.blobs_per_sec | . * 10 | round / 10' "$file")
    mib_sec=$(jq -r '.mib_per_sec | . * 100 | round / 100' "$file")
    p50=$(jq -r '.latency_ms.p50 | . * 100 | round / 100' "$file")
    p95=$(jq -r '.latency_ms.p95 | . * 100 | round / 100' "$file")
    p99=$(jq -r '.latency_ms.p99 | . * 100 | round / 100' "$file")

    echo "| ${label} | ${blobs_sec} | ${mib_sec} | ${p50} | ${p95} | ${p99} |"
}
```

## Input JSON Schemas (from Phase 30)

These are the exact JSON structures produced by Phase 30 that the report generator consumes.

### Loadgen Output (scenario-ingest-*.json)
```json
{
  "scenario": "fixed",
  "total_blobs": 200,
  "duration_sec": 4.0,
  "blobs_per_sec": 50.0,
  "mib_per_sec": 0.049,
  "latency_ms": {
    "p50": 1.2, "p95": 3.5, "p99": 5.1,
    "min": 0.8, "max": 7.2, "mean": 1.5
  },
  "blob_sizes": { "small_1k": 200, "medium_100k": 0, "large_1m": 0 },
  "errors": 0,
  "notifications_received": 200,
  "notifications_expected": 200
}
```

### Sync Latency (scenario-sync-latency.json)
```json
{
  "scenario": "sync-latency",
  "blob_count": 200,
  "sync_1hop_ms": 12345,
  "sync_2hop_ms": 23456,
  "sync_interval_seconds": 10,
  "loadgen": { /* loadgen JSON */ }
}
```

### Late-Joiner (scenario-latejoin.json)
```json
{
  "scenario": "late-joiner",
  "blob_count": 200,
  "catchup_ms": 15000,
  "note": "Time from node4 healthy to full convergence"
}
```

### Trusted vs PQ (scenario-trusted-vs-pq.json)
```json
{
  "scenario": "trusted-vs-pq",
  "pq_mode": { /* loadgen JSON */ },
  "trusted_mode": { /* loadgen JSON */ }
}
```

### Docker Stats (docker-stats-{label}-{phase}.json)
```json
[
  {
    "container": "chromatindb-node1",
    "cpu": "12.34%",
    "mem_usage": "45.2MiB / 512MiB",
    "mem_perc": "8.83%",
    "net_io": "1.2MB / 3.4MB",
    "block_io": "100kB / 200kB",
    "pids": "5"
  }
]
```

### Hardware Info (hardware-info.json) -- NEW, created by this phase
```json
{
  "cpu_model": "AMD Ryzen 9 7950X",
  "cores": 32,
  "ram_total": "62Gi",
  "kernel": "6.19.8-arch1-1",
  "docker_version": "27.5.1",
  "disk_type": "SSD"
}
```

## Report Structure (Markdown Output)

The report follows this exact section order per CONTEXT.md decisions:

```
1. Header: title, provenance (date, commit, version)
2. Executive Summary: 4 headline metrics table
3. System Profile: hardware specs from hardware-info.json
4. Topology: ASCII diagram + config summary
5. Benchmark Parameters: BLOB_COUNT, RATE, drain timeout, sync interval
6. Scenario: Ingest Throughput
   - Table: sizes as rows, blobs/sec + MiB/sec + p50/p95/p99 as columns
   - Resource summary: peak CPU% and memory per node
7. Scenario: Sync Latency + Multi-Hop
   - Table: 1-hop ms, 2-hop ms
   - Sync latency as multiple of sync_interval
   - Resource summary
8. Scenario: Late-Joiner Catch-Up
   - Table: catch-up ms
   - Resource summary
9. Scenario: Trusted vs PQ Comparison
   - Side-by-side table: PQ column, Trusted column, Delta % column
   - Resource summary
10. Caveats
11. Raw Data: list of JSON files in results directory
```

## Combined JSON Summary (benchmark-summary.json)

Aggregates all scenario results into one machine-readable file:

```json
{
  "generated": "2026-03-16T12:00:00Z",
  "commit": "abc1234",
  "version": "v0.6.0",
  "hardware": { /* hardware-info.json content */ },
  "parameters": {
    "blob_count": 200,
    "rate": 50,
    "drain_timeout": 10,
    "sync_interval_seconds": 10
  },
  "scenarios": {
    "ingest_1k": { /* or null if not run */ },
    "ingest_100k": { /* or null */ },
    "ingest_1m": { /* or null */ },
    "sync_latency": { /* or null */ },
    "latejoin": { /* or null */ },
    "trusted_vs_pq": { /* or null */ }
  }
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Separate reporting scripts | Integrated `generate_report()` in same script | This phase | Single pipeline, no out-of-sync risk |
| Timestamped result dirs | Overwrite `deploy/results/` each run | This phase decision | Simpler, git tracks if needed |
| Manual report writing | Auto-generated from JSON | This phase | Reproducible, consistent formatting |

## Open Questions

1. **Version string source**
   - What we know: No VERSION variable in CMakeLists.txt. Milestone is v0.6.0.
   - What's unclear: Whether to use `git describe --tags` or hardcode "v0.6.0"
   - Recommendation: Use `git describe --tags --always 2>/dev/null || echo "dev"`. This handles tagged releases automatically. If no tag exists, falls back to commit hash which is already captured separately.

2. **Docker stats file pairing**
   - What we know: Stats are captured as `docker-stats-{label}-{phase}.json` where phase is "pre" or "post" and label includes the scenario name.
   - What's unclear: The naming convention has label and phase swapped in the `capture_stats` function call vs the filename template -- the function takes `(phase, label)` but the file is `docker-stats-${label}-${phase}.json`.
   - Recommendation: Use glob matching `docker-stats-*-{scenario}*.json` to find stats files for a given scenario. The actual naming from the script is: `capture_stats "pre" "ingest-1k"` produces `docker-stats-ingest-1k-pre.json`. This is consistent.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Manual validation (bash script) |
| Config file | None (bash script, not a test framework) |
| Quick run command | `bash deploy/run-benchmark.sh --report-only` |
| Full suite command | `bash deploy/run-benchmark.sh` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| OBS-02 | Markdown report generated with all sections | manual | `bash deploy/run-benchmark.sh --report-only && cat deploy/results/REPORT.md` | N/A -- verified by inspection |
| OBS-03 | Machine-readable JSON output | manual | `bash deploy/run-benchmark.sh --report-only && jq . deploy/results/benchmark-summary.json` | N/A -- verified by inspection |

### Sampling Rate
- **Per task commit:** Verify `--report-only` produces valid REPORT.md and benchmark-summary.json (requires sample JSON data or a prior full run)
- **Per wave merge:** Full `run-benchmark.sh` pipeline
- **Phase gate:** Manual inspection of REPORT.md structure and benchmark-summary.json validity

### Wave 0 Gaps
None -- no test framework needed. This is a bash script enhancement verified by running it and inspecting output. The `--report-only` flag enables fast iteration without re-running benchmarks.

## Sources

### Primary (HIGH confidence)
- `/home/mika/dev/chromatin-protocol/deploy/run-benchmark.sh` -- existing 513-line script, all function signatures, JSON output patterns, arg parsing conventions
- `/home/mika/dev/chromatin-protocol/loadgen/loadgen_main.cpp` -- `stats_to_json()` function (lines 227-251) defines exact loadgen JSON schema
- `/home/mika/dev/chromatin-protocol/.planning/phases/31-report-generation/31-CONTEXT.md` -- all user decisions locked

### Secondary (MEDIUM confidence)
- jq arithmetic and string operations: well-established, stable API since jq 1.5
- Linux hardware detection commands (lscpu, free, uname): standard POSIX/Linux utilities, stable interfaces

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- bash + jq already in use, no new tools
- Architecture: HIGH -- function integration points are explicit in CONTEXT.md
- Pitfalls: HIGH -- derived from direct inspection of docker stats format strings and jq edge cases
- JSON schemas: HIGH -- derived from source code inspection of loadgen and scenario functions

**Research date:** 2026-03-16
**Valid until:** Indefinite (bash/jq are stable; JSON schemas are project-internal)
