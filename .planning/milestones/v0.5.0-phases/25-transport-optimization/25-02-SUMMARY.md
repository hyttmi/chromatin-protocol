---
phase: 25-transport-optimization
plan: 02
subsystem: net, handshake
tags: [asio, hkdf, chacha20, lightweight-handshake, trust-check]

requires:
  - phase: 25-transport-optimization
    plan: 01
    provides: trusted_peers config, TrustedHello/PQRequired transport types, is_trusted_address
provides:
  - derive_lightweight_session_keys for HKDF-based key derivation without ML-KEM
  - Branching do_handshake with lightweight/PQ/mismatch fallback paths
  - TrustCheck function wiring from PeerManager through Server to Connection
  - Integration tests for all three handshake paths
affects: [26-documentation]

tech-stack:
  added: []
  patterns:
    - "Initiator-first handshake: initiator sends first message, responder reads and branches"
    - "Mismatch fallback: PQRequired message forces PQ upgrade without connection failure"
    - "TrustCheck lambda chain: PeerManager -> Server -> Connection"

key-files:
  created: []
  modified:
    - db/net/handshake.h
    - db/net/handshake.cpp
    - db/net/connection.h
    - db/net/connection.cpp
    - db/net/server.h
    - db/net/server.cpp
    - tests/net/test_handshake.cpp
    - tests/net/test_connection.cpp

key-decisions:
  - "Initiator-first protocol: initiator sends TrustedHello or KemPubkey, responder reads first message and branches — avoids deadlock"
  - "Mismatch sends PQRequired (empty payload), initiator falls back to full PQ — no connection failure on trust disagreement"
  - "HKDF IKM = nonces (64 bytes), Salt = signing pubkeys (5184 bytes) — same HKDF context strings as PQ path for consistency"
  - "5 sub-methods instead of inline branching: do_handshake_initiator_trusted, do_handshake_initiator_pq, do_handshake_responder_trusted, do_handshake_responder_pq_fallback, do_handshake_responder_pq"

patterns-established:
  - "Handshake branching: responder reads first message type to determine protocol path"
  - "Trust mismatch fallback: PQRequired response triggers PQ upgrade transparently"

requirements-completed: [TOPT-01, TOPT-02]

duration: 19min
completed: 2026-03-15
---

# Phase 25 Plan 02: Lightweight Handshake Protocol Summary

**Branching do_handshake with HKDF-derived session keys for trusted peers, PQ fallback for untrusted, and mismatch recovery via PQRequired**

## Performance

- **Duration:** 19 min
- **Started:** 2026-03-15T04:38:33Z
- **Completed:** 2026-03-15T04:57:10Z
- **Tasks:** 2
- **Files modified:** 8

## Accomplishments
- derive_lightweight_session_keys produces symmetric directional keys via HKDF (nonces as IKM, signing pubkeys as salt)
- do_handshake refactored into 5 sub-methods handling all trust combinations
- Lightweight handshake exchanges TrustedHello messages (nonce + signing_pk), derives session keys without ML-KEM
- Mismatch fallback: responder sends PQRequired, initiator generates KEM keypair and continues PQ handshake
- TrustCheck function wired from PeerManager through Server to every Connection instance
- 3 new handshake unit tests + 3 integration tests (lightweight, PQ regression, mismatch), all passing
- 313 total tests, 1191 assertions, zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Implement derive_lightweight_session_keys and trust-check wiring** - `b9479ed` (feat, TDD)
2. **Task 2: Implement lightweight handshake path with mismatch fallback** - `5a138ee` (feat)

## Files Created/Modified
- `db/net/handshake.h` - Added derive_lightweight_session_keys declaration
- `db/net/handshake.cpp` - Implemented HKDF key derivation with nonces as IKM and signing pubkeys as salt
- `db/net/connection.h` - Added TrustCheck type, setter, and 5 handshake sub-method declarations
- `db/net/connection.cpp` - Refactored do_handshake into branching logic with lightweight/PQ/mismatch paths
- `db/net/server.h` - Added TrustCheck type, setter, and trust_check_ member
- `db/net/server.cpp` - Pass trust_check_ to all Connection instances (accept, connect, reconnect, connect_once)
- `tests/net/test_handshake.cpp` - Added 3 tests: symmetric directional keys, different nonces, deterministic
- `tests/net/test_connection.cpp` - Added 3 integration tests: lightweight loopback, PQ regression, mismatch fallback

## Decisions Made
- Initiator-first protocol avoids deadlock (initiator sends first, responder reads and branches)
- PQRequired has empty payload — message type alone signals the fallback
- HKDF uses same context strings ("chromatin-init-to-resp-v1", "chromatin-resp-to-init-v1") as PQ path
- 5 sub-methods keep each handshake path clean and testable

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 25 complete: all transport optimization requirements (TOPT-01, TOPT-02, TOPT-03) satisfied
- Ready for Phase 26: Documentation & Release

---
*Phase: 25-transport-optimization*
*Completed: 2026-03-15*
