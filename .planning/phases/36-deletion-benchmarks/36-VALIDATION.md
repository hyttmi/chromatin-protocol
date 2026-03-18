---
phase: 36
slug: deletion-benchmarks
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-18
---

# Phase 36 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Manual integration testing via Docker + Catch2 for unit tests |
| **Config file** | deploy/run-benchmark.sh (orchestration), db/CMakeLists.txt (unit tests) |
| **Quick run command** | `cd build && cmake --build . --target chromatindb_loadgen && ./chromatindb_loadgen --help` |
| **Full suite command** | `bash deploy/run-benchmark.sh` |
| **Estimated runtime** | ~15 minutes (full benchmark suite with tombstone scenarios) |

---

## Sampling Rate

- **After every task commit:** Build loadgen and verify new flags work (`--delete --help`)
- **After every plan wave:** `bash deploy/run-benchmark.sh` (full benchmark run)
- **Before `/gsd:verify-work`:** Full benchmark suite green, REPORT.md contains tombstone sections
- **Max feedback latency:** 30 seconds (build check), 15 minutes (full suite)

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 36-01-01 | 01 | 1 | BENCH-01 | integration | `bash deploy/run-benchmark.sh` | No -- Wave 0 | pending |
| 36-01-02 | 01 | 1 | BENCH-01 | build | `cmake --build build --target chromatindb_loadgen` | Yes | pending |
| 36-02-01 | 02 | 2 | BENCH-01/02/03 | integration | `bash deploy/run-benchmark.sh` | No -- Wave 0 | pending |
| 36-02-02 | 02 | 2 | BENCH-01/02/03 | report | `bash deploy/run-benchmark.sh --report-only` | No -- Wave 0 | pending |

*Status: pending / green / red / flaky*

---

## Wave 0 Requirements

- Loadgen `--delete` mode does not exist -- must be implemented
- Loadgen does not output `blob_hashes` in JSON -- must be added
- Loadgen does not support `--identity-file` / `--identity-save` -- must be added
- `run_scenario_tombstone_*()` functions do not exist in run-benchmark.sh
- Storage-based GC polling does not exist (blob count metric is unusable)
- Report generation does not include tombstone sections
- benchmark-summary.json does not include tombstone scenario keys

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Full benchmark suite runs end-to-end with Docker | BENCH-01/02/03 | Requires Docker infrastructure | Run `bash deploy/run-benchmark.sh`, verify REPORT.md has tombstone sections |
| GC reclamation timing is reasonable | BENCH-03 | Empirical validation needed | Check GC latency values in REPORT.md are within expected range |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s (build check)
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
