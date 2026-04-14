# Requirements: chromatindb v4.0.0 Relay Architecture v3

**Defined:** 2026-04-14
**Core Value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers — making data censorship-resistant and technically unstoppable.

## v4.0.0 Requirements

### Concurrency Model

- [x] **CONC-01**: Relay runs a single io_context thread for all I/O (HTTP accept, connections, UDS, SSE, timers) — no multi-threaded ioc.run()
- [x] **CONC-02**: CPU-heavy operations (TLS handshake, ML-DSA-87 signature verify, large JSON parse/serialize) offload to a thread pool via crypto::offload() pattern, resuming on the event loop thread after completion
- [x] **CONC-03**: All shared data structures (UdsMultiplexer, RequestRouter, SubscriptionTracker, WriteTracker, TokenStore, ResponsePromiseMap) accessed only from the single event loop thread — no mutexes, no strands
- [x] **CONC-04**: Remove all std::mutex and asio::strand code added in Phase 999.10 — clean single-threaded design
- [x] **CONC-05**: relay_main.cpp creates one thread running ioc.run(), plus a configurable thread pool for offload work

### Verification

- [x] **VER-01**: Relay compiles and all unit tests pass with single-threaded model
- [x] **VER-02**: Relay runs ASAN-clean under benchmark tool at 1, 10, and 100 concurrent HTTP clients with zero heap-use-after-free or data race reports
- [x] **VER-03**: Relay handles SIGHUP config reload and SIGTERM graceful shutdown correctly under single-threaded model

### Performance Benchmarking

- [x] **PERF-01**: Throughput benchmark produces blobs/sec at 1, 10, and 100 concurrent HTTP clients recorded in benchmark report
- [x] **PERF-02**: Latency benchmark measures per-operation round-trip time (p50/p95/p99) through HTTP relay recorded in benchmark report
- [x] **PERF-03**: Large blob benchmark measures write+read throughput at 1 MiB, 10 MiB, 50 MiB, 100 MiB with MiB/sec recorded
- [x] **PERF-04**: Mixed workload benchmark measures small-query latency degradation under concurrent large-blob load

### Thread Pool Offload

- [x] **OFF-01**: offload_if_large() helper exists with 64 KB threshold (65536 bytes), conditionally dispatches CPU-heavy callables to the thread pool and transfers back to event loop
- [x] **OFF-02**: asio::thread_pool& reference injected into DataHandlers, QueryHandlerDeps, and UdsMultiplexer via constructor, wired from relay_main.cpp offload_pool
- [x] **OFF-03**: All json_to_binary() and binary_to_json() call sites in HTTP handlers (handlers_query.cpp, handlers_data.cpp) wrapped with offload_if_large() using payload size as threshold input
- [x] **OFF-04**: UDS AEAD encrypt/decrypt offloaded with counter-by-value capture (D-10), notification/broadcast binary_to_json pre-translated in read_loop() coroutine before synchronous dispatch

### Chunked Streaming for Large Blobs

- [ ] **CHUNK-01**: UDS chunked sub-frame protocol for payloads >= 1 MiB — large payloads broken into 1 MiB sub-frames within a single logical message, small messages use existing single-frame format
- [ ] **CHUNK-02**: Per-chunk AEAD authentication with shared monotonic nonce counter — each 1 MiB sub-frame encrypted/decrypted independently with sequential nonces from the same counter
- [ ] **CHUNK-03**: MAX_BLOB_DATA_SIZE raised from 100 MiB to 500 MiB in node (db/net/framing.h) and relay (http_connection.h MAX_BODY_SIZE, uds_multiplexer.h MAX_FRAME_SIZE)
- [ ] **CHUNK-04**: Bounded backpressure queue (4-chunk depth) between UDS producer and HTTP consumer — producer pauses when queue is full, prevents OOM
- [ ] **CHUNK-05**: HTTP upload streaming — relay reads HTTP body in 1 MiB chunks and forwards each to UDS incrementally via chunked sub-frames, for blobs >= 1 MiB
- [ ] **CHUNK-06**: HTTP download streaming — relay streams UDS sub-frame chunks to HTTP client using chunked transfer encoding (Transfer-Encoding: chunked), for blobs >= 1 MiB
- [ ] **CHUNK-07**: HttpResponse scatter-gather writes using Asio buffer sequences for all responses — eliminates header+body string concatenation
- [ ] **CHUNK-08**: Small blobs under 1 MiB handled inline with existing full-buffer path — no streaming overhead for common case

## Out of Scope

| Feature | Reason |
|---------|--------|
| Multi-threaded io_context | Root cause of all ASAN bugs — single-threaded is the fix |
| Strand confinement | Proved insufficient (Phase 999.10 abandoned) |
| HTTP/2 | Not needed pre-MVP |
| WebSocket fallback | Deleted in Phase 999.9 |
| Client-side chunked transfer encoding for uploads | Content-Length streaming sufficient pre-MVP |
| Zero-copy splice/sendfile | OS-level optimization, complex — future |
| Streaming FlatBuffer parsing | Raw blob data bypasses FlatBuffer |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| CONC-01 | Phase 111 | Complete |
| CONC-02 | Phase 111 | Complete |
| CONC-03 | Phase 111 | Complete |
| CONC-04 | Phase 111 | Complete |
| CONC-05 | Phase 111 | Complete |
| VER-01 | Phase 111 | Complete |
| VER-02 | Phase 112 | Complete |
| VER-03 | Phase 112 | Complete |
| PERF-01 | Phase 113 | Complete |
| PERF-02 | Phase 113 | Complete |
| PERF-03 | Phase 113 | Complete |
| PERF-04 | Phase 113 | Complete |
| OFF-01 | Phase 114 | Complete |
| OFF-02 | Phase 114 | Complete |
| OFF-03 | Phase 114 | Complete |
| OFF-04 | Phase 114 | Complete |
| CHUNK-01 | Phase 115 | Planned |
| CHUNK-02 | Phase 115 | Planned |
| CHUNK-03 | Phase 115 | Planned |
| CHUNK-04 | Phase 115 | Planned |
| CHUNK-05 | Phase 115 | Planned |
| CHUNK-06 | Phase 115 | Planned |
| CHUNK-07 | Phase 115 | Planned |
| CHUNK-08 | Phase 115 | Planned |

**Coverage:**
- v4.0.0 requirements: 24 total
- Mapped to phases: 24/24 (100%)
