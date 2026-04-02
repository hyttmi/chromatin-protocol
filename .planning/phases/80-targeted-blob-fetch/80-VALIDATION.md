---
phase: 80
slug: targeted-blob-fetch
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-02
---

# Phase 80 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (latest via FetchContent) |
| **Config file** | CMakeLists.txt catch_discover_tests(chromatindb_tests) |
| **Quick run command** | `cd build && ctest -R "peer" --output-on-failure` |
| **Full suite command** | `cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ctest -R "peer\|message_filter" --output-on-failure`
- **After every plan wave:** Run `cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 80-01-01 | 01 | 1 | WIRE-02 | unit | `cd build && ctest -R "message_filter" --output-on-failure` | ❌ W0 | ⬜ pending |
| 80-01-02 | 01 | 1 | WIRE-03 | unit | `cd build && ctest -R "message_filter" --output-on-failure` | ❌ W0 | ⬜ pending |
| 80-02-01 | 02 | 2 | PUSH-05 | integration | `cd build && ctest -R "peer" --output-on-failure` | ❌ W0 | ⬜ pending |
| 80-02-02 | 02 | 2 | PUSH-06 | unit | `cd build && ctest -R "peer" --output-on-failure` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/peer/test_peer_manager.cpp` — new test cases for BlobFetch request/response, dedup, not-found, sync suppression
- [ ] `db/tests/relay/test_message_filter.cpp` — verify types 60/61 are blocked

*Existing infrastructure covers framework and fixtures; only new test cases needed.*

---

## Manual-Only Verifications

*All phase behaviors have automated verification.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
