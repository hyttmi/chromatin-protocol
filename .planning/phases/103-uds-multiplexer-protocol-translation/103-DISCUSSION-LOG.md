# Phase 103: UDS Multiplexer & Protocol Translation - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-04-10
**Phase:** 103-uds-multiplexer-protocol-translation
**Areas discussed:** UDS Connection Lifecycle, Translation Table Design, FlatBuffers Linking, Data Message Handling
**Mode:** --auto (all decisions auto-selected)

---

## UDS Connection Lifecycle

| Option | Description | Selected |
|--------|-------------|----------|
| Use db/net/Connection directly | Link relay against db/net/ for UDS transport | |
| Own lightweight UDS client | Relay implements its own UDS client with TrustedHello | ✓ |

**User's choice:** [auto] Own lightweight UDS client (recommended -- Phase 100 D-01 prohibits db/ dependency)
**Notes:** Relay cannot link against db/ per Phase 100 D-01 (db/ will move to separate repo). TrustedHello is simple enough to reimplement. No AEAD needed for UDS.

| Option | Description | Selected |
|--------|-------------|----------|
| Fail to start if node unavailable | Relay exits with error on startup | |
| Start and retry async | Accept WS clients, reject requests until UDS ready | ✓ |

**User's choice:** [auto] Start and retry async (recommended -- relay shouldn't crash if node is temporarily down)
**Notes:** Phase 104 formalizes auto-reconnect with subscription replay. Phase 103 just needs basic retry.

---

## Translation Table Design

| Option | Description | Selected |
|--------|-------------|----------|
| Per-type handler functions | 38 separate encode/decode functions | |
| Generic table-driven encoder | Single encoder/decoder consuming FieldSpec metadata | ✓ |

**User's choice:** [auto] Generic table-driven encoder (recommended -- Phase 102 schema explicitly designed for this, success criterion #3 requires it)
**Notes:** FieldSpec metadata in json_schema.h defines field names, encodings, and optionality. Generic encoder iterates this array. FlatBuffer types (3 of 40) get special-case handling.

---

## FlatBuffers Linking

| Option | Description | Selected |
|--------|-------------|----------|
| Link against db/wire/ | Share FlatBuffer generated headers with db/ | |
| Copy .fbs to relay/wire/ | Generate locally, maintain clean separation | ✓ |

**User's choice:** [auto] Copy .fbs to relay/wire/ (recommended -- maintains Phase 100 D-01 separation)
**Notes:** Relay gets its own copy of transport.fbs and blob.fbs. FlatBuffers FetchContent in relay/CMakeLists.txt compiles them. TransportCodec reimplemented in relay.

---

## Data Message Handling

| Option | Description | Selected |
|--------|-------------|----------|
| Size-based binary frame threshold | Switch to binary frame above N bytes | |
| Type-based binary frame rule | ReadResponse/BatchReadResponse always binary | ✓ |

**User's choice:** [auto] Type-based binary frame rule (recommended -- simpler, no threshold tuning)
**Notes:** Binary opcode signals large payload to clients. JSON payload format is identical regardless of opcode.

---

## Claude's Discretion

- Internal API design for UdsMultiplexer, RequestRouter, and translator functions
- TrustedHello handshake byte-level encoding details
- Error messages for edge cases
- Test organization
- Whether WsSession needs a new send_binary() method or reuses write_frame with opcode param

## Deferred Ideas

None -- discussion stayed within phase scope.
