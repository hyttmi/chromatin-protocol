---
phase: 20-metrics-completeness-consistency
status: passed
verified: 2026-03-13
verifier: orchestrator (inline)
---

# Phase 20: Metrics Completeness & Consistency - Verification

## Phase Goal
All NodeMetrics counters are observable in log output and shutdown cancels all timers consistently.

## Requirements Cross-Reference

| Req ID | Plan | Description | Status | Evidence |
|--------|------|-------------|--------|----------|
| OPS-06 | 20-01 | SIGUSR1 dumps current metrics via spdlog | SATISFIED | `dump_metrics()` calls `log_metrics_line()` which now includes all 10 counters including rate_limited, connected_total, disconnected_total |
| OPS-07 | 20-01 | Metrics logged periodically (60s timer) via spdlog | SATISFIED | `metrics_timer_loop()` calls `log_metrics_line()` which now includes all 10 counters |

**All 2 requirement IDs accounted for. No orphaned requirements.**

## Success Criteria Verification

### 1. log_metrics_line() includes rate_limited, peers_connected_total, peers_disconnected_total
**Status: PASSED**

Format string in `db/peer/peer_manager.cpp` now reads:
```
metrics: peers={} connected_total={} disconnected_total={} blobs={} storage={:.1f}MiB syncs={} ingests={} rejections={} rate_limited={} uptime={}
```
Arguments: `peers_.size()`, `metrics_.peers_connected_total`, `metrics_.peers_disconnected_total`, `blob_count`, `storage_mib`, `metrics_.syncs`, `metrics_.ingests`, `metrics_.rejections`, `metrics_.rate_limited`, `uptime`

10 placeholders, 10 arguments. Both `dump_metrics()` (SIGUSR1) and `metrics_timer_loop()` (60s) call `log_metrics_line()`.

### 2. on_shutdown_ cancels all 5 timers matching PeerManager::stop()
**Status: PASSED**

on_shutdown_ lambda (lines 148-159) now cancels:
1. `expiry_timer_`
2. `sync_timer_`
3. `pex_timer_`
4. `flush_timer_`
5. `metrics_timer_`

Same order as `PeerManager::stop()` (lines 184-193). Inline comment `// Cancel all timers -- must match PeerManager::stop()` documents the coupling.

### 3. Stale "Phase 18 stub" comment removed
**Status: PASSED**

`grep -ri 'Phase.*stub' db/ tests/` returns zero matches. The comment at `tests/peer/test_peer_manager.cpp:1416` was deleted. The REQUIRE assertion below it was preserved.

## Build & Test Verification

- **Build**: Clean compilation, no errors, no warnings related to changes
- **Tests**: 284/284 passed, 0 failures
- **Test command**: `cd build && ctest --output-on-failure`

## Must-Have Truth Verification

| Must-Have | Status |
|-----------|--------|
| log_metrics_line() emits all 10 counters: peers, connected_total, disconnected_total, blobs, storage, syncs, ingests, rejections, rate_limited, uptime | PASSED |
| SIGUSR1 dump output includes rate_limited, connected_total, and disconnected_total (via shared log_metrics_line call path) | PASSED |
| Periodic 60s metrics output includes rate_limited, connected_total, and disconnected_total (via shared log_metrics_line call path) | PASSED |
| on_shutdown_ lambda cancels all 5 timers in the same order as PeerManager::stop() | PASSED |
| No stale 'Phase N stub' comments remain in db/ or tests/ | PASSED |

## Score: 5/5 must-haves verified

## Verdict: PASSED

---
*Phase: 20-metrics-completeness-consistency*
*Verified: 2026-03-13*
