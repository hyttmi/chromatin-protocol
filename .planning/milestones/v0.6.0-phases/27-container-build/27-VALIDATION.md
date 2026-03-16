---
phase: 27
slug: container-build
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-15
---

# Phase 27 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Docker CLI + bash verification (no unit test framework) |
| **Config file** | None — validation is via Docker commands and container inspection |
| **Quick run command** | `docker build -t chromatindb . && docker run --rm chromatindb version` |
| **Full suite command** | Run all validation checks below sequentially |
| **Estimated runtime** | ~120 seconds (dominated by Docker build) |

---

## Sampling Rate

- **After every task commit:** Run `docker build -t chromatindb . && docker run --rm chromatindb version`
- **After every plan wave:** Run full suite command (all tests below)
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 120 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 27-01-01 | 01 | 1 | DOCK-01 | smoke | `docker build -t chromatindb .` | N/A (deliverable) | ⬜ pending |
| 27-01-02 | 01 | 1 | DOCK-01 | smoke | `docker run --rm chromatindb version` | N/A | ⬜ pending |
| 27-01-03 | 01 | 1 | DOCK-01 | smoke | `docker inspect chromatindb --format '{{.Config.Volumes}}'` | N/A | ⬜ pending |
| 27-01-04 | 01 | 1 | DOCK-01 | smoke | `docker run --rm --entrypoint id chromatindb` (uid != 0) | N/A | ⬜ pending |
| 27-01-05 | 01 | 1 | DOCK-01 | integration | `docker run -d -p 4200:4200 chromatindb` + TCP check | N/A | ⬜ pending |
| 27-01-06 | 01 | 1 | DOCK-01 | integration | Host peer connects to container node | N/A | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

*Existing infrastructure covers all phase requirements. The Dockerfile IS the deliverable — no test stubs needed before implementation.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Host peer connects | DOCK-01 | Requires local chromatindb build + network | Start container, run local node with container as bootstrap peer, verify handshake |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 120s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
