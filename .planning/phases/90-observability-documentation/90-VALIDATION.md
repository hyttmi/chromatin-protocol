---
phase: 90
slug: observability-documentation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-05
---

# Phase 90 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (C++ unit tests), pytest (Python SDK tests) |
| **Config file** | db/CMakeLists.txt (C++), sdk/python/pyproject.toml (Python) |
| **Quick run command** | `cmake --build build && cd build && ctest --output-on-failure -R metrics` |
| **Full suite command** | `cmake --build build && cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds (C++ build + tests), ~3 seconds (Python SDK tests) |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && cd build && ctest --output-on-failure -R metrics`
- **After every plan wave:** Run full C++ and Python suites
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 90-01-01 | 01 | 1 | OPS-02 | unit | `ctest -R metrics` | ❌ W0 | ⬜ pending |
| 90-01-02 | 01 | 1 | OPS-03 | unit | `ctest -R metrics` | ❌ W0 | ⬜ pending |
| 90-02-01 | 02 | 2 | DOC-01 | grep | `grep -c SyncNamespaceAnnounce db/PROTOCOL.md` | ✅ | ⬜ pending |
| 90-02-02 | 02 | 2 | DOC-02 | grep | `grep -c 'metrics\|observability' README.md` | ✅ | ⬜ pending |
| 90-02-03 | 02 | 2 | DOC-03 | grep | `grep -c 'Brotli\|compress' sdk/python/README.md` | ✅ | ⬜ pending |
| 90-02-04 | 02 | 2 | DOC-04 | grep | `grep -c 'metrics\|observability' sdk/python/docs/getting-started.md` | ✅ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/net/test_metrics_endpoint.cpp` — test stubs for OPS-02, OPS-03 (HTTP listener, Prometheus format)

*Existing C++ test infrastructure and Python pytest setup cover all other needs.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Prometheus scrape compatibility | OPS-02 | End-to-end requires running Prometheus | Start node with metrics_bind, curl /metrics, verify parseable |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
