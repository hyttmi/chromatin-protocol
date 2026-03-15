# Project Research Summary

**Project:** chromatindb v0.6.0
**Domain:** Docker deployment, load generation, and performance benchmarking for a distributed PQ-secure blob store
**Researched:** 2026-03-15
**Confidence:** HIGH

## Executive Summary

chromatindb v0.6.0 is a real-world validation milestone, not a feature milestone. The codebase ships with 17,124 LOC, 284 tests, and an in-process microbenchmark (`chromatindb_bench`), but has never been run as a multi-node Docker deployment under sustained load. This milestone closes that gap by adding: a multi-stage Dockerfile, a docker-compose multi-node topology, a custom C++ load generator (`chromatindb_loadgen`), and benchmark orchestration scripts that produce a structured results report. No new library dependencies are required — all tooling is built on top of the existing `chromatindb_lib`.

The critical design constraint driving all tooling decisions is that chromatindb speaks a custom binary protocol (FlatBuffers over PQ-encrypted TCP with ML-KEM-1024 + ML-DSA-87). No off-the-shelf load testing tool can participate in the network, so the load generator must be a C++ binary linking `chromatindb_lib`. This is the same pattern used by `chromatindb_bench`. The infrastructure choice is Docker Compose (not Kubernetes), `docker stats` + bash (not Prometheus + Grafana), and gcc:14-bookworm / debian:bookworm-slim (not Alpine, which has glibc/musl ABI risk with liboqs). All tooling choices prioritize getting accurate numbers with minimal complexity overhead.

The dominant risks are measurement correctness, not implementation difficulty. Three pitfalls can silently produce wrong results: libmdbx MMAP on overlay2 filesystem (use named volumes), coordinated omission in the load generator (use timer-driven scheduling, not response-driven), and benchmarking a Debug build instead of Release (Dockerfile must explicitly set `CMAKE_BUILD_TYPE=Release`). All other pitfalls are detectable from `docker stats` output. The overall implementation risk is low — Docker, compose, and bash are well-understood; the load generator complexity is bounded by reusing the existing `Connection` class for the full protocol stack.

## Key Findings

### Recommended Stack

No new library dependencies. The v0.6.0 stack is infrastructure only: Docker multi-stage builds (gcc:14-bookworm builder, debian:bookworm-slim runtime), docker-compose v2 for topology management, a new `chromatindb_loadgen` CMake target linking `chromatindb_lib`, and bash scripts using `docker stats --no-stream --format '{{json .}}'` for metrics collection. Alpine is explicitly ruled out due to musl libc ABI issues with liboqs and libsodium. Ninja replaces make in the Dockerfile for faster incremental builds during Docker layer cache misses. `std::chrono::steady_clock` (not `high_resolution_clock`) is used for all latency measurement to guarantee monotonicity.

**Core technologies:**
- `gcc:14-bookworm` / `debian:bookworm-slim`: build + runtime images — glibc required by liboqs/libsodium; slim runtime is ~100 MB
- Docker Compose v2: multi-node topology (5 nodes + loadgen service) — no Kubernetes complexity at this scale
- `chromatindb_loadgen` C++ binary: load generator linking `chromatindb_lib` — only way to speak PQ-encrypted protocol
- `docker stats` + bash + jq: resource profiling — zero-overhead cgroup reads, no monitoring stack needed
- `std::chrono::steady_clock`: latency timing — monotonic, matches existing `chromatindb_bench` patterns

### Expected Features

Research identifies 9 table-stakes features and 8 differentiators. The MVP priority order is: Dockerfile → docker-compose topology → load generator → core scenarios (S1 ingest throughput, S4 sync latency, S5 multi-hop, S6 late-joiner) → resource profiling → results report.

**Must have (table stakes):**
- Multi-stage Dockerfile — reproducible isolated build; removes host-machine dependency
- docker-compose 3-node+ chain topology — proves multi-hop sync works; A→B→C chain required
- Load generator binary (`chromatindb_loadgen`) — sustained traffic generation against real PQ-encrypted protocol
- Ingest throughput measurement (blobs/sec, MiB/sec) — most fundamental blob store metric
- Sync/replication latency — wall-clock time from write on A to availability on B
- Multi-hop propagation time — validates transitive sync beyond direct peers
- Late-joiner scenario — proves new nodes can join and converge on existing data
- Resource profiling (CPU, memory, disk I/O via `docker stats`) — context for interpreting throughput numbers
- Benchmark results report (markdown, structured tables with hardware specs + topology) — actionable output

