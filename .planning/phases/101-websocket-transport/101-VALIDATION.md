---
phase: 101
slug: websocket-transport
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-04-09
---

# Phase 101 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Catch2 3.7.1 |
| **Config file** | `relay/tests/CMakeLists.txt` |
| **Quick run command** | `cd build && ctest --test-dir . -R relay -j4 --output-on-failure` |
| **Full suite command** | `cd build && ctest --test-dir . -R relay -j4 --output-on-failure` |
| **Estimated runtime** | ~5 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd build && cmake --build . && ctest --test-dir . -R relay -j4 --output-on-failure`
- **After every plan wave:** Run `cd build && cmake --build . && ctest --test-dir . -R relay -j4 --output-on-failure`
- **Before `/gsd:verify-work`:** Full suite must be green
- **Max feedback latency:** 15 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 101-01-XX | 01 | 1 | TRANS-03 | unit | `ctest -R test_ws_frame` | ❌ W0 | ⬜ pending |
| 101-01-XX | 01 | 1 | TRANS-03 | unit | `ctest -R test_ws_handshake` | ❌ W0 | ⬜ pending |
| 101-02-XX | 02 | 1 | TRANS-01 | integration (live) | Manual: `websocat wss://localhost:4201` | N/A | ⬜ pending |
| 101-02-XX | 02 | 1 | TRANS-02 | integration (live) | Manual: `websocat ws://localhost:4201` | N/A | ⬜ pending |
| 101-XX-XX | XX | X | TRANS-04 | unit/live | `ctest -R test_ws_acceptor` or live test | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `relay/tests/test_ws_frame.cpp` — stubs for TRANS-03 frame encode/decode, mask/unmask, length encoding, fragmentation, ping/pong, close frames
- [ ] `relay/tests/test_ws_handshake.cpp` — stubs for TRANS-03 upgrade parsing, Sec-WebSocket-Accept key computation, 426 response
- [ ] `relay/tests/CMakeLists.txt` update — add new test files, link `OpenSSL::Crypto` for SHA-1 test dependency

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| WSS connection accepted with TLS | TRANS-01 | Requires TLS cert/key and running relay | Start relay with cert/key config, connect with `websocat wss://localhost:4201` |
| Plain WS connection accepted | TRANS-02 | Requires running relay without TLS config | Start relay without cert/key, connect with `websocat ws://localhost:4201` |
| SIGHUP reloads TLS cert/key | TRANS-04 | Requires running relay + cert swap + SIGHUP signal | Start relay, replace cert, `kill -HUP`, reconnect with new cert |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 15s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
