---
phase: 71-transport-pq-handshake
plan: 01
subsystem: transport
tags: [asyncio, aead, chacha20-poly1305, framing, nonce, python-sdk]

requires:
  - phase: 70-crypto-foundation-identity
    provides: aead_encrypt/aead_decrypt, exception hierarchy, crypto constants

provides:
  - "_framing.py: make_nonce, send_raw, recv_raw, send_encrypted, recv_encrypted, MAX_FRAME_SIZE"
  - "HandshakeError and ConnectionError exception subclasses"
  - "pytest-asyncio configured with integration marker"

affects: [71-02-PLAN, 71-03-PLAN, 72-data-operations]

tech-stack:
  added: [pytest-asyncio]
  patterns: [counter-based-nonce, length-prefixed-framing, mock-stream-testing]

key-files:
  created:
    - sdk/python/chromatindb/_framing.py
    - sdk/python/tests/test_framing.py
  modified:
    - sdk/python/chromatindb/exceptions.py
    - sdk/python/pyproject.toml
    - .gitignore

key-decisions:
  - "ChromatinConnectionError alias avoids shadowing builtin ConnectionError"
  - "pytest asyncio_mode=auto eliminates per-test @pytest.mark.asyncio decorators"

patterns-established:
  - "MockStreamWriter/MockStreamReader for unit testing async frame IO without network"
  - "Counter-based nonce: [4 zero bytes][8-byte BE counter] matching C++ framing.cpp"

requirements-completed: [XPORT-04, XPORT-05]

duration: 3min
completed: 2026-03-29
---

# Phase 71 Plan 01: Encrypted Frame IO Summary

**Length-prefixed frame IO with ChaCha20-Poly1305 AEAD encryption, counter-based nonce management, and 20 unit tests**

## Performance

- **Duration:** 3 min
- **Started:** 2026-03-29T12:15:53Z
- **Completed:** 2026-03-29T12:19:06Z
- **Tasks:** 1 (TDD)
- **Files modified:** 5

## Accomplishments

- Implemented _framing.py with 6 exports: make_nonce, send_raw, recv_raw, send_encrypted, recv_encrypted, MAX_FRAME_SIZE
- Extended exception hierarchy with HandshakeError and ConnectionError subclasses of ProtocolError
- Created 20 unit tests covering nonce construction, raw frame IO, encrypted frame IO, roundtrips, and error handling
- Configured pytest-asyncio with auto mode and integration test markers
- All 86 tests pass (66 existing Phase 70 + 20 new)

## Task Commits

Each task was committed atomically:

1. **Task 1: Exception subclasses and framing module with tests** - `25e1383` (feat)

## Files Created/Modified

- `sdk/python/chromatindb/_framing.py` - Encrypted frame IO layer: nonce construction, length-prefixed raw send/recv, AEAD-encrypted send/recv with counter management
- `sdk/python/tests/test_framing.py` - 20 unit tests: nonce values, frame encoding, oversized frame rejection, incomplete read handling, AEAD encrypt/decrypt roundtrip, wrong-key detection
- `sdk/python/chromatindb/exceptions.py` - Added HandshakeError(ProtocolError) and ConnectionError(ProtocolError) subclasses
- `sdk/python/pyproject.toml` - Added asyncio_mode=auto and integration test marker
- `.gitignore` - Fixed .venv pattern to match symlinks in worktrees

## Decisions Made

- Used `ChromatinConnectionError` as internal alias for `chromatindb.exceptions.ConnectionError` to avoid shadowing the Python builtin `ConnectionError` -- SDK users import explicitly from `chromatindb.exceptions`
- Set `asyncio_mode = "auto"` in pyproject.toml to avoid requiring `@pytest.mark.asyncio` on every async test function
- Used custom MockStreamWriter/MockStreamReader classes instead of unittest.mock.AsyncMock for clearer test intent and simpler assertion patterns

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Installed missing pytest-asyncio dependency**
- **Found during:** Task 1 (TDD RED phase)
- **Issue:** pytest-asyncio not installed in venv, async tests cannot run
- **Fix:** `pip install pytest-asyncio` in the SDK venv
- **Files modified:** None (runtime dependency only)
- **Verification:** All 20 async tests run and pass
- **Committed in:** 25e1383

**2. [Rule 3 - Blocking] Fixed .gitignore for worktree symlinked .venv**
- **Found during:** Task 1 (commit prep)
- **Issue:** `.gitignore` had `sdk/python/.venv/` (with trailing slash) which does not match symlinks
- **Fix:** Changed to `sdk/python/.venv` (without trailing slash)
- **Files modified:** .gitignore
- **Verification:** `git status` no longer shows .venv as untracked
- **Committed in:** 25e1383

---

**Total deviations:** 2 auto-fixed (2 blocking)
**Impact on plan:** Both fixes necessary for test execution and clean git state. No scope creep.

## Issues Encountered

None beyond the auto-fixed deviations above.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- _framing.py provides the foundation for 71-02 (PQ handshake state machine) which uses send_raw/recv_raw for KEM exchange and send_encrypted/recv_encrypted for auth exchange
- HandshakeError ready for use in _handshake.py
- ConnectionError ready for use in _transport.py background reader
- pytest-asyncio configured for all future async tests

## Self-Check: PASSED

- [x] sdk/python/chromatindb/_framing.py exists
- [x] sdk/python/tests/test_framing.py exists
- [x] sdk/python/chromatindb/exceptions.py exists
- [x] .planning/phases/71-transport-pq-handshake/71-01-SUMMARY.md exists
- [x] Commit 25e1383 exists in git log

---
*Phase: 71-transport-pq-handshake*
*Completed: 2026-03-29*
