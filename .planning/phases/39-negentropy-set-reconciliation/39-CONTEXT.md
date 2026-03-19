# Phase 39: Set Reconciliation - Context

**Gathered:** 2026-03-19
**Status:** Ready for planning

<domain>
## Phase Boundary

Replace the O(N) full hash list exchange in sync Phase B with a custom range-based set reconciliation protocol, making sync cost proportional to differences (O(diff)) not total blobs (O(N)), and eliminating the ~3.4M blob MAX_FRAME_SIZE cliff. Negentropy library dropped in favor of a custom implementation — zero external dependency, native SHA3-256 if needed, ~400-500 lines of owned code.

</domain>

<decisions>
## Implementation Decisions

### Reconciliation algorithm
- Custom XOR-fingerprint range-based set reconciliation (NOT negentropy)
- Both sides sort their 32-byte blob hashes. Exchange XOR fingerprints for ranges. If fingerprints match → range identical, skip. If not → split range in half, recurse
- XOR fingerprint: XOR of all hashes in a range. SHA3-256 blob hashes are uniformly distributed, XOR gives excellent collision resistance with zero hash function calls
- Split threshold: 16 items — below this, send items directly instead of recursing (16 * 32 = 512 bytes vs one more round-trip)
- Initiator drives all reconciliation rounds (matches existing initiator-driven sync pattern)

### Wire protocol
- 3 new message types: ReconcileInit (27), ReconcileRanges (28), ReconcileItems (29)
- ReconcileInit: starts reconciliation for a namespace, first byte of payload is version byte (SYNC-09 forward compatibility)
- ReconcileRanges: range fingerprints exchanged back and forth during recursion
- ReconcileItems: below-threshold items sent directly (resolved leaf ranges)
- HashList (12) removed entirely from enum and all code — clean break, pre-MVP no backward compat needed
- Remove all HashList encode/decode helpers and tests

### Sync flow integration
- Replace Phase B only — Phase A (namespace list exchange + cursor logic) and Phase C (blob transfer one-at-a-time) stay unchanged
- After reconciliation resolves missing hashes, stream diffs into Phase C blob requests (interleaving strategy at Claude's discretion)
- Output of reconciliation feeds into existing BlobRequest/BlobTransfer flow

### Cursor integration
- Cursor-hit namespaces still skip reconciliation entirely (unchanged)
- Cursor-miss triggers reconciliation on the FULL hash set for that namespace (not incremental since-seq)
- After successful reconciliation + blob transfer, store peer's advertised seq_num (unchanged cursor update logic)
- Full resync mechanism unchanged — full resync = reconcile all namespaces regardless of cursor

### Claude's Discretion
- Internal module structure (single file vs separate reconciliation.h/.cpp)
- Exact message interleaving strategy for streaming diffs into Phase C
- How sorted hash vectors are built and cached during reconciliation
- Whether to offload XOR fingerprint computation to thread pool (likely unnecessary — XOR is trivially fast)

</decisions>

<specifics>
## Specific Ideas

- XOR fingerprint is trivially fast — no hash function needed, just bitwise XOR over 32-byte arrays in a range. This avoids the SHA-256 vs SHA3-256 patching problem that made negentropy undesirable.
- The algorithm is essentially binary search on sorted arrays with XOR checksums — conceptually simple, well-understood, deterministic.
- Negentropy was dropped specifically to avoid the SHA3-256 patching hassle. The custom approach is comparable in code size (~400-500 lines vs negentropy's 1000+ header) with zero dependency management.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `SyncProtocol::collect_namespace_hashes()` (db/sync/sync_protocol.h:50): Reads seq_map to build hash vectors — reusable as the input to reconciliation
- `SyncProtocol::diff_hashes()` (db/sync/sync_protocol.h:54): Static set-difference — may be replaced by reconciliation output
- `SyncProtocol::encode/decode_hash_list()`: Will be REMOVED (HashList message type deleted)
- `SyncProtocol::encode/decode_blob_transfer()`: Stays unchanged (Phase C)
- `SyncProtocol::encode/decode_namespace_list()`: Stays unchanged (Phase A)

### Established Patterns
- Sync message encode/decode as static methods on SyncProtocol
- `run_sync_with_peer()` coroutine in PeerManager with Phase A/B/C structure
- `recv_sync_msg()` with timeout for waiting on peer responses
- One-blob-at-a-time transfer with `BLOB_TRANSFER_TIMEOUT` (120s) vs `SYNC_TIMEOUT` (30s)
- `peer->sync_inbox` message queue for sync messages

### Integration Points
- `PeerManager::run_sync_with_peer()` (db/peer/peer_manager.cpp:627): Phase B section (lines ~664-767) replaced with reconciliation calls
- `PeerManager::on_peer_message()`: Needs to route new message types (27, 28, 29) to sync_inbox
- `transport.fbs`: Add ReconcileInit=27, ReconcileRanges=28, ReconcileItems=29; remove HashList=12
- `SyncProtocol` class: Add reconciliation encode/decode methods, or new Reconciliation class
- `db/PROTOCOL.md`: Update Phase B documentation

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 39-negentropy-set-reconciliation*
*Context gathered: 2026-03-19*
