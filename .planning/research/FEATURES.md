# Feature Landscape

**Domain:** Real-world validation and performance benchmarking for a distributed, PQ-secure blob store (chromatindb v0.6.0)
**Researched:** 2026-03-15
**Overall confidence:** HIGH (well-established domain; tooling is mature and patterns are standard)

## Context

chromatindb v0.5.0 shipped with 17,124 LOC C++20, 284 unit tests, and a standalone microbenchmark binary (`chromatindb_bench`) that measures crypto ops, data path, and sync throughput in-process. What is missing is real-world, multi-node validation: running actual daemon instances in a realistic topology, injecting load, measuring end-to-end behavior under sustained traffic, and producing a report with actionable numbers.

The existing `NodeMetrics` struct already tracks ingests, rejections, syncs, rate-limited disconnects, and peer connection totals. The existing `SIGUSR1` handler and periodic metrics log provide runtime observability. The benchmark binary covers microbenchmarks (SHA3, ML-DSA-87, ML-KEM-1024, ChaCha20-Poly1305, blob ingest, sync throughput, PQ handshake, notification dispatch). This milestone builds on top of that foundation.

---

## Table Stakes

Features users (here: developers and operators evaluating chromatindb) expect for any distributed database performance validation. Missing any of these would make the benchmark results incomplete or untrustworthy.

| Feature | Why Expected | Complexity | Dependencies |
|---------|--------------|------------|--------------|
| **Multi-stage Dockerfile** | Reproducible build in an isolated container. Standard practice for C++ projects. Ensures benchmark results are not tied to the developer's machine. | Low | CMake, FetchContent (all deps fetched at build time) |
| **docker-compose multi-node topology** | Minimum 3-node network (A<->B<->C for multi-hop). Without this, you are testing localhost-to-localhost, which proves nothing about real networking, PQ handshake overhead, or sync protocol correctness under latency. | Low | Dockerfile |
| **Load generator tool** | A binary or script that creates signed blobs and writes them to a running node. Without a load generator, there is no way to produce sustained traffic. Must support configurable blob sizes (1 KiB, 64 KiB, 1 MiB, 10 MiB) and configurable write rate or burst mode. | Med | NodeIdentity (keygen), wire protocol (connect + send Data messages) |
| **Ingest throughput measurement** | Blobs/sec and MiB/sec at the ingesting node under sustained load. This is the most fundamental metric for any blob store. Must measure at multiple blob sizes. | Low | Load generator, NodeMetrics (already exists: ingests counter) |
| **Sync/replication latency** | Wall-clock time from "blob written to node A" to "blob available on node B". This is the single most-asked question about any replicated system. Must measure at configurable sync intervals. | Med | Multi-node topology, timestamping in load generator + verification on peer |
| **Multi-hop propagation time** | Time for a blob written to node A to reach node C (which only connects to B, not A). Validates that the sync protocol actually propagates data transitively. Without this, you have no evidence that the sequential sync protocol works beyond direct peers. | Med | 3+ node topology with chain connectivity (A-B-C, no A-C link) |
| **Late-joiner scenario** | Start a new node D after data has been written, connect it to the network, measure time to fully catch up. Every replicated system must prove that new nodes can join and converge. | Med | Multi-node topology, load generator (pre-seed data), timing infrastructure |
| **Resource profiling (CPU, memory, disk I/O)** | Measure resource consumption under load. Without this, throughput numbers are meaningless because you do not know if you are CPU-bound (PQ crypto), memory-bound (100 MiB blobs), or I/O-bound (libmdbx writes). | Low | `docker stats` or cgroup metrics; no custom tooling needed |
| **Benchmark results report** | A structured document (markdown) with tables and analysis. Numbers without context are useless. Must include hardware specs, topology, workload description, and interpretation. | Low | All measurement features above |

## Differentiators

Features that go beyond table stakes. Not expected for a v0.6.0 internal validation milestone, but would add significant value if included.

