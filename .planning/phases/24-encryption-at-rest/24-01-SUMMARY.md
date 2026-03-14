---
phase: 24-encryption-at-rest
plan: 01
subsystem: storage
tags: [chacha20-poly1305, hkdf-sha256, encryption-at-rest, libsodium, mdbx]

requires:
  - phase: 23-ttl-flexibility
    provides: "TTL flexibility and tombstone expiry in storage layer"
provides:
  - "Transparent encryption/decryption of all blob values in mdbx"
  - "Master key auto-generation and HKDF-based key derivation"
  - "Startup validation against unencrypted databases"
affects: [26-documentation-release]

tech-stack:
  added: []
  patterns: ["envelope encryption [version][nonce][ciphertext+tag]", "HKDF key derivation with context label", "AEAD associated data binding to storage key"]

key-files:
  created:
    - db/crypto/master_key.h
    - db/crypto/master_key.cpp
    - tests/crypto/test_master_key.cpp
  modified:
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/CMakeLists.txt
    - CMakeLists.txt
    - tests/storage/test_storage.cpp

key-decisions:
  - "Single derived key for all blobs (not per-blob) — matches EAR-02 'one key per node'"
  - "HKDF context label: 'chromatindb-dare-v1' — versioned for future key rotation"
  - "AEAD AD = 64-byte mdbx key (namespace||content_hash) — binds ciphertext to storage location"
  - "Version byte 0x01 — enables future scheme migration"
  - "Full scan on startup for unencrypted data detection (not sampling)"

patterns-established:
  - "Envelope format: [version:1][nonce:12][ciphertext+tag:N+16] for all encrypted mdbx values"
  - "Master key pattern: load_or_generate_master_key() mirrors NodeIdentity::load_or_generate()"
  - "Encrypt after encode, decrypt before decode — keeps wire codec unaware of encryption"

requirements-completed: [EAR-01, EAR-02, EAR-03, EAR-04]

duration: 15min
completed: 2026-03-14
---

# Phase 24: Encryption at Rest Summary

**ChaCha20-Poly1305 encryption for all stored blob payloads with HKDF-derived key from auto-generated master key**

## Performance

- **Duration:** 15 min
- **Completed:** 2026-03-14
- **Tasks:** 1
- **Files modified:** 8

## Accomplishments
- All blob values in mdbx encrypted with ChaCha20-Poly1305 envelope format
- Master key auto-generated on first run with 0600 permissions, loaded on subsequent runs
- HKDF-SHA256 key derivation with "chromatindb-dare-v1" context label
- All 6 read paths in storage.cpp decrypt transparently
- Startup validation scans all blob values for encryption version header
- 297 tests pass (287 existing unchanged + 7 master key + 4 encryption-at-rest)

## Task Commits

1. **Task 1: Create master key module and add encryption to Storage** - `e02f103` (feat)

## Files Created/Modified
- `db/crypto/master_key.h` - Master key load/generate/derive API
- `db/crypto/master_key.cpp` - File I/O with 0600 permissions, HKDF derivation
- `db/storage/storage.h` - Added SecureBytes include
- `db/storage/storage.cpp` - Encrypt on store, decrypt on all read paths, startup validation
- `db/CMakeLists.txt` - Added master_key.cpp to library sources
- `CMakeLists.txt` - Added test_master_key.cpp to test sources
- `tests/crypto/test_master_key.cpp` - 7 tests for master key lifecycle
- `tests/storage/test_storage.cpp` - 4 tests verifying encrypted storage (raw mdbx inspection)

## Decisions Made
- Single HKDF-derived key for all blobs (not per-blob derivation) per EAR-02
- AEAD associated data = mdbx key (64 bytes) to bind ciphertext to storage location
- Full scan (not sampling) for startup unencrypted data detection
- No second Storage constructor overload — existing constructor auto-loads master key internally

## Deviations from Plan
None - plan executed as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Encryption at rest fully operational
- Ready for Phase 25 (Transport Optimization) or Phase 26 (Documentation)
- README will need documentation of master key management (Phase 26)

---
*Phase: 24-encryption-at-rest*
*Completed: 2026-03-14*
