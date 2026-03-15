# Architecture Patterns

**Domain:** Docker deployment, load generation, and benchmark infrastructure for chromatindb
**Researched:** 2026-03-15

## Recommended Architecture

The v0.6.0 milestone adds three new components alongside the existing `chromatindb` daemon and `chromatindb_bench` micro-benchmark. None modify existing source code -- they are new artifacts that treat the existing binary as a black box.

```
repo root
  |
  +-- db/                          (EXISTING - untouched)
  |     chromatindb_lib, main.cpp
  |
  +-- bench/                       (EXISTING - untouched)
  |     bench_main.cpp             (micro-benchmarks: crypto, data path)
  |
  +-- tools/
  |     loadgen/
  |       loadgen_main.cpp         (NEW - load generator binary)
  |
  +-- docker/
  |     Dockerfile                 (NEW - multi-stage build)
  |     docker-compose.yml         (NEW - multi-node topology)
  |     configs/
  |       node1.json ... node5.json
  |
  +-- benchmark/
  |     run_benchmark.sh           (NEW - orchestrates full benchmark suite)
  |     scenarios/
  |       ingest_throughput.sh
  |       sync_latency.sh
  |       late_joiner.sh
  |       multi_hop.sh
  |     collect_metrics.sh         (NEW - polls SIGUSR1, parses logs)
  |     report.sh                  (NEW - generates markdown report)
  |
  +-- CMakeLists.txt               (MODIFIED - add loadgen target)
```

### Component Boundaries

| Component | Responsibility | Communicates With |
|-----------|---------------|-------------------|
| `chromatindb` (existing) | Daemon binary. Stores/replicates blobs. | Peers via PQ-encrypted TCP on port 4200 |
| `chromatindb_bench` (existing) | In-process micro-benchmarks (crypto, codec, sync). | chromatindb_lib directly (no network) |
| `chromatindb_loadgen` (NEW) | Network load generator. Connects as a real peer, writes blobs at controlled rates. | chromatindb nodes via TCP (full protocol client) |
| `Dockerfile` (NEW) | Builds chromatindb + loadgen binaries in container. | CMake build system |
| `docker-compose.yml` (NEW) | Defines multi-node topology with networking. | Docker engine |
| `benchmark/*.sh` (NEW) | Orchestrates scenarios, collects metrics, produces report. | docker-compose, loadgen, node logs |

### Data Flow

**Benchmark execution flow:**

```
run_benchmark.sh
  |
  +-- docker compose up (N nodes)
  |     node1 (bootstrap) <-> node2, node3, node4, node5
  |
  +-- loadgen connects to node1
  |     generates signed blobs (mixed sizes: 1K, 64K, 1M, 10M)
  |     measures: ingest ack latency per blob
  |
  +-- collect_metrics.sh
  |     docker exec ... kill -USR1 <pid>  (triggers SIGUSR1 metrics dump)
  |     docker logs --since ... nodeN     (captures periodic metrics lines)
  |     parses: ingests, syncs, storage, peers, uptime
  |
  +-- scenario scripts measure:
  |     - Time for all N nodes to converge (have same blob count)
  |     - Multi-hop propagation delay (write to node1, appear on node5)
  |     - Late-joiner catch-up time (start node after data loaded)
  |
  +-- report.sh
        generates benchmark/results/YYYY-MM-DD_HH-MM.md
```

## New Component Details

### 1. Load Generator (`tools/loadgen/`)

The load generator is a **separate C++ binary** that links `chromatindb_lib` and acts as a protocol-compliant peer. It does NOT inject blobs via any internal API -- it connects over TCP, performs the full PQ handshake (or trusted handshake if configured), and sends `Data` messages exactly as a real peer would.

**Why a C++ binary, not a script:** The chromatindb protocol is custom binary (FlatBuffers over PQ-encrypted TCP). No existing tool (curl, wrk, etc.) can speak it. The load generator must perform ML-KEM-1024 key exchange, AEAD-encrypted framing, and FlatBuffer-encoded transport messages. All of this is already implemented in `chromatindb_lib`.

