---
gsd_state_version: 1.0
milestone: v3.1.0
milestone_name: milestone
status: executing
stopped_at: Completed 999.9-08-PLAN.md
last_updated: "2026-04-13T10:23:12.211Z"
last_activity: 2026-04-13
progress:
  total_phases: 14
  completed_phases: 9
  total_plans: 28
  completed_plans: 27
  percent: 33
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-09)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** v3.1.0 Relay Live Hardening -- Phase 999.9 HTTP transport in progress.

## Current Position

Phase: 999.9
Plan: 6 of 9
Status: Ready to execute
Last activity: 2026-04-13

Progress: [###-------] 33%

## Accumulated Context

### Decisions

- Config restoration guard pattern for tests that mutate relay config via SIGHUP
- 10-second SO_RCVTIMEO for SIGTERM test to accommodate worst-case shutdown sequence
- catch_error flag pattern for GCC co_await-in-catch limitation (Phase 999.2-01)
- Static free function for send_error_response helper (Phase 999.2-01)
- [Phase 999.2]: ErrorResponse uses compound decoder (not flat) for human-readable error code/type names
- [Phase 999.2]: Alphabetical sort: 'error' < 'exists_request' -- plan's sort position was wrong, corrected
- [Phase 999.2]: ErrorResponse E2E: validation_failed path preferred over malformed_payload (relay rejects truly malformed requests)
- [Phase 999.3]: purge_stale name kept unchanged -- callback-based overload is optional extension, not rename-worthy
- [Phase 999.3]: Both errors_total and request_timeouts_total counters increment on timeout for general + specific monitoring
- [Phase 999.7]: Used chromatindb::util endian.h functions for all BE conversions instead of inline bit shifts
- [Phase 999.7]: Manual BE push_back pattern retained for relay (no vector-append helper in endian.h)
- [Phase 999.5]: OPCODE_BINARY constant kept in ws_frame.h for receive-side validation
- [Phase 999.8]: Manifest magic in engine namespace (not wire/codec) -- chunking is an engine convention
- [Phase 999.8]: store_blobs_atomic uses per-namespace quota accumulation for batch quota checking
- [Phase 999.8]: Duplicate blobs in atomic batch get Duplicate status while new blobs still store
- [Phase 999.8]: store_chunked uses crypto::offload per-chunk for ML-DSA-87 signing
- [Phase 999.8]: read_chunked is synchronous (not a coroutine) -- matches get_blob pattern
- [Phase 109]: Blob size check uses base64 upper-bound estimate for fast O(1) rejection
- [Phase 109]: Health endpoint returns JSON with UDS connectivity status (200/503)
- [Phase 109]: WriteTracker owned by UdsMultiplexer, session disconnect cleanup via set_write_tracker pointer pattern
- [Phase 999.9-01]: HttpResponse is header-only with inline static builders (no .cpp needed)
- [Phase 999.9-01]: TokenStore dual-map for O(1) lookup by both token and session_id
- [Phase 999.9-01]: relay/http/ namespace (chromatindb::relay::http) for all HTTP transport components
- [Phase 999.9-02]: SessionDispatch uses std::function callbacks rather than virtual base class for transport abstraction
- [Phase 999.9-02]: send_error fallback to send_json when not set -- error paths safe by default
- [Phase 999.9]: HttpRouter handler receives body as separate vector for zero-copy large blob payloads
- [Phase 999.9]: ResponsePromise uses asio::steady_timer signal pattern (same as Session drain), template wait() for flexible timeout, non-owning pointer in map
- [Phase 999.9]: HttpRouter extended with AsyncHandler + dispatch_async for coroutine-based handlers
- [Phase 999.9]: ReadResponse raw binary pass-through (skip status byte) for application/octet-stream blob data
- [Phase 999.9]: SseWriter uses type-erased WriteFn to decouple from TLS/plain stream
- [Phase 999.9]: MetricsCollector simplified to pure formatter; /metrics and /health served by main HTTP server
- [Phase 999.9]: UdsMultiplexer route_response checks ResponsePromiseMap before WS dispatch for HTTP request/response

### Pending Todos

- Plan 03: Handler integration (wire chunking into message dispatch)

### Blockers/Concerns

None.

### Session

Last session: 2026-04-13T10:23:12.207Z
Stopped at: Completed 999.9-08-PLAN.md
