---
phase: 43
slug: storage-logging
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-20
---

# Phase 43 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | db/CMakeLists.txt (BUILD_TESTING section) |
| **Quick run command** | `cd build && ./db/chromatindb_tests "[config]" "[storage]" -c "compact"` |
| **Full suite command** | `cd build && ./db/chromatindb_tests` |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && ./db/chromatindb_tests "[config]" "[storage]" -c "compact"`
- **After every plan wave:** Run `cd build && ./db/chromatindb_tests`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 43-01-01 | 01 | 1 | OPS-04, OPS-05 | unit | `./db/chromatindb_tests "[logging]" -c "compact"` | ❌ W0 | ⬜ pending |
| 43-01-02 | 01 | 1 | OPS-03 | unit | `./db/chromatindb_tests "[config]" -c "compact"` | ❌ W0 | ⬜ pending |
| 43-02-01 | 02 | 1 | STOR-01 | unit | `./db/chromatindb_tests "[storage][tombstone-gc]" -c "compact"` | ❌ W0 | ⬜ pending |
| 43-02-02 | 02 | 1 | STOR-02 | unit | `./db/chromatindb_tests "[storage][cursor-compaction]" -c "compact"` | Partial | ⬜ pending |
| 43-02-03 | 02 | 1 | STOR-03 | unit | `./db/chromatindb_tests "[storage][integrity]" -c "compact"` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `db/tests/storage/test_storage.cpp` — stubs for STOR-01 tombstone GC entry-count verification
- [ ] `db/tests/storage/test_storage.cpp` — stubs for STOR-03 integrity_scan() tests
- [ ] `db/tests/config/test_config.cpp` — add log_file, log_max_size_mb, log_max_files, log_format validation tests
- [ ] Logging tests (OPS-04/OPS-05) may require capturing sink output; code review + manual verification acceptable

*Existing infrastructure covers cursor compaction (cleanup_stale_cursors tested) and partial metrics (log_metrics_line format string).*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| JSON log output valid JSON | OPS-04 | Requires running daemon and parsing output | Start node with log_format=json, verify output parses as JSON |
| File rotation at max size | OPS-05 | Requires sustained log output to trigger rotation | Run node under load, verify log files rotate at configured max_size_mb |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
