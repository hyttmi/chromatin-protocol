---
phase: 98
slug: ttl-enforcement
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-08
---

# Phase 98 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 (C++20) |
| **Config file** | db/tests/CMakeLists.txt |
| **Quick run command** | `cmake --build build && cd build && ctest --output-on-failure -R ttl` |
| **Full suite command** | `cmake --build build && cd build && ctest --output-on-failure` |
| **Estimated runtime** | ~30 seconds (TTL tests), ~120 seconds (full suite) |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && cd build && ctest --output-on-failure -R ttl`
- **After every plan wave:** Run `cmake --build build && cd build && ctest --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 120 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 98-01-01 | 01 | 1 | TTL-01 | unit | `ctest -R ttl_saturating` | ❌ W0 | ⬜ pending |
| 98-01-02 | 01 | 1 | TTL-01 | unit | `ctest -R ttl_expired` | ❌ W0 | ⬜ pending |
| 98-02-01 | 02 | 1 | TTL-02 | unit | `ctest -R ttl_query` | ❌ W0 | ⬜ pending |
| 98-02-02 | 02 | 1 | TTL-02 | unit | `ctest -R ttl_blobfetch` | ❌ W0 | ⬜ pending |
| 98-03-01 | 03 | 1 | TTL-03 | unit | `ctest -R ttl_ingest` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/test_ttl_enforcement.cpp` — TTL enforcement test file covering saturating arithmetic, expiry checks, query filtering, BlobFetch rejection, ingest validation
- [ ] Test helpers from `db/tests/test_helpers.h` — reuse existing TempDir, make_signed_blob, listening_address

*Existing Catch2 infrastructure covers framework needs.*

---

## Manual-Only Verifications

*All phase behaviors have automated verification.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 120s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
