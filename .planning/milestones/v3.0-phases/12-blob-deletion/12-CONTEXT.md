# Phase 12: Blob Deletion - Context

**Gathered:** 2026-03-07
**Status:** Ready for planning

<domain>
## Phase Boundary

Namespace owners can permanently delete blobs via signed tombstones that replicate across the network. Tombstones are permanent (TTL=0), propagate via sync, and block future arrival of deleted blobs. Also includes liboqs build optimization (BUILD-01: strip unused algorithms).

</domain>

<decisions>
## Implementation Decisions

### Delete command interface
- New DELETE wire message type (not reuse of WRITE path). Client sends DELETE(namespace, blob_hash, signature). Node constructs and stores the tombstone internally.
- DELETE_ACK response with blob_hash + confirmation, mirroring the existing WriteAck pattern (blob_hash, seq_num, status).
- Idempotent: deleting an already-deleted blob returns success with 'duplicate' status (same pattern as WRITE dedup).
- Pre-emptive tombstones allowed: deleting a non-existent blob creates the tombstone anyway. Handles distributed race conditions — tombstone blocks the blob if it arrives later from another peer.

### Tombstone identity & content
- Tombstone is a BlobData with magic prefix in the data field, followed by the 32-byte SHA3-256 target hash (total: ~36 bytes in data).
- TTL=0 marks tombstone as permanent (survives expiry pruning).
- Minimal metadata: target hash only. Deletion timestamp is implicit via BlobData.timestamp. No reason field.
- Content-addressed like any other blob: SHA3-256(encoded tombstone FlatBuffer). Same hashing path, tombstone hash is unique and deterministic.

### Rejection signaling
- Silent rejection when a blob arrives and a tombstone exists: return 'duplicate' status. No new error codes needed.
- During sync, no back-propagation of tombstones. Tombstones propagate via normal hash-list diff in the next sync round. Simpler protocol.
- Debug-level logging for tombstone rejections (blob hash + namespace). Useful for diagnosing sync behavior.
- READ returns 'not found' for tombstoned blobs. No distinction between never-existed and deleted. No information leakage.

### liboqs build optimization (BUILD-01)
- CMake flags approach: -DOQS_ENABLE_SIG_ML_DSA=ON etc., disable everything else. Clean, maintainable, upstream-supported.
- Separate plan (12-02), independent from tombstone work (12-01). Can be verified separately.
- No specific build time target — strip everything unused, minimize as much as possible.
- Also strip liboqs test/benchmark targets. chromatindb tests at its own level.

### Claude's Discretion
- Magic prefix byte sequence choice
- Tombstone storage layout within existing sub-databases
- Exact FlatBuffer schema changes (if any)
- Sync protocol message ordering details
- CMake flag enumeration for liboqs stripping

</decisions>

<specifics>
## Specific Ideas

- DELETE command mirrors WRITE semantics: idempotent, pre-emptive allowed, same ack pattern
- Tombstoned blobs are invisible to clients — "not found" response, no deletion history exposed
- Tombstone propagation relies entirely on existing sync mechanism (no special sync path)

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `BlobData` struct (`db/wire/codec.h`): namespace_id, pubkey, data, ttl, timestamp, signature — tombstones use this as-is with magic prefix in data
- `BlobEngine::ingest()` (`db/engine/engine.h`): fail-fast validation pipeline (structural → namespace → signature → store) — extend with tombstone detection
- `WriteAck` / `IngestResult` pattern: DELETE_ACK can mirror this for consistent response handling
- `SyncProtocol` (`db/sync/sync_protocol.h`): hash-list diff + one-blob-at-a-time transfer — tombstones flow through unchanged
- `Storage` (`db/storage/storage.h`): 3 sub-databases (blobs, sequence, expiry) — tombstones stored in blobs, no expiry entry (TTL=0)

### Established Patterns
- Content-addressing via SHA3-256 of encoded FlatBuffer — tombstones use same path
- Canonical signing input: `SHA3-256(namespace||data||ttl||timestamp)` — tombstone signing uses same canonical form
- Fail-fast validation: cheapest check first (Step 0 pattern) — tombstone check should be early in ingest pipeline
- Dedup via 'duplicate' status — reuse for idempotent delete and tombstone-blocked writes

### Integration Points
- Wire protocol (`db/wire/transport_generated.h`): new DELETE / DELETE_ACK message types in TransportMsgType enum
- `BlobEngine::ingest()`: add tombstone-exists check before storing regular blobs
- `Storage`: new method to check tombstone existence by target hash; method to delete blob by hash
- `SyncProtocol::ingest_blobs()`: handle tombstone blobs — delete target blob, store tombstone
- `CMakeLists.txt`: liboqs FetchContent block — add algorithm-stripping CMake flags

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 12-blob-deletion*
*Context gathered: 2026-03-07*
