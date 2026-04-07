# Phase 95: Code Deduplication - Context

**Gathered:** 2026-04-07
**Status:** Ready for planning

<domain>
## Phase Boundary

Replace all duplicate encoding/decoding patterns across the C++ codebase with shared, tested utility functions. This is pure refactoring -- no new features, no new wire types, no behavioral changes. The extracted utilities should be improved (bounds checking, span-based interfaces) not just moved verbatim.

</domain>

<decisions>
## Implementation Decisions

### Consolidation Depth
- **D-01:** Improve while extracting -- shared utilities get bounds checking, span-based interfaces, and proper error handling from day one. Not just copy-paste into a header.
- **D-02:** This means the shared helpers may have slightly different signatures than the inline originals. Call sites adapt to the new interfaces.

### Header Organization
- **D-03:** Topic-focused headers, each near its domain:
  - `db/util/endian.h` -- BE read/write helpers (write_u16_be, write_u32_be, write_u64_be, read_u16_be, read_u32_be, read_u64_be) under `chromatindb::util`
  - `db/net/auth_helpers.h` -- encode_auth_payload / decode_auth_payload (currently static in handshake.cpp, duplicated 4x in connection.cpp)
  - `db/crypto/verify_helpers.h` -- verify_with_offload coroutine for conditional thread pool signature verification
  - `db/util/blob_helpers.h` -- namespace/hash extraction and blob reference encoding helpers
- **D-04:** Follows existing pattern: `db/util/hex.h` already lives under `chromatindb::util`

### Signature Verification Scope
- **D-05:** All 6 verification-with-offload sites are in scope (not just the 4 in DEDUP-03):
  - 4 identical if(pool_)/else blocks in connection.cpp (lines ~378, ~477, ~612, ~702)
  - 2 unconditional offload sites in engine.cpp (lines ~236, ~365)
- **D-06:** Extract a shared `verify_with_offload` coroutine that handles pool/no-pool branch, Signer::verify call, and error path. Engine.cpp's bundled build_signing_input+verify pattern should also use this (may need a variant or the caller bundles separately).

### Test Strategy
- **D-07:** Each new utility header gets its own dedicated test file:
  - `db/tests/util/test_endian.h` (or .cpp)
  - `db/tests/net/test_auth_helpers.cpp`
  - `db/tests/crypto/test_verify_helpers.cpp`
  - `db/tests/util/test_blob_helpers.cpp`
- **D-08:** Tests cover edge cases the inline code never tested: overflow, empty input, boundary sizes, malformed payloads. Happy paths are already covered by existing 615+ tests.
- **D-09:** All 615+ existing unit tests must pass under ASAN/TSAN/UBSAN with zero regressions after extraction.

### Claude's Discretion
- Exact function signatures for the shared helpers (as long as they include bounds checking and use spans)
- Internal organization within each header (ordering, helper grouping)
- Whether verify_with_offload returns bool or a result type with error context

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Dedup Targets (source files with duplication)
- `db/net/connection.cpp` -- 4 inline auth_payload encode blocks (lines 338-348, 436-446, 630-640, 720-730), 4 verify-with-offload blocks, 15 inline BE shift patterns
- `db/net/handshake.cpp` -- encode_auth_payload/decode_auth_payload static functions (lines 114, 137) to be promoted to shared
- `db/engine/engine.cpp` -- 2 verify-with-offload sites (lines ~236, ~365)
- `db/sync/sync_protocol.cpp` -- file-local write_u32_be/write_u64_be/read_u32_be/read_u64_be (lines 129-157)
- `db/sync/reconciliation.cpp` -- duplicate file-local write_u32_be/read_u32_be (lines 15-26)
- `db/peer/peer_manager.cpp` -- 11 inline BE shift patterns, 37 memcpy(32) patterns
- `db/wire/codec.cpp` -- 6 inline BE shift patterns
- `db/storage/storage.cpp` -- 3 inline BE shift patterns
- `db/net/framing.cpp` -- 3 inline BE shift patterns

### Existing Patterns (reuse/extend)
- `db/util/hex.h` -- existing utility header pattern under `chromatindb::util`
- `db/crypto/signing.h` -- Signer::verify static method (the function being offloaded)
- `db/crypto/thread_pool.h` -- crypto::offload helper (already exists, used by verify sites)
- `db/tests/test_helpers.h` -- shared test helper pattern

### Requirements
- `.planning/REQUIREMENTS.md` -- DEDUP-01 through DEDUP-05

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/util/hex.h` -- existing utility header under `chromatindb::util`, sets the pattern for new utility headers
- `crypto::offload()` in `db/crypto/thread_pool.h` -- already provides thread pool dispatch, verify_with_offload wraps this
- `db/tests/test_helpers.h` -- shared test utilities (TempDir, make_signed_blob, etc.)

### Established Patterns
- Namespace `chromatindb::util` for utility headers
- Static file-local helpers in .cpp files (current approach being replaced)
- `#pragma once` header guards throughout
- `std::span<const uint8_t>` for byte buffer parameters
- `co_await crypto::offload(pool_, lambda)` for CPU-heavy work

### Integration Points
- 11 source files consume BE encoding -- all must switch to `db/util/endian.h`
- connection.cpp's 4 handshake paths consume auth_payload -- all must switch to shared helpers
- 6 verify sites across connection.cpp and engine.cpp -- all must use verify_with_offload
- CMakeLists.txt needs new test targets for utility test files

</code_context>

<specifics>
## Specific Ideas

No specific requirements -- open to standard approaches for the extraction.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 95-code-deduplication*
*Context gathered: 2026-04-07*
