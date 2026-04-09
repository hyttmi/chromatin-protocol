---
phase: 102
slug: authentication-json-schema
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-09
---

# Phase 102 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | relay/tests/CMakeLists.txt |
| **Quick run command** | `cd build && ctest -R relay --output-on-failure` |
| **Full suite command** | `cd build && ctest -R relay --output-on-failure` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest -R relay --output-on-failure`
- **After every plan wave:** Run `cd build && ctest -R relay --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 5 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 102-01-01 | 01 | 1 | AUTH-01 | unit | `ctest -R test_authenticator` | ❌ W0 | ⬜ pending |
| 102-01-02 | 01 | 1 | AUTH-02 | unit | `ctest -R test_authenticator` | ❌ W0 | ⬜ pending |
| 102-01-03 | 01 | 1 | AUTH-03 | unit | `ctest -R test_authenticator` | ❌ W0 | ⬜ pending |
| 102-01-04 | 01 | 1 | AUTH-04 | integration | Manual / TSAN | N/A | ⬜ pending |
| 102-01-05 | 01 | 1 | SESS-03 | unit | `ctest -R test_relay_config` | ✅ extend | ⬜ pending |
| 102-02-01 | 02 | 1 | PROT-02 | unit | `ctest -R test_type_registry` | ❌ W0 | ⬜ pending |
| 102-02-02 | 02 | 1 | PROT-03 | unit | `ctest -R test_type_registry` | ❌ W0 | ⬜ pending |
| 102-02-03 | 02 | 1 | PROT-05 | unit | `ctest -R test_message_filter` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `relay/tests/test_authenticator.cpp` — stubs for AUTH-01, AUTH-02, AUTH-03
- [ ] `relay/tests/test_message_filter.cpp` — stubs for PROT-05
- [ ] `relay/tests/test_type_registry.cpp` — stubs for PROT-02, PROT-03
- [ ] Extend `relay/tests/test_relay_config.cpp` — SESS-03 (max_connections, allowed_client_keys)

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| ML-DSA-87 verify runs on thread pool, not IO loop | AUTH-04 | Requires TSAN or manual trace inspection | Build with TSAN, run auth flow, verify no data races on OQS_SIG_verify call |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
