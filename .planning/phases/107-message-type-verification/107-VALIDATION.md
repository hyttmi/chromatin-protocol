---
phase: 107
slug: message-type-verification
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-11
---

# Phase 107 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Custom C++ smoke test (not Catch2) |
| **Config file** | N/A — standalone binary |
| **Quick run command** | `cmake --build build -j$(nproc) --target relay_smoke_test` |
| **Full suite command** | `/tmp/chromatindb-test/run-smoke.sh` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build -j$(nproc) --target relay_smoke_test`
- **After every plan wave:** Run `/tmp/chromatindb-test/run-smoke.sh`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 107-01-01 | 01 | 1 | E2E-01 | build | `cmake --build build -j$(nproc) --target relay_smoke_test` | ✅ | ⬜ pending |
| 107-01-02 | 01 | 1 | E2E-01 | integration | `/tmp/chromatindb-test/run-smoke.sh` | ✅ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] Extend `ws_recv_text()` to handle binary WS frames (opcode 0x02) — covers ReadResponse/BatchReadResponse
- [ ] Add SHA3-256 signing input helper function — covers Data(8) write
- [ ] Add Data message construction helper — covers write/read/delete chain

*These are foundational changes required before any new test types can be added.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| StorageFull/QuotaExceeded broadcast | E2E-01 (partial) | Cannot trigger from normal client request | Verify node signal types are in type_registry; translation code exists. Accept as untestable in smoke test. |

*All other phase behaviors have automated verification via the extended smoke test.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
