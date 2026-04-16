---
phase: 116
slug: cli-rename-contact-groups
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-16
---

# Phase 116 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.x |
| **Config file** | `cli/tests/CMakeLists.txt` |
| **Quick run command** | `cd build && ctest --test-dir cli/tests -j$(nproc) --output-on-failure` |
| **Full suite command** | `cd build && ctest --test-dir cli/tests -j$(nproc) --output-on-failure` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run quick run command
- **After every plan wave:** Run full suite command
- **Before `/gsd-verify-work`:** Full suite must be green
- **Max feedback latency:** 5 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Threat Ref | Secure Behavior | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|------------|-----------------|-----------|-------------------|-------------|--------|
| TBD | 01 | 1 | ERGO-01 | — | N/A | build | `cmake --build build -j$(nproc) && ./build/cli/cdb --version` | ❌ W0 | ⬜ pending |
| TBD | 02 | 1 | CONT-01 | — | N/A | unit | `ctest --test-dir build/cli/tests --output-on-failure -R group` | ❌ W0 | ⬜ pending |
| TBD | 02 | 1 | CONT-02 | — | N/A | unit | `ctest --test-dir build/cli/tests --output-on-failure -R group` | ❌ W0 | ⬜ pending |
| TBD | 03 | 2 | CONT-03 | — | N/A | unit | `ctest --test-dir build/cli/tests --output-on-failure -R share` | ❌ W0 | ⬜ pending |
| TBD | 04 | 2 | CONT-04 | — | N/A | unit | `ctest --test-dir build/cli/tests --output-on-failure -R import` | ❌ W0 | ⬜ pending |
| TBD | 04 | 2 | CONT-05 | — | N/A | unit | `ctest --test-dir build/cli/tests --output-on-failure -R schema` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `cli/tests/test_contacts.cpp` — stubs for group CRUD, schema migration
- [ ] Test fixtures for SQLite in-memory databases

*Existing Catch2 infrastructure covers framework needs.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| `cdb` runs as primary executable | ERGO-01 | Binary name check | `./build/cli/cdb --version` outputs `cdb 1.0.0` |
| `--share @group` end-to-end | CONT-03 | Requires running node | Start node, create group, `cdb put --share @team file` |

---

## Validation Sign-Off

- [ ] All tasks have automated verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
