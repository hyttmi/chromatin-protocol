# Phase 107: Message Type Verification - Context

**Gathered:** 2026-04-11
**Status:** Ready for planning

<domain>
## Phase Boundary

Prove every relay-allowed message type (38 client types + 2 node signals) translates correctly through the full relay pipeline (JSON -> binary -> node -> binary -> JSON) against a live node. This is a testing/verification phase -- the relay code is already built, we're validating it works for all types.

</domain>

<decisions>
## Implementation Decisions

### Test Tool Architecture
- **D-01:** Extend the existing `tools/relay_smoke_test.cpp` with all remaining message types. One binary, one run, all 38 types verified. Already has auth, WS framing, and result tracking infrastructure.
- **D-02:** Do NOT create a separate E2E test binary or Catch2 integration test. Keep it simple -- one tool that does everything.

### Signed Blob Production
- **D-03:** Build ML-DSA-87 blob signing + FlatBuffer encoding directly into the smoke test. The tool already loads a RelayIdentity key -- add SHA3-256(namespace||data||ttl||timestamp) hash, ML-DSA-87 sign, FlatBuffer encode. ~50 lines of signing code.
- **D-04:** This enables testing Data(8) write -> WriteAck(30), ReadRequest(31) -> ReadResponse(32), Delete(17) -> DeleteAck(18), and BatchReadRequest(53) -> BatchReadResponse(54) paths that Phase 106 had to skip.

### Error Response Coverage
- **D-05:** Test 3-4 core error paths to prove the relay translates errors correctly: read nonexistent blob, stats for nonexistent namespace, metadata for nonexistent hash, and one malformed request. Not exhaustive -- enough to prove the pattern works.
- **D-06:** Each error response must have a "type" field and structured error information in JSON.

### Test Execution Model
- **D-07:** Extend the existing `/tmp/chromatindb-test/run-smoke.sh` script to start node+relay, run the extended smoke test, and dump all logs. Same one-command workflow used for Phase 106 live testing.
- **D-08:** User runs one script, gets full results. Claude reads the logs afterward.

### Claude's Discretion
- Grouping order of the 38 types within the test (by category, by wire type number, etc.)
- Exact FlatBuffer encoding approach for signed blobs (reuse relay lib or build minimal encoder)
- Specific error messages to validate beyond structural correctness
- Whether to add timing/performance annotations to the test output

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Existing Test Tools (extend these)
- `tools/relay_smoke_test.cpp` -- Current 13-test smoke test to extend with all 38 types
- `tools/relay_uds_tap.cpp` -- UDS tap tool for binary fixture capture (reference for wire format)
- `tools/CMakeLists.txt` -- Build configuration for test tools
- `/tmp/chromatindb-test/run-smoke.sh` -- Test runner script to extend

### Type Registry (source of truth for 38+2 types)
- `relay/translate/type_registry.h` -- All 40 TypeEntry entries with JSON name <-> wire type mapping
- `relay/translate/type_registry.cpp` -- type_from_string() and type_to_string() lookups

### Translation Code (what's being verified)
- `relay/translate/translator.cpp` -- json_to_binary() and binary_to_json() with all compound decoders
- `relay/translate/json_schema.h` -- FieldSpec/MessageSchema with FieldEncoding types
- `relay/translate/json_schema.cpp` -- Schema registry for all message types

### Wire Format References
- `relay/wire/transport_codec.h` -- TransportMessage encode/decode
- `relay/wire/blob_codec.h` -- FlatBuffer blob decode (ReadResponse, BatchReadResponse)
- `db/schemas/transport.fbs` -- FlatBuffers schema for wire format

### Blob Signing (for Data write path)
- `db/crypto/signing.cpp` -- ML-DSA-87 sign/verify (reference implementation)
- `db/wire/codec.h` -- build_signing_input() for SHA3-256(namespace||data||ttl||timestamp)
- `relay/identity/relay_identity.h` -- RelayIdentity::sign() already used in smoke test

### Phase 106 Results
- `.planning/phases/106-bug-fixes/106-01-SUMMARY.md` -- Compound decoder fixes
- `.planning/phases/106-bug-fixes/106-03-SUMMARY.md` -- Tap tool + smoke test results

### Requirements
- `.planning/REQUIREMENTS.md` -- E2E-01 requirement definition

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `relay_smoke_test.cpp` -- Already has TCP connect, WS upgrade, ML-DSA-87 auth, JSON send/recv, result tracking (13 tests passing). Extend with remaining types.
- `relay_uds_tap.cpp` -- Reference for wire format payload construction (all 11 compound request payloads correct after Phase 106 fixes)
- `RelayIdentity` -- Already loaded in smoke test, has `sign()` method for blob signing
- `ws_send_text()` / `ws_recv_text()` -- Blocking WS frame helpers already in smoke test

### Established Patterns
- Smoke test pattern: send JSON -> recv JSON -> validate fields -> record(name, pass/fail, detail)
- Tap tool pattern: construct binary payload -> send via transport codec -> recv -> validate type
- FlatBuffer blob encoding: see `db/wire/codec.h` for canonical format

### Integration Points
- Smoke test connects to relay on 127.0.0.1:4201 (same as Phase 106)
- Node listens on UDS at /tmp/chromatindb-test/node.sock
- run-smoke.sh manages node+relay lifecycle

</code_context>

<specifics>
## Specific Ideas

- The smoke test should produce a clear pass/fail summary at the end showing all 38 types
- Blob signing in the test tool means we can do a full write->read->verify roundtrip
- Error responses should be validated for JSON structure, not specific error message text (messages may change)
- The extended run-smoke.sh should be the same one-command workflow used in Phase 106

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 107-message-type-verification*
*Context gathered: 2026-04-11*