| Feature | Value Proposition | Complexity | Dependencies |
|---------|-------------------|------------|--------------|
| **Mixed workload scenarios** | Combine writes, reads, deletes, and delegation operations in a single load profile. Closer to real-world usage than pure-write benchmarks. Shows how PQ signature verification on ingest competes with sync and pub/sub. | Med | Load generator with multi-operation support, delegation blob creation |
| **Automated benchmark runner script** | A single `./run-benchmarks.sh` that builds Docker images, starts compose, runs all scenarios sequentially, collects metrics, and produces the report. Makes benchmarks reproducible by anyone, not just the original developer. | Med | All table stakes features |
| **Network condition simulation** | Use `tc` (traffic control) inside Docker to add latency and bandwidth constraints between nodes. Simulates WAN conditions for a system designed for internet deployment. Shows how PQ handshake cost (ML-KEM-1024 ciphertext = 1568 bytes per direction) behaves under real latency. | High | docker-compose with `cap_add: NET_ADMIN`, `tc` in container image |
| **Pub/sub notification latency** | Measure time from blob ingest on publisher to notification receipt on subscriber. Validates the real-time notification pipeline end-to-end, not just the in-process callback latency already measured in `chromatindb_bench`. | Med | Multi-node topology, load generator, subscriber client or log-based measurement |
| **Storage limit behavior under load** | Fill a node to `max_storage_bytes`, continue writing, verify DISK_FULL rejection propagates correctly, measure throughput degradation (if any) as storage fills up. | Low | Load generator, config with `max_storage_bytes` |
| **Concurrent writers to same namespace** | Multiple load generators writing to the same namespace via delegation. Measures contention and delegation verification overhead on the ingest hot path. | Med | Delegation blob setup, multiple load generator instances |
| **Machine-readable results output** | JSON output from the benchmark runner (alongside markdown). Enables trend tracking across versions and automated regression detection. | Low | Benchmark runner script |
| **Trusted vs PQ transport comparison** | Run the same workload with `trusted_peers` (lightweight handshake) vs full PQ handshake. Quantifies the PQ overhead in connect-heavy scenarios. | Low | Two compose configs differing only in trusted_peers setting |

## Anti-Features

Features to explicitly NOT build for this milestone.

| Anti-Feature | Why Avoid | What to Do Instead |
|--------------|-----------|-------------------|
| **Grafana/Prometheus monitoring stack** | Massive complexity for a validation milestone. The existing spdlog metrics output + `docker stats` provides everything needed. Adding a monitoring stack is an operational concern, not a benchmark concern. | Parse spdlog output and `docker stats` for metrics. Write a simple log-scraping script if needed. |
| **Custom benchmark framework** | The existing `chromatindb_bench` already covers microbenchmarks. Building a general-purpose framework is YAGNI. | Write scenario-specific measurement scripts. Reuse `chromatindb_bench` for micro-level numbers. |
| **YCSB integration** | YCSB is Java-based and designed for key-value stores with standard CRUD APIs. chromatindb has a custom binary protocol with PQ crypto -- YCSB cannot speak it. The effort to write a YCSB binding exceeds the value. | Build a purpose-built load generator that speaks the chromatindb wire protocol directly. |
| **HTTP/REST benchmark endpoint** | chromatindb explicitly has no HTTP API (out of scope per PROJECT.md). Adding one for benchmarking introduces attack surface and deps that must be removed later. | Benchmark the actual binary protocol. The load generator connects via TCP and does the PQ handshake, exactly like a real peer. |
| **Kubernetes/orchestration** | Overkill for 3-5 node validation. Docker Compose is sufficient. K8s adds networking complexity (CNI, service mesh) that obscures the measurements. | Use docker-compose with explicit network configuration. |
| **Automated CI benchmark pipeline** | Valuable eventually, but premature for v0.6.0. The goal is to get numbers once, not to run benchmarks on every commit. | Run benchmarks manually. Document the procedure so CI can be added later. |
| **Cross-platform Docker images** | Building ARM64 + x86_64 images adds build complexity. The benchmarks only need to run on the developer's machine. | Build for the host architecture only (linux/amd64 or whatever the dev machine is). |
| **Chaos engineering / fault injection** | Network partitions, node crashes, and Byzantine behavior testing is valuable but belongs in a separate milestone focused on resilience, not performance. | Note as future work. The late-joiner scenario is a mild form of this. |

## Feature Dependencies

```
Dockerfile ─────────────────────────────┐
                                        v
                              docker-compose topology
                                        │
                        ┌───────────────┼───────────────┐
                        v               v               v
               Load generator    Resource profiling   Network setup
                        │               │
            ┌───────────┼───────────┐   │
            v           v           v   v
    Ingest throughput  Sync latency  Multi-hop propagation
            │           │           │
            v           v           v
         Late-joiner scenario (needs pre-seeded data + new node)
                        │
                        v
              Benchmark results report
```

Key dependency chain:
- Dockerfile must exist before docker-compose
- docker-compose must work before any multi-node scenario
- Load generator must exist before any throughput/latency measurement
- All measurements must complete before the results report

## Feature Details

### Dockerfile (Multi-Stage Build)

