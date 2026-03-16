---
phase: 28
slug: load-generator
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-15
---

# Phase 28 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 v3.7.1 |
| **Config file** | CMakeLists.txt (catch_discover_tests) |
| **Quick run command** | `cmake --build build && ./build/chromatindb_tests -t "[loadgen]"` |
| **Full suite command** | `cmake --build build && ctest --test-dir build` |
| **Estimated runtime** | ~30 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build && ./build/chromatindb_tests -t "[loadgen]"`
- **After every plan wave:** Run `cmake --build build && ctest --test-dir build`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 28-01-01 | 01 | 1 | LOAD-01 | build | `cmake --build build` (loadgen target compiles) | No - W0 | ⬜ pending |
| 28-01-02 | 01 | 1 | LOAD-01 | integration | Start node, run loadgen `--count 5`, verify 5 blobs ingested | No - manual | ⬜ pending |
| 28-01-03 | 01 | 1 | LOAD-02 | design | Inspect: sends via steady_timer, latency from scheduled time | No - code review | ⬜ pending |
| 28-01-04 | 01 | 1 | LOAD-03 | integration | Run loadgen `--mixed --count 100`, verify size distribution in JSON | No - manual | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `loadgen/loadgen_main.cpp` — the loadgen source file (new)
- [ ] CMakeLists.txt update — add `chromatindb_loadgen` target
- [ ] Dockerfile update — build and copy `chromatindb_loadgen` binary

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Loadgen connects, handshakes, sends accepted blobs | LOAD-01 | E2E integration requires running daemon | Start node, run `chromatindb_loadgen --target 127.0.0.1:4200 --count 5`, check node logs for 5 ingested blobs |
| Timer-driven scheduling (no coordinated omission) | LOAD-02 | Design/code review verification | Inspect code: sends scheduled by steady_timer at fixed intervals, latency measured from scheduled send time |
| Mixed-size blob distribution | LOAD-03 | Output inspection | Run `chromatindb_loadgen --target 127.0.0.1:4200 --mixed --count 100`, verify JSON output shows small/medium/large distribution |
| JSON stats output (blobs/sec, MiB/sec, p50/p95/p99) | LOAD-03 | Output format verification | Run loadgen, pipe stdout to `jq`, verify all required fields present |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
