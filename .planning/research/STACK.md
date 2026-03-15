# Technology Stack: v0.6.0 Real-World Validation

**Project:** chromatindb v0.6.0
**Researched:** 2026-03-15
**Confidence:** HIGH (Docker/compose are mature, load generator is custom C++, profiling uses standard Linux/Docker tooling)

## Executive Summary

No new library dependencies are needed. The v0.6.0 milestone is entirely about infrastructure and tooling around the existing binary. The stack additions are: a Dockerfile (multi-stage build), a docker-compose.yml (multi-node topology), a standalone C++ load generator binary (reusing `chromatindb_lib`), and bash scripts for metrics collection via `docker stats`.

The load generator is a C++ binary because it needs to speak chromatindb's PQ-encrypted protocol -- no external tool can do ML-KEM-1024 handshakes and ML-DSA-87 signed blob writes. Building it as a second CMake target that links `chromatindb_lib` gives it full access to the crypto, wire format, and connection code. This is the same pattern already used by `chromatindb_bench`.

Resource profiling uses `docker stats --no-stream --format '{{json .}}'` polled by a bash script. No Prometheus, no Grafana, no cAdvisor. The goal is benchmark numbers in a markdown report, not a monitoring dashboard.

## Recommended Stack

### Docker Build

| Technology | Version | Purpose | Why |
|------------|---------|---------|-----|
| gcc:14-bookworm | GCC 14.3 on Debian Bookworm | Build stage base image | Official Docker image. GCC 14 has full C++20 support including coroutines. Debian Bookworm uses glibc (not musl), avoiding ABI issues with liboqs and libsodium. |
| debian:bookworm-slim | Bookworm slim | Runtime stage base image | ~80 MB. Matches the glibc from build stage. No compiler toolchain in production image. Smaller attack surface than full bookworm. |
| Docker multi-stage build | -- | Separate build from runtime | Build stage: ~2 GB (gcc, cmake, git, build artifacts). Runtime stage: ~100 MB (slim base + binary + shared libs). |

**Why not Alpine:** Alpine uses musl libc. liboqs and libsodium assume glibc on Linux. musl has known performance regressions with some crypto code paths and occasional ABI incompatibilities with C++ exception handling. The 60 MB size savings is not worth the risk.

**Why not `scratch`/static linking:** chromatindb depends on liboqs, libsodium, and libmdbx which are built as static libs via FetchContent, but the runtime still needs glibc, libstdc++, libpthread, and libdl. A fully static musl build would require cross-compilation setup that adds complexity for zero benefit in a Docker context.

### Container Orchestration

| Technology | Version | Purpose | Why |
|------------|---------|---------|-----|
| docker compose | v2 (compose file format 3.8+) | Multi-node topology | Defines 3-5 node network with custom bridge, health checks, and volume mounts. No Kubernetes needed -- this is a benchmark, not production deployment. |

### Load Generator

| Technology | Version | Purpose | Why |
|------------|---------|---------|-----|
| Custom C++ binary (`chromatindb_loadgen`) | -- | Generate load against running nodes | Must speak PQ-encrypted protocol. Links `chromatindb_lib` like the existing `chromatindb_bench`. No external load testing tool (wrk, vegeta, k6) can do ML-KEM-1024 handshakes. |
| `std::chrono::steady_clock` | C++20 stdlib | Timing measurements | Already used in `chromatindb_bench`. `steady_clock` is monotonic and safe across threads. Avoid `high_resolution_clock` -- it may alias to `system_clock` on some platforms (non-monotonic). |
| nlohmann/json | 3.11.3 (existing dep) | Results output | JSON output from load generator, consumed by analysis script. Already a project dependency. |

### Resource Profiling

| Technology | Version | Purpose | Why |
|------------|---------|---------|-----|
| `docker stats` | Docker CLI built-in | CPU, memory, network I/O, block I/O per container | Reads from cgroups filesystem, near-zero overhead. JSON output with `--format '{{json .}}'`. No additional dependencies. |
| bash + jq | System tools | Metrics collection script | Poll `docker stats --no-stream` at intervals, append to CSV/JSON. Parse and aggregate for report. `jq` is available in debian:bookworm-slim. |

