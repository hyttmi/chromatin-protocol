---
phase: 71-transport-pq-handshake
verified: 2026-03-29T13:00:00Z
status: passed
score: 18/18 must-haves verified
re_verification: false
---

# Phase 71: Transport PQ Handshake Verification Report

**Phase Goal:** Transport layer: TCP framing, PQ handshake (ML-KEM-1024 + ML-DSA-87), AEAD session, ChromatinClient context manager
**Verified:** 2026-03-29
**Status:** passed
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

All truths are consolidated across the three plans (71-01, 71-02, 71-03).

| #  | Truth | Status | Evidence |
|----|-------|--------|----------|
| 1  | make_nonce(0) produces 12 bytes: 4 zero bytes + 8-byte BE counter | VERIFIED | `_framing.py:38` — `b"\x00\x00\x00\x00" + struct.pack(">Q", counter)`. Test: `TestMakeNonce::test_nonce_zero` passes |
| 2  | send_raw writes [4-byte BE length][data] and drains | VERIFIED | `_framing.py:48-49` — `writer.write(struct.pack(">I", len(data)) + data)` + `await writer.drain()`. Tested |
| 3  | recv_raw reads [4-byte BE length] then readexactly(length) bytes | VERIFIED | `_framing.py:69-88` — reads header then payload. Tested |
| 4  | recv_raw rejects frames exceeding MAX_FRAME_SIZE | VERIFIED | `_framing.py:78-80` raises `ProtocolError`. MAX_FRAME_SIZE = 115343360. Tested |
| 5  | send_encrypted encrypts plaintext with AEAD, sends via send_raw, returns incremented counter | VERIFIED | `_framing.py:110-113` — aead_encrypt + send_raw + `return counter + 1`. Tested |
| 6  | recv_encrypted reads via recv_raw, decrypts with AEAD, returns plaintext and incremented counter | VERIFIED | `_framing.py:137-144` — recv_raw + aead_decrypt + `return plaintext, counter + 1`. Tested |
| 7  | AEAD uses empty associated data (b'') | VERIFIED | `_framing.py:111,138` — `aead_encrypt(plaintext, b"", ...)` and `aead_decrypt(ciphertext, b"", ...)` |
| 8  | Handshake initiator sends KemPubkey([kem_pk:1568][sig_pk:2592]) as first raw message | VERIFIED | `_handshake.py:130-132` — `payload = kem_pk + identity.public_key` (1568+2592 bytes). Tested via `test_sends_kem_pubkey_first` |
| 9  | Handshake initiator receives KemCiphertext and decapsulates shared secret | VERIFIED | `_handshake.py:135-148` — decodes KemCiphertext, calls `kem.decap_secret(kem_ct)`. Tested |
| 10 | Session keys derived via HKDF with empty salt, contexts 'chromatin-init-to-resp-v1' and 'chromatin-resp-to-init-v1' | VERIFIED | `_handshake.py:100-103` — `hkdf_extract(b"", shared_secret)` then expand with both contexts. Tested |
| 11 | Session fingerprint = SHA3-256(shared_secret \|\| initiator_sig_pk \|\| responder_sig_pk) | VERIFIED | `_handshake.py:103` — `sha3_256(shared_secret + initiator_sig_pk + responder_sig_pk)`. Tested |
| 12 | Auth payload encodes as [4-byte LE pubkey_size][pubkey][signature] | VERIFIED | `_handshake.py:55` — `struct.pack("<I", len(signing_pubkey)) + signing_pubkey + signature`. Tested |
| 13 | Auth exchange uses transport counters starting at 0, leaving them at 1 post-handshake | VERIFIED | `_handshake.py:161,165` — `send_encrypted(... 0)` and `recv_encrypted(... 0)`. Returns `send_ctr=1, recv_ctr=1`. Tested |
| 14 | Background reader dispatches responses by request_id to pending futures | VERIFIED | `_transport.py:87-89` — `self._pending.pop(request_id).set_result(...)`. Tested |
| 15 | Background reader routes notifications (request_id=0) to asyncio.Queue | VERIFIED | `_transport.py:92-98` — `self._notifications.put_nowait(...)`. Tested |
| 16 | Background reader auto-responds to Ping with Pong | VERIFIED | `_transport.py:77-78` — `await self._send_pong()`. Tested |
| 17 | ChromatinClient.connect() returns async context manager with __aenter__/__aexit__ | VERIFIED | `client.py:62-113` — both methods implemented. __aenter__ does TCP + handshake, __aexit__ sends Goodbye + stop |
| 18 | Integration tests skip gracefully when relay is unreachable | VERIFIED | `test_integration.py:27-43` — `_relay_reachable()` TCP probe + `pytest.mark.skipif`. 4 tests collected |

