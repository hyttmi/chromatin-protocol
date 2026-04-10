---
phase: 103
slug: uds-multiplexer-protocol-translation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-10
---

# Phase 103 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (already in relay/tests/) |
| **Config file** | relay/CMakeLists.txt (test target: relay_tests) |
| **Quick run command** | `cmake --build build && ./build/relay_tests` |
| **Full suite command** | `cmake --build build && ./build/relay_tests` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && ./build/relay_tests`
- **After every plan wave:** Run `cmake --build build && ./build/relay_tests`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 10 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 103-01-01 | 01 | 1 | MUX-01 | unit | `./build/relay_tests "[uds]"` | ❌ W0 | ⬜ pending |
| 103-01-02 | 01 | 1 | MUX-02 | unit | `./build/relay_tests "[request_router]"` | ❌ W0 | ⬜ pending |
| 103-02-01 | 02 | 1 | PROT-01 | unit | `./build/relay_tests "[translator]"` | ❌ W0 | ⬜ pending |
| 103-02-02 | 02 | 1 | PROT-04 | unit | `./build/relay_tests "[binary_frame]"` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `relay/tests/test_request_router.cpp` — stubs for MUX-02
- [ ] `relay/tests/test_translator.cpp` — stubs for PROT-01
- [ ] `relay/tests/test_uds_multiplexer.cpp` — stubs for MUX-01

*Existing Catch2 infrastructure in relay/tests/ covers framework needs.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| UDS connect to running node | MUX-01 | Requires running chromatindb node | Start node, start relay, verify UDS connection in logs |
| End-to-end JSON request/response | PROT-01 | Requires node + relay + WebSocket client | Send JSON request via wscat, verify JSON response |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 10s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
