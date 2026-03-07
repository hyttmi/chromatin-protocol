# Phase 11: Larger Blob Support - Context

**Gathered:** 2026-03-07
**Status:** Ready for planning

<domain>
## Phase Boundary

Bump blob limit to 100 MiB with safe sync and transport. Nodes accept, store, and sync blobs up to 100 MiB without memory exhaustion or sync failure. Oversized blobs and malformed frame headers are rejected before resource allocation.

</domain>

<decisions>
## Implementation Decisions

### Transport strategy
- Raise MAX_FRAME_SIZE to accommodate 100 MiB blobs (no chunking/streaming)
- Single frame per blob — keep the existing monolithic send/receive pattern
- Both layers validate size: transport rejects frames > MAX_FRAME_SIZE (BLOB-05), engine rejects blob data > 100 MiB (BLOB-01)

### Blob size limit
- MAX_BLOB_DATA_SIZE is a constexpr protocol invariant (like TTL), not configurable
- Enforced at ingest as Step 0 — before structural checks, namespace verification, or signature verification
- This is the cheapest possible check (one integer comparison)

### Sync flow — blob transfer
- One blob per message during Phase C transfer (not batched)
- For each missing hash: send request, receive single blob, ingest, repeat
- Bounded memory: only one blob in flight per direction at a time
- Hash lists (Phase A/B) remain all-at-once — hashes are 32 bytes each, even 10K blobs = 320 KB

### Memory & buffer allocation
- Allocate receive buffer upfront after validating declared frame size (current pattern, just raised limit)
- Frame size limit IS the memory cap — no separate per-connection memory budget needed
- Full blob loaded in memory for signature verification (already in recv buffer, SHA3-256 is fast on 100 MiB)

### Malformed frame handling
- Disconnect immediately + record strike when a peer declares a frame length > MAX_FRAME_SIZE
- No error response sent — oversized frame declaration is either a bug or an attack
- Matches existing strike system behavior

### Timeout scaling
- Per-blob timeout during sync Phase C (not per-session)
- On timeout: skip the blob, log warning, continue syncing remaining blobs
- Timed-out blobs retry on the next sync cycle
- No strike for timeouts — could be slow network, not malice

### Claude's Discretion
- Exact MAX_FRAME_SIZE value (calculate minimum for 100 MiB blob + FlatBuffer overhead + AEAD tag, add reasonable headroom)
- Hash index implementation for BLOB-03 (store hashes to avoid loading blob data during collect_namespace_hashes)
- New sync message types vs reusing existing types for one-blob-at-a-time transfer
- Timeout formula (size-based vs adaptive, min/max bounds)
- Whether sync phases A/B also get explicit timeouts

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/net/framing.h`: MAX_FRAME_SIZE constexpr, write_frame/read_frame — needs value bump
- `db/net/connection.cpp`: recv_raw() already validates frame length before allocating — just needs the limit raised
- `db/engine/engine.cpp`: BlobEngine::ingest() validation pipeline — needs size check inserted as Step 0
- `db/sync/sync_protocol.h/cpp`: SyncProtocol with encode/decode helpers — needs one-blob-at-a-time transfer mode
- `db/peer/peer_manager.h`: Strike system (STRIKE_THRESHOLD, record_strike) — reuse for oversized frame rejection
- `db/wire/codec.h`: wire::BlobData, encode_blob, decode_blob, blob_hash — used by sync

### Established Patterns
- Sequential sync protocol (Phase A/B/C) avoids TCP deadlock — extend, don't restructure
- Timer-cancel pattern for async message queues (PeerInfo::sync_inbox) — reuse for blob request/response
- Constexpr protocol invariants (TTL) — MAX_BLOB_DATA_SIZE follows this pattern
- Fail-fast validation in engine (cheap → expensive) — size check goes first

### Integration Points
- `db/net/framing.h` — MAX_FRAME_SIZE value change affects all connections
- `db/engine/engine.cpp` — ingest() gains size check before existing Step 1
- `db/sync/sync_protocol.cpp` — collect_namespace_hashes() must stop loading full blobs
- `db/sync/sync_protocol.cpp` — encode/decode_blob_transfer() replaced by single-blob messages
- `db/peer/peer_manager.cpp` — sync orchestration (run_sync_with_peer) needs one-at-a-time loop + per-blob timeout
- `db/storage/storage.h` — may need hash-in-index storage for BLOB-03
- `db/wire/transport.fbs` — may need new message types for blob request/response

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 11-larger-blob-support*
*Context gathered: 2026-03-07*