**What:** A two-stage Dockerfile. Stage 1 (`builder`): full build environment with gcc/g++, cmake, git, and all FetchContent dependencies compiled from source. Stage 2 (`runtime`): minimal image (e.g., `debian:bookworm-slim`) with just the `chromatindb` binary and runtime deps (libstdc++, libc).

**Why multi-stage:** The build stage will be large (liboqs, libsodium, FlatBuffers, libmdbx all compile from source). The runtime image should be small. FetchContent fetches everything at build time -- no external package managers needed.

**Key concern:** liboqs build is slow even with algorithm stripping. Docker layer caching mitigates this (dependency layer rarely changes). The `COPY CMakeLists.txt` and `COPY db/CMakeLists.txt` layers should be separated from `COPY . .` to maximize cache reuse.

**Includes both binaries:** `chromatindb` (daemon) and `chromatindb_bench` (microbenchmark) should both be in the image. Optionally the load generator if it is a separate binary.

### docker-compose Topology

**What:** A `docker-compose.yml` defining at minimum:
- `node-a`: Bootstrap node, accepts inbound. Binds port 4200.
- `node-b`: Connects to node-a as bootstrap peer. Binds port 4201.
- `node-c`: Connects to node-b only (NOT node-a). Binds port 4202. This creates the chain topology needed for multi-hop testing.

Each node gets its own `data_dir` volume, its own config file, and pre-generated identity keys.

**Network:** A single Docker bridge network. All nodes can reach each other by DNS name (`node-a`, `node-b`, `node-c`) but bootstrap config controls actual connections.

**Config generation:** A setup script that generates node identities (via `chromatindb keygen`) and writes per-node JSON config files before compose starts. Or: use an entrypoint script that runs `keygen` on first start.

### Load Generator

**What:** A standalone C++ binary (`chromatindb_loadgen`) that:
1. Generates a node identity (or loads one from disk)
2. Connects to a target node via TCP + PQ handshake (like a real peer)
3. Creates signed blobs with configurable size and count
4. Sends them as Data messages via the wire protocol
5. Measures and reports: blobs sent, total bytes, elapsed time, blobs/sec, MiB/sec

**Why a separate binary, not a script:** The wire protocol is binary (FlatBuffers over PQ-encrypted TCP). No scripting language can speak it without reimplementing the entire handshake and framing layer. The load generator links against `chromatindb_lib` and reuses all existing infrastructure (wire codec, handshake, framing, AEAD channel).

**CLI interface:**
```
chromatindb_loadgen --target <host:port> --count <N> --size <bytes> [--rate <blobs/sec>] [--data-dir <path>]
```

**Blob size distribution:** Support fixed size (e.g., `--size 1024`) and optionally a mix mode (e.g., `--mix small:70,medium:20,large:10` for 1KiB/64KiB/1MiB distribution). Fixed size is table stakes; mix mode is a differentiator.

### Measurement Methodology

**Ingest throughput:** Measured at the load generator side (send rate) and at the node side (from `NodeMetrics.ingests` delta over time window, observable via periodic metrics log). Report both: send throughput and accept throughput.

**Sync latency:** The load generator writes blob B to node A at time T1. A monitoring process (or log-scraping script) checks node B's metrics log for the ingest of blob B. The difference T2-T1 is sync latency. Since the sync runs on a configurable interval (default 60s), this measurement reflects the actual sync behavior, not an artificial immediate-sync scenario. Use `sync_interval_seconds: 5` for benchmarking to get meaningful numbers without long waits.

**Multi-hop propagation:** Same as sync latency but measured at node C (which only connects to node B). Time T3-T1 where T3 is when blob B appears on node C. Requires at least 2 sync intervals.

**Late-joiner:** Write N blobs to the network. Start a new node D connected to node B. Measure time from node D startup to node D having all N blobs (verified by `list_namespaces` blob count matching).

**Resource profiling:** Use `docker stats --no-stream` at regular intervals during load, capturing CPU%, memory usage, and block I/O. Alternatively, read cgroup files directly for more precision. Record at 1-second intervals during load for meaningful time series.

### Benchmark Scenarios