**Architecture:**

```cpp
// tools/loadgen/loadgen_main.cpp
//
// Links chromatindb_lib. Uses:
// - identity::NodeIdentity (generate keypair for load identity)
// - net::Connection (full protocol client: handshake + send)
// - wire::encode_blob, wire::build_signing_input (blob construction)
// - crypto::Signer (sign blobs)
//
// Does NOT use: Server, PeerManager, Storage, Engine
// (it's a client, not a node)
```

**CLI interface:**

```
chromatindb_loadgen --target <host:port> \
                    --blobs <count> \
                    --blob-size <bytes> \
                    --rate <blobs/sec> \
                    [--mixed-sizes]    \
                    [--ttl <seconds>]  \
                    [--data-dir <path>]  # for persistent identity
```

**Key design decisions:**

- Generates its own ML-DSA-87 keypair (unique namespace per run, or persistent with --data-dir)
- Connects as a single peer to the target node -- measures ingest from client perspective
- Reports: total time, avg/p50/p95/p99 per-blob latency, throughput (blobs/sec, MB/sec)
- `--mixed-sizes` mode: distributes blobs across 1 KiB (40%), 64 KiB (30%), 1 MiB (20%), 10 MiB (10%)
- Output: JSON lines to stdout (one line per blob with latency), summary to stderr
- Single-threaded (Asio io_context) -- matches how a real relay would write

**What it links:**

```cmake
add_executable(chromatindb_loadgen tools/loadgen/loadgen_main.cpp)
target_link_libraries(chromatindb_loadgen PRIVATE chromatindb_lib)
```

No new dependencies. Reuses everything from the existing library.

### 2. Dockerfile (`docker/Dockerfile`)

Multi-stage build. Builder compiles from source; runtime image is minimal.

**Stage 1: Builder**

```dockerfile
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates ninja-build \
    python3 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O2" && \
    cmake --build build --target chromatindb chromatindb_loadgen
```

**Stage 2: Runtime**

```dockerfile
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/chromatindb /usr/local/bin/
COPY --from=builder /src/build/chromatindb_loadgen /usr/local/bin/

EXPOSE 4200
VOLUME /data

ENTRYPOINT ["chromatindb"]
CMD ["run", "--data-dir", "/data", "--log-level", "info"]
```

**Key decisions:**

- `debian:bookworm-slim` over Alpine because liboqs and libsodium build cleanly on glibc; musl can cause subtle issues with PQ crypto assembly. Confidence: HIGH (liboqs docs explicitly test Debian/Ubuntu).
- Ninja for parallel build inside container (faster than make).
- Release build with -O2 (not -O3 -- diminishing returns, larger binary).
- Both `chromatindb` and `chromatindb_loadgen` in the same image -- loadgen can run in a separate container from the same image.
- No `chromatindb_bench` in the image -- micro-benchmarks are developer-local, not Docker concerns.
- `VOLUME /data` for persistent identity + storage across container restarts.
- Single `EXPOSE 4200` -- one port, one protocol.

### 3. Docker Compose (`docker/docker-compose.yml`)

Defines a 5-node topology suitable for benchmark scenarios.

