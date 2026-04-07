---
phase: 92-kem-key-versioning
verified: 2026-04-07T03:10:00Z
status: passed
score: 14/14 must-haves verified
re_verification: false
---

# Phase 92: KEM Key Versioning Verification Report

**Phase Goal:** Users can rotate their ML-KEM encryption keypair so a compromised key cannot decrypt future data, while old data remains readable with old keys
**Verified:** 2026-04-07T03:10:00Z
**Status:** passed
**Re-verification:** No -- initial verification

## Goal Achievement

### Observable Truths

| #  | Truth | Status | Evidence |
|----|-------|--------|---------|
| 1  | Identity.rotate_kem() generates a new ML-KEM-1024 keypair and retains the old keypair in the key ring | VERIFIED | identity.py:421-485, `self._kem_ring.append((new_version, new_pk, new_kem))` at line 483 |
| 2  | Identity.key_version returns the current (highest) version number, starting at 0 for fresh identities | VERIFIED | identity.py:404-408, returns `self._kem_ring[-1][0]` or 0 |
| 3  | Identity.save() writes numbered KEM files for all ring entries | VERIFIED | identity.py:339-347, writes `{stem}.kem.{version}` / `{stem}.kpub.{version}` for ring length > 1 or version > 0 |
| 4  | Identity.load() discovers numbered KEM files via glob and reconstructs full key ring | VERIFIED | identity.py:199-250, `parent.glob(f"{stem}.kem.[0-9]*")` with isdigit() filter |
| 5  | Pre-rotation identities (no numbered files) are treated as version 0 with ring of length 1 -- no files created on load | VERIFIED | identity.py:252-254, falls through to single-entry ring; save() guards with `len > 1 or version > 0` |
| 6  | First rotate_kem() on a pre-rotation identity writes kem.0.* (copy of original) + kem.1.* (new) + updates canonical kem.sec/kem.pub | VERIFIED | identity.py:450-480, checks `current_version == 0 and not v0_sec_path.exists()` |
| 7  | _build_kem_ring_map() returns {sha3_256(pk): kem_obj} for all ring entries with secret keys | VERIFIED | identity.py:487-501 |
| 8  | UserEntry v2 format includes key_version:4 BE; USERENTRY_VERSION is 0x02; old 0x01 entries rejected | VERIFIED | _directory.py:38, 202, 226-228; `data[4] != USERENTRY_VERSION` returns None |
| 9  | kem_sig signs (kem_pk \|\| key_version_be); verify_user_entry takes key_version parameter | VERIFIED | _directory.py:192-193, 260-277 |
| 10 | _populate_cache keeps highest key_version entry per signing key | VERIFIED | _directory.py:707-713, `if existing is not None and key_version <= existing.key_version: continue` |
| 11 | resolve_recipient() returns Identity with latest KEM public key | VERIFIED | _directory.py:503-516, delegates to get_user() which returns highest-version entry |
| 12 | envelope_decrypt with a rotated identity decrypts data encrypted under the old KEM key | VERIFIED | _envelope.py:254-276, ring map scan matches any historical pk_hash; test_decrypt_with_rotated_key_old_envelope passes |
| 13 | envelope_decrypt with a rotated identity decrypts data encrypted under the new KEM key | VERIFIED | _envelope.py:254-276; test_decrypt_with_rotated_key_new_envelope passes |
| 14 | NotARecipientError raised when identity has no matching key in ring or is public-only | VERIFIED | _envelope.py:255-256, 267-268; tests pass |

**Score:** 14/14 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `sdk/python/chromatindb/identity.py` | Key ring storage, rotate_kem(), numbered file save/load, lazy migration, _build_kem_ring_map() | VERIFIED | All methods present and implemented; `def rotate_kem` at line 421 |
| `sdk/python/tests/test_identity.py` | Unit tests for key ring, rotation, persistence, lazy migration | VERIFIED | `test_rotate_kem_increments_version` at line 330; 20 new tests present |
| `sdk/python/chromatindb/_directory.py` | UserEntry v2 format, key_version in DirectoryEntry, resolve_recipient(), highest-version cache | VERIFIED | All patterns verified; `async def resolve_recipient` at line 503 |
| `sdk/python/tests/test_directory.py` | Updated UserEntry codec tests, resolve_recipient tests | VERIFIED | `test_resolve_recipient_returns_identity` at line 362 |
| `sdk/python/chromatindb/_envelope.py` | envelope_decrypt with key ring fallback via _build_kem_ring_map | VERIFIED | `ring_map = identity._build_kem_ring_map()` at line 254; bisect import removed |
| `sdk/python/tests/test_envelope.py` | Key ring decrypt roundtrip tests | VERIFIED | `test_decrypt_with_rotated_key_old_envelope` at line 619; 5 new tests present |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| identity.py | disk files | `{stem}.kem.{N}` / `{stem}.kpub.{N}` numbered file pattern | WIRED | `f"{stem}.kem.{version}"` and `f"{stem}.kpub.{version}"` in save() and rotate_kem() |
| identity.py | _envelope.py | `_build_kem_ring_map()` method | WIRED | Called directly in envelope_decrypt line 254; method returns populated dict for secret-key identities |
| _directory.py | identity.py | `identity.key_version` property for encode_user_entry | WIRED | `key_version = identity.key_version` at _directory.py:190 |
| _directory.py | cache | `_populate_cache` highest-version-wins comparison | WIRED | `key_version <= existing.key_version` at _directory.py:707 |

