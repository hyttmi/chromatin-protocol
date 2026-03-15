# Domain Pitfalls

**Domain:** Docker deployment, load generation, and performance benchmarking for a C++20 distributed database (chromatindb)
**Researched:** 2026-03-15
**Stack context:** libmdbx (MMAP), liboqs (ML-DSA-87, ML-KEM-1024), libsodium (ChaCha20-Poly1305), Standalone Asio (C++20 coroutines), 100 MiB blob support, DARE encryption

## Critical Pitfalls

Mistakes that cause incorrect results, container crashes, or fundamental architecture problems requiring rework.

### Pitfall 1: libmdbx MMAP on Docker overlay2 filesystem

**What goes wrong:** libmdbx uses `write_mapped_io` mode (WRITEMAP), which relies on the OS kernel writing dirty memory-mapped pages directly to the database file. Docker's default overlay2 storage driver operates at the file level with copy-on-write semantics. Writing the libmdbx data files to a container's writable layer (overlay2) instead of a volume causes catastrophic performance degradation and risks data corruption. Every mmap write triggers overlay2's copy-up mechanism, turning single-page writes into full-file copies.

**Why it happens:** The Dockerfile stores data in the container filesystem by default. Developers test locally where the container filesystem is on ext4 and it "works," but performance is 10-100x worse than bare metal and results are meaningless. In the worst case, overlay2's copy-on-write interacts badly with libmdbx's page-level MMAP writes, causing write amplification and potential corruption on unclean shutdown.

**Consequences:** Benchmark numbers are orders of magnitude worse than reality. Data corruption on container restart. OOM kills because overlay2 holds multiple copies of the database file in memory. Results that cannot be compared to bare-metal performance.

**Prevention:** Always use Docker named volumes or bind mounts for libmdbx data directories. In docker-compose, map a named volume to the storage_path. Never store database files in the container's writable layer.

```yaml
volumes:
  node1_data:
services:
  node1:
    volumes:
      - node1_data:/data  # MUST be a volume, not container filesystem
```

**Detection:** If `docker stats` shows unexpectedly high memory and the container's disk usage grows much faster than expected data volume, overlay2 copy-up is likely the cause. Compare `used_bytes()` from chromatindb metrics against `docker system df -v` container size.

### Pitfall 2: MMAP memory accounting vs Docker cgroups memory limits

**What goes wrong:** Docker sets cgroup memory limits (`--memory` or `mem_limit`), but libmdbx's memory-mapped files interact unpredictably with cgroup v1/v2 memory accounting. MMAP'd file pages are counted against the container's memory limit as page cache. With the current `size_upper = 64 GiB` geometry and a 10 GB benchmark dataset, the kernel may page in far more data than the container's memory limit allows, triggering the OOM killer even though the application's heap usage is minimal.

**Why it happens:** The cgroup memory controller counts both anonymous memory (heap/stack) and file-backed memory (page cache from mmap). libmdbx's WRITEMAP mode means the entire mapped region can be paged in by the kernel for reads and writes. A container with `mem_limit: 2g` trying to mmap a 10 GB database will be killed by the OOM killer when page cache pressure exceeds the limit, even though actual RSS is only ~100 MB.

**Consequences:** Containers are killed mid-benchmark with `OOMKilled: true`. Inconsistent benchmark results because the OOM killer terminates the process at unpredictable points. If memory limits are set too low, legitimate database operations fail.