**Should have (differentiators):**
- Automated `run-benchmark.sh` script — reproducible one-command benchmark execution
- Machine-readable JSON output alongside markdown — enables regression tracking
- Mixed workload scenarios (writes + reads + deletes + delegation) — realistic usage profile
- Trusted vs PQ handshake comparison — quantifies PQ handshake overhead (ML-KEM-1024)
- Storage limit behavior under load (fill to `max_storage_bytes`, verify rejection) — validates edge case
- Concurrent writers via delegation — measures delegation verification overhead

**Defer (v2+):**
- Network condition simulation via `tc` — WAN latency/bandwidth simulation; high complexity, resilience milestone scope
- Pub/sub notification latency measurement — requires subscriber client; not critical for v0.6.0
- Automated CI benchmark pipeline — premature; run manually first
- Cross-platform Docker images (ARM64 + x86_64) — unnecessary for dev-machine benchmarking
- Chaos engineering / fault injection — belongs in a dedicated resilience milestone

### Architecture Approach

All new components are additive — zero changes to `db/` source. The load generator is a protocol-compliant peer (uses `Connection::create_outbound()`, performs full PQ handshake, sends `Data` messages) rather than an internal API bypass. Metrics collection uses the existing SIGUSR1 dump and periodic spdlog metrics lines, parsed by bash scripts. The 5-node topology has `node1` as bootstrap, `node2`/`node3` as direct peers, `node4` as a 2-hop peer (bootstraps to node2), and `node5` as the late-joiner (started via compose `--profile late-joiner`). The `CMakeLists.txt` change is 3 lines to add the `chromatindb_loadgen` target.

**Major components:**
1. `tools/loadgen/loadgen_main.cpp` + CMake target — protocol client that generates and sends signed blobs, measures per-blob ACK latency
2. `docker/Dockerfile` + `docker/docker-compose.yml` + `docker/configs/nodeN.json` — containerized 5-node mesh with health checks, named volumes, bridge networking
3. `benchmark/*.sh` — orchestration scripts: scenario runners, metrics collector (SIGUSR1 + log parsing), markdown report generator

**Key patterns:**
- Load generator as protocol client (not internal API bypass): measures real end-to-end cost including PQ crypto
- Docker multi-stage build with FetchContent layer caching: `COPY CMakeLists.txt` before `COPY . .` for cache hits
- Metrics via SIGUSR1 + log parsing: no HTTP endpoint, no new attack surface, existing infrastructure
- Scenario scripts with convergence polling: isolated scenarios, poll `blobs=` from metrics log until all nodes match

### Critical Pitfalls

1. **libmdbx MMAP on Docker overlay2** — Always use Docker named volumes for `/data`. Storing libmdbx files in the container's writable layer causes 10-100x performance degradation and potential corruption from overlay2 copy-on-write interacting with WRITEMAP mode.

2. **Coordinated omission in load generator** — Use Asio `steady_timer` to drive a fixed-rate schedule. Record `scheduled_send_time`, not `actual_send_time`. A response-driven loop produces P99 latency numbers that are 10x better than real-world tail latency under load.

3. **Benchmarking Debug builds** — The top-level `CMakeLists.txt` hardcodes Debug mode. The Dockerfile must explicitly pass `-DCMAKE_BUILD_TYPE=Release`. PQ crypto inner loops are 2-10x slower in Debug; ML-DSA-87 sign takes >10ms in Debug vs ~1-3ms in Release.

4. **MMAP memory vs Docker cgroup memory limits** — libmdbx mmap'd pages count against the container's memory limit as page cache. For a 10 GB dataset, set container `mem_limit` to at least 2x expected mmap size, or skip memory limits during benchmarking and measure actual usage via `docker stats` instead.

5. **Docker bridge network overhead invalidating sync latency** — Bridge network adds 10-15% latency overhead and caps throughput at ~580 Mbit/s. Use `network_mode: host` for performance-critical runs, or document bridge overhead explicitly by running a baseline TCP benchmark first.

## Implications for Roadmap

Based on the architecture build order (Dockerfile → load generator → compose → benchmark scripts → report) and the feature dependency chain, research suggests 5 phases:

