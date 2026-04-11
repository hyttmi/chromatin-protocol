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
- [ ] **Phase 109: New Features** - Source exclusion for notifications, relay-side blob size limit, and /health endpoint
- [ ] **Phase 110: Performance Benchmarking** - Throughput, latency, large blob, and mixed workload benchmarks

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
**Goal**: Relay gains source exclusion for notifications, configurable blob size limits, and a health check endpoint
**Depends on**: Phase 106
**Requirements**: FEAT-01, FEAT-02, FEAT-03
**Success Criteria** (what must be TRUE):
  1. When a client writes a blob to a namespace it is subscribed to, that client does NOT receive its own notification -- other subscribers do
  2. A client attempting to write a blob larger than the configured max_blob_size receives a rejection before the relay forwards it to the node
  3. HTTP GET /health returns 200 with a JSON body indicating relay and UDS connection status; returns 503 when the UDS connection to the node is down
**Plans**: TBD
**UI hint**: no

### Phase 110: Performance Benchmarking
**Goal**: Relay performance is measured under realistic workloads to establish baselines and identify bottlenecks
**Depends on**: Phase 108, Phase 109
**Requirements**: PERF-01, PERF-02, PERF-03, PERF-04
**Success Criteria** (what must be TRUE):
  1. Throughput benchmark produces messages/sec numbers at 1, 10, and 100 concurrent WebSocket clients with results recorded in a benchmark report
  2. Latency benchmark measures relay overhead by comparing same-operation timing through relay vs direct UDS, with per-operation overhead percentages recorded
  3. Large blob benchmark demonstrates successful write+read of PDF-size (1-10 MiB) and X-ray/DICOM-size (50-100 MiB) blobs through the relay with throughput numbers recorded
  4. Mixed workload benchmark runs concurrent small metadata queries alongside large blob transfers and reports whether small-message latency degrades under large-blob load
**Plans**: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 106 -> 107 -> 108 -> 109 -> 110
(Phase 109 can execute in parallel with 107-108 since it only depends on 106)

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 106. Bug Fixes | 3/3 | Complete    | 2026-04-11 |
| 107. Message Type Verification | 1/1 | Complete    | 2026-04-11 |
| 108. Live Feature Verification | 1/2 | Complete    | 2026-04-11 |
| 109. New Features | 0/0 | Not started | - |
| 110. Performance Benchmarking | 0/0 | Not started | - |

## Backlog

### Phase 999.1: Tombstone TTL lifecycle management (BACKLOG)
**Goal:** [Captured for future planning]
**Requirements:** TBD
**Plans:** 2/2 plans complete

### Phase 999.2: Node silent failure on malformed requests (BACKLOG)
**Goal:** Node records strikes but sends NO error response on malformed requests — client hangs forever. Every request must guarantee a response. Add error response type for rejected messages.
**Requirements:** ERR-01, ERR-02, ERR-03, ERR-04, ERR-05, ERR-06
**Plans:** 3 plans
Plans:
- [ ] 999.2-01-PLAN.md — Node ErrorResponse(63): error_codes.h, transport.fbs, 43 silent paths, metrics, PROTOCOL.md
- [ ] 999.2-02-PLAN.md — Relay ErrorResponse: type_registry, message_filter, json_schema, translator, unit tests
- [ ] 999.2-03-PLAN.md — Smoke test E2E verification of ErrorResponse through relay+node pipeline

### Phase 999.3: Relay request timeout with error feedback (BACKLOG)
**Goal:** RequestRouter lets requests sit pending 60s before silent cleanup. Add configurable per-request timeout (e.g. 10s) that sends `{"type": "error", "code": "timeout"}` to the client when the node doesn't respond.
**Requirements:** TBD
**Plans:** 0 plans

### Phase 999.4: Relay ASAN heap-use-after-free on shutdown (BACKLOG)
**Goal:** Relay shows heap-use-after-free (not just leaks) during shutdown — likely a coroutine accessing a destroyed session or io_context. Real memory safety bug that needs investigation.
**Requirements:** TBD
**Plans:** 0 plans

### Phase 999.5: Binary WS frame inconsistency for JSON responses (BACKLOG)
**Goal:** ReadResponse/BatchReadResponse sent as binary WS frames (opcode 0x02) but payload is JSON text. Either use text frames for JSON or send actual binary. Current approach forces clients to special-case two types for no benefit.
**Requirements:** TBD
**Plans:** 0 plans

### Phase 999.6: Notification echo on own writes (BACKLOG)
**Goal:** Client writing to a subscribed namespace gets both write_ack AND notification for its own write. Phase 109 (source-exclusion) is planned but currently every client must handle/ignore its own echo.
**Requirements:** TBD
**Plans:** 0 plans