### Benchmarking Analysis

| Technology | Version | Purpose | Why |
|------------|---------|---------|-----|
| bash scripts | -- | Orchestrate test scenarios | Start topology, run load generator, collect metrics, tear down. Reproducible via `./run-benchmark.sh`. |
| Markdown report | -- | Results presentation | Load generator outputs JSON, script converts to markdown tables. Same format as existing `chromatindb_bench` output. |

## What NOT to Add

| Technology | Why Not |
|------------|---------|
| Prometheus + Grafana | Over-engineering. This is a one-shot benchmark, not continuous monitoring. `docker stats` + bash gives the same numbers with zero setup. PROJECT.md: "No HTTP/REST API". |
| cAdvisor | Container-level metrics available via `docker stats` already. cAdvisor adds a web UI and REST API we don't need. |
| Kubernetes / k3s | Docker compose handles 3-5 nodes on one host trivially. K8s adds massive complexity for zero benefit at this scale. |
| wrk / vegeta / k6 / locust | None of these speak chromatindb's PQ-encrypted binary protocol. They are HTTP-only tools. |
| Google Benchmark (gbenchmark) | Already have a working benchmark harness in `chromatindb_bench`. The load generator measures network-level throughput, not microbenchmarks. A framework adds nothing. |
| perf / flamegraph | Useful for profiling hotspots, but not for the v0.6.0 goal (throughput/latency numbers). Can be added later if bottlenecks are found. Does not work easily in Docker containers without `--privileged` and `--cap-add SYS_ADMIN`. |
| Testcontainers | Java/Go/.NET library for spinning up Docker containers in tests. We are writing bash + compose, not JUnit tests. |
| Redis / Kafka / RabbitMQ | No message broker needed. Load generator connects directly to chromatindb nodes via TCP. |
| InfluxDB / TimescaleDB | Time-series database for metrics storage. Completely unnecessary -- we write a markdown report, not a dashboard. |

## Docker Build Details

### Multi-Stage Dockerfile Structure

```
Stage 1: "builder" (gcc:14-bookworm)
  - Install cmake, git, ninja-build, pkg-config
  - COPY source code
  - cmake + build (Release mode, no tests)
  - Produces: chromatindb, chromatindb_loadgen binaries

Stage 2: "runtime" (debian:bookworm-slim)
  - Install only: libstdc++6 (if dynamically linked)
  - COPY --from=builder /app/build/chromatindb /usr/local/bin/
  - COPY --from=builder /app/build/chromatindb_loadgen /usr/local/bin/
  - EXPOSE 4200
  - HEALTHCHECK: connect to port 4200 (TCP check)
  - ENTRYPOINT ["chromatindb"]
```

**Build mode:** `CMAKE_BUILD_TYPE=Release` in Docker. Debug builds are for development only. Release enables -O2 and strips debug info, reducing binary size and improving benchmark accuracy.

**Why ninja-build:** Faster than make for parallel builds. Matters in Docker build context where rebuild speed affects iteration time. cmake generates ninja files with `-G Ninja`.

**Binary size estimate:** chromatindb Release binary is likely ~5-10 MB (liboqs ML-DSA/ML-KEM + libsodium + libmdbx + FlatBuffers + Asio headers-only = moderate static lib size). Runtime image total: ~90-110 MB.

### Docker Compose Topology

```yaml
# 3-node bootstrap network + 1 late-joiner + 1 load generator
services:
  node1:  # Bootstrap node (seed)
  node2:  # Connects to node1
  node3:  # Connects to node1
  node4:  # Late-joiner (started after load test)
  loadgen: # Load generator (connects to node1)
```

**Network:** Single custom bridge network (`chromatindb-bench`). All nodes on same L2 segment. Docker DNS resolves service names (node1, node2, etc.) to container IPs.