### Phase 1: Dockerfile and Container Build
**Rationale:** Everything else requires a working Docker image. Must be first to validate the build works in a clean environment without host deps leaking in. Addresses the Debug build pitfall and FetchContent build time pitfall head-on.
**Delivers:** `docker build -t chromatindb:v0.6.0 .` succeeds; binary runs in debian:bookworm-slim container; both `chromatindb` and `chromatindb_loadgen` binaries present in image.
**Addresses:** Multi-stage Dockerfile (table stakes), `.dockerignore`, Release build configuration
**Avoids:** Pitfall 7 (layer cache ordering — CMakeLists.txt before source), Pitfall 8 (Release build explicit in Dockerfile)

### Phase 2: Load Generator (`chromatindb_loadgen`)
**Rationale:** Can be developed and tested locally against a host-running node before Docker Compose exists. Protocol correctness validates before containerization adds complexity. This is the highest-complexity new component.
**Delivers:** `chromatindb_loadgen --target localhost:4200 --blobs 1000 --mixed-sizes` works against a local node; outputs JSON per-blob latency + summary to stdout; timer-driven fixed-rate scheduling prevents coordinated omission.
**Addresses:** Load generator binary (table stakes), ingest throughput measurement (table stakes)
**Avoids:** Pitfall 3 (coordinated omission — use steady_timer for scheduling), Pitfall 9 (load gen self-bottleneck — dedicated container, pre-generate keys), Pitfall 13 (steady_clock not system_clock)

### Phase 3: Docker Compose Topology
**Rationale:** Requires Dockerfile (Phase 1). The 5-node compose topology with health checks and named volumes is prerequisite for all multi-node scenarios. Validates that nodes form a mesh and sync before adding load.
**Delivers:** `docker compose up` starts 4-node mesh (node1-4); nodes connect, handshake, and begin syncing; `docker compose --profile late-joiner up node5` adds late-joiner; named volumes on all nodes.
**Addresses:** docker-compose multi-node topology (table stakes), `docker/configs/nodeN.json`, health checks, named volumes
**Avoids:** Pitfall 1 (named volumes, not container filesystem), Pitfall 2 (memory limit config — set generously or omit), Pitfall 11 (shutdown ordering via depends_on)

### Phase 4: Benchmark Scenarios and Measurement
**Rationale:** Requires Phase 2 (load generator) and Phase 3 (compose topology). Runs the four core scenarios (S1 ingest throughput, S4 sync latency, S5 multi-hop, S6 late-joiner) with resource profiling. The convergence detection pattern (poll `blobs=` via SIGUSR1) is the central measurement mechanism.
**Delivers:** All four benchmark scenarios produce JSON results files; `collect_metrics.sh` captures CPU/memory/I/O from `docker stats` during runs; warmup phase runs before timing starts; scenarios are independently invocable.
**Addresses:** Ingest throughput, sync latency, multi-hop propagation, late-joiner (all table stakes), resource profiling (table stakes)
**Avoids:** Pitfall 4 (document bridge overhead, test host networking for sync scenarios), Pitfall 5 (CPU pinning via cpuset), Pitfall 12 (warmup phase before timed measurement), Pitfall 14 (pre-check disk space calculation), Pitfall 15 (separate ingest/sync/propagation measurements — never conflate)

### Phase 5: Report Generation and Analysis
**Rationale:** Final phase aggregates all JSON results into the markdown report. No new infrastructure needed — pure documentation and scripting. Includes `run_benchmark.sh` master script that automates Phases 3-5 end-to-end.
**Delivers:** `benchmark/results/YYYY-MM-DD_HH-MM.md` with hardware specs, topology diagram, per-scenario results tables, resource usage, microbenchmark results (from existing `chromatindb_bench`), and analysis. Automated `run_benchmark.sh` orchestrates the full suite.
**Addresses:** Benchmark results report (table stakes), automated benchmark runner script (differentiator)
**Avoids:** Pitfall 15 (explicitly separate ingest/sync/propagation/late-joiner in report sections)

### Phase Ordering Rationale

- Dockerfile first: validates the full build chain in isolation; problems surface before multi-node complexity is added
- Load generator before compose: local testing is faster than debugging inside containers; protocol correctness is the highest-risk item
- Compose after both: combines them and requires both to exist; health check + depends_on sequencing needs a working binary
- Benchmark scripts last: should be simple wiring by the time they are written, since all components are validated
- Report last: requires all measurement data; `run_benchmark.sh` is the natural final artifact

### Research Flags

