---
phase: 49
slug: network-resilience-reconciliation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-21
---

# Phase 49 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Bash integration tests (shell scripts + helpers.sh) |
| **Config file** | tests/integration/run-integration.sh |
| **Quick run command** | `bash tests/integration/run-integration.sh --skip-build --filter "<test>"` |
| **Full suite command** | `bash tests/integration/run-integration.sh --skip-build` |
| **Estimated runtime** | ~600 seconds (10 tests, sequential) |

---

## Sampling Rate

- **After every task commit:** Run `bash tests/integration/run-integration.sh --skip-build --filter "<test_name>"`
- **After every plan wave:** Run `bash tests/integration/run-integration.sh --skip-build`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** ~60 seconds per individual test

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 49-01-01 | 01 | 1 | NET-03 | integration | `run-integration.sh --skip-build --filter net03` | ❌ W0 | ⬜ pending |
| 49-01-02 | 01 | 1 | NET-04 | integration | `run-integration.sh --skip-build --filter net04` | ❌ W0 | ⬜ pending |
| 49-01-03 | 01 | 1 | NET-05 | integration | `run-integration.sh --skip-build --filter net05` | ❌ W0 | ⬜ pending |
| 49-02-01 | 02 | 2 | NET-01 | integration | `run-integration.sh --skip-build --filter net01` | ❌ W0 | ⬜ pending |
| 49-02-02 | 02 | 2 | NET-02 | integration | `run-integration.sh --skip-build --filter net02` | ❌ W0 | ⬜ pending |
| 49-03-01 | 03 | 3 | NET-06 | integration | `run-integration.sh --skip-build --filter net06` | ❌ W0 | ⬜ pending |
| 49-03-02 | 03 | 3 | RECON-01 | integration | `run-integration.sh --skip-build --filter recon01` | ❌ W0 | ⬜ pending |
| 49-03-03 | 03 | 3 | RECON-02 | integration | `run-integration.sh --skip-build --filter recon02` | ❌ W0 | ⬜ pending |
| 49-03-04 | 03 | 3 | RECON-03 | integration | `run-integration.sh --skip-build --filter recon03` | ❌ W0 | ⬜ pending |
| 49-03-05 | 03 | 3 | RECON-04 | integration | `run-integration.sh --skip-build --filter recon04` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `tests/integration/docker-compose.mesh.yml` — 5-node mesh topology (fixed IPs)
- [ ] `tests/integration/configs/node{1-5}-mesh.json` — 5-node mesh configs
- [ ] `tests/integration/docker-compose.recon.yml` — 2-node reconciliation topology
- [ ] `tests/integration/configs/node{1-2}-recon.json` — 2-node recon configs

*All test script stubs are created by their respective plans.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| RECON-03 version byte rejection | RECON-03 | AEAD encryption prevents wire injection of malformed ReconcileInit from outside | Verify via unit test coverage of `decode_reconcile_init`; Docker test confirms node stays healthy through sync rounds |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 60s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
