# Roadmap: chromatindb v3.1.0 Relay Live Hardening

## Overview

Fix all bugs found in live relay+node testing, verify every feature works end-to-end against a running node, add missing relay capabilities (source exclusion, blob size limits, health check), and benchmark throughput and latency under realistic workloads including large blob transfers. Node code (db/) is frozen. Five phases: bug fixes, message type verification, live feature verification, new features, and performance benchmarking.

## Phases

**Phase Numbering:**
- Continues from v3.0.0 (Phases 100-105)
- v3.1.0 starts at Phase 106

- [x] **Phase 106: Bug Fixes** - Fix compound type translation failures and audit all std::visit + coroutine lambda patterns for ASAN safety (completed 2026-04-11)
- [x] **Phase 107: Message Type Verification** - Verify all 38 relay-allowed message types translate correctly through relay with live node (completed 2026-04-11)
- [x] **Phase 108: Live Feature Verification** - Verify pub/sub, rate limiting, SIGHUP reload, and graceful shutdown end-to-end (completed 2026-04-11)
- [x] **Phase 109: New Features** - Source exclusion for notifications, relay-side blob size limit, and /health endpoint (completed 2026-04-13)
- [ ] **Phase 110: Performance Benchmarking** - Throughput, latency, large blob, and mixed workload benchmarks (unblocked by 999.9 HTTP transport)

## Phase Details

### Phase 106: Bug Fixes
**Goal**: All known relay bugs are fixed -- compound response translation works and coroutine-unsafe std::visit patterns are eliminated
**Depends on**: Nothing (first phase of v3.1.0)
**Requirements**: FIX-01, FIX-02
**Success Criteria** (what must be TRUE):
  1. binary_to_json produces valid JSON for NodeInfoResponse, StatsResponse, NamespaceStatsResponse, StorageStatusResponse, and all other compound response types when fed live node data
  2. Every std::visit call site in relay/ either uses get_if/get branching or is provably safe (no coroutine lambda captures of variant alternatives)
  3. Relay runs clean under ASAN with no stack-use-after-return warnings during a basic request/response cycle
**Plans**: 3 plans
Plans:
- [x] 106-01-PLAN.md — Fix compound decoder bugs + schema updates + unit tests
- [x] 106-02-PLAN.md — Coroutine safety audit (relay fix + db read-only) + documentation
- [x] 106-03-PLAN.md — UDS tap tool + WebSocket smoke test + sanitizer validation

### Phase 107: Message Type Verification
**Goal**: Every relay-allowed message type is proven to translate correctly through the full relay pipeline against a live node
**Depends on**: Phase 106
**Requirements**: E2E-01
**Success Criteria** (what must be TRUE):
  1. A test script sends each of the 38 relay-allowed message types as JSON through the WebSocket relay, receives a response, and the response is valid JSON with correct field types
  2. Response payloads match expected values for known test data (e.g., a written blob can be read back with correct content)
  3. Error responses (invalid namespace, nonexistent blob) return properly structured JSON error messages
**Plans**: 1 plan
Plans:
- [x] 107-01-PLAN.md — Extend smoke test with all 38 message types (signed blob write, binary WS frames, error paths, fire-and-forget, notification)

### Phase 108: Live Feature Verification
**Goal**: Pub/sub, rate limiting, config reload, and graceful shutdown all work correctly in a live relay+node environment
**Depends on**: Phase 107
**Requirements**: E2E-02, E2E-03, E2E-04, E2E-05
**Success Criteria** (what must be TRUE):
  1. A client subscribes to a namespace, another client writes a blob, and the subscriber receives a JSON notification with the correct namespace and blob hash
  2. A client exceeding the configured rate limit is disconnected by the relay after sustained violation
  3. Sending SIGHUP to the relay process reloads TLS certificates, ACL (allowed client keys), rate limit settings, and metrics_bind address -- verified by observing changed behavior without restart
  4. Sending SIGTERM to the relay process results in all connected clients receiving WebSocket close frames before the process exits
**Plans**: 2 plans
Plans:
- [x] 108-01-PLAN.md — Extract shared test helpers + create relay_feature_test skeleton with CMake target
- [ ] 108-02-PLAN.md — Implement pub/sub, rate limit, SIGHUP, SIGTERM tests + update run-smoke.sh

### Phase 109: New Features
**Goal**: Source exclusion for notifications (node + relay), configurable blob size limits, and a health check endpoint
**Depends on**: Phase 106
**Requirements**: FEAT-01, FEAT-02, FEAT-03
**Success Criteria** (what must be TRUE):
  1. When a client writes a blob to a namespace it is subscribed to, that client does NOT receive its own notification -- other subscribers do. Fix at BOTH layers: node's on_blob_ingested() skips Notification(21) to the originating UDS connection, relay skips forwarding to the originating WebSocket session.
  2. A client attempting to write a blob larger than the configured max_blob_size receives a rejection before the relay forwards it to the node
  3. HTTP GET /health returns 200 with a JSON body indicating relay and UDS connection status; returns 503 when the UDS connection to the node is down
