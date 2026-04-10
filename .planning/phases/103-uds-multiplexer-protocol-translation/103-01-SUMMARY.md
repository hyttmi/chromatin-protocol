---
phase: 103-uds-multiplexer-protocol-translation
plan: 01
subsystem: relay
tags: [flatbuffers, aead, chacha20-poly1305, hkdf, base64, hex, transport-codec, request-routing]

requires:
  - phase: 102-authentication-json-schema
    provides: "Type registry, JSON schema metadata, message filter, WsSession auth state machine"
provides:
  - "AEAD encrypt/decrypt + HKDF key derivation via OpenSSL (relay/wire/aead.h)"
  - "TransportMessage FlatBuffer encode/decode (relay/wire/transport_codec.h)"
  - "Request-ID multiplexing and response routing (relay/core/request_router.h)"
  - "Shared hex, base64, endian utilities (relay/util/)"
  - "Copied FlatBuffers generated headers (relay/wire/transport_generated.h, blob_generated.h)"
affects: [103-02, 104-subscription-notification-fanout]

tech-stack:
  added: [flatbuffers-v25.2.10]
  patterns: [RAII EVP wrappers for OpenSSL, counter-based AEAD nonce, relay-scoped request_id multiplexing]

key-files:
  created:
    - relay/util/hex.h
    - relay/util/endian.h
    - relay/util/base64.h
    - relay/util/base64.cpp
    - relay/wire/transport_generated.h
    - relay/wire/blob_generated.h
    - relay/wire/transport_codec.h
    - relay/wire/transport_codec.cpp
    - relay/wire/aead.h
    - relay/wire/aead.cpp
    - relay/core/request_router.h
    - relay/core/request_router.cpp
    - relay/tests/test_base64.cpp
    - relay/tests/test_transport_codec.cpp
    - relay/tests/test_aead.cpp
    - relay/tests/test_request_router.cpp
  modified:
    - relay/CMakeLists.txt
    - relay/tests/CMakeLists.txt

key-decisions:
  - "RAII wrappers (CipherCtx, PKeyCtx) for OpenSSL EVP contexts -- no manual cleanup paths"
  - "RequestRouter.set_next_relay_rid() public method for test access instead of friend class"
  - "Empty HKDF salt handled with non-null dummy pointer -- OpenSSL EVP requires non-null even for 0-length salt"

patterns-established:
  - "relay/util/ namespace chromatindb::relay::util for shared header-only and compiled utilities"
  - "relay/wire/ namespace chromatindb::relay::wire for transport-level codec and crypto"
  - "FlatBuffers FetchContent with FLATBUFFERS_BUILD_TESTS/INSTALL/FLATC OFF"

requirements-completed: [MUX-02]

duration: 16min
completed: 2026-04-10
---

# Phase 103 Plan 01: Wire Infrastructure & Request Router Summary

**Stateless wire building blocks: ChaCha20-Poly1305 AEAD via OpenSSL, FlatBuffer TransportCodec, request-ID multiplexer, hex/base64/endian utilities -- 33 new tests (139 assertions)**

## Performance

- **Duration:** 16 min
- **Started:** 2026-04-10T02:38:51Z
- **Completed:** 2026-04-10T02:54:59Z
- **Tasks:** 2
- **Files created:** 16
- **Files modified:** 2

## Accomplishments
- AEAD encrypt/decrypt with ChaCha20-Poly1305 and HKDF-SHA256 extract/expand via OpenSSL EVP, with RAII context wrappers
- TransportCodec for FlatBuffer TransportMessage encode/decode, exact reimplementation of db/net/protocol.h for relay isolation
- RequestRouter for relay-scoped request_id assignment (starts at 1, wraps UINT32_MAX to 1 skipping 0), response resolution, client disconnect cleanup, and stale entry purge
- Shared utilities: hex encode/decode (header-only), base64 via OpenSSL EVP_EncodeBlock/EVP_DecodeBlock, big-endian read/write helpers
- FlatBuffers dependency added to relay CMake with FetchContent (v25.2.10)
- All 126 relay tests pass (908 assertions) -- 93 existing + 33 new

## Task Commits

Each task was committed atomically:

1. **Task 1: Shared utilities, AEAD crypto, and FlatBuffers wire layer** - `08b27cb` (feat)
2. **Task 2: RequestRouter with request_id multiplexing** - `225a12f` (feat)

