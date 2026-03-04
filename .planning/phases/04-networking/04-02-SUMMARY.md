---
phase: 04-networking
plan: 02
status: complete
started: "2026-03-04"
completed: "2026-03-04"
duration: ~10min
---

# Plan 04-02: PQ Handshake Protocol

## What Was Built
Implemented the PQ handshake protocol with ML-KEM-1024 key exchange, HKDF-SHA256 directional session key derivation, and ML-DSA-87 mutual authentication. Fully testable in memory without TCP sockets.

## Key Files

### Created
- `src/net/handshake.h` -- SessionKeys, derive_session_keys, HandshakeInitiator, HandshakeResponder
- `src/net/handshake.cpp` -- Full implementation of handshake state machines
- `tests/net/test_handshake.cpp` -- 5 test cases covering key derivation, full handshake, self-handshake, auth failure, invalid messages

### Modified
- `CMakeLists.txt` -- Added handshake.cpp and test_handshake.cpp

## Protocol Design
- KemPubkey payload: [kem_pubkey (1568B)][signing_pubkey (2592B)]
- KemCiphertext payload: [ciphertext (1568B)][signing_pubkey (2592B)]
- Auth payload: [4B LE pubkey_size][pubkey][signature] -- encrypted with session keys
- Session fingerprint: SHA3-256(shared_secret || initiator_signing_pk || responder_signing_pk)
- HKDF context strings: "chromatin-init-to-resp-v1", "chromatin-resp-to-init-v1"

## Decisions Made
- Both sides include signing pubkey in KEM exchange messages (needed for fingerprint before auth)
- Responder verifies initiator's auth pubkey matches the one sent during KEM exchange
- Auth payload uses simple length-prefix format rather than FlatBuffers (keeps it flat)
- Handshake self-test supported (same identity on both sides)

## Test Results
- 5 new test cases, 35 assertions -- all passing
- 120 total test cases, 479 total assertions -- no regressions

## Self-Check: PASSED
- [x] Key derivation produces directional symmetry
- [x] Session fingerprint identical on both sides
- [x] Full handshake completes in memory
- [x] Invalid signatures rejected
- [x] Invalid message types rejected
- [x] All existing tests pass