**Plans**: 3 plans
Plans:
- [x] 109-01-PLAN.md — Node source exclusion fix + WriteTracker class + unit tests
- [x] 109-02-PLAN.md — Blob size limit config + health endpoint + SIGHUP wiring
- [x] 109-03-PLAN.md — Wire WriteTracker into UdsMultiplexer notification fan-out + session cleanup

### Phase 110: Performance Benchmarking
**Goal**: Relay performance is measured under realistic workloads to establish baselines and identify bottlenecks. HTTP transport enables raw binary blob benchmarks without base64 overhead.
**Depends on**: Phase 109, Phase 999.9, Phase 999.10 (thread-safety)
**Requirements**: PERF-01, PERF-02, PERF-03, PERF-04
**Success Criteria** (what must be TRUE):
  1. Throughput benchmark produces messages/sec numbers at 1, 10, and 100 concurrent HTTP clients with results recorded in a benchmark report
  2. Latency benchmark measures relay overhead by comparing same-operation timing through relay vs direct UDS, with per-operation overhead percentages recorded
  3. Large blob benchmark demonstrates successful write+read of 1 MiB, 10 MiB, 50 MiB, and 100 MiB blobs through the HTTP relay with MiB/sec throughput recorded
  4. Mixed workload benchmark runs concurrent small metadata queries alongside large blob transfers and reports whether small-message latency degrades under large-blob load
**Plans**: 2 plans
Plans:
- [ ] 110-01-PLAN.md — Build Python HTTP relay benchmark tool (4 workloads)
- [ ] 110-02-PLAN.md — Run benchmarks against live relay+node and generate report

## Progress

**Execution Order:**
Phases execute in numeric order: 106 -> 107 -> 108 -> 109 -> 110
(Phase 109 can execute in parallel with 107-108 since it only depends on 106)

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 106. Bug Fixes | 3/3 | Complete    | 2026-04-11 |
| 107. Message Type Verification | 1/1 | Complete    | 2026-04-11 |
| 108. Live Feature Verification | 1/2 | Complete    | 2026-04-11 |
| 109. New Features | 3/3 | Complete    | 2026-04-13 |
| 110. Performance Benchmarking | 0/2 | Not started | - |

## Backlog

### Phase 999.1: Tombstone TTL lifecycle management (BACKLOG)
**Goal:** [Captured for future planning]
**Requirements:** TBD
**Plans:** 3/3 plans complete

### Phase 999.2: Node silent failure on malformed requests (BACKLOG)
**Goal:** Node records strikes but sends NO error response on malformed requests — client hangs forever. Every request must guarantee a response. Add error response type for rejected messages.
**Requirements:** ERR-01, ERR-02, ERR-03, ERR-04, ERR-05, ERR-06
**Plans:** 3/3 plans complete
Plans:
- [x] 999.2-01-PLAN.md — Node ErrorResponse(63): error_codes.h, transport.fbs, 43 silent paths, metrics, PROTOCOL.md
- [x] 999.2-02-PLAN.md — Relay ErrorResponse: type_registry, message_filter, json_schema, translator, unit tests
- [x] 999.2-03-PLAN.md — Smoke test E2E verification of ErrorResponse through relay+node pipeline

### Phase 999.3: Relay request timeout with error feedback (BACKLOG)
**Goal:** RequestRouter lets requests sit pending 60s before silent cleanup. Add configurable per-request timeout (e.g. 10s) that sends `{"type": "error", "code": "timeout"}` to the client when the node doesn't respond.
**Requirements:** TIMEOUT-01, TIMEOUT-02, TIMEOUT-03, TIMEOUT-04, TIMEOUT-05, TIMEOUT-06, TIMEOUT-07
**Plans:** 2/2 plans complete
Plans:
- [x] 999.3-01-PLAN.md — Error code + PendingRequest extension + callback purge_stale + config field + unit tests
- [x] 999.3-02-PLAN.md — UdsMultiplexer timeout wiring + relay_main SIGHUP + metrics counter

### Phase 999.4: Relay ASAN heap-use-after-free on shutdown (DELETED)
**Goal:** DELETED — was specific to WebSocket transport coroutines. WS code deleted in Phase 999.9. If ASAN issues recur with HTTP transport, create a new backlog item.
**Plans:** N/A

### Phase 999.5: Binary WS frame inconsistency for JSON responses (BACKLOG)
**Goal:** Remove binary WS frame send path -- all JSON responses (including ReadResponse/BatchReadResponse) sent as text frames via send_json()
**Requirements:** WSTEXT-01, WSTEXT-02, WSTEXT-03, WSTEXT-04
**Plans:** 1/1 plans complete
Plans:
- [x] 999.5-01-PLAN.md — Remove send_binary, is_binary_response, binary marker detection, route_response branching