**Health checks:** TCP connect to port 4200. chromatindb accepts connections immediately after `io_context.run()` starts. No application-level health endpoint needed -- if the TCP socket accepts, the node is ready.

```yaml
healthcheck:
  test: ["CMD-SHELL", "timeout 1 bash -c '</dev/tcp/localhost/4200' || exit 1"]
  interval: 2s
  timeout: 3s
  retries: 5
  start_period: 10s
```

**depends_on with condition:** `node2` and `node3` depend on `node1` with `condition: service_healthy`. This ensures bootstrap node is accepting connections before peers try to connect.

**Volumes:** Each node gets a named volume for `/data` (libmdbx storage). This allows inspecting data after tests and prevents tmpfs performance artifacts.

### libmdbx Container Considerations

libmdbx uses `write_mapped_io` mode with mmap. In Docker containers:

1. **`vm.max_map_count`:** Default is 65530 on most Linux hosts. libmdbx with a 64 GiB geometry upper limit needs many mmap regions. For benchmarking with ~10 GB dataset across 3-5 nodes, the default should be sufficient. If tests fail with `MDBX_MAP_FULL` or `ENOMEM`, increase on host: `sysctl -w vm.max_map_count=262144`. This is a **host-level** setting, not per-container.

2. **`--shm-size`:** Not needed. libmdbx uses file-backed mmap, not POSIX shared memory (`/dev/shm`).

3. **Storage driver:** Use the default overlay2. libmdbx writes to named volumes which bypass the overlay filesystem entirely (bind-mounted directly to host paths). No performance penalty.

4. **Memory limits:** Set `mem_limit` in compose to prevent OOM-kill from affecting results. Recommend 512 MB per node for a 10 GB total dataset test (each node stores ~3-4 GB after replication, but libmdbx mmap pages in/out).

## Load Generator Design

### Binary: `chromatindb_loadgen`

A new CMake target in the root `CMakeLists.txt`:

```cmake
add_executable(chromatindb_loadgen tools/loadgen_main.cpp)
target_link_libraries(chromatindb_loadgen PRIVATE chromatindb_lib)
```

**Why C++ binary, not Python/bash script:** The load generator must:
1. Perform ML-KEM-1024 key exchange (PQ handshake)
2. Encrypt all traffic with ChaCha20-Poly1305
3. Sign blobs with ML-DSA-87
4. Encode blobs in FlatBuffers wire format
5. Use the framing protocol (4-byte length prefix + encrypted frame)

All of this is implemented in `chromatindb_lib`. A Python wrapper would require FFI bindings for 5 different subsystems. A C++ binary gets it for free by linking the library.

### Load Generator Capabilities

```
chromatindb_loadgen --target node1:4200 \
                    --duration 60 \
                    --blob-sizes "1k,10k,100k,1m" \
                    --rate 100 \
                    --output results.json
```

| Parameter | Purpose |
|-----------|---------|
| `--target` | Node to send blobs to |
| `--duration` | Test duration in seconds |
| `--blob-sizes` | Comma-separated blob sizes (mixed workload) |
| `--rate` | Target blobs/sec (0 = max throughput) |
| `--output` | JSON results file |
| `--scenario` | Predefined scenario: `ingest`, `sync-latency`, `late-joiner` |

### Measurement Points

The load generator measures **at the application level**, not the network level:

| Metric | How Measured | Where |
|--------|-------------|-------|
| Ingest throughput (blobs/sec) | Count ACKs per second | Load generator: time between send and ACK receive |
| Ingest latency (p50/p95/p99) | Histogram of send-to-ACK times | Load generator: `steady_clock` before send, after ACK |
| Sync propagation time | Write to node1, poll node2/node3 for blob | Load generator: `steady_clock` from write ACK to query hit |
| Multi-hop propagation | Write to node1, poll node3 (which syncs from node2) | Load generator: same technique, topology-dependent |
| Late-joiner catch-up | Start node4, measure time to full sync | Script: `steady_clock` from node4 start to hash-count match |