Phases with standard patterns (skip research-phase):
- **Phase 1 (Dockerfile):** Well-documented Docker multi-stage C++ pattern. gcc:14-bookworm is the obvious choice. No ambiguity in toolchain selection.
- **Phase 3 (Docker Compose):** Standard compose topology. Health checks and depends_on are well-documented. Named volumes are boilerplate.
- **Phase 5 (Report):** Pure documentation and scripting. No technical risk. Bash templating is trivial.

Phases needing careful design during planning (read source before writing the plan):
- **Phase 2 (Load Generator):** The `Connection` class API and how to use `create_outbound()` as a client (not server) needs careful reading of existing source before writing the plan. The coordinated omission fix requires verified Asio timer patterns. Read `db/net/connection.h` and `bench/bench_main.cpp` first.
- **Phase 4 (Benchmark Scenarios):** The convergence detection via SIGUSR1 + log parsing is brittle; spdlog format must be verified against actual log output before committing to it. The multi-hop scenario requires confirming PEX propagates correctly through the node1→node2→node4 chain. Test the 4-node mesh manually before scripting the scenarios.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | All choices are standard. Docker official images, docker stats, steady_clock — all well-documented. Only minor uncertainty: liboqs behavior on bookworm-slim vs full bookworm (same glibc, low risk). |
| Features | HIGH | Well-established distributed systems benchmarking domain. Feature set is conservative and achievable in 5 phases. YCSB workload philosophy adapted cleanly to blob store model. |
| Architecture | HIGH | Based directly on existing source code analysis. Component boundaries are clear. The `CMakeLists.txt` change is trivially small. Metrics system via SIGUSR1 already exists and works. |
| Pitfalls | HIGH | libmdbx MMAP + overlay2 is a documented failure mode with issue tracker references. Coordinated omission is well-researched (ScyllaDB, VLDB paper). Debug build risk confirmed from CMakeLists.txt source. |

**Overall confidence:** HIGH

### Gaps to Address

- **`Connection::create_outbound()` client usage:** The load generator acts as a protocol client, not a server. The exact API surface — how to connect outbound, how to receive ACK responses, what the message loop looks like from the client side — needs verification against source before Phase 2 planning. The pattern is clear from `bench_main.cpp` but the outbound client path may differ.

- **SIGUSR1 log format stability:** The metrics parsing scripts depend on specific spdlog output format (e.g., `blobs=N`, `ingests=N`). This format must be verified to be stable and grep/awk-parseable before committing to log-scraping as the convergence detection mechanism for Phase 4.

- **libmdbx geometry in containers:** The current `size_upper = 64 GiB` geometry combined with Docker memory limits creates the Pitfall 2 risk. The benchmark containers may need a config override to reduce `size_upper` to 2x expected dataset size. Needs clarification during Phase 3 planning — whether this is a config field or requires a code change.

- **trusted_peers CIDR vs IP:** The architecture research confirms that `trusted_peers` takes individual IP addresses, not CIDR ranges, making Docker bridge subnet wildcarding impossible. The recommendation is empty `trusted_peers` (full PQ handshake) — a one-time ~5-15ms cost per connection. This should be confirmed during Phase 3 and explicitly documented in benchmark results so readers know PQ handshakes are active.

## Sources

### Primary (HIGH confidence)
- Existing `chromatindb_lib` source code — validates load generator linking pattern, confirms SIGUSR1 metrics format, confirms libmdbx WRITEMAP usage
- Docker official documentation (multi-stage builds, compose services, container stats, resource constraints)
- libmdbx GitHub — WRITEMAP + Docker overlay2 interaction risk
- Coordinated Omission paper (VLDB) and ScyllaDB writeup — load generator scheduling correctness requirements
- liboqs ML-DSA performance data (OQS) — confirms Debug vs Release cost difference is material
- OverlayFS storage driver documentation (Docker Docs) — overlay2 copy-on-write behavior

### Secondary (MEDIUM confidence)
- Alpine vs Debian musl/glibc ABI analysis — supports bookworm-slim choice over Alpine
- Docker bridge vs host networking performance analysis — 10-15% overhead estimate
- Docker stats JSON format examples — parsing patterns for metrics collection scripts
- std::chrono::steady_clock vs high_resolution_clock analysis (2025) — confirms steady_clock recommendation
- Docker Compose health check guide — depends_on with condition: service_healthy pattern

### Tertiary (LOW confidence)
- Docker bridge bandwidth ceiling (~580 Mbit/s) — single source; should be verified against host hardware during Phase 4 measurement

---
*Research completed: 2026-03-15*
*Ready for roadmap: yes*
