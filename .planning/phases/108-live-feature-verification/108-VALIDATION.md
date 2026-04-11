---
phase: 108
slug: live-feature-verification
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-11
---

# Phase 108 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Standalone C++ binary (not Catch2 — same as relay_smoke_test) |
| **Config file** | `tools/CMakeLists.txt` (add relay_feature_test target) |
| **Quick run command** | `./build/tools/relay_feature_test --identity /tmp/chromatindb-test/keys/identity.key --relay-pid $RELAY_PID --config /tmp/chromatindb-test/relay.json` |
| **Full suite command** | `/tmp/chromatindb-test/run-smoke.sh` (runs smoke test + feature test) |
| **Estimated runtime** | ~15 seconds |

---

## Sampling Rate

- **After every task commit:** Build relay_feature_test and run individually with live relay
- **After every plan wave:** Full `/tmp/chromatindb-test/run-smoke.sh` run (smoke test + feature test)
- **Before `/gsd:verify-work`:** Full suite must be green, ASAN clean
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 108-01-01 | 01 | 1 | E2E-02,E2E-03,E2E-04,E2E-05 | E2E binary | `./build/tools/relay_feature_test --identity ... --relay-pid ... --config ...` | ❌ W0 | ⬜ pending |
| 108-01-02 | 01 | 1 | E2E-02,E2E-03,E2E-04,E2E-05 | Integration | `/tmp/chromatindb-test/run-smoke.sh` | ✅ (updated) | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `tools/relay_feature_test.cpp` — new binary covering E2E-02 through E2E-05
- [ ] `tools/relay_test_helpers.h` — shared helpers extracted from relay_smoke_test
- [ ] `tools/CMakeLists.txt` update — add relay_feature_test target
- [ ] `/tmp/chromatindb-test/run-smoke.sh` update — add relay_feature_test invocation with --relay-pid

*All test infrastructure is new for this phase.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| TLS cert reload via SIGHUP | E2E-04 | Test env uses plain WS, no TLS configured | Verified by code path sharing with rate limit reload (same handler). Unit tests cover TLS reload method. |
| ACL reload via SIGHUP | E2E-04 | No ACL configured in test env | Same code path as rate limit reload. Unit tests cover authenticator.reload_allowed_keys(). |
| metrics_bind reload via SIGHUP | E2E-04 | Requires metrics endpoint inspection | Same SIGHUP handler path. Rate limit change proves the handler fires. |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
