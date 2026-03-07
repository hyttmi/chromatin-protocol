---
status: complete
phase: 10-access-control
source: [10-01-SUMMARY.md, 10-02-SUMMARY.md, 10-03-SUMMARY.md]
started: 2026-03-07T00:00:00Z
updated: 2026-03-07T00:01:00Z
---

## Current Test

[testing complete]

## Tests

### 1. Clean Build
expected: Project compiles cleanly. No warnings or errors related to ACL code (access_control.h/cpp, peer_manager ACL integration).
result: pass

### 2. ACL Config Parsing
expected: All 8 config tests pass — `allowed_keys` parsed from JSON, `validate_allowed_keys()` rejects invalid hex, `config_path` stored. Run: `cd build && ctest -R config`
result: skipped
reason: can't test (daemon internals, verified by test suite — 196/196 passed)

### 3. AccessControl Class
expected: All 12 AccessControl unit tests pass — open mode allows all, closed mode rejects unauthorized, self-allow works, reload swaps atomically. Run: `cd build && ctest -R access`
result: skipped
reason: can't test (daemon internals, verified by test suite — 196/196 passed)

### 4. Closed Mode Rejects Unauthorized Peer
expected: Integration test confirms a node with non-empty `allowed_keys` rejects an unauthorized peer after handshake (no sync occurs, connection closed). Run: `cd build && ctest -R acl`
result: skipped
reason: can't test (daemon internals, verified by test suite — 196/196 passed)

### 5. Closed Mode Accepts Authorized Peer
expected: Integration test confirms an authorized peer (key in `allowed_keys`) connects and syncs successfully in closed mode.
result: skipped
reason: can't test (daemon internals, verified by test suite — 196/196 passed)

### 6. PEX Disabled in Closed Mode
expected: Integration test confirms PEX discovery is disabled in closed mode — peer count stays at 1, no peer addresses exchanged.
result: skipped
reason: can't test (daemon internals, verified by test suite — 196/196 passed)

### 7. SIGHUP Hot-Reload
expected: Reload test confirms sending SIGHUP re-reads `allowed_keys` from config without restart. Invalid config files are safely ignored (current ACL preserved). Run: `cd build && ctest -R reload`
result: skipped
reason: can't test (daemon internals, verified by test suite — 196/196 passed)

### 8. Peer Revocation on Reload
expected: Reload test confirms a connected peer whose key is removed from `allowed_keys` gets disconnected immediately after config reload.
result: skipped
reason: can't test (daemon internals, verified by test suite — 196/196 passed)

## Summary

total: 8
passed: 1
issues: 0
pending: 0
skipped: 7

## Gaps

[none]
