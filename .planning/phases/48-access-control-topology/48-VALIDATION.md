---
phase: 48
slug: access-control-topology
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-21
---

# Phase 48 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Bash integration tests (shell scripts) |
| **Config file** | tests/integration/run-integration.sh |
| **Quick run command** | `bash tests/integration/run-integration.sh --skip-build --filter "<test_name>"` |
| **Full suite command** | `bash tests/integration/run-integration.sh --skip-build` |
| **Estimated runtime** | ~300 seconds (6 integration tests, Docker startup overhead) |

---

## Sampling Rate

- **After every task commit:** Run `bash tests/integration/run-integration.sh --skip-build --filter "<test_name>"`
- **After every plan wave:** Run `bash tests/integration/run-integration.sh --skip-build`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 120 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 48-01-01 | 01 | 1 | ACL-01 | integration | `bash tests/integration/run-integration.sh --skip-build --filter acl01` | W0 | pending |
| 48-01-02 | 01 | 1 | ACL-02 | integration | `bash tests/integration/run-integration.sh --skip-build --filter acl02` | W0 | pending |
| 48-02-01 | 02 | 2 | ACL-03 | integration | `bash tests/integration/run-integration.sh --skip-build --filter acl03` | W0 | pending |
| 48-02-02 | 02 | 2 | ACL-04 | integration | `bash tests/integration/run-integration.sh --skip-build --filter acl04` | W0 | pending |
| 48-03-01 | 03 | 3 | ACL-05 | integration | `bash tests/integration/run-integration.sh --skip-build --filter acl05` | W0 | pending |
| 48-03-02 | 03 | 3 | TOPO-01 | integration | `bash tests/integration/run-integration.sh --skip-build --filter topo01` | W0 | pending |

*Status: pending / green / red / flaky*

---

## Wave 0 Requirements

- [ ] `tests/integration/test_acl01_closed_garden.sh` -- covers ACL-01
- [ ] `tests/integration/test_acl02_namespace_sovereignty.sh` -- covers ACL-02
- [ ] `tests/integration/test_acl03_delegation_write.sh` -- covers ACL-03
- [ ] `tests/integration/test_acl04_revocation.sh` -- covers ACL-04
- [ ] `tests/integration/test_acl05_sighup_reload.sh` -- covers ACL-05
- [ ] `tests/integration/test_topo01_connection_dedup.sh` -- covers TOPO-01
- [ ] `tests/integration/docker-compose.acl.yml` -- 3-node ACL topology
- [ ] `tests/integration/docker-compose.dedup.yml` -- 2-node mutual-peer topology
- [ ] `tests/integration/configs/node*-acl.json` -- ACL test configs
- [ ] `tests/integration/configs/node*-dedup.json` -- dedup test configs
- [ ] Loadgen `--delegate` flag -- creates delegation blob for ACL-03/04
- [ ] Connection dedup in `db/peer/peer_manager.cpp` -- code change for TOPO-01

---

## Manual-Only Verifications

*All phase behaviors have automated verification.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 120s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