**Prevention:**
1. Set `size_upper` in libmdbx geometry to a value proportional to the expected dataset size, not 64 GiB. For a 10 GB benchmark, `size_upper = 12 GiB` with headroom.
2. Set Docker memory limits to at least 2x the expected mmap size, or use `--memory-swap=-1` to allow unlimited swap (letting the kernel page out mmap'd data).
3. Alternatively, skip memory limits entirely for benchmarking containers and measure actual usage via `docker stats`.
4. Document the relationship between `max_storage_bytes` config, libmdbx geometry, and container memory limits.

**Detection:** Check `docker inspect <container> | grep OOMKilled`. Monitor `docker stats` memory column during benchmark runs. If memory usage climbs steadily to the limit, mmap page cache is the cause.

### Pitfall 3: Coordinated omission in the load generator

**What goes wrong:** The load generator sends requests at a rate that is throttled by response time from the system under test (SUT). When the SUT slows down (e.g., during sync, GC, or large blob ingest), the load generator also slows down, producing artificially low latency measurements. The tail latency (P99, P99.9) appears much better than it actually is.

**Why it happens:** A naive load generator loop looks like: `send request -> wait for response -> record latency -> send next request`. When the SUT takes 500ms instead of 5ms, the generator only measures 500ms for that one request but "omits" the hundreds of requests that should have arrived during that 500ms window. The result: median and P99 latencies look acceptable, but real-world clients would experience queuing delays.

**Consequences:** Benchmark results show P99 latency of ~50ms when real-world P99 under load would be ~500ms. Throughput numbers are optimistic because back-pressure from the SUT artificially caps the load. Performance claims based on these numbers are misleading.

**Prevention:** Use a timer-driven schedule (not response-driven) for request generation. Pre-schedule request timestamps at the target rate, and measure latency as `(response_time - scheduled_send_time)` rather than `(response_time - actual_send_time)`. For chromatindb's load generator:
- Use Asio's `steady_timer` to drive a fixed-rate schedule
- Record the scheduled send time, not the actual send time
- Allow requests to queue if the SUT is slow
- Report both "service time" and "response time" (including queue wait)

**Detection:** If throughput exactly matches the load generator's send rate at all load levels (never plateaus or degrades), the generator has coordinated omission. Real systems degrade under overload.

### Pitfall 4: Docker bridge network overhead invalidating sync latency measurements

**What goes wrong:** Docker Compose's default bridge network adds 10-15% latency overhead and reduces throughput by 12-20% compared to host networking. When measuring sync latency and replication time between containers, the benchmark measures Docker's network overhead rather than chromatindb's actual sync performance.

**Why it happens:** Docker's default bridge network creates a virtual Ethernet bridge (docker0) with veth pairs for each container. Every packet traverses the bridge, incurring iptables NAT rules, netfilter connection tracking, and virtual switch forwarding. For a benchmark measuring "time to replicate 1000 blobs between 3 nodes," a significant portion of the measured time is Docker networking overhead, not chromatindb logic.

**Consequences:** Sync latency numbers include 5-50 microseconds of per-packet Docker overhead. Throughput measurements are bottlenecked by bridge network bandwidth (~580 Mbit/s) rather than the application. Multi-hop propagation times include compounded per-hop networking overhead. Results cannot be used to predict bare-metal performance.

**Prevention:**
1. Use `network_mode: host` for performance-critical benchmark runs (each node on a different port).
2. If bridge networking is required for topology isolation, measure and report the Docker networking overhead separately (run a TCP ping benchmark between containers first).
3. Run the same benchmark with both bridge and host networking to quantify the delta.
4. Report results with explicit "Docker bridge" or "host networking" labels so readers know what was measured.

**Detection:** Run `iperf3` between containers before benchmarking. If bandwidth is below 90% of host-to-host bandwidth, Docker networking is a bottleneck. Compare `chromatindb_bench` (in-process) results against Docker-network results to quantify overhead.

## Moderate Pitfalls

### Pitfall 5: PQ crypto CPU cost hidden by single-host Docker benchmarks

**What goes wrong:** ML-DSA-87 sign/verify and ML-KEM-1024 encaps/decaps are CPU-intensive operations. When running multiple chromatindb nodes on the same Docker host, they compete for CPU cores. The benchmark measures degraded crypto performance due to CPU contention, not the system's actual throughput on dedicated hardware.

**Prevention:**
1. Pin each container to specific CPU cores using `cpuset` in docker-compose (`cpuset: "0,1"` for node1, `cpuset: "2,3"` for node2).
2. Limit total containers to fewer cores than available (leave at least 1 core free for the load generator and Docker daemon).
3. Report CPU pinning configuration alongside benchmark results.
4. Measure per-operation crypto costs separately (already done in `chromatindb_bench`) and report them independently from network-level benchmarks.

**Detection:** Monitor `docker stats` CPU% during benchmarks. If any container exceeds 100% (multi-core) or if total CPU across all containers exceeds host capacity, contention is affecting results.

### Pitfall 6: Clock skew in timestamp-dependent measurements

**What goes wrong:** chromatindb's blob timestamps are Unix timestamps used for TTL calculations. All containers on the same Docker host share the host's system clock, so there is no clock skew between nodes. This makes TTL and expiry behavior unrealistically perfect compared to real-world multi-host deployments where clocks can drift by seconds.

**Prevention:**
1. Document that same-host Docker benchmarks do not test clock skew scenarios.
2. For TTL/expiry validation, inject clock offsets via the existing injectable `Clock` function in `Storage` constructor -- but this is a code-level concern, not a Docker concern.
3. Do not claim "TTL works correctly in distributed deployments" based solely on same-host Docker tests.

**Detection:** Compare blob timestamps across nodes. If they are always identical (sub-millisecond), the benchmark is not exercising clock drift scenarios.

### Pitfall 7: Dockerfile build time explosion from FetchContent

**What goes wrong:** The current CMake build uses `FetchContent` to download and build 8 dependencies (liboqs, libsodium, flatbuffers, catch2, spdlog, nlohmann/json, libmdbx, asio). A naive Dockerfile that copies source and runs `cmake --build .` will rebuild all dependencies from scratch on every source change, turning a 2-minute incremental build into a 15-30 minute full build.

**Prevention:**
1. Use a multi-stage Dockerfile with dependency caching. Copy only `CMakeLists.txt` and `db/CMakeLists.txt` first, run the dependency download/build, then copy source files. This leverages Docker layer caching so dependencies are only rebuilt when CMakeLists.txt changes.
2. Use BuildKit cache mounts (`--mount=type=cache`) for the CMake build directory to preserve object files across builds.
3. Consider pre-building a "deps" Docker image that includes all compiled dependencies, and use it as the base for the build stage.
4. Build in Release mode (`-DCMAKE_BUILD_TYPE=Release`) for benchmarks -- the current CMakeLists.txt hardcodes Debug mode, which includes debug symbols and disables optimizations, making benchmark results meaningless.

**Detection:** If `docker build` takes more than 5 minutes and the only change was a `.cpp` file, dependency caching is not working.

### Pitfall 8: Benchmarking Debug builds instead of Release builds

**What goes wrong:** The top-level `CMakeLists.txt` hardcodes `CMAKE_BUILD_TYPE Debug`. Running benchmarks against a Debug build produces results that are 2-10x slower than Release due to disabled optimizations (`-O0`), enabled assertions, and debug symbol overhead. PQ crypto operations (ML-DSA-87 sign/verify) are particularly affected because liboqs has hot inner loops that benefit enormously from compiler optimization and auto-vectorization.

**Prevention:**
1. The Dockerfile must explicitly set `CMAKE_BUILD_TYPE=Release` or `RelWithDebInfo` for benchmark builds.
2. Verify the build type in benchmark output (print compiler flags or check binary with `file` command).
3. Never compare Debug benchmark numbers against published performance claims from other systems that use Release builds.

**Detection:** Check `cmake --build` output for `-O0` vs `-O2`/`-O3` flags. If ML-DSA-87 sign takes >10ms (vs ~1-3ms in Release), it is a Debug build.

### Pitfall 9: Load generator as a bottleneck (running in same container or underpowered)

**What goes wrong:** The load generator runs in the same container as a chromatindb node, or shares CPU/memory resources, and becomes the bottleneck instead of the system under test. The benchmark measures the load generator's capacity, not chromatindb's.

**Prevention:**
1. Run the load generator in its own dedicated container with dedicated CPU cores.
2. The load generator should be a lightweight binary that does minimal work per request: generate a blob, sign it, send over TCP. Pre-generate keys and blob data before the timed portion.
3. Verify the load generator is not CPU-bound: its CPU usage should be well below 100% during the benchmark.
4. Use asynchronous I/O (Asio coroutines) in the load generator to avoid blocking on network operations.

**Detection:** If increasing load generator resources (more CPU, more instances) increases measured throughput, the generator was the bottleneck.

### Pitfall 10: Not accounting for PQ handshake cost in connection setup benchmarks

**What goes wrong:** Every new connection to chromatindb requires a PQ handshake (ML-KEM-1024 encaps/decaps + ML-DSA-87 auth). This takes ~5-15ms per connection (based on existing bench results). A load generator that creates new connections per request will be bottlenecked by handshake cost. A load generator that reuses connections will show much better throughput but may not reflect real-world reconnection patterns.

**Prevention:**
1. Measure and report handshake cost separately from data transfer benchmarks.
2. The load generator should support both modes: persistent connections (for throughput testing) and fresh connections (for connection establishment testing).
3. For trusted peers (localhost), the lightweight handshake path should be measured separately to show the performance gain.
4. Note: chromatindb v0.5.0 supports trusted peer handshake that skips ML-KEM-1024 -- ensure the benchmark tests both paths.

**Detection:** If adding `trusted_peers: ["172.18.0.0/16"]` (Docker bridge subnet) dramatically changes throughput, handshake cost is dominating the benchmark.

## Minor Pitfalls

### Pitfall 11: Container shutdown order affecting benchmark results

**What goes wrong:** When docker-compose stops containers, they shut down in parallel. If the load generator is still writing while nodes are shutting down, final sync/replication measurements are corrupted by connection-reset errors.

**Prevention:** Use `depends_on` with health checks and explicit shutdown ordering. Stop the load generator first, wait for all sync to complete, then stop nodes in reverse dependency order. Use the existing graceful shutdown (SIGTERM handling) in chromatindb.

### Pitfall 12: Forgetting to warm up libmdbx before benchmarking

**What goes wrong:** libmdbx's mmap'd data starts cold (not in page cache). The first batch of reads/writes touches disk, causing high latency. If the benchmark starts timing immediately, the cold-start penalty inflates average latency.

**Prevention:** Include a warmup phase that writes and reads a representative dataset before starting the timed benchmark. Discard warmup measurements. The existing `bench_main.cpp` already does warmup for individual operations -- the Docker load generator needs the same pattern at the system level.

### Pitfall 13: Using `std::chrono::high_resolution_clock` for latency measurement

**What goes wrong:** The existing `bench_main.cpp` uses `high_resolution_clock`, which on Linux is typically an alias for `steady_clock` (monotonic). This is acceptable for in-process benchmarks. However, if the Docker load generator uses `system_clock` for latency measurement, NTP adjustments inside the container can cause negative latency measurements or time jumps.

**Prevention:** Always use `std::chrono::steady_clock` for duration/latency measurements in the load generator. Use `system_clock` only for wall-clock timestamps in logs. The existing bench code uses `high_resolution_clock` which is fine on Linux but should be explicitly `steady_clock` in new code for clarity.

### Pitfall 14: Large blob benchmarks exhausting container disk space

**What goes wrong:** The v0.6.0 target includes a 10 GB dataset with mixed blob sizes up to 100 MiB. With 3 nodes replicating, that is 30 GB of storage plus libmdbx overhead (metadata, free pages). A Docker volume backed by a host filesystem partition with insufficient space causes `MDBX_MAP_FULL` or filesystem ENOSPC errors mid-benchmark.

**Prevention:** Calculate total disk requirement before running: `dataset_size * node_count * 1.5` (50% overhead for libmdbx metadata, free pages, and DARE encryption overhead of 29 bytes per blob). For a 10 GB dataset with 3 nodes: ~45 GB minimum. Verify available space with `df -h` on the Docker volume's backing filesystem.

### Pitfall 15: Not separating ingest throughput from replication throughput

**What goes wrong:** A benchmark that measures "end-to-end throughput" (load generator -> node1 -> sync to node2 -> sync to node3) conflates three different performance characteristics: ingest rate, sync protocol efficiency, and multi-hop propagation. A single number hides which component is the bottleneck.

**Prevention:** Measure each independently:
1. **Ingest throughput:** Load generator writes to a single node, no peers connected. Measures raw storage + crypto cost.
2. **Sync throughput:** Two pre-loaded nodes connect and sync. Measures hash-list diff + transfer efficiency.
3. **Multi-hop propagation:** Three+ nodes in a chain. Measures end-to-end replication time.
4. **Late-joiner:** A fresh node joins after dataset is loaded. Measures bulk sync from zero.

## Phase-Specific Warnings

| Phase Topic | Likely Pitfall | Mitigation |
|-------------|---------------|------------|
| Dockerfile & docker-compose | Pitfall 1 (overlay2), Pitfall 7 (build time), Pitfall 8 (Debug build) | Named volumes for data, multi-stage build, Release mode |
| Load generator tool | Pitfall 3 (coordinated omission), Pitfall 9 (self-bottleneck), Pitfall 10 (handshake cost) | Timer-driven scheduling, dedicated container, measure handshake separately |
| Ingest throughput benchmark | Pitfall 2 (OOM from mmap), Pitfall 12 (cold start) | Memory limits >> mmap size, warmup phase |
| Sync latency benchmark | Pitfall 4 (bridge overhead), Pitfall 6 (clock skew) | Host networking or documented overhead, note same-host limitation |
| Multi-node topology | Pitfall 5 (CPU contention), Pitfall 14 (disk space) | CPU pinning, pre-calculate disk requirements |
| Resource profiling | Pitfall 2 (mmap vs cgroup accounting) | Understand that `docker stats` memory includes page cache from mmap |
| Results report | Pitfall 15 (conflated metrics) | Separate ingest/sync/propagation/late-joiner measurements |

## Sources

- [libmdbx GitHub - usage and container notes](https://github.com/erthink/libmdbx) (HIGH confidence)
- [Docker cgroup memory limits bypassed for mmap'ed tmpfs files - moby/moby #39004](https://github.com/moby/moby/issues/39004) (HIGH confidence)
- [Docker bridge vs host network performance analysis](https://www.compilenrun.com/docs/devops/docker/docker-performance/docker-network-performance/) (MEDIUM confidence)
- [Container network benchmark study - Hathora](https://blog.hathora.dev/benchmarking-container-performance/) (MEDIUM confidence)
- [Coordinated Omission in NoSQL Database Benchmarking](https://vsis-www.informatik.uni-hamburg.de/getDoc.php/publications/569/Coordinated_Omission_in_NoSQL_Database_Benchmarking-Friedrich.pdf) (HIGH confidence)
- [ScyllaDB: On Coordinated Omission](https://www.scylladb.com/2021/04/22/on-coordinated-omission/) (HIGH confidence)
- [High Scalability: Your Load Generator is Probably Lying to You](http://highscalability.com/blog/2015/10/5/your-load-generator-is-probably-lying-to-you-take-the-red-pi.html) (HIGH confidence)
- [Dockerization Impacts in Database Performance](https://arxiv.org/pdf/1812.04362) (MEDIUM confidence)
- [Docker resource constraints documentation](https://docs.docker.com/engine/containers/resource_constraints/) (HIGH confidence)
- [C++ clocks: high_resolution_clock myths](https://www.sandordargo.com/blog/2025/12/10/clocks-part-4-high_resolution_clock) (MEDIUM confidence)
- [ML-DSA-87 performance characteristics - OQS](https://openquantumsafe.org/liboqs/algorithms/sig/ml-dsa.html) (HIGH confidence)
- [Multi-stage Docker builds for C++](https://devblogs.microsoft.com/cppblog/using-multi-stage-containers-for-c-development/) (MEDIUM confidence)
- [OverlayFS storage driver - Docker Docs](https://docs.docker.com/storage/storagedriver/overlayfs-driver/) (HIGH confidence)
