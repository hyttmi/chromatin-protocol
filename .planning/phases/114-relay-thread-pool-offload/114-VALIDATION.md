---
phase: 114
slug: relay-thread-pool-offload
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-14
---

# Phase 114 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3 |
| **Config file** | relay/tests/CMakeLists.txt |
| **Quick run command** | `cd build && ctest --test-dir relay/tests -j$(nproc) --output-on-failure` |
| **Full suite command** | `cd build && cmake --build . --target relay_tests && ctest --test-dir relay/tests -j$(nproc) --output-on-failure` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest --test-dir relay/tests -j$(nproc) --output-on-failure`
- **After every plan wave:** Run `cd build && cmake --build . --target relay_tests && ctest --test-dir relay/tests -j$(nproc) --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| TBD | TBD | TBD | TBD | unit | `ctest --test-dir relay/tests` | TBD | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements.

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Large blob non-blocking | TBD | Requires concurrent client timing | Start relay, send 100MB blob from client A, verify client B small queries complete within normal latency |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