```yaml
services:
  node1:
    build: ..
    volumes:
      - node1-data:/data
      - ./configs/node1.json:/etc/chromatindb/config.json:ro
    command: ["run", "--config", "/etc/chromatindb/config.json", "--data-dir", "/data"]
    ports:
      - "4201:4200"    # Expose for loadgen access from host
    networks:
      - chromatin

  node2:
    build: ..
    volumes:
      - node2-data:/data
      - ./configs/node2.json:/etc/chromatindb/config.json:ro
    command: ["run", "--config", "/etc/chromatindb/config.json", "--data-dir", "/data"]
    networks:
      - chromatin
    depends_on:
      - node1

  node3:
    build: ..
    # ... same pattern, bootstraps to node1
    depends_on:
      - node1

  node4:
    build: ..
    # bootstraps to node2 (tests multi-hop: node1 -> node2 -> node4)
    depends_on:
      - node2

  node5:
    # NOT started initially -- used for late-joiner scenario
    build: ..
    profiles: ["late-joiner"]
    # ...

  loadgen:
    build: ..
    entrypoint: ["chromatindb_loadgen"]
    command: ["--target", "node1:4200", "--blobs", "1000", "--mixed-sizes"]
    networks:
      - chromatin
    depends_on:
      - node1
    profiles: ["bench"]

networks:
  chromatin:
    driver: bridge

volumes:
  node1-data:
  node2-data:
  node3-data:
  node4-data:
  node5-data:
```

**Topology rationale:**

```
node1 (bootstrap) <--- node2 <--- node4
       ^
       |
      node3

node5 (late-joiner, starts after data loaded)
```

- **node1** is the bootstrap/seed node. loadgen writes here.
- **node2, node3** bootstrap to node1 (direct sync, 1-hop).
- **node4** bootstraps to node2 (2-hop from node1 -- tests multi-hop propagation via PEX).
- **node5** starts late via `docker compose --profile late-joiner up node5` to test catch-up.

**Config files** (`docker/configs/nodeN.json`):

```json
{
  "bind_address": "0.0.0.0:4200",
  "bootstrap_peers": ["node1:4200"],
  "max_peers": 32,
  "sync_interval_seconds": 5,
  "log_level": "info",
  "trusted_peers": ["172.16.0.0/12"]
}
```

Notable: `sync_interval_seconds: 5` (not the default 60) for faster benchmark convergence. `trusted_peers` includes the Docker bridge network range to skip PQ handshake overhead between containers -- this isolates protocol overhead from the benchmarks.

**Wait -- trusted_peers takes IP addresses, not CIDR.** Looking at `validate_trusted_peers`, each entry must be `asio::ip::make_address` parseable. Docker internal DNS resolves container names to IPs, but we cannot predict them. Two solutions:

1. Use `trusted_peers: []` and accept PQ handshake overhead (adds ~200ms per connection setup, but only happens once per peer pair -- negligible for benchmarks measuring sustained throughput).
2. Fixed IPs via compose `networks.chromatin.ipam` config.

**Recommendation:** Option 1 (empty trusted_peers). PQ handshake is a one-time cost per connection. Benchmarks measure steady-state throughput and sync, not connection setup. Keeping PQ handshakes active also makes the benchmark more representative of real-world deployment. If handshake benchmarking is needed, `chromatindb_bench` already measures it in isolation.

### 4. Benchmark Orchestration (`benchmark/`)

Shell scripts that drive docker-compose and parse output. No new languages or dependencies beyond bash, docker, and basic Unix tools (awk, jq).

**`run_benchmark.sh`** -- Master script:

```bash
#!/usr/bin/env bash
set -euo pipefail

RESULTS_DIR="benchmark/results/$(date +%Y-%m-%d_%H-%M)"
mkdir -p "$RESULTS_DIR"

# Phase 1: Build
docker compose -f docker/docker-compose.yml build

# Phase 2: Start topology (nodes 1-4)
docker compose -f docker/docker-compose.yml up -d node1 node2 node3 node4
sleep 5  # Wait for mesh to form

# Phase 3: Generate identity for loadgen (persistent across scenarios)
docker compose -f docker/docker-compose.yml run --rm loadgen \
  chromatindb keygen --data-dir /data

# Phase 4: Run scenarios
./benchmark/scenarios/ingest_throughput.sh "$RESULTS_DIR"
./benchmark/scenarios/sync_latency.sh "$RESULTS_DIR"
./benchmark/scenarios/multi_hop.sh "$RESULTS_DIR"
./benchmark/scenarios/late_joiner.sh "$RESULTS_DIR"

# Phase 5: Collect final metrics
./benchmark/collect_metrics.sh "$RESULTS_DIR"

# Phase 6: Generate report
./benchmark/report.sh "$RESULTS_DIR"

# Cleanup
docker compose -f docker/docker-compose.yml down -v
```

