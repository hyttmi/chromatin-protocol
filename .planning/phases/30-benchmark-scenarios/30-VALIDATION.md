---
phase: 30
slug: benchmark-scenarios
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-16
---

# Phase 30 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Manual bash execution + output inspection |
| **Config file** | None (bash script) |
| **Quick run command** | `bash -n deploy/run-benchmark.sh` |
| **Full suite command** | `bash deploy/run-benchmark.sh` |
| **Estimated runtime** | ~300 seconds (full benchmark suite with topology restarts) |

---

## Sampling Rate

- **After every task commit:** Run `bash -n deploy/run-benchmark.sh`
- **After every plan wave:** Run `bash deploy/run-benchmark.sh` and verify results/ output
- **Before `/gsd:verify-work`:** Full suite must be green — all 7 result JSON files present with valid content
- **Max feedback latency:** 5 seconds (syntax check), 300 seconds (full run)

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 30-01-01 | 01 | 1 | LOAD-04 | integration | `bash -n deploy/run-benchmark.sh` | ❌ W0 | ⬜ pending |
| 30-01-02 | 01 | 1 | PERF-01 | integration | Verify `results/scenario-ingest-*.json` exist with blobs_per_sec, mib_per_sec, latency_ms | ❌ W0 | ⬜ pending |
| 30-01-03 | 01 | 1 | OBS-01 | integration | Verify `results/docker-stats-*.json` files contain CPU/mem data | ❌ W0 | ⬜ pending |
| 30-02-01 | 02 | 1 | PERF-02 | integration | Verify `results/scenario-sync-latency.json` contains sync_1hop_ms > 0 | ❌ W0 | ⬜ pending |
| 30-02-02 | 02 | 1 | PERF-03 | integration | Verify `results/scenario-sync-latency.json` contains sync_2hop_ms > sync_1hop_ms | ❌ W0 | ⬜ pending |
| 30-02-03 | 02 | 1 | PERF-04 | integration | Verify `results/scenario-latejoin.json` contains catchup_ms > 0 | ❌ W0 | ⬜ pending |
| 30-02-04 | 02 | 1 | PERF-05 | integration | Verify `results/scenario-trusted-vs-pq.json` contains pq_mode and trusted_mode | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `deploy/run-benchmark.sh` — main benchmark orchestration script
- [ ] `deploy/results/` — directory created by script at runtime
- [ ] `jq` availability check — script should verify jq is installed

*Existing infrastructure covers Docker Compose topology (Phase 29) and loadgen binary (Phase 28).*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Resource profiling captures meaningful data | OBS-01 | Docker stats values depend on actual workload; cannot assert specific CPU/mem thresholds | Inspect docker-stats JSON files for non-zero CPU and mem values after benchmark run |
| Sync latency values are plausible | PERF-02, PERF-03 | Wall-clock times depend on sync interval (~10s); values should be in 10-30s range | Check sync_1hop_ms < sync_2hop_ms and both > 0 |
| Late-joiner catch-up completes | PERF-04 | Catch-up time depends on data volume and sync timing | Check catchup_ms > 0 and node4 reaches expected blob count |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 300s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
