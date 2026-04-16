---
phase: 117
slug: blob-type-indexing-ls-filtering
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-16
---

# Phase 117 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.x |
| **Config file** | `db/tests/CMakeLists.txt`, `cli/tests/CMakeLists.txt` |
| **Quick run command** | `cd build && ctest --output-on-failure -j$(nproc)` |
| **Full suite command** | `cd build && ctest --output-on-failure -j$(nproc)` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest --output-on-failure -j$(nproc)`
- **After every plan wave:** Run `cd build && ctest --output-on-failure -j$(nproc)`
- **Before `/gsd-verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Threat Ref | Secure Behavior | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|------------|-----------------|-----------|-------------------|-------------|--------|
| 117-01-01 | 01 | 1 | TYPE-01 | — | N/A | unit | `cd build && ctest --output-on-failure -R storage` | ✅ | ⬜ pending |
| 117-01-02 | 01 | 1 | TYPE-04 | — | N/A | unit | `cd build && ctest --output-on-failure -R storage` | ❌ W0 | ⬜ pending |
| 117-02-01 | 02 | 2 | TYPE-02, TYPE-03 | — | N/A | unit | `cd build && ctest --output-on-failure -R list` | ❌ W0 | ⬜ pending |
| 117-03-01 | 03 | 3 | ERGO-02, ERGO-03 | — | N/A | unit | `cd build && ctest --output-on-failure -R cli` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] Storage type index unit tests — stubs for TYPE-01, TYPE-04
- [ ] ListRequest/ListResponse wire format tests — stubs for TYPE-02, TYPE-03
- [ ] CLI ls filtering tests — stubs for ERGO-02, ERGO-03

*Existing test infrastructure (Catch2, CMake) covers framework needs.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Schema migration on live node | TYPE-01 | Requires running node with existing data | Start node with pre-existing blobs, verify backfill completes |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
