---
phase: 47-crypto-transport-verification
plan: 02
subsystem: testing
tags: [integration-tests, docker, sha3-256, ml-dsa-87, content-addressing, non-repudiation, tamper-detection, dare, aead]

# Dependency graph
requires:
  - phase: 47-01
    provides: "chromatindb_verify CLI tool (hash + sig subcommands) and integration test harness"
provides:
  - "CRYPT-01 content addressing Docker integration test"
  - "CRYPT-02 non-repudiation Docker integration test with negative tamper test"
  - "CRYPT-03 tamper detection Docker integration test (DARE AEAD bit-flip verification)"
  - "chromatindb_verify hash-fields and sig-fields subcommands for field-level crypto verification"
  - "chromatindb_loadgen --verbose-blobs flag for per-blob field output"
affects: [48-sync-replication-verification, 49-acl-delegation-verification, 50-operational-verification]

# Tech tracking
tech-stack:
  added: []
  patterns: [verbose-blob-fields-stderr, field-level-crypto-verification, bit-flip-tamper-detection]

key-files:
  created:
    - tests/integration/test_crypt01_content_addressing.sh
    - tests/integration/test_crypt02_nonrepudiation.sh
    - tests/integration/test_crypt03_tamper_detection.sh
  modified:
    - loadgen/loadgen_main.cpp
    - tools/verify_main.cpp

key-decisions:
  - "hash-fields/sig-fields subcommands take raw hex values, avoiding FlatBuffer dependency in test assertions"
  - "--verbose-blobs writes BLOB_FIELDS JSON to stderr per blob for test consumption"
  - "CRYPT-03 handles both mdbx corruption and DARE AEAD failure as valid tamper detection"
  - "CRYPT-03 retry with multiple offsets if initial bit flip hits mdbx free space"

patterns-established:
  - "Verbose loadgen pattern: --verbose-blobs emits BLOB_FIELDS:{json} lines to stderr for field-level test assertions"
  - "Field-level crypto verification: hash-fields and sig-fields avoid FlatBuffer round-trip in test scripts"
  - "Tamper detection pattern: stop node, xxd bit-flip on data.mdb, restart and check for AEAD/mdbx errors"

requirements-completed: [CRYPT-01, CRYPT-02, CRYPT-03]

# Metrics
duration: 5min
completed: 2026-03-21
---

# Phase 47 Plan 02: Crypto Integration Tests Summary

**Three Docker integration tests verifying SHA3-256 content addressing, ML-DSA-87 non-repudiation with negative test, and DARE AEAD tamper detection via bit-flip on data.mdb**

## Performance

- **Duration:** 5 min
- **Started:** 2026-03-21T06:28:10Z
- **Completed:** 2026-03-21T06:33:28Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments
- CRYPT-01 test independently recomputes SHA3-256(namespace||data||ttl||timestamp) via hash-fields and verifies match against loadgen's verbose output
- CRYPT-02 test independently verifies ML-DSA-87 signature via sig-fields, plus negative test with tampered signature byte
- CRYPT-03 test flips a single bit in DARE-encrypted data.mdb and verifies either AEAD auth failure or mdbx corruption detection
- Added hash-fields and sig-fields subcommands to chromatindb_verify for field-level crypto verification without FlatBuffer decoding
- Added --verbose-blobs flag to loadgen emitting per-blob BLOB_FIELDS JSON to stderr

## Task Commits

Each task was committed atomically:

1. **Task 1: CRYPT-01 content addressing and CRYPT-02 non-repudiation tests** - `9497936` (feat)
2. **Task 2: CRYPT-03 tamper detection test** - `27bde4d` (feat)

## Files Created/Modified
- `tests/integration/test_crypt01_content_addressing.sh` - Content addressing verification (independent signing digest recomputation)
- `tests/integration/test_crypt02_nonrepudiation.sh` - Non-repudiation verification (ML-DSA-87 sig-fields + negative test)
- `tests/integration/test_crypt03_tamper_detection.sh` - Tamper detection (bit-flip data.mdb, AEAD/mdbx failure)
- `loadgen/loadgen_main.cpp` - Added --verbose-blobs flag (BLOB_FIELDS JSON per blob to stderr)
- `tools/verify_main.cpp` - Added hash-fields and sig-fields subcommands

## Decisions Made
- hash-fields and sig-fields take raw hex values via CLI flags rather than FlatBuffer bytes -- enables independent verification without encoding dependency
- --verbose-blobs writes to stderr (not stdout) to preserve clean JSON stats output on stdout
- CRYPT-03 accepts both mdbx corruption detection and DARE AEAD failure as valid outcomes since either prevents serving corrupted data
- CRYPT-03 includes retry logic with multiple offsets (16384, 32768, 49152, 65536) to handle cases where initial flip hits mdbx free/meta pages

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All three CRYPT-01/02/03 test scripts ready for execution via run-integration.sh
- hash-fields and sig-fields subcommands available for future test scripts if needed
- --verbose-blobs pattern reusable for any test needing per-blob field inspection
- Phase 47 Plan 03 (CRYPT-04/05/06 transport tests) can proceed

## Self-Check: PASSED

All files exist. All commits verified.

---
*Phase: 47-crypto-transport-verification*
*Completed: 2026-03-21*