**Metrics collection strategy:**

The existing metrics system provides two collection paths:

1. **SIGUSR1 dump** (on-demand, detailed): `docker exec <container> kill -USR1 1` triggers a full metrics dump to the spdlog output. Captured via `docker logs`.
2. **Periodic metrics line** (every 60s): Automatically logged with format `metrics: peers=N connected_total=N ... ingests=N syncs=N ...`

Both are in spdlog format, parseable with grep/awk. The benchmark scripts parse these to extract:
- `ingests` counter (monotonically increasing)
- `syncs` counter
- `storage` (MiB)
- `blobs` count (sum of latest_seq_num across namespaces)
- `peers` count

**Convergence detection:**

To measure "time until all nodes have the same data," the benchmark polls each node's metrics and compares blob counts:

```bash
# Pseudocode for convergence check
while true; do
  counts=()
  for node in node1 node2 node3 node4; do
    docker exec "$node" kill -USR1 1
    count=$(docker logs --tail 20 "$node" 2>&1 | grep "blobs=" | tail -1 | sed 's/.*blobs=\([0-9]*\).*/\1/')
    counts+=("$count")
  done
  if all_equal "${counts[@]}"; then
    break
  fi
  sleep 1
done
```

This works because `blobs=` in the metrics line is the sum of `latest_seq_num` across namespaces -- the exact number the loadgen wrote.

## Patterns to Follow

### Pattern 1: Load Generator as Protocol Client

**What:** The load generator is a standalone binary that links `chromatindb_lib` and uses `Connection` to speak the full chromatindb protocol. It does NOT bypass crypto or framing.

**When:** Always. There is no shortcut -- the protocol is custom binary.

**Why:** This measures real end-to-end performance including PQ handshake, AEAD encryption, FlatBuffer encoding, signature verification, and storage. Using an internal API bypass would produce unrealistic numbers.

**Structure:**

```cpp
// loadgen connects as a regular peer
auto identity = NodeIdentity::generate();
asio::io_context ioc;

// Resolve target, connect TCP
auto socket = co_await async_connect(resolver, target);

// Create outbound connection (handles handshake)
auto conn = Connection::create_outbound(std::move(socket), identity);
co_await conn->run();  // Handshake completes

// Now send blobs as Data messages
for (int i = 0; i < blob_count; ++i) {
    auto blob = make_blob(identity, data, ttl, timestamp);
    auto encoded = wire::encode_blob(blob);

    auto start = Clock::now();
    co_await conn->send_message(TransportMsgType_Data, encoded);
    // Wait for ACK (WriteAck response from node)
    auto ack = co_await recv_ack(conn);
    auto end = Clock::now();

    record_latency(end - start);
    maybe_rate_limit(rate);
}
```

### Pattern 2: Docker Multi-Stage Build for C++ with FetchContent

**What:** Two-stage Dockerfile: builder with full toolchain, runtime with only libstdc++.

**When:** For any C++ project using CMake FetchContent.

**Why:** FetchContent downloads and builds all dependencies from source. The builder stage needs git and network access; the runtime stage only needs the final binaries. Final image is ~30 MB instead of ~2 GB.

**Gotcha:** FetchContent `GIT_SHALLOW TRUE` requires git. The builder stage must have `git` and `ca-certificates` installed. The `cmake --build build` step compiles all FetchContent deps from source -- this takes 5-10 minutes on first build. Docker layer caching helps if `db/` and `CMakeLists.txt` haven't changed.

### Pattern 3: Metrics via Existing SIGUSR1 + Log Parsing