**Score:** 18/18 truths verified

---

### Required Artifacts

| Artifact | Expected | Lines | Status | Details |
|----------|----------|-------|--------|---------|
| `sdk/python/chromatindb/_framing.py` | Frame IO and AEAD encryption layer | 144 | VERIFIED | Exports: make_nonce, send_raw, recv_raw, send_encrypted, recv_encrypted, MAX_FRAME_SIZE |
| `sdk/python/chromatindb/exceptions.py` | HandshakeError and ConnectionError subclasses | 76 | VERIFIED | `HandshakeError(ProtocolError)` at line 65, `ConnectionError(ProtocolError)` at line 69 |
| `sdk/python/tests/test_framing.py` | Unit tests for framing layer | 305 | VERIFIED | 20 test functions (min_lines: 80 met) |
| `sdk/python/chromatindb/_handshake.py` | PQ handshake initiator | 179 | VERIFIED | Exports: perform_handshake, encode_auth_payload, decode_auth_payload, derive_session_keys |
| `sdk/python/chromatindb/_transport.py` | Background reader, request dispatch | 233 | VERIFIED | Transport class with send_ping (FIFO deque), send_request, send_goodbye, stop |
| `sdk/python/chromatindb/client.py` | Public ChromatinClient API | 113 | VERIFIED | ChromatinClient.connect(), __aenter__, __aexit__, ping() |
| `sdk/python/tests/test_handshake.py` | Unit tests for handshake logic | 470 | VERIFIED | 11 test functions (min_lines: 60 met) |
| `sdk/python/tests/test_client.py` | Unit tests for client lifecycle | 414 | VERIFIED | 10 test functions (min_lines: 40 met) |
| `sdk/python/tests/test_integration.py` | End-to-end integration tests | 82 | VERIFIED | 4 test functions, pytestmark with integration + skipif |

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `_framing.py` | `crypto.py` | `aead_encrypt, aead_decrypt` import | WIRED | `_framing.py:18` — `from chromatindb.crypto import aead_decrypt, aead_encrypt` |
| `_handshake.py` | `_framing.py` | `send_raw, recv_raw, send_encrypted, recv_encrypted` | WIRED | `_handshake.py:25` — `from chromatindb._framing import recv_encrypted, recv_raw, send_encrypted, send_raw` |
| `_handshake.py` | `crypto.py` | `sha3_256` for session fingerprint | WIRED | `_handshake.py:27` — `from chromatindb.crypto import sha3_256` |
| `_handshake.py` | `_hkdf.py` | `hkdf_extract, hkdf_expand` for session keys | WIRED | `_handshake.py:26` — `from chromatindb._hkdf import hkdf_expand, hkdf_extract` |
| `_transport.py` | `_framing.py` | `send_encrypted, recv_encrypted` for frame IO | WIRED | `_transport.py:14` — `from chromatindb._framing import recv_encrypted, send_encrypted` |
| `client.py` | `_handshake.py` | `perform_handshake` call during connect | WIRED | `client.py:16` — `from chromatindb._handshake import perform_handshake`; called at `client.py:75` |
| `client.py` | `_transport.py` | `Transport` class for reader/dispatch | WIRED | `client.py:17` — `from chromatindb._transport import Transport`; instantiated at `client.py:90` |
| `test_integration.py` | `client.py` | `ChromatinClient.connect()` context manager | WIRED | `test_integration.py:19,49` — `from chromatindb import ChromatinClient` + `async with ChromatinClient.connect(` |
| `__init__.py` | `client.py`, `exceptions.py` | Package-level exports | WIRED | `__init__.py:6,17-31` — ChromatinClient, HandshakeError, ConnectionError in `__all__` |

---

### Data-Flow Trace (Level 4)

Not applicable — all Phase 71 artifacts are transport/connection layer code (not data-rendering components). Data flows through the crypto pipeline: plaintext → AEAD encrypt → wire → AEAD decrypt → plaintext. This pipeline was verified by the roundtrip tests and the full handshake simulation test.

---

### Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| All 107 unit tests pass | `.venv/bin/python3 -m pytest tests/ -x -q --ignore=tests/test_integration.py` | 107 passed, 0 failed | PASS |
| Phase 71 specific tests pass | `.venv/bin/python3 -m pytest test_framing.py test_handshake.py test_client.py -v` | 41 passed | PASS |
| Package imports correctly | `python3 -c "from chromatindb import ChromatinClient, HandshakeError, ConnectionError; print('OK')"` | OK | PASS |
| Integration tests collect | `.venv/bin/python3 -m pytest tests/test_integration.py --co -q` | 4 tests collected | PASS |

---

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| XPORT-02 | 71-02, 71-03 | SDK performs ML-KEM-1024 key exchange with relay (PQ handshake initiator) | SATISFIED | `_handshake.py` performs 4-step KEM exchange. `test_completes_4step_exchange` + `test_handshake_connect_disconnect` (integration) |
| XPORT-03 | 71-02, 71-03 | SDK performs mutual ML-DSA-87 authentication during handshake | SATISFIED | `_handshake.py:157-177` — sign fingerprint, encode/decode auth payload, verify peer sig. `test_raises_on_invalid_peer_auth` |
| XPORT-04 | 71-01, 71-03 | SDK encrypts/decrypts all post-handshake frames with ChaCha20-Poly1305 AEAD | SATISFIED | `_framing.py` send/recv_encrypted, `_transport.py` reader loop. `test_full_send_recv_roundtrip` + `test_ping_pong` (integration) |
| XPORT-05 | 71-01, 71-03 | SDK maintains correct per-direction AEAD nonce counters | SATISFIED | `_framing.py` counter increments per call; `_transport.py` tracks `_send_counter`/`_recv_counter` independently. `test_recv_encrypted_decrypts_and_increments_counter` |
| XPORT-07 | 71-02, 71-03 | SDK supports connection lifecycle (connect, disconnect via Goodbye, context manager) | SATISFIED | `client.py` — `connect()` classmethod, `__aenter__` with timeout, `__aexit__` sends Goodbye + stops transport. `test_aexit_sends_goodbye` + `test_handshake_connect_disconnect` (integration) |

**All 5 requirements satisfied. No orphaned requirements detected for Phase 71.**

---

### Anti-Patterns Found

No blocking or warning-level anti-patterns found.

The following `pass` statements in `_transport.py` are legitimate:
- Line 98: `pass` in `except asyncio.QueueFull` — intentionally drops notification overflow (documented)
- Line 205: `pass` in `except Exception` in `send_goodbye` — best-effort send on disconnect (documented)
- Lines 215, 223: `pass` in `except Exception` in `stop()` — cleanup error suppression (expected for teardown)

These are not stubs — they are documented, intentional exception handling in teardown/best-effort paths.

---

### Notable Deviations Fixed During Execution

Three bugs were found and fixed during Plan 03 human verification against the live relay — these strengthen confidence that the implementation is correct:

1. **Wrong default port** — SDK hardcoded 4433, relay default is 4201. Fixed in `conftest.py` and `pyproject.toml`.
2. **Ping hang** — `send_request()` expected relay to echo `request_id`, but relay sends Pong with `request_id=0`. Fixed with dedicated `Transport.send_ping()` using `_pending_pings` FIFO deque.
3. **Timeout scope** — `asyncio.open_connection` to non-routable IP was not covered by timeout. Fixed by wrapping TCP connect in separate `wait_for`.

All three fixes are committed and tested.

---

### Human Verification Required

#### 1. Live relay integration tests

**Test:** `cd sdk/python && .venv/bin/python3 -m pytest tests/test_integration.py -x -v` when KVM relay at 192.168.1.200:4201 is online
**Expected:** All 4 integration tests PASSED — handshake completes, Ping/Pong works, clean Goodbye, timeout raises HandshakeError
**Why human:** Requires a live C++ relay on KVM swarm. Cannot run in automated context.

**Note:** Per the 71-03-SUMMARY.md self-check, human verification was already performed during Plan 03 execution and all tests passed. This item is recorded for completeness.

---

### Gaps Summary

No gaps. All 18 observable truths verified. All 9 artifacts exist and are substantive. All 9 key links wired. All 5 requirements satisfied. 107 unit tests pass. Integration tests collect correctly and skip gracefully when relay is offline.

---

_Verified: 2026-03-29_
_Verifier: Claude (gsd-verifier)_
