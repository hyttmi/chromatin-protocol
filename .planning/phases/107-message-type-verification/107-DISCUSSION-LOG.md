# Phase 107: Message Type Verification - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-11
**Phase:** 107-message-type-verification
**Areas discussed:** Test tool architecture, Signed blob problem, Error response coverage, Test execution model

---

## Test Tool Architecture

| Option | Description | Selected |
|--------|-------------|----------|
| Extend relay_smoke_test (Recommended) | Add remaining ~31 types to existing smoke test. One binary, one run, all types verified. | Yes |
| New dedicated E2E test binary | Separate tools/relay_e2e_test.cpp for exhaustive coverage. Keeps smoke test minimal. | |
| Catch2 integration test | Relay test suite with a live node fixture. More structured but requires test harness changes. | |

**User's choice:** Extend relay_smoke_test
**Notes:** Already has auth, WS framing, and result tracking. Simplest path.

---

## Signed Blob Problem

| Option | Description | Selected |
|--------|-------------|----------|
| Build signing into the test tool (Recommended) | Add blob signing logic -- SHA3-256 hash, ML-DSA-87 sign, FlatBuffer encode. ~50 lines. | Yes |
| Pre-built binary fixture | Generate a signed blob offline, embed as fixture. Tied to specific key. | |
| Skip write/read/delete paths | Only test query types. Leaves gap in E2E-01 coverage. | |

**User's choice:** Build signing into the test tool
**Notes:** Identity already loaded, just need FlatBuffer encoding + signing.

---

## Error Response Coverage

| Option | Description | Selected |
|--------|-------------|----------|
| Core error paths (Recommended) | 3-4 key error cases: nonexistent blob, bad namespace, bad hash, malformed request. | Yes |
| Exhaustive error matrix | Every type's error path. ~20+ cases. Thorough but time-consuming. | |
| Minimal -- just check structure | One bad request, verify error has type+error fields. | |

**User's choice:** Core error paths
**Notes:** Enough to prove relay error translation works without going overboard.

---

## Test Execution Model

| Option | Description | Selected |
|--------|-------------|----------|
| Extend run-smoke.sh (Recommended) | Update existing script. Same workflow as Phase 106. One command, all results. | Yes |
| Standalone -- user starts node+relay | User manages lifecycle separately. More flexible but more steps. | |
| Docker compose | Fully automated containers. Adds Docker dependency. | |

**User's choice:** Extend run-smoke.sh
**Notes:** Same one-command workflow already validated in Phase 106.

---

## Claude's Discretion

- Type grouping order within the test
- FlatBuffer encoding approach for signed blobs
- Specific error messages to validate
- Whether to add timing annotations

## Deferred Ideas

None
