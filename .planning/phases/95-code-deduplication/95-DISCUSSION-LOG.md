# Phase 95: Code Deduplication - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-07
**Phase:** 95-code-deduplication
**Areas discussed:** Consolidation depth, Header organization, Signature verify sites, Test strategy

---

## Consolidation Depth

| Option | Description | Selected |
|--------|-------------|----------|
| Extract as-is | Move existing code verbatim to shared headers. Zero behavior change, minimal diff. | |
| Improve while extracting | Add bounds checking, span-based interfaces, and better error handling to shared helpers. | ✓ |
| Extract + assert only | Move code as-is but add debug asserts for buffer sizes. | |

**User's choice:** Improve while extracting
**Notes:** Shared utilities should be the "correct" versions from day one, not just relocated inline code.

---

## Header Organization

| Option | Description | Selected |
|--------|-------------|----------|
| Topic-focused | db/util/endian.h, db/net/auth_helpers.h, db/crypto/verify_helpers.h, db/util/blob_helpers.h. Each near its domain. | ✓ |
| Single util header | db/util/encoding.h for everything. Fewer files, one include. | |
| All in db/util/ | Everything under util/ regardless of domain. Simple to find. | |

**User's choice:** Topic-focused (Recommended)
**Notes:** Follows existing pattern of db/util/hex.h. Domain-adjacent placement.

---

## Signature Verify Sites

| Option | Description | Selected |
|--------|-------------|----------|
| All 6 sites | Extract shared verify_with_offload coroutine for 4 connection.cpp + 2 engine.cpp sites. | ✓ |
| Connection.cpp only | Only deduplicate the 4 identical blocks in connection.cpp. Leave engine.cpp alone. | |
| Reassess scope | Re-scope DEDUP-03 based on actual findings. | |

**User's choice:** All 6 sites
**Notes:** Found 6 sites (not 4 as DEDUP-03 stated): 4 identical if(pool_)/else blocks in connection.cpp + 2 unconditional offload in engine.cpp.

---

## Test Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Unit tests per header | Each new utility header gets its own test file. Edge cases the inline code never tested. | ✓ |
| Existing tests only | Zero new test files. 615+ existing tests as proof. | |
| Focused edge-case tests | One new test file covering only improved behaviors (bounds checks, overflow guards). | |

**User's choice:** Unit tests per header (Recommended)
**Notes:** Cover edge cases (overflow, empty input, boundary sizes, malformed payloads). Happy paths covered by existing 615+ tests.

## Claude's Discretion

- Exact function signatures for shared helpers
- Internal organization within each header
- Whether verify_with_offload returns bool or result type

## Deferred Ideas

None
