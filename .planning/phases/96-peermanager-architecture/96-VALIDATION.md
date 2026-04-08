---
phase: 96
slug: peermanager-architecture
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-08
---

# Phase 96 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (C++20) |
| **Config file** | db/tests/CMakeLists.txt |
| **Quick run command** | `cd build && ctest --output-on-failure -R "peer_manager\|connection_manager\|sync\|message_dispatch\|pex\|metrics"` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest --output-on-failure -R "peer_manager\|connection_manager\|sync\|message_dispatch\|pex\|metrics"`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 96-01-01 | 01 | 1 | ARCH-01 | unit + integration | `cd build && ctest --output-on-failure` | ✅ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

*Existing infrastructure covers all phase requirements.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| ASAN/TSAN/UBSAN clean | ARCH-01 | Requires sanitizer build | Build with `-DSANITIZER=address`, run full suite |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
