# Phase 106: Bug Fixes - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-11
**Phase:** 106-bug-fixes
**Areas discussed:** Verification strategy, FIX-02 scope, ASAN test workflow

---

## Verification Strategy

### Q1: How to verify compound translation fixes (FIX-01)?

| Option | Description | Selected |
|--------|-------------|----------|
| Captured binary fixtures | Capture real binary responses from live node, save as test fixtures | |
| Live node tests only | Test scripts that talk to running node through relay | |
| Both | Fixtures for reproducible unit tests + live smoke test | ✓ |

**User's choice:** Both
**Notes:** Fixtures catch regressions, live tests catch drift.

### Q2: Fix approach for compound types?

| Option | Description | Selected |
|--------|-------------|----------|
| Fix in place | Debug each decoder, fix offset/parsing bugs. Minimal diff. | |
| Rewrite decoders | Cursor-based reader with auto offset tracking | |
| Fix + add guards | Fix bugs in place, add consistent bounds checking to all decoders | ✓ |

**User's choice:** Fix + add guards
**Notes:** Middle ground -- prevents future parsing bugs without full rewrite.

### Q3: Where should captured binary fixtures live?

| Option | Description | Selected |
|--------|-------------|----------|
| relay/tests/fixtures/ | Binary files named by type | |
| Inline hex in tests | Hex string literals in test_translator.cpp | |
| Both | Binary files for large responses, inline hex for small types | ✓ |

**User's choice:** Both

### Q4: How to capture fixtures from live node?

| Option | Description | Selected |
|--------|-------------|----------|
| Manual capture script | One-off script, dump raw binary responses | |
| Relay debug logging | Temporary hexdump mode in relay | |
| UDS tap tool | Reusable tool connecting to node UDS, writes raw responses | ✓ |

**User's choice:** UDS tap tool
**Notes:** Reusable for Phase 107 message type verification.

---

## FIX-02 Scope

### Q1: Broader sweep coverage?

| Option | Description | Selected |
|--------|-------------|----------|
| Lambda captures in coroutines | Captured references dangling across co_await | ✓ |
| Shared pointer lifetimes | shared_from_this patterns around co_await | ✓ |
| Container invalidation | Container access across co_await suspension points | ✓ |
| Strand confinement | Shared state access without proper strand/atomic protection | ✓ |

**User's choice:** All four categories
**Notes:** Comprehensive coroutine safety audit.

### Q2: What happens when sweep finds issues?

| Option | Description | Selected |
|--------|-------------|----------|
| Fix all in Phase 106 | Fix every issue in relay/ within this phase | ✓ |
| Document only | Record findings, defer fixes | |
| Fix critical, defer minor | Fix crashes/UB, document theoretical concerns | |

**User's choice:** Fix all in Phase 106
**Notes:** Clean foundation before E2E testing.

### Q3: Sweep scope -- relay only or include db/?

| Option | Description | Selected |
|--------|-------------|----------|
| Relay only | db/ frozen for v3.1.0, sweep relay/ only | |
| Both relay/ and db/ | Also check db/ peer/ code | ✓ (modified) |

**User's choice:** Both, but only fix relay/. Document db/ findings for later manual sweep.
**Notes:** User wants to do a more thorough db/ sweep later independently.

### Q4: db/ audit format?

| Option | Description | Selected |
|--------|-------------|----------|
| Section in audit doc | Add db/ section to COROUTINE-AUDIT.md | |
| Separate db/ report | Write DB-COROUTINE-FINDINGS.md | ✓ |
| Inline in audit doc | Same doc, clearly separated section | |

**User's choice:** Separate DB-COROUTINE-FINDINGS.md

### Q5: db/ areas of concern?

| Option | Description | Selected |
|--------|-------------|----------|
| PeerManager/peer/ decomposed code | 6-component decomposition from Phase 96 | ✓ |
| Sync protocol (Phase A/B/C) | Many co_await suspension points | ✓ |
| Connection lifecycle | Shared state from multiple coroutines | ✓ |
| No specific focus | General sweep | |

**User's choice:** All three specific areas
**Notes:** These are the areas with most coroutine complexity.

### Q6: Documentation approach?

| Option | Description | Selected |
|--------|-------------|----------|
| Code comments at fix sites | Comments explaining coroutine safety issue | |
| Separate audit doc | COROUTINE-AUDIT.md listing every coroutine | |
| Both | Comments at fix sites + summary audit doc | ✓ |

**User's choice:** Both

---

## ASAN Test Workflow

### Q1: Current ASAN build approach?

| Option | Description | Selected |
|--------|-------------|----------|
| CMake preset exists | Existing sanitizer build preset | ✓ |
| Manual flags | Pass flags manually to CMAKE_CXX_FLAGS | |
| Not sure / varies | Inconsistent workflow | |

**User's choice:** CMake preset exists

### Q2: What does 'ASAN clean' mean?

| Option | Description | Selected |
|--------|-------------|----------|
| Unit tests clean | All relay unit tests pass under ASAN | |
| Unit + live smoke test | Unit tests + basic request/response cycle | |
| Full integration | Full E2E test suite under ASAN | ✓ |

**User's choice:** Full integration

### Q3: Which sanitizers?

| Option | Description | Selected |
|--------|-------------|----------|
| ASAN (address) | Buffer overflow, use-after-free, use-after-return | |
| UBSAN (undefined behavior) | Integer overflow, null deref, alignment | |
| TSAN (thread) | Data races, lock order violations | |
| All three separately | ASAN+UBSAN in one build, TSAN separate | ✓ |

**User's choice:** All three separately

### Q4: Handling Phase 107 dependency for full integration?

| Option | Description | Selected |
|--------|-------------|----------|
| Minimal smoke test now | Basic smoke test exercising key paths | |
| Defer ASAN integration to 107 | Unit tests clean in 106, full in 107 | |
| Smoke test now + full in 107 | Smoke test covering fixed types + coroutine paths | ✓ |

**User's choice:** Smoke test now + full in 107
**Notes:** Phase 107 extends smoke test to all 38 types.

---

## Claude's Discretion

- Specific bounds-check patterns for compound decoders
- Smoke test framework choice
- COROUTINE-AUDIT.md format and severity rating scheme

## Deferred Ideas

None -- discussion stayed within phase scope
