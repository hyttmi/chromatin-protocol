---
phase: 31
slug: report-generation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-16
---

# Phase 31 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Manual validation (bash script) |
| **Config file** | None (bash script, not a test framework) |
| **Quick run command** | `bash deploy/run-benchmark.sh --report-only` |
| **Full suite command** | `bash deploy/run-benchmark.sh` |
| **Estimated runtime** | ~5 seconds (--report-only with existing JSON) |

---

## Sampling Rate

- **After every task commit:** `bash -n deploy/run-benchmark.sh` (syntax check)
- **After every plan wave:** `bash deploy/run-benchmark.sh --report-only` (requires sample JSON data or prior full run)
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 5 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 31-01-01 | 01 | 1 | OBS-02, OBS-03 | manual | `bash -n deploy/run-benchmark.sh && echo OK` | ✅ | ⬜ pending |
| 31-01-02 | 01 | 1 | OBS-02 | manual | Human review of REPORT.md structure | N/A | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

*Existing infrastructure covers all phase requirements.*

No test framework needed. This is a bash script enhancement verified by running it and inspecting output. The `--report-only` flag enables fast iteration without re-running benchmarks.

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Markdown report contains all sections (hardware, topology, params, scenarios, resources, analysis, caveats) | OBS-02 | Output is prose/tables — needs human review for correctness and readability | Run `--report-only`, review `deploy/results/REPORT.md` |
| benchmark-summary.json contains all scenario data | OBS-03 | JSON structure correctness verified by `jq .` | Run `--report-only`, validate with `jq . deploy/results/benchmark-summary.json` |
| --report-only flag works without running benchmarks | OBS-02 | Requires existing JSON results from prior run | Run full suite once, then test `--report-only` separately |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