## Files Created/Modified
- `relay/util/hex.h` - Shared to_hex/from_hex in chromatindb::relay::util namespace
- `relay/util/endian.h` - Big-endian read/write helpers (u16/u32/u64)
- `relay/util/base64.h` - Base64 encode/decode declarations
- `relay/util/base64.cpp` - Base64 via OpenSSL EVP_EncodeBlock/EVP_DecodeBlock
- `relay/wire/transport_generated.h` - Copied FlatBuffers generated header
- `relay/wire/blob_generated.h` - Copied FlatBuffers generated header
- `relay/wire/transport_codec.h` - DecodedMessage struct + TransportCodec encode/decode
- `relay/wire/transport_codec.cpp` - FlatBufferBuilder encode, Verifier + GetTransportMessage decode
- `relay/wire/aead.h` - AEAD constants, make_nonce, aead_encrypt/decrypt, hkdf_extract/expand
- `relay/wire/aead.cpp` - ChaCha20-Poly1305 via EVP, HKDF via EVP_PKEY_HKDF, RAII wrappers
- `relay/core/request_router.h` - PendingRequest struct + RequestRouter class
- `relay/core/request_router.cpp` - register/resolve/remove/purge with C++20 std::erase_if
- `relay/tests/test_base64.cpp` - 6 test cases (RFC 4648 vectors, roundtrips, edge cases)
- `relay/tests/test_transport_codec.cpp` - 8 test cases (roundtrip, invalid, large payload)
- `relay/tests/test_aead.cpp` - 10 test cases (AEAD roundtrip, wrong key/counter, HKDF)
- `relay/tests/test_request_router.cpp` - 9 test cases (register, resolve, remove, wrap, purge)
- `relay/CMakeLists.txt` - Added FlatBuffers FetchContent, new sources, flatbuffers link
- `relay/tests/CMakeLists.txt` - Added 4 new test files

## Decisions Made
- Used RAII wrappers (CipherCtx, PKeyCtx) for OpenSSL EVP contexts to guarantee cleanup on all code paths
- Made RequestRouter.set_next_relay_rid() a public method instead of using a friend test class -- simpler, matches the project's testing patterns
- Handle empty HKDF salt with a non-null dummy pointer since OpenSSL EVP_PKEY_CTX_set1_hkdf_salt rejects null pointer even with length 0

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed HKDF empty salt handling**
- **Found during:** Task 1 (AEAD crypto)
- **Issue:** OpenSSL EVP_PKEY_CTX_set1_hkdf_salt fails when passed null pointer even with salt_len=0, but the protocol uses empty HKDF salt
- **Fix:** Pass a dummy non-null byte pointer with length 0 when salt span is empty
- **Files modified:** relay/wire/aead.cpp
- **Verification:** HKDF extract with empty salt test passes
- **Committed in:** 08b27cb (Task 1 commit)

**2. [Rule 1 - Bug] Fixed FlatBuffers unscoped enum usage in tests**
- **Found during:** Task 1 (test compilation)
- **Issue:** Tests used `TransportMsgType::Ping` syntax (scoped enum) but FlatBuffers generates unscoped enum with `TransportMsgType_Ping` naming
- **Fix:** Changed to `TransportMsgType_Ping` etc. with `using namespace chromatindb::wire`
- **Files modified:** relay/tests/test_transport_codec.cpp
- **Verification:** All transport_codec tests compile and pass
- **Committed in:** 08b27cb (Task 1 commit)

---

**Total deviations:** 2 auto-fixed (2 bugs)
**Impact on plan:** Both fixes necessary for correctness. No scope creep.

## Issues Encountered
- Worktree was behind master (missing Phases 101-102). Resolved with fast-forward merge before starting tasks.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All wire-level building blocks ready for Plan 02: UDS multiplexer, translator, and WsSession integration
- TransportCodec for FlatBuffer envelope encoding needed by UDS client
- AEAD crypto ready for future encrypted UDS path (currently trusted/unencrypted per D-03)
- RequestRouter ready for request_id rewriting in the UDS forwarding pipeline
- hex/base64/endian utilities ready for the JSON<->binary translator

## Self-Check: PASSED

- All 16 created files verified present on disk
- Both task commits (08b27cb, 225a12f) verified in git log
- All 126 relay tests pass (908 assertions)

---
*Phase: 103-uds-multiplexer-protocol-translation*
*Completed: 2026-04-10*