### Data-Flow Trace (Level 4)

| Artifact | Data Variable | Source | Produces Real Data | Status |
|----------|---------------|--------|--------------------|--------|
| identity.py:rotate_kem | `new_pk`, `new_kem` | `oqs.KeyEncapsulation("ML-KEM-1024").generate_keypair()` | Yes -- live PQ key generation | FLOWING |
| _directory.py:_populate_cache | `key_version` | `decode_user_entry(result.data)` from blob scan | Yes -- decoded from real blob bytes | FLOWING |
| _envelope.py:envelope_decrypt | `ring_map` | `identity._build_kem_ring_map()` over `self._kem_ring` | Yes -- dict populated from actual KEM ring entries | FLOWING |

### Behavioral Spot-Checks

| Behavior | Command | Result | Status |
|----------|---------|--------|--------|
| identity+directory+envelope tests pass | `python -m pytest tests/test_identity.py tests/test_directory.py tests/test_envelope.py -x -q` | 201 passed | PASS |
| Full SDK test suite (no regressions) | `python -m pytest tests/ -q --ignore=tests/test_integration.py` | 584 passed | PASS |
| Commits from plan summaries verified in git log | git log --oneline | 1f6295e, e4463fb, cc34f5d, 1861183, 2e3ceee, b62afe7 all present | PASS |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|---------|
| KEY-01 | 92-01-PLAN.md | Owner can rotate KEM keypair via Identity.rotate_kem(); old secret keys retained in identity key ring for backward decryption | SATISFIED | rotate_kem() at identity.py:421; _kem_ring with full history; test_rotate_kem_retains_old_key_in_ring passes |
| KEY-02 | 92-02-PLAN.md | Directory tracks key version history; UserEntry v2 includes key_version field; resolve_recipient() returns latest KEM public key | SATISFIED | USERENTRY_VERSION=0x02 at _directory.py:38; key_version in DirectoryEntry; resolve_recipient() at line 503; highest-version cache at line 707. NOTE: REQUIREMENTS.md checkbox `[ ]` and traceability table say "Pending" -- this is a documentation tracking error; implementation is fully present and all tests pass. |
| KEY-03 | 92-03-PLAN.md | write_encrypted() uses recipient's latest KEM public key; read_encrypted() falls back to older keys via pk_hash matching | SATISFIED | envelope_encrypt uses recipient.kem_public_key (latest); envelope_decrypt uses _build_kem_ring_map() for ring scan; test_decrypt_with_rotated_key_old_envelope + new + two_rotations all pass |

**Note on KEY-02 tracking discrepancy:** REQUIREMENTS.md line 11 has `[ ]` (unchecked) and the traceability table at line 44 shows "Pending". The implementation in _directory.py is complete and tests pass. The document was not updated after plan 92-02 completed. This is a cosmetic tracking error only -- the code satisfies the requirement.

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| None | - | - | - | - |

No stubs, placeholders, or incomplete implementations found. All return values flow real data.

### Human Verification Required

None. All behaviors are verifiable programmatically. The rotation workflow (generate, save, rotate_kem, reload, decrypt old envelope) is fully unit-tested.

### Gaps Summary

No gaps. All three plans completed successfully:

- Plan 92-01: Identity key ring, rotate_kem(), numbered file persistence, _build_kem_ring_map() -- fully implemented and tested (49 identity tests pass)
- Plan 92-02: UserEntry v2 format with key_version, verify_user_entry signature update, highest-version cache, resolve_recipient() -- fully implemented and tested (directory tests pass)
- Plan 92-03: envelope_decrypt key ring fallback replacing bisect binary search -- fully implemented and tested (52 envelope tests pass, bisect import removed)

Full SDK test suite: 584 tests pass, 0 failures.

The phase goal is achieved: users can call Identity.rotate_kem() to generate a new keypair, register the new key via directory.register(), and encrypt future data to the new key. Data encrypted under any historical key (matched by pk_hash from the key ring) continues to decrypt correctly. Compromised old keys cannot decrypt new data since new data is encrypted under the new key only.

---

_Verified: 2026-04-07T03:10:00Z_
_Verifier: Claude (gsd-verifier)_
