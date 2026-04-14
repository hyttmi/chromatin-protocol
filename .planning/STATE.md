---
gsd_state_version: 1.0
milestone: v4.0.0
milestone_name: milestone
status: executing
stopped_at: Phase 115 context gathered
last_updated: "2026-04-14T11:46:12.314Z"
last_activity: 2026-04-14
progress:
  total_phases: 5
  completed_phases: 4
  total_plans: 8
  completed_plans: 8
  percent: 0
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-14)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** Phase 114 — relay-thread-pool-offload

## Current Position

Phase: 114 (relay-thread-pool-offload) — EXECUTING
Plan: 2 of 2
Status: Ready to execute
Last activity: 2026-04-14

Progress: [----------] 0%

## Performance Metrics

**Velocity:**

- Total plans completed: 0
- Average duration: -
- Total execution time: 0 hours

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| - | - | - | - |

**Recent Trend:**

- Last 5 plans: -
- Trend: -

*Updated after each plan completion*
| Phase 111 P02 | 12min | 2 tasks | 23 files |
| Phase 111 P03 | 10min | 2 tasks | 2 files |
| Phase 113 P01 | 1min | 2 tasks | 1 files |
| Phase 113 P02 | 10min | 2 tasks | 2 files |
| Phase 114 P02 | 10min | 2 tasks | 4 files |

## Accumulated Context

### Roadmap Evolution

- Phase 114 added: Relay Thread Pool Offload — fix event loop starvation by offloading CPU-heavy work to thread pool
- Phase 115 added: Chunked Streaming for Large Blobs — eliminate full-blob buffering, implement chunked I/O through relay

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Previous milestone decisions archived to milestones/v3.1.0-ROADMAP.md.

- [Phase 999.10]: Strand confinement failed under ASAN -- strands don't survive co_await on promise timers. Single-threaded rewrite chosen.
- [v4.0.0]: Single io_context thread + thread pool offload, same pattern as node's PeerManager
- [Phase 111]: ML-DSA-87 verify offloaded to thread pool via offload() with transfer-back in http_router.cpp
- [Phase 111]: No deviations needed -- Plan 02 changes were clean, tests adapted mechanically
- [Phase 113]: Port 4280/4281 for perf harness, no pass/fail thresholds (baseline only), all relay_benchmark.py defaults
- [Phase 113]: WriteAck JSON field is 'hash' not 'blob_hash' -- benchmark script fixed (Rule 1 bug)
- [Phase 113]: Benchmark results: 14 MiB/s large blob, 952 blobs/sec peak, +5250% p99 degradation under mixed load — single-threaded event loop is the bottleneck
- [Phase 114]: Binary blob transfer (multipart HTTP) deferred to Phase 115 — fix starvation first, then eliminate base64
- [Phase 114]: AEAD counters captured by value on event loop before offload lambda (D-10 correctness)
- [Phase 114]: Notification/broadcast pre-translated in coroutine read_loop(), dispatched to sync handlers
- [Phase 114]: Query json_to_binary stays inline (size=0) -- payloads always sub-KB

### Pending Todos

None.

### Blockers/Concerns

None.

## Session Continuity

Last session: 2026-04-14T11:46:12.310Z
Stopped at: Phase 115 context gathered
Resume file: .planning/phases/115-chunked-streaming-for-large-blobs/115-CONTEXT.md