### Results Format

JSON output compatible with the existing markdown table format from `chromatindb_bench`:

```json
{
  "test": "ingest-throughput",
  "duration_seconds": 60,
  "total_blobs": 5847,
  "total_bytes": 59873280,
  "throughput_blobs_sec": 97.45,
  "throughput_mb_sec": 0.95,
  "latency_p50_ms": 8.2,
  "latency_p95_ms": 14.7,
  "latency_p99_ms": 23.1,
  "errors": 0
}
```

## Resource Profiling Approach

### `docker stats` Polling Script

```bash
#!/bin/bash
# collect-metrics.sh -- polls docker stats every 2 seconds
INTERVAL=2
OUTPUT="metrics.csv"

echo "timestamp,container,cpu_pct,mem_usage_mb,mem_limit_mb,net_rx_mb,net_tx_mb,block_r_mb,block_w_mb" > "$OUTPUT"

while true; do
  docker stats --no-stream --format '{{.Name}},{{.CPUPerc}},{{.MemUsage}},{{.NetIO}},{{.BlockIO}}' \
    | grep chromatindb \
    | while read line; do
        echo "$(date +%s),$line" >> "$OUTPUT"
      done
  sleep $INTERVAL
done
```

**Available fields from `docker stats --format`:**

| Placeholder | Description |
|-------------|-------------|
| `.Name` | Container name |
| `.CPUPerc` | CPU percentage |
| `.MemUsage` | Memory usage / limit |
| `.MemPerc` | Memory percentage |
| `.NetIO` | Network I/O (rx / tx) |
| `.BlockIO` | Block I/O (read / write) |
| `.PIDs` | Number of PIDs |

**JSON format:** `docker stats --no-stream --format '{{json .}}'` outputs full JSON per container. Parseable with `jq`.

### What We Measure vs. What We Don't

| Measure | Tool | Sufficient for v0.6.0 |
|---------|------|----------------------|
| CPU per container | `docker stats` | Yes -- shows if crypto is the bottleneck |
| Memory per container | `docker stats` | Yes -- shows if mmap growth is bounded |
| Network I/O per container | `docker stats` | Yes -- shows sync traffic patterns |
| Disk I/O per container | `docker stats` | Yes -- shows libmdbx write amplification |
| Application-level latency | Load generator | Yes -- measures the metric users care about |
| Kernel-level CPU profiling | NOT measured | Not needed for v0.6.0 -- no optimization target yet |
| Lock contention | NOT measured | Single-threaded io_context -- no locks |
| Syscall tracing | NOT measured | Over-engineering for benchmark goals |

## Alternatives Considered

| Category | Recommended | Alternative | Why Not |
|----------|-------------|-------------|---------|
| Build image | gcc:14-bookworm | gcc:13-bookworm | GCC 14 has better C++20 coroutine codegen. No reason to use an older compiler. |
| Build image | gcc:14-bookworm | clang:18 | GCC builds work today. Clang would require testing the entire build. No benefit for benchmarking. |
| Runtime image | debian:bookworm-slim | alpine:3.20 | musl libc ABI issues with liboqs/libsodium. 60 MB savings not worth the risk. |
| Runtime image | debian:bookworm-slim | distroless | Distroless has no shell, making health checks and debugging harder. Benchmark images need shell access for troubleshooting. |
| Orchestration | docker compose | Kubernetes | Massively over-engineered for 3-5 containers on one host. |
| Orchestration | docker compose | Podman compose | Docker is the de facto standard. Podman compose compatibility is imperfect. |
| Load generator | C++ binary linking chromatindb_lib | Python + ctypes/cffi | Would need FFI for 5+ subsystems (handshake, AEAD, signing, codec, framing). Months of wrapper work for a one-shot tool. |
| Load generator | C++ binary linking chromatindb_lib | Go binary with CGo | Same problem -- needs C bindings for every subsystem. |
| Metrics collection | docker stats + bash | Prometheus + Grafana | Dashboard not needed. We want numbers in a markdown report, not graphs. |
| Metrics collection | docker stats + bash | cAdvisor | Extra container running a web UI we would never look at. |
| Timing | `std::chrono::steady_clock` | `std::chrono::high_resolution_clock` | `high_resolution_clock` may alias to `system_clock` (non-monotonic). `steady_clock` is guaranteed monotonic. Already used in `chromatindb_bench`. |
| Build system | ninja | make | Ninja is ~10-20% faster for parallel builds. Matters during Docker image builds where FetchContent downloads and compiles all deps. |

