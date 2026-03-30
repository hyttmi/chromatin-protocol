---
gsd_state_version: 1.0
milestone: v1.6.0
milestone_name: Python SDK
status: executing
stopped_at: Completed 73-03-PLAN.md
last_updated: "2026-03-30T15:06:51.538Z"
last_activity: 2026-03-30
progress:
  total_phases: 5
  completed_phases: 4
  total_plans: 12
  completed_plans: 12
  percent: 26
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-29)

**Core value:** Any node can receive a signed blob, verify its ownership via cryptographic proof, store it, and replicate it to peers -- making data censorship-resistant and technically unstoppable.
**Current focus:** Phase 72 — core-data-operations

## Current Position

Phase: 73 (extended-queries-pub-sub) -- EXECUTING
Plan: 3 of 3
Status: Ready to execute
Last activity: 2026-03-30

Progress: [##........] 26%

## Performance Metrics

**Velocity:**

- Total plans completed: 1
- Average duration: 27min
- Total execution time: 0.45 hours

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| Phase 70 P01 | 4min | 2 tasks | 10 files |
| Phase 70 P02 | 27min | 1 tasks | 3 files |

*Updated after each plan completion*
| Phase 70 P03 | 7min | 2 tasks | 9 files |
| Phase 71 P02 | 5min | 2 tasks | 7 files |
| Phase 72 P01 | 4min | 2 tasks | 4 files |
| Phase 72 P02 | 5min | 2 tasks | 3 files |
| Phase 72 P03 | 2min | 2 tasks | 1 files |
| Phase 73 P01 | 6min | 2 tasks | 4 files |
| Phase 73 P02 | 6min | 2 tasks | 4 files |
| Phase 73 P03 | 3min | 2 tasks | 1 files |

## Accumulated Context

### Decisions

- Relay message filter flipped from whitelist (38 types) to blocklist (21 peer-internal types) -- new client message types pass through without relay changes
- Python SDK first, using liboqs-python for PQ crypto (no C extensions)
- SDK directory layout: sdk/python/ (sdk/c/, sdk/c++/, sdk/rust/, sdk/js/ reserved for future)
- Live KVM test swarm: 192.168.1.200 (bootstrap + relay), .201 and .202 (join-only nodes)
- PROTOCOL.md HKDF salt is wrong (says SHA3-256(pubkeys), C++ uses empty salt) -- SDK follows C++ source, fix docs in Phase 74
- Mixed endianness: BE framing, LE auth payload and signing input fields -- explicit per-field encoding required
- [Phase 70 P01]: setuptools.build_meta backend (not _legacy) for Python 3.14 compatibility
- [Phase 70 P01]: D-24 version override -- liboqs-python~=0.14.0, flatbuffers~=25.12 (research-corrected)
- [Phase 70 P01]: FlatBuffers generated code excluded from ruff linting (auto-generated PascalCase)
- [Phase 70 P02]: Test vector generator links chromatindb_lib, outputs JSON to stdout (same pattern as chromatindb_verify)
- [Phase 70 P02]: SDK test vectors: C++ generates authoritative JSON, Python validates against it
- [Phase 70 P03]: Pure-Python HKDF-SHA256 via stdlib hmac+hashlib (no libsodium HKDF binding needed)
- [Phase 70 P03]: FlatBuffer payload decode via per-element Payload(j) loop -- avoids numpy dependency
- [Phase 70 P03]: Removed auto-generated .pyi stubs from FlatBuffers codegen (buggy numpy and Literal types)
- [Phase 71]: Transport._writer typed as object for mock compatibility; notification queue maxsize=1000; request_id starts at 1; send_lock serializes outgoing frames
- [Phase 72 P01]: Codec as standalone _codec.py module -- binary encode/decode separate from client.py high-level API
- [Phase 72 P01]: decode_delete_ack separate function (not alias) -- independent error messages per type
- [Phase 72]: Use SDK custom ConnectionError for D-16 timeout wrapping, not builtin
- [Phase 72]: ML-DSA-87 signatures are non-deterministic -- test FlatBuffer fields individually, not byte comparison
- [Phase 72]: ML-DSA-87 non-deterministic signatures mean same data produces unique blob_hash -- duplicate test verifies distinct hashes
- [Phase 73 P01]: Notification expanded beyond D-03 to include blob_size and is_tombstone from wire format
- [Phase 73 P01]: All new decode functions return typed dataclasses (not raw tuples)
- [Phase 73 P01]: PeerInfo single type with empty peers list for untrusted (not separate types)
- [Phase 73]: subscribe/unsubscribe use fire-and-forget send_message since C++ node processes inline without response
- [Phase 73]: notifications() async iterator uses 1s timeout on queue.get() to check transport.closed and exit cleanly
- [Phase 73]: NamespaceList blob_count >= 0 for other namespaces (deleted blobs leave 0-count entries on live node)

### Pending Todos

None.

### Blockers/Concerns

None.

## Session Continuity

Last session: 2026-03-30T15:06:51.535Z
Stopped at: Completed 73-03-PLAN.md
Resume file: None
