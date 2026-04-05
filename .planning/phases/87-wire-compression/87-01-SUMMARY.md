---
phase: 87-wire-compression
plan: 01
subsystem: crypto
tags: [brotli, compression, envelope-encryption, sdk-python, aead]

# Dependency graph
requires:
  - phase: 78-envelope-encryption
    provides: envelope_encrypt/decrypt with suite=0x01 (ML-KEM + ChaCha20-Poly1305)
provides:
  - Brotli compression in SDK envelope encryption (suite=0x02)
  - DecompressionError exception class
  - Streaming decompression bomb protection (100 MiB cap)
  - Backward-compatible decrypt for both suite=0x01 and suite=0x02
affects: [87-02, sdk-python, envelope-format, protocol-docs]

# Tech tracking
tech-stack:
  added: [brotli~=1.2]
  patterns: [compress-before-encrypt, suite-byte-dispatch, streaming-decompress-with-cap]

key-files:
  created: []
  modified:
    - sdk/python/chromatindb/_envelope.py
    - sdk/python/chromatindb/exceptions.py
    - sdk/python/pyproject.toml
    - sdk/python/tests/test_envelope.py

key-decisions:
  - "Brotli quality 6 balances compression ratio vs CPU time"
  - "256-byte compression threshold avoids overhead on small payloads"
  - "Streaming Brotli Decompressor for bomb protection (not full decompress-then-check)"
  - "compress=True default makes compression opt-out not opt-in"

patterns-established:
  - "Suite byte dispatch: encrypt writes actual_suite, decrypt/parse accept set of valid suites"
  - "Compress-before-encrypt: compression happens before AEAD, decompression after AEAD decrypt"
  - "Expansion fallback: if compressed >= original, emit uncompressed suite=0x01"

requirements-completed: [COMP-01, COMP-02, COMP-03, COMP-04]

# Metrics
duration: 5min
completed: 2026-04-05
---

# Phase 87 Plan 01: SDK Envelope Compression Summary

**Brotli compression in SDK envelope encryption with suite=0x02, 256-byte threshold, expansion fallback, and 100 MiB decompression bomb protection**

## Performance

- **Duration:** 5 min
- **Started:** 2026-04-05T15:19:36Z
- **Completed:** 2026-04-05T15:24:21Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Implemented Brotli compression in envelope_encrypt with compress=True default, producing suite=0x02 envelopes
- Added backward-compatible decrypt/parse accepting both suite=0x01 and suite=0x02
- Added _safe_decompress with streaming Brotli Decompressor and 100 MiB output cap for bomb protection
- 16 new compression tests (47 total), all passing with zero regressions on existing 31 tests

## Task Commits

Each task was committed atomically:

1. **Task 1: Add DecompressionError, brotli dep, and implement envelope compression** - `48a6bec` (test: RED), `06ed587` (feat: GREEN)
2. **Task 2: Comprehensive test suite for envelope compression** - `0786278` (test)

_TDD flow: RED test committed first, then implementation (GREEN) with all production code._

## Files Created/Modified
- `sdk/python/chromatindb/_envelope.py` - Added CIPHER_SUITE_ML_KEM_CHACHA_BROTLI=0x02, _safe_decompress, compress param in encrypt, decompression in decrypt, updated parse
- `sdk/python/chromatindb/exceptions.py` - Added DecompressionError(CryptoError) exception class
- `sdk/python/pyproject.toml` - Added brotli~=1.2 dependency, fixed license classifier for setuptools compat
- `sdk/python/tests/test_envelope.py` - 16 new tests: roundtrip, threshold, expansion fallback, bomb protection, multi-recipient, parse

## Decisions Made
- Brotli quality 6 (research recommended range 4-6, chose 6 for better ratio on typical SDK payloads)
- 256-byte compression threshold (below this, overhead exceeds benefit)
- compress=True as default (opt-out not opt-in, per plan D-07)
- Streaming Decompressor over decompress-then-check (catches bombs before full allocation)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Fixed setuptools license classifier conflict**
- **Found during:** Task 1 (venv setup)
- **Issue:** Python 3.14 setuptools rejects `License :: OSI Approved :: MIT License` classifier when `license = "MIT"` is set in pyproject.toml (PEP 639 enforcement)
- **Fix:** Removed the deprecated license classifier from classifiers list
- **Files modified:** sdk/python/pyproject.toml
- **Verification:** pip install -e ".[dev]" succeeds
- **Committed in:** 06ed587 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Necessary for build environment. No scope creep.

## Issues Encountered
- SDK venv had broken python3.14 symlink (known from research). Recreated with `python3 -m venv .venv`.
- Pre-existing test failures in test_client.py (async mock issues) and test_integration.py (requires live relay). Not related to this plan.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Suite=0x02 envelope compression fully operational in SDK
- Ready for Plan 02: SDK compress parameter exposure in client API methods
- All existing envelope tests pass, backward compatibility confirmed

## Self-Check: PASSED

All files exist, all commit hashes found in git log.

---
*Phase: 87-wire-compression*
*Completed: 2026-04-05*
