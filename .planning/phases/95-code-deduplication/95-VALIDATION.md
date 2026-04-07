---
phase: 95
slug: code-deduplication
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-07
---

# Phase 95 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (C++ unit tests) |
| **Config file** | db/CMakeLists.txt (test executable sources) |
| **Quick run command** | `cmake --build build && cd build && ctest --output-on-failure -R "endian\|auth_helpers\|verify_helper\|extract_helpers"` |
| **Full suite command** | `cmake --build build && cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run quick run command (new helper tests)
- **After every plan wave:** Run full suite command (all 615+ tests)
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 95-01-01 | 01 | 1 | DEDUP-01 | unit | `ctest -R endian` | ❌ W0 | ⬜ pending |
| 95-02-01 | 02 | 1 | DEDUP-02 | unit | `ctest -R auth_helpers` | ❌ W0 | ⬜ pending |
| 95-03-01 | 03 | 2 | DEDUP-03 | unit | `ctest -R verify_helper` | ❌ W0 | ⬜ pending |
| 95-04-01 | 04 | 2 | DEDUP-04 | unit | `ctest -R extract_helpers` | ❌ W0 | ⬜ pending |
| 95-XX-XX | all | 3 | DEDUP-05 | regression | `ctest --output-on-failure` | ✅ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/test_endian.cpp` — tests for write_u16_be, write_u32_be, write_u64_be, store_u16_be, store_u32_be, store_u64_be, read_u16_be, read_u32_be, read_u64_be
- [ ] `db/tests/test_auth_helpers.cpp` — tests for auth payload encode/decode shared functions
- [ ] `db/tests/test_verify_helper.cpp` — tests for verify_with_offload shared method
- [ ] `db/tests/test_extract_helpers.cpp` — tests for namespace+hash extraction helper
- [ ] Add new test files to CMakeLists.txt test executable sources

*Existing infrastructure covers framework — Catch2 already configured.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| ASAN/TSAN/UBSAN clean | DEDUP-05 | Sanitizer builds require separate CMake config | Build with -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" and run full suite |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