Based on industry standard practices for distributed database benchmarking (informed by YCSB workload philosophy adapted to chromatindb's blob store model):

| Scenario | Description | Measures | Config |
|----------|-------------|----------|--------|
| **S1: Sustained ingest** | Write 10,000 blobs of 1 KiB to node A | Ingest throughput (blobs/sec, MiB/sec), CPU/memory | sync_interval: 5s |
| **S2: Large blob ingest** | Write 100 blobs of 10 MiB to node A | Large-blob throughput, memory peak | sync_interval: 5s |
| **S3: Mixed size ingest** | Write 5,000 blobs with 70% 1KiB / 20% 64KiB / 10% 1MiB | Realistic throughput profile | sync_interval: 5s |
| **S4: Replication latency** | Write 100 blobs to A, measure appearance on B | Sync latency distribution (min/avg/p99/max) | sync_interval: 5s |
| **S5: Multi-hop propagation** | Write 100 blobs to A, measure appearance on C (A->B->C) | End-to-end propagation time | sync_interval: 5s |
| **S6: Late joiner** | Write 1,000 blobs to A, then start node D connected to B | Catch-up time, sync throughput during catchup | sync_interval: 5s |
| **S7: Dataset scale** | Write ~10 GB total data (mixed sizes), let full replication complete | Total replication time, steady-state resource usage | sync_interval: 30s |

### Benchmark Results Report

**Format:** Markdown document with:
1. Hardware specs (CPU model, cores, RAM, disk type)
2. Software versions (chromatindb version, Docker version, kernel)
3. Topology diagram
4. Per-scenario results table (blobs/sec, MiB/sec, latency percentiles)
5. Resource usage plots or tables (CPU%, memory over time)
6. Microbenchmark results from `chromatindb_bench` (already exists)
7. Analysis and observations
8. Known limitations and future work

## MVP Recommendation

Prioritize (in order):

1. **Dockerfile** -- everything else depends on this
2. **docker-compose topology** (3 nodes, chain connectivity) -- enables all multi-node scenarios
3. **Load generator binary** -- enables all measurement scenarios
4. **Scenarios S1 + S4 + S5 + S6** -- ingest throughput, sync latency, multi-hop, and late-joiner are the four table-stakes validations
5. **Resource profiling** -- collect during S1 and S7
6. **Benchmark results report** -- document everything

Defer:
- **S3 (mixed sizes):** nice-to-have but S1+S2 cover the range. Low marginal value.
- **Network simulation (tc):** High complexity, belongs in a resilience-focused milestone.
- **Pub/sub latency measurement:** Requires a subscriber client. Can be added to the load generator later but is not critical for v0.6.0.
- **Automated benchmark runner script:** Valuable but can be assembled after individual pieces work. Manual is fine for first run.
- **Machine-readable JSON output:** Add after markdown report exists.
- **Trusted vs PQ comparison:** Low effort but lower priority than core scenarios.

## Complexity Assessment

| Feature | Estimated Effort | Risk |
|---------|-----------------|------|
| Dockerfile (multi-stage) | Small (1 phase) | Low -- standard pattern, all deps via FetchContent |
| docker-compose + config gen | Small (1 phase) | Low -- well-understood tooling |
| Load generator binary | Medium (1-2 phases) | Medium -- must implement client-side handshake + send path |
| Ingest throughput measurement | Small (part of load gen) | Low -- trivially measured at sender |
| Sync latency measurement | Medium (1 phase) | Medium -- requires timestamp correlation across nodes |
| Multi-hop propagation | Small (same infra as sync latency) | Low -- just measure at a different node |
| Late-joiner scenario | Small (1 phase) | Low -- add node to compose, measure convergence |
| Resource profiling | Small (script) | Low -- `docker stats` parsing |
| Results report | Small (1 phase) | Low -- documentation |

**Total estimated phases:** 5-7

## Sources

- [Aerospike: Best practices for database benchmarking](https://aerospike.com/blog/best-practices-for-database-benchmarking/)
- [CockroachDB: Performance under Adversity](https://www.cockroachlabs.com/blog/database-testing-performance-under-adversity/)
- [YCSB Overview (Duke CS)](https://courses.cs.duke.edu/fall13/compsci590.4/838-CloudPapers/ycsb.pdf)
- [benchANT: YCSB Guide](https://benchant.com/blog/ycsb)
- [GeeksforGeeks: Benchmarking Distributed Systems](https://www.geeksforgeeks.org/system-design/benchmarking-distributed-systems/)
- [Abilian: Testing and Benchmarking Distributed Systems](https://lab.abilian.com/Tech/Devops%20&%20Cloud/Testing%20and%20Benchmarking%20Distributed%20Systems/)
- [Docker: Containerize C++ with multi-stage builds](https://docs.docker.com/guides/cpp/multistage/)
- [Docker: Runtime metrics](https://docs.docker.com/engine/containers/runmetrics/)
- [Integrate.io: Database Replication Speed Metrics](https://www.integrate.io/blog/database-replication-speed-metrics/)
- [Microsoft C++ Blog: Multi-stage containers for C++ development](https://devblogs.microsoft.com/cppblog/using-multi-stage-containers-for-c-development/)
