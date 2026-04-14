---
phase: 112
slug: asan-verification
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-14
---

# Phase 112 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Bash scripts + relay_benchmark.py (ASAN instrumented builds) |
| **Config file** | relay/lsan_suppressions.txt |
| **Quick run command** | `ASAN_OPTIONS=detect_leaks=0 ./build-asan/relay/chromatindb_relay --config relay/config/test.json &; python3 tools/relay_benchmark.py --concurrency 1 --iterations 5` |
| **Full suite command** | `bash tools/relay_asan_test.sh` |
| **Estimated runtime** | ~300 seconds |

---

## Sampling Rate

- **After every task commit:** Run quick ASAN smoke (1 client, 5 iterations)
- **After every plan wave:** Run full ASAN test suite (1/10/100 clients + signals)
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 300 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 112-01-01 | 01 | 1 | VER-02 | integration | `bash tools/relay_asan_test.sh --concurrency 1` | ❌ W0 | ⬜ pending |
| 112-01-02 | 01 | 1 | VER-02 | integration | `bash tools/relay_asan_test.sh --concurrency 10` | ❌ W0 | ⬜ pending |
| 112-01-03 | 01 | 1 | VER-02 | integration | `bash tools/relay_asan_test.sh --concurrency 100` | ❌ W0 | ⬜ pending |
| 112-02-01 | 02 | 1 | VER-03 | integration | `bash tools/relay_asan_test.sh --signal sighup` | ❌ W0 | ⬜ pending |
| 112-02-02 | 02 | 1 | VER-03 | integration | `bash tools/relay_asan_test.sh --signal sigterm` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `tools/relay_asan_test.sh` — orchestration script for ASAN test runs
- [ ] `relay/lsan_suppressions.txt` — LSAN suppression file for accepted shutdown leaks
- [ ] ASAN build available: `cmake -DSANITIZER=asan -B build-asan && cmake --build build-asan`

*Existing relay_benchmark.py covers load generation. New scripts needed for ASAN orchestration.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| TLS cert swap on SIGHUP | VER-03 | Requires TLS-enabled config + cert files | 1. Start ASAN relay with TLS. 2. Replace cert file. 3. Send SIGHUP. 4. Connect new client — verify new cert served. |

*TLS cert swap requires manual cert file management; all other behaviors are fully automated.*

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 300s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