**What:** Use the existing SIGUSR1 metrics dump and periodic metrics log lines rather than adding a new metrics endpoint.

**When:** For v0.6.0 benchmarks. A Prometheus/JSON metrics endpoint would be a v0.7.0+ concern.

**Why:** The metrics system already exists (shipped in v0.4.0). It provides all the counters needed: ingests, syncs, peers, storage, rejections, rate_limited. Adding HTTP/JSON metrics would require a new dependency (HTTP server library) and a new attack surface -- violating the project's "no HTTP/REST API" constraint.

**How to parse:**

```bash
# Extract latest metrics from container log
docker logs node1 2>&1 | grep "^.*metrics:" | tail -1 | \
  awk -F'[ =]' '{for(i=1;i<=NF;i++) if($i=="ingests") print $(i+1)}'
```

### Pattern 4: Scenario Scripts with Convergence Polling

**What:** Each benchmark scenario is a self-contained shell script that sets up conditions, runs the load, waits for convergence, and records timing.

**When:** For measuring distributed behavior (replication, multi-hop, late-joiner).

**Why:** Distributed systems measurement needs scenario isolation. Each scenario starts from a known state, runs a specific workload, and measures a specific outcome. Combining scenarios would make results impossible to interpret.

## Anti-Patterns to Avoid

### Anti-Pattern 1: Bypassing the Protocol for Load Generation

**What:** Writing blobs directly to storage or calling engine.ingest() from the load generator.

**Why bad:** Produces unrealistic benchmark numbers. Skips PQ handshake time, AEAD encryption overhead, FlatBuffer encoding, network framing, signature verification on the server side. The whole point of v0.6.0 is measuring real-world performance.

**Instead:** Use `Connection::create_outbound()` and send `Data` messages over the encrypted channel.

### Anti-Pattern 2: HTTP Metrics Endpoint

**What:** Adding an HTTP server to chromatindb for metrics collection.

**Why bad:** Violates the project's explicit "no HTTP/REST API" constraint. Adds a dependency (HTTP server library), a new port, and a new attack surface. Contradicts the security posture of "one port, one protocol."

**Instead:** SIGUSR1 + log parsing. It already works. For future needs, a Unix domain socket with JSON output would be more aligned with the project's philosophy.

### Anti-Pattern 3: Embedding Docker or Compose Logic in CMake

**What:** Adding `add_custom_target(docker_build ...)` or similar to CMakeLists.txt.

**Why bad:** Docker is an orchestration concern, not a build concern. CMake builds binaries. Docker packages binaries into containers. Mixing them creates circular dependencies and makes CI harder to reason about.

**Instead:** Dockerfile calls `cmake` internally. Build scripts call `docker build`. Clean separation.

### Anti-Pattern 4: Running Benchmarks Inside the Builder Container

**What:** Compiling and running benchmarks in the same Docker stage.

**Why bad:** Builder images are huge (2+ GB with build tools). Running benchmarks in them wastes resources and makes the Docker build non-cacheable (benchmark results vary between runs, invalidating layers).

**Instead:** Multi-stage build produces lean runtime images. Benchmarks run in runtime containers orchestrated by compose + shell scripts.

### Anti-Pattern 5: Polling Node Storage Directly for Convergence

**What:** Mounting node data volumes and reading libmdbx files from the host.

**Why bad:** libmdbx is ACID with MVCC -- reading the database while the daemon has it open requires proper locking. Direct file reads risk corrupted reads. Also couples the benchmark to storage internals.

**Instead:** Use the metrics system (SIGUSR1 or periodic log lines) which reads storage state through the proper API.

## Integration Points: New vs Modified

### New Files (to create)

