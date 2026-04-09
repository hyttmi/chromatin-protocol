---
phase: 97-protocol-crypto-safety
plan: 02
subsystem: crypto, protocol, networking
tags: [aead, nonce, pubkey-validation, flatbuffers, ml-dsa-87, chacha20-poly1305]

requires:
  - phase: 95-shared-utilities
    provides: "auth_helpers.h, endian.h shared utility headers"
provides:
  - "Exact pubkey size validation in decode_auth_payload and decode_blob"
  - "AEAD AD length bound (64 KiB MAX_AD_LENGTH)"
  - "Nonce exhaustion kill at 2^63 in send_encrypted/recv_encrypted"
  - "CRYPTO-02 pubkey binding test documenting existing invariant"
affects: [97-protocol-crypto-safety, connection, handshake, codec]

tech-stack:
  added: []
  patterns:
    - "Step 0 cheap validation before expensive crypto (pubkey size check before sig verify)"
    - "Defense-in-depth bounds on AEAD associated data"
    - "Pre-increment nonce check prevents counter overflow"

key-files:
  created: []
  modified:
    - db/net/auth_helpers.h
    - db/wire/codec.cpp
    - db/crypto/aead.h
    - db/crypto/aead.cpp
    - db/net/connection.h
    - db/net/connection.cpp
    - db/tests/net/test_auth_helpers.cpp
    - db/tests/wire/test_codec.cpp
    - db/tests/crypto/test_aead.cpp
    - db/tests/net/test_connection.cpp
    - db/tests/net/test_handshake.cpp

key-decisions:
  - "Pubkey size validated against exact Signer::PUBLIC_KEY_SIZE (2592) constant"
  - "AEAD MAX_AD_LENGTH set to 65536 (64 KiB) as defense-in-depth bound"
  - "Nonce exhaustion threshold at 2^63 (half of uint64 max) with clean close"
  - "test-only set_send/recv_counter_for_test on Connection for nonce exhaustion testing"

patterns-established:
  - "Exact constant validation: use Signer::PUBLIC_KEY_SIZE not magic numbers"
  - "Pre-increment safety checks: validate before mutating counters"

requirements-completed: [PROTO-02, PROTO-03, PROTO-04, CRYPTO-01, CRYPTO-02]

duration: 30min
completed: 2026-04-08
---

# Phase 97 Plan 02: Protocol & Crypto Safety - Validation and Bounds Summary

**Pubkey size validation in auth/codec, AEAD AD 64 KiB bound, nonce exhaustion kill at 2^63, PQ handshake pubkey binding test**

## Performance

- **Duration:** 30 min
- **Started:** 2026-04-08T10:14:05Z
- **Completed:** 2026-04-08T10:44:44Z
- **Tasks:** 3
- **Files modified:** 11

## Accomplishments
- decode_auth_payload now rejects any pubkey size != 2592 (ML-DSA-87 PUBLIC_KEY_SIZE) as Step 0 cheap check before expensive sig verify
- decode_blob throws runtime_error on wrong pubkey size in FlatBuffer blobs, catching malformed wire data early
- AEAD encrypt/decrypt enforce MAX_AD_LENGTH (64 KiB) defense-in-depth bound with runtime_error
- send_encrypted/recv_encrypted kill connection cleanly when nonce counter >= 2^63 with error log
- CRYPTO-02 pubkey binding invariant documented by test: attacker's valid signature rejected because pubkey != initiator_signing_pubkey_

## Task Commits

Each task was committed atomically:

1. **Task 1: Add pubkey size validation to auth_helpers.h and codec.cpp with tests** - `2b9d750` (feat)
2. **Task 2: Add AEAD AD bounds and nonce exhaustion checks with tests** - `6b3c68e` (feat)
3. **Task 3: Add CRYPTO-02 pubkey binding verification test** - `cff51b7` (test)

## Files Created/Modified
- `db/net/auth_helpers.h` - Added signing.h include, exact pubkey size check in decode_auth_payload
- `db/wire/codec.cpp` - Added signing.h include, pubkey size validation in decode_blob
- `db/crypto/aead.h` - Added MAX_AD_LENGTH constant (65536)
- `db/crypto/aead.cpp` - Added AD length guard in encrypt and decrypt functions
- `db/net/connection.h` - Added set_send/recv_counter_for_test methods
- `db/net/connection.cpp` - Added NONCE_LIMIT pre-check in send_encrypted and recv_encrypted
- `db/tests/net/test_auth_helpers.cpp` - Updated existing tests for new validation, added rejection/acceptance tests
- `db/tests/wire/test_codec.cpp` - Updated existing tests for new validation, added wrong-size rejection test
- `db/tests/crypto/test_aead.cpp` - Added oversized AD rejection tests and exact-max acceptance test
- `db/tests/net/test_connection.cpp` - Added nonce exhaustion integration test with forced counter
- `db/tests/net/test_handshake.cpp` - Added CRYPTO-02 pubkey binding verification test

## Decisions Made
- Pubkey size check uses `Signer::PUBLIC_KEY_SIZE` constant (not magic 2592) for maintainability
- NONCE_LIMIT defined as `static constexpr` local to each function (not header constant) -- avoids polluting API
- Test-only counter setters are public methods (not ifdef-gated) -- follows existing codebase pattern of test helpers, harmless in production
- Existing tests updated to use correct ML-DSA-87 key sizes (2592 bytes) instead of arbitrary sizes

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Updated existing tests broken by new validation**
- **Found during:** Task 1
- **Issue:** Existing test_auth_helpers.cpp round-trip test used 64-byte pubkey (now rejected), pubkey_size=0 test expected success (now rejected), test_codec.cpp "empty data" test used 3-byte pubkey (now throws)
- **Fix:** Updated round-trip test to use 2592-byte pubkey, changed pubkey_size=0 test to expect nullopt, fixed codec empty-data test to use 2592-byte pubkey, fixed ForceDefaults test to include valid pubkey
- **Files modified:** db/tests/net/test_auth_helpers.cpp, db/tests/wire/test_codec.cpp
- **Verification:** All 59 test cases across modified areas pass (197 assertions)
- **Committed in:** 2b9d750 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug fix for existing tests)
**Impact on plan:** Necessary correctness update -- existing tests used invalid pubkey sizes that the new validation correctly rejects.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Known Stubs
None

## Next Phase Readiness
- All 5 requirements (PROTO-02, PROTO-03, PROTO-04, CRYPTO-01, CRYPTO-02) implemented and tested
- Ready for Plan 03 (remaining requirements)
- No blockers or concerns

---
*Phase: 97-protocol-crypto-safety*
*Completed: 2026-04-08*
