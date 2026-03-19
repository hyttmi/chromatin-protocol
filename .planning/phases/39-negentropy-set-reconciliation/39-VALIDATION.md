---
phase: 39
slug: negentropy-set-reconciliation
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-19
---

# Phase 39 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 |
| **Config file** | db/CMakeLists.txt (test target: chromatindb_tests) |
| **Quick run command** | `./build/db/chromatindb_tests "[reconciliation]"` |
| **Full suite command** | `./build/db/chromatindb_tests` |
| **Estimated runtime** | ~60 seconds |

---

## Sampling Rate

- **After every task commit:** Run `./build/db/chromatindb_tests "[reconciliation]"`
- **After every plan wave:** Run `./build/db/chromatindb_tests`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 60 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| TBD | 01 | 1 | SYNC-06 | unit | `./build/db/chromatindb_tests "[reconciliation]"` | ❌ W0 | ⬜ pending |
| TBD | 01 | 1 | SYNC-07 | unit+integration | `./build/db/chromatindb_tests "[reconciliation]"` | ❌ W0 | ⬜ pending |
| TBD | 02 | 1 | SYNC-08 | integration | `./build/db/chromatindb_tests "[sync]"` | ✅ existing | ⬜ pending |
| TBD | 02 | 1 | SYNC-09 | unit | `./build/db/chromatindb_tests "[reconciliation]"` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/sync/test_reconciliation.cpp` — unit tests for XOR fingerprint, range splitting, encode/decode, diff computation
- [ ] Extend `db/tests/sync/test_sync_protocol.cpp` — integration tests for reconciliation within sync flow

*Existing test infrastructure (Catch2, test helpers, daemon harness) covers all framework needs.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| O(diff) wire traffic scaling | SYNC-07 | Requires large namespace (1M+ blobs) to observe scaling | Deferred to Phase 41 benchmark validation |

*Note: Unit tests verify correctness. Scaling verification is Phase 41's responsibility.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 60s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
