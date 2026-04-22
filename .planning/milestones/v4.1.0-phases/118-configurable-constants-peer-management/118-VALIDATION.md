---
phase: 118
slug: configurable-constants-peer-management
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-16
---

# Phase 118 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3 |
| **Config file** | `db/tests/CMakeLists.txt` |
| **Quick run command** | `cd build && ctest --test-dir db/tests -j$(nproc) --output-on-failure` |
| **Full suite command** | `cd build && ctest --test-dir db/tests -j$(nproc) --output-on-failure` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest --test-dir db/tests -j$(nproc) --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --test-dir db/tests -j$(nproc) --output-on-failure`
- **Before `/gsd-verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Threat Ref | Secure Behavior | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|------------|-----------------|-----------|-------------------|-------------|--------|
| 118-01-01 | 01 | 1 | CONF-01 | — | N/A | unit | `ctest -R test_config` | ✅ | ⬜ pending |
| 118-01-02 | 01 | 1 | CONF-02 | — | N/A | unit | `ctest -R test_config` | ✅ | ⬜ pending |
| 118-01-03 | 01 | 1 | CONF-03 | — | Invalid values rejected | unit | `ctest -R test_config` | ✅ | ⬜ pending |
| 118-02-01 | 02 | 2 | PEER-01 | — | N/A | unit | `ctest -R test_peer` | ❌ W0 | ⬜ pending |
| 118-02-02 | 02 | 2 | PEER-02 | — | N/A | unit | `ctest -R test_peer` | ❌ W0 | ⬜ pending |
| 118-02-03 | 02 | 2 | PEER-03 | — | N/A | unit | `ctest -R test_peer` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/peer/test_peer_commands.cpp` — stubs for PEER-01, PEER-02, PEER-03
- [ ] Config test coverage already exists in `db/tests/config/test_config.cpp`

*Existing test infrastructure covers config requirements. Peer command tests need new test file.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| SIGHUP reload | CONF-02 | Requires running node process | Start node, change config.json, send SIGHUP, verify log output |
| add-peer triggers SIGHUP | PEER-01 | Requires running node process | Start node, run `chromatindb add-peer`, verify config updated and node reloaded |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
