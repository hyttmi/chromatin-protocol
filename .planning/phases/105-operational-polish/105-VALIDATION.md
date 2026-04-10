---
phase: 105
slug: operational-polish
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-10
---

# Phase 105 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | relay/tests/CMakeLists.txt |
| **Quick run command** | `cmake --build build && ./build/relay/tests/chromatindb_relay_tests` |
| **Full suite command** | `cmake --build build && ./build/relay/tests/chromatindb_relay_tests` |
| **Estimated runtime** | ~2 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && ./build/relay/tests/chromatindb_relay_tests`
- **After every plan wave:** Run `cmake --build build && ./build/relay/tests/chromatindb_relay_tests`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 105-01-01 | 01 | 1 | OPS-01 | unit | `./build/relay/tests/chromatindb_relay_tests "[metrics_collector]"` | ❌ W0 | ⬜ pending |
| 105-01-02 | 01 | 1 | OPS-01 | unit | `./build/relay/tests/chromatindb_relay_tests "[metrics_collector]"` | ❌ W0 | ⬜ pending |
| 105-01-03 | 01 | 1 | OPS-03 | unit | `./build/relay/tests/chromatindb_relay_tests "[rate_limiter]"` | ❌ W0 | ⬜ pending |
| 105-01-04 | 01 | 1 | OPS-03 | unit | `./build/relay/tests/chromatindb_relay_tests "[rate_limiter]"` | ❌ W0 | ⬜ pending |
| 105-02-01 | 02 | 1 | OPS-02 | unit | `./build/relay/tests/chromatindb_relay_tests "[relay_config]"` | Extend existing | ⬜ pending |
| 105-02-02 | 02 | 1 | SESS-04 | integration | Manual (requires running relay) | N/A | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `relay/tests/test_rate_limiter.cpp` — stubs for OPS-03 (token bucket logic)
- [ ] `relay/tests/test_metrics_collector.cpp` — stubs for OPS-01 (Prometheus format, counter increment)
- [ ] Extend `relay/tests/test_relay_config.cpp` — covers OPS-02 (new config fields parse/validate)

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Graceful shutdown drains then closes | SESS-04 | Requires running relay with active sessions | Start relay, connect client, send SIGTERM, verify drain-then-close sequence in logs |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