| Path | Purpose |
|------|---------|
| `tools/loadgen/loadgen_main.cpp` | Load generator binary source |
| `docker/Dockerfile` | Multi-stage container build |
| `docker/docker-compose.yml` | Multi-node topology |
| `docker/configs/node1.json` | Bootstrap node config |
| `docker/configs/node2.json` | Direct peer config |
| `docker/configs/node3.json` | Direct peer config |
| `docker/configs/node4.json` | Multi-hop peer config (bootstraps to node2) |
| `docker/configs/node5.json` | Late-joiner config |
| `benchmark/run_benchmark.sh` | Master orchestration script |
| `benchmark/scenarios/ingest_throughput.sh` | Ingest throughput scenario |
| `benchmark/scenarios/sync_latency.sh` | Sync convergence scenario |
| `benchmark/scenarios/multi_hop.sh` | Multi-hop propagation scenario |
| `benchmark/scenarios/late_joiner.sh` | Late-joiner catch-up scenario |
| `benchmark/collect_metrics.sh` | Metrics collection utility |
| `benchmark/report.sh` | Markdown report generator |
| `.dockerignore` | Exclude build/, .git, .planning from context |

### Modified Files (minimal)

| Path | Change | Why |
|------|--------|-----|
| `CMakeLists.txt` | Add `chromatindb_loadgen` target (3 lines) | New binary needs build definition |

### Untouched Files

All of `db/` -- the entire existing codebase. The load generator links `chromatindb_lib` as a consumer. No API changes, no new methods, no modifications to any existing header or source file.

## Build Order (Dependency-Aware)

```
Phase 1: Dockerfile + .dockerignore
  Prereq: none (existing binary builds fine)
  Tests: `docker build -t chromatindb .` succeeds, binary runs

Phase 2: Load generator (tools/loadgen/)
  Prereq: none (links existing chromatindb_lib)
  Modification: CMakeLists.txt (add target)
  Tests: `chromatindb_loadgen --target localhost:4200 --blobs 10` works against local node

Phase 3: Docker Compose + configs
  Prereq: Phase 1 (Dockerfile exists)
  Tests: `docker compose up` starts 4-node mesh, peers connect

Phase 4: Benchmark scripts + scenarios
  Prereq: Phase 2 (loadgen binary), Phase 3 (compose topology)
  Tests: `./benchmark/run_benchmark.sh` produces results/

Phase 5: Report generation + analysis
  Prereq: Phase 4 (benchmark data exists)
  Tests: benchmark/results/ contains markdown report with tables
```

**Rationale for ordering:**
- Dockerfile first because it validates the build works in a clean environment (no host deps leaking in).
- Load generator before compose because it can be tested locally (run a node on host, loadgen against it).
- Compose after both because it needs the Dockerfile and loadgen validates the protocol works.
- Benchmark scripts last because they orchestrate everything above.

## Scalability Considerations

| Concern | 5 nodes (benchmark) | 20 nodes | 100 nodes |
|---------|---------------------|----------|-----------|
| Memory per node | ~50 MB base + data | Same | Same |
| Build time | ~10 min first, cached after | Same image | Same image |
| Convergence time | Seconds (5-node PEX) | Minutes (PEX propagation) | Need structured bootstrap |
| Data volume | 10 GB target dataset | Host disk bound | Need storage limits |
| Container overhead | Negligible | Docker bridge scaling | Consider host networking |

For v0.6.0, 5 nodes is sufficient to validate sync, multi-hop, and late-joiner behavior. The 10 GB dataset target exercises real storage and I/O patterns without requiring specialized hardware.

## Sources

- Existing codebase analysis (all source files read directly) -- HIGH confidence
- chromatindb config format from `db/config/config.cpp` -- HIGH confidence
- SIGUSR1 metrics system from `db/peer/peer_manager.cpp` -- HIGH confidence
- Docker multi-stage build pattern -- HIGH confidence (standard practice, official Docker docs)
- Docker Compose networking -- HIGH confidence (bridge networking is default and well-documented)
- liboqs build requirements (Debian/glibc) -- MEDIUM confidence (verified against liboqs CI configs in build/_deps, not independently verified for bookworm-slim specifically)