### Phase 999.6: Notification echo on own writes (CLOSED — fixed in Phase 109)
**Goal:** Fixed by Phase 109 source exclusion (node + relay). WriteTracker prevents echo via SSE.
**Plans:** N/A

### Phase 999.7: Standardize all endianness to big-endian (BACKLOG)
**Goal:** Eliminate the two remaining LE encoding exceptions (auth payload pubkey_size, canonical signing input ttl/timestamp) plus relay/tools inline LE copies. All wire encoding standardized to BE. PROTOCOL.md updated. Zero LE references in codebase.
**Requirements:** BE-01, BE-02, BE-03, BE-04, BE-05, BE-06
**Plans:** 2/2 plans complete
Plans:
- [x] 999.7-01-PLAN.md — Node BE standardization: codec.cpp signing input + auth_helpers.h payload + PROTOCOL.md + db/ test updates
- [x] 999.7-02-PLAN.md — Relay+tools BE standardization: uds_multiplexer + relay_uds_tap + relay_test_helpers + codebase LE sweep

### Phase 999.8: Database layer chunking for large files (BACKLOG)
**Goal:** Large blobs may exceed practical limits for single-blob storage and replication. Add chunking support at the database layer -- split large files into fixed-size chunks, reassemble on read. Pre-MVP, no backward compat needed.
**Requirements:** CHUNK-01, CHUNK-02, CHUNK-03, CHUNK-04, CHUNK-05, CHUNK-06
**Plans:** 2/2 plans complete
Plans:
- [x] 999.8-01-PLAN.md — Manifest format utilities (chunking.h/cpp) + atomic multi-blob storage (store_blobs_atomic)
- [x] 999.8-02-PLAN.md — Engine store_chunked/read_chunked API + integration tests + PROTOCOL.md

### Phase 999.9: HTTP transport for relay data operations (BACKLOG)
**Goal:** Replace entire WebSocket relay transport with HTTP + SSE. All request/response operations become HTTP endpoints with raw binary bodies for blob data and JSON for queries. Pub/sub notifications use SSE. WebSocket code deleted entirely. Unblocks Phase 110 benchmarks.
**Depends on**: Phase 109 (uses WriteTracker, health endpoint, blob size limit)
**Requirements:** HTTP-01, HTTP-02, HTTP-03, HTTP-04, HTTP-05, HTTP-06, HTTP-07, HTTP-08, HTTP-09, HTTP-10, HTTP-11, HTTP-12, HTTP-13, HTTP-14, HTTP-15, HTTP-16, HTTP-17, HTTP-18, HTTP-19, HTTP-20, HTTP-21, HTTP-22, HTTP-23, HTTP-24, HTTP-25, HTTP-26, HTTP-27, HTTP-28, HTTP-29, HTTP-30, HTTP-31
**Success Criteria** (what must be TRUE):
  1. Client authenticates via POST /auth/challenge + POST /auth/verify and receives a bearer token
  2. POST /blob with raw binary body forwards to node and returns JSON WriteAck
  3. GET /blob/{ns}/{hash} returns raw binary blob data (application/octet-stream)
  4. All 15 query endpoints return correct JSON via existing translator
  5. SSE notification stream delivers events for subscribed namespaces
  6. Source exclusion: writer does not receive its own notification via SSE
  7. All WebSocket code deleted, relay compiles with only HTTP transport
  8. SIGHUP and SIGTERM work correctly with HTTP transport
**Plans**: 10 plans
Plans:
- [x] 999.9-01-PLAN.md — HTTP parser + response builder + token store (standalone units)
- [x] 999.9-02-PLAN.md — UdsMultiplexer decoupling from ws::SessionManager
- [x] 999.9-03-PLAN.md — HTTP server + router + auth endpoints
- [x] 999.9-04-PLAN.md — ResponsePromise awaitable mechanism
- [x] 999.9-05-PLAN.md — Data endpoints (blob write/read/delete, batch read)
- [x] 999.9-06-PLAN.md — Query endpoints (list, stats, exists, metadata, etc.)
- [x] 999.9-07-PLAN.md — SSE notification stream + subscribe/unsubscribe
- [x] 999.9-08-PLAN.md — relay_main rewire + MetricsCollector merge + integration
- [x] 999.9-09-PLAN.md — WebSocket code deletion + cleanup + verification
- [x] 999.9-10-PLAN.md — Gap closure: wire SSE streaming + subscription cleanup + test fix

### Phase 999.10: Relay thread-safety overhaul for multi-threaded HTTP (BACKLOG)
**Goal:** Relay was designed for single-threaded WS. With HTTP transport and hardware_concurrency() threads, UdsMultiplexer send_queue_, RequestRouter, WriteTracker, SubscriptionTracker, TokenStore all have concurrent access. ASAN crashes confirmed under benchmark load. Need systematic fix: strand-confine all shared state or add proper synchronization throughout. Blocks Phase 110 benchmarks.
**Requirements:** TBD
**Plans:** 0 plans
