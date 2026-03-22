---
phase: 41
slug: benchmark-validation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-19
---

# Phase 41 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (unit tests) + Docker benchmark suite (integration) |
| **Config file** | db/CMakeLists.txt (Catch2) + deploy/run-benchmark.sh (Docker) |
| **Quick run command** | `bash -n deploy/run-benchmark.sh` (syntax check) |
| **Full suite command** | `bash deploy/run-benchmark.sh` |
| **Estimated runtime** | ~600 seconds (Docker build + all scenarios) |

---

## Sampling Rate

- **After every task commit:** Run `bash -n deploy/run-benchmark.sh` (syntax check)
- **After every plan wave:** Run `bash deploy/run-benchmark.sh` (full benchmark)
- **Before `/gsd:verify-work`:** Full suite must produce passing report
- **Max feedback latency:** 5 seconds (syntax check), ~600 seconds (full run)

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| TBD | 01 | 1 | SYNC-10 (O(diff)) | integration | `bash deploy/run-benchmark.sh` | Partially (need new scenario) | ⬜ pending |
| TBD | 01 | 1 | SYNC-10 (throughput) | integration | `bash deploy/run-benchmark.sh` | ✅ (existing ingest-1m) | ⬜ pending |
| TBD | 01 | 1 | SYNC-10 (regression) | integration | `bash deploy/run-benchmark.sh` | ✅ (existing ingest-1k + sync) | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] New reconcile-scaling scenario function in `deploy/run-benchmark.sh`
- [ ] Benchmark node configs for short sync interval + no cooldown (sync_cooldown_seconds=0)
- [ ] Report generation updates for reconciliation scaling and comparison sections
- [ ] Archive v0.6.0 baseline to `deploy/results/v0.6.0-baseline/`

*The full benchmark run takes ~10-15 minutes (Docker build + all scenarios)*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Report readability | SYNC-10 | Human judgment on report clarity | Review deploy/results/REPORT.md for clear baseline vs current comparison |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 600s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