## Installation / Build Changes

### New CMake Target

```cmake
# In root CMakeLists.txt, after chromatindb_bench:
add_executable(chromatindb_loadgen tools/loadgen_main.cpp)
target_link_libraries(chromatindb_loadgen PRIVATE chromatindb_lib)
```

### New Files

```
Dockerfile                          # Multi-stage build
docker-compose.yml                  # 3-5 node topology
tools/loadgen_main.cpp              # Load generator binary
tools/collect-metrics.sh            # docker stats poller
tools/run-benchmark.sh              # Full benchmark orchestration
tools/generate-report.sh            # Convert JSON results to markdown
configs/node1.json                  # Per-node configs for compose
configs/node2.json
configs/node3.json
configs/node4.json                  # Late-joiner config
```

### No Changes to Existing Dependencies

The FetchContent block in `CMakeLists.txt` remains identical. No new `FetchContent_Declare` calls. The load generator reuses `chromatindb_lib` which already links all dependencies.

```bash
# Build locally (no Docker)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Build Docker image
docker build -t chromatindb:v0.6.0 .

# Run benchmark
docker compose up -d
./tools/run-benchmark.sh
docker compose down
```

## Sources

- [Docker multi-stage builds documentation](https://docs.docker.com/build/building/multi-stage/) -- official Docker docs on multi-stage builds -- **HIGH confidence**
- [gcc Docker Hub official image](https://hub.docker.com/_/gcc) -- gcc:14-bookworm image verified available -- **HIGH confidence**
- [Docker Compose services reference](https://docs.docker.com/reference/compose-file/services/) -- healthcheck, depends_on, networks syntax -- **HIGH confidence**
- [Docker container stats reference](https://docs.docker.com/reference/cli/docker/container/stats/) -- format placeholders for docker stats -- **HIGH confidence**
- [Docker runtime metrics](https://docs.docker.com/engine/containers/runmetrics/) -- cgroup-based metrics collection -- **HIGH confidence**
- [Alpine vs Debian for C++ containers](https://www.turnkeylinux.org/blog/alpine-vs-debian) -- musl vs glibc ABI analysis -- **MEDIUM confidence**
- [Docker Compose health checks guide (2025)](https://www.tvaidyan.com/2025/02/13/health-checks-in-docker-compose-a-practical-guide/) -- depends_on with condition: service_healthy -- **MEDIUM confidence**
- [Docker stats JSON format](https://kylewbanks.com/blog/docker-stats-memory-cpu-in-json-format) -- --format '{{json .}}' pattern -- **MEDIUM confidence**
- [std::chrono::high_resolution_clock analysis (2025)](https://www.sandordargo.com/blog/2025/12/10/clocks-part-4-high_resolution_clock) -- confirms steady_clock is safer choice -- **MEDIUM confidence**
- Existing `chromatindb_bench` source (`bench/bench_main.cpp`) -- validates pattern of linking chromatindb_lib for tooling -- **HIGH confidence** (source code)
- Existing `db/storage/storage.cpp` -- libmdbx write_mapped_io + 64 GiB geometry confirms mmap dependency -- **HIGH confidence** (source code)

---
*Stack research for: chromatindb v0.6.0 -- Real-World Validation*
*Researched: 2026-03-15*
