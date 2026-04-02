# Project Research Summary

**Project:** chromatindb v2.0.0 -- Event-Driven Architecture
**Domain:** Push-based sync, event-driven maintenance, and SDK resilience for a decentralized PQ-secure database node
**Researched:** 2026-04-02
**Confidence:** HIGH

## Executive Summary

The v2.0.0 milestone replaces chromatindb's timer-paced sync model with push-based peer notifications and event-driven maintenance. The core change is surgical: when a blob is ingested, notify all connected peers immediately (BlobNotify), let them fetch individual blobs on demand (BlobFetchRequest/Response), and fall back to the existing XOR-fingerprint reconciliation as a safety net at 10-15 minute intervals. On the SDK side, auto-reconnect with jittered exponential backoff and subscription restoration make the Python client resilient to transient failures. All of this is achievable with zero new dependencies -- every capability exists in the current stack (Standalone Asio, libmdbx, asyncio).

The single most critical risk is AEAD nonce desync from concurrent sends on the same connection. The current system maintains a strict invariant: at most one coroutine writes to a connection at any time. Push notifications, keepalive pings, and targeted fetch responses break this invariant by introducing multiple send paths. The fix -- a per-connection send queue that serializes all outgoing frames -- must be implemented before any push notification feature ships. The Python SDK already has this pattern (`_send_lock`); the C++ node needs it too. A secondary critical risk is notification storms during reconcile-on-connect: when a peer catches up on thousands of missed blobs, each ingested blob triggers a notification to all other peers. Suppressing notifications during sync ingestion prevents this cascade.

The recommended approach is seven phases: (1) connection send queue + push notification infrastructure, (2) targeted blob fetch completing the push loop, (3) event-driven expiry replacing the periodic scan, (4) reconcile-on-connect formalization + safety-net timer + cursor cleanup, (5) bidirectional keepalive, (6) SDK auto-reconnect with subscription restoration, (7) documentation refresh. Phases 3, 5, and 6 are independent and can be built in parallel with phases 1-2 and 4.

## Key Findings

### Recommended Stack

Zero new dependencies. Every v2.0.0 capability is provided by the existing stack. No new C++ libraries, no new pip packages.

**Core technologies (all existing, no changes):**
- **Standalone Asio 1.38.0:** `steady_timer` with dynamic `expires_after()` for event-driven expiry, `co_spawn` with `detached` for per-peer notification fan-out. All features are stable, non-experimental.
- **libmdbx:** `cursor.to_first()` on the existing `expiry_map` gives O(1) earliest-expiry lookup. No schema changes, no new sub-databases.
- **FlatBuffers:** Add 3 enum values to `transport.fbs` (BlobNotify=59, BlobFetch=60, BlobFetchResponse=61). Regenerate headers. Wire-incompatible change; acceptable pre-v2.
- **Python asyncio + stdlib:** `asyncio.sleep()` + `random.uniform()` for jittered exponential backoff. No `tenacity`, no `backoff` library.

**Explicitly rejected:** `asio::experimental::channel` (unnecessary for single-threaded dispatch, experimental API), `asio::system_timer` (wall-clock affected by NTP), any new pip dependencies for retry/backoff logic.

### Expected Features

**Must have (table stakes):**
- Push notification on blob ingest -- core value prop of event-driven sync
- Targeted blob fetch from notification -- completes the push-then-pull loop
- Reconcile-on-connect -- catchup after downtime (already exists, preserve)
- Safety-net periodic reconciliation (10-15 min) -- correctness backstop
- Bidirectional keepalive (Ping/Pong) -- dead connection detection in under 30s
- SDK auto-reconnect with exponential backoff -- transparent recovery
- SDK subscription restoration on reconnect -- pub/sub continuity
- Event-driven expiry (next-expiry timer) -- replace periodic full-table scan
- Disconnect-triggered cursor cleanup -- replace 6h timer

**Should have (differentiators):**
- Event-driven expiry is technically a differentiator (most systems use periodic scan), but included in v2.0.0 because the pattern is clean and the existing expiry_map already provides sorted-by-timestamp ordering

**Defer (v2.x+):**
- Notification coalescing/batching -- only if bulk ingest throughput is an issue
- Per-peer notification deduplication -- only if notification volume becomes a problem
- Configurable heartbeat interval -- YAGNI until operator requests
- Gossip-based propagation -- only if peer count exceeds 32
- Guaranteed delivery with hint storage -- only if safety-net proves insufficient

**Anti-features (do NOT build):**
- Full-state push (blob data in notification) -- blobs can be 100 MiB, push metadata only
- Causal ordering / vector clocks -- overkill for advisory notifications
- Transparent write retry in SDK -- writes are not idempotent in user intent
- Connection-scoped push state (per-peer bloom filters) -- complexity not justified at 3-32 nodes

### Architecture Approach

The architecture is a surgical extension of the existing single-threaded Asio event loop. PeerManager gains three new capabilities: (1) `notify_all_peers()` fan-out after every blob ingest, with source exclusion and UDS skip; (2) inline BlobFetch handling without requiring a sync session; (3) a keepalive coroutine sending Ping to all TCP peers. Storage gains one new API: `next_expiry_time()` (O(1) cursor seek on existing expiry_map). The SDK gains reconnect logic in ChromatinClient (not Transport, because reconnection requires a new TCP connection and new PQ handshake).

**Major components (new or modified):**
1. **Connection send queue** -- serializes all outgoing frames per connection, guaranteeing AEAD nonce ordering. Single writer coroutine drains a deque. This is the foundational safety mechanism.
2. **BlobNotify/BlobFetch/BlobFetchResponse** (wire types 59-61) -- push notification metadata (77 bytes, reuses Notification payload format), targeted single-blob fetch (64-byte request), and response with found/not-found status byte.
3. **`notify_all_peers()`** -- broadened-scope fan-out (all TCP peers, not just subscribers). Skips source peer and UDS connections. Also calls existing `notify_subscribers()` for backward-compatible client pub/sub.
4. **Event-driven expiry timer** -- `expiry_timer_loop()` using `Storage::next_expiry_time()` to set dynamic deadline instead of fixed 60s interval. Timer-cancel pattern for wake on new blob ingest.
5. **SDK auto-reconnect** -- jittered exponential backoff (1s base, 2x growth, 60s cap, 10-25% jitter) in ChromatinClient. Re-subscribes on reconnect. Emits reconnection event so application can catch up.
6. **Relay blocklist update** -- block BlobNotify, BlobFetch, BlobFetchResponse (peer-internal, not client-facing).

### Critical Pitfalls

1. **AEAD nonce desync from concurrent sends (CRITICAL)** -- Push notifications, keepalive pings, and fetch responses create multiple send paths per connection. Two coroutines calling `send_encrypted()` interleaved at co_await points produce out-of-order frames on the wire. **Fix:** Per-connection send queue (deque + single writer coroutine). Must be implemented FIRST, before any push feature.

2. **Notification storm during reconcile-on-connect (CRITICAL)** -- A reconnecting peer catching up on 10,000 blobs triggers 10,000 notifications to all other peers. SDK notification queue (maxsize=1000) overflows silently. **Fix:** Suppress peer notifications during sync ingestion. Only client-originated writes (Data/Delete) trigger `notify_all_peers()`.

3. **Event-driven expiry timer stall (CRITICAL)** -- If the timer fires and nobody rearms it, expired blobs accumulate until the safety-net scan. **Fix:** Always rearm in the expiry handler. Keep the safety-net periodic scan as a correctness backstop. Do NOT remove the periodic scan.

4. **SDK auto-reconnect resurrects stale state (CRITICAL)** -- Connection-scoped state (subscriptions, request IDs, nonces) cannot be transparently restored. **Fix:** Auto-reconnect must NOT be fully transparent. Emit a reconnection event. Re-subscribe automatically but notify the application. Reset request_id to 1.

5. **Cursor cleanup on disconnect removes warm cursors (MODERATE)** -- Immediate cursor compaction means a peer that reconnects 2 seconds later triggers a full reconciliation. **Fix:** Grace period before compaction (5 minutes). Preserve cursors for bootstrap peers.

## Implications for Roadmap

Based on combined research, the suggested structure is 7 phases. The dependency graph is: Phase 1 -> Phase 2 -> Phase 4; Phases 3, 5, 6 are independent; Phase 7 depends on all.

### Phase 1: Connection Send Queue + Push Notification Infrastructure

**Rationale:** The send queue is a prerequisite for every subsequent feature that sends messages (notifications, keepalive, fetch responses). Without it, AEAD nonce desync makes the system worse than what it replaces. Push notification wire types (BlobNotify/BlobFetch/BlobFetchResponse) are added here because everything else depends on them existing.
**Delivers:** Per-connection send queue in C++ Connection class. BlobNotify/BlobFetch/BlobFetchResponse in transport.fbs. `notify_all_peers()` method wired into all ingest paths (client Data, client Delete, sync callback). Relay blocklist updated. NodeInfoResponse supported_types updated. Notification suppression during sync ingestion (prevents notification storm).
**Addresses:** Push notification on ingest (table stakes), relay filter update.
**Avoids:** AEAD nonce desync (Pitfall 1), notification storm (Pitfall 2), relay passthrough of peer-internal types (Pitfall 5).

### Phase 2: Targeted Blob Fetch

**Rationale:** Completes the push-then-pull loop. Depends on Phase 1 message types and send queue. With this phase, peers can receive a notification and fetch the specific blob without waiting for full reconciliation.
**Delivers:** BlobFetch handler in `on_peer_message()` (inline coroutine, no sync session). BlobFetchResponse handler that ingests via `BlobEngine::ingest()` (full validation). BlobNotify handler with `has_blob()` dedup check. Source exclusion to prevent notification echo. Dedup guard: skip BlobNotify from syncing peers.
**Addresses:** Targeted blob fetch (table stakes).
**Avoids:** Validation bypass on fetch (Pitfall 10), sync rate limiter bypass (Pitfall 7), fetch loop without local check (anti-pattern).

### Phase 3: Event-Driven Expiry

**Rationale:** Independent of push sync. Can be built in parallel with Phases 1-2. Replaces the 60s periodic full-table scan with a precision timer targeting the next expiring blob. The existing expiry_map already provides sorted-by-timestamp ordering, so this is O(1) to implement.
**Delivers:** `Storage::next_expiry_time()` method (cursor.to_first() on expiry_map). Rewritten `expiry_timer_loop()` with dynamic deadline. Timer-cancel wake on blob ingest with TTL. Safety-net periodic scan preserved at 10-15 min interval.
**Addresses:** Event-driven expiry (differentiator).
**Avoids:** Expiry timer stall (Pitfall 3), co_await between query and timer reset (Pitfall 14).

### Phase 4: Reconcile-on-Connect + Safety Net + Cursor Cleanup

**Rationale:** Depends on push sync working (Phase 2). Once push handles real-time propagation, the 60s sync timer becomes a 600s safety net. Cursor cleanup moves to disconnect-triggered with a grace period.
**Delivers:** Explicit reconcile-on-connect (formalize existing behavior). `safety_net_reconciliation_seconds` config (default 600). Disconnect-triggered cursor compaction with 5-minute grace period. Removal of `cursor_compaction_loop()`.
**Addresses:** Reconcile-on-connect (preserve), safety-net reconciliation (reconfigure), disconnect-triggered cursor cleanup.
**Avoids:** Cursor loss on transient disconnect (Pitfall 6), safety-net timer drift (Pitfall 11).

### Phase 5: Bidirectional Keepalive

**Rationale:** Independent of push sync but enhances reliability. Depends on the send queue (Phase 1) being in place -- keepalive Ping is another send path that would race without the queue. Can be built in parallel with Phases 3-4.
**Delivers:** `keepalive_loop()` coroutine in PeerManager (30s interval, Ping to all TCP peers). Wired into `cancel_all_timers()` and `stop()`. Existing receiver-side inactivity timeout (120s) remains unchanged -- keepalive ensures regular traffic so dead connections are detected faster.
**Addresses:** Bidirectional keepalive (table stakes).
**Avoids:** Keepalive racing with notifications (Pitfall 9 -- resolved by send queue from Phase 1).

### Phase 6: SDK Auto-Reconnect

**Rationale:** SDK-only work, independent of C++ phases. Can be built in parallel with Phases 3-5. Building it after the server-side architecture is stable means the reconnect behavior is tested against the final system.
**Delivers:** Reconnect loop in ChromatinClient with jittered exponential backoff (1s-60s). Subscription restoration via re-Subscribe on new connection. Reconnection event emission (not fully transparent). `_intentional_close` flag to distinguish close() from connection loss. One retry after reconnect for read operations; writes surface errors to caller.
**Addresses:** SDK auto-reconnect (table stakes), SDK subscription restoration (table stakes).
**Avoids:** Stale subscription state (Pitfall 4), transparent retry of non-idempotent writes (anti-feature).

### Phase 7: Documentation Refresh

**Rationale:** Documents the final state after all behavior is implemented and tested. Depends on all previous phases.
**Delivers:** PROTOCOL.md updates (BlobNotify/BlobFetch/BlobFetchResponse wire formats, updated sync model, keepalive spec, notification ordering semantics). README.md architecture description. SDK README auto-reconnect API. Tutorial reconnect handling.
**Addresses:** Documentation refresh (table stakes).
**Avoids:** Ordering guarantees not specified (Pitfall 8).

### Phase Ordering Rationale

- Phase 1 first because the send queue is a safety prerequisite for all concurrent send paths. Push notification wire types are needed by everything downstream. Notification suppression during sync prevents storms from day one.
- Phase 2 before Phase 4 because the safety-net timer interval increase (60s to 600s) only makes sense once push sync is working as the primary propagation mechanism.
- Phases 3, 5, 6 are independent and can be sequenced in any order or built in parallel. Phase 3 (event-driven expiry) is purely Storage + PeerManager timer work. Phase 5 (keepalive) is a simple timer coroutine. Phase 6 (SDK reconnect) is pure Python.
- Phase 7 last because documentation should reflect final implemented behavior, not planned behavior.

### Research Flags

Phases likely needing deeper research during planning:
- **Phase 1 (Send Queue + Push Infra):** The send queue is the most impactful structural change to the C++ Connection class in 78 phases. Needs careful design -- queue depth limits, backpressure behavior, interaction with `close()` and shutdown. Recommend `/gsd:research-phase` before planning.
- **Phase 6 (SDK Auto-Reconnect):** Reconnection touches transport, handshake, and client layers. The interaction between reconnect events, Directory cache invalidation, and application-level catch-up needs careful API design. Recommend `/gsd:research-phase`.

Phases with standard patterns (skip research-phase):
- **Phase 2 (Targeted Fetch):** Well-documented pattern. Inline handler in `on_peer_message()`, same as ReadRequest/ExistsRequest. Uses existing `BlobEngine::ingest()` pipeline.
- **Phase 3 (Event-Driven Expiry):** Timer-cancel pattern already used for 8+ coroutines. `Storage::next_expiry_time()` is a trivial cursor seek. No new patterns.
- **Phase 4 (Reconcile + Safety Net):** Config changes and timer interval adjustment. Cursor cleanup is a method call in an existing callback. Grace period is a simple steady_timer.
- **Phase 5 (Keepalive):** Identical to existing timer-loop coroutines. Existing Ping/Pong wire types. No new patterns.
- **Phase 7 (Docs):** Standard documentation work.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | Zero new dependencies. All capabilities verified against existing codebase and Asio 1.38.0 docs. Patterns proven across 78 phases. |
| Features | HIGH | Feature landscape well-established across CouchDB, Syncthing, Cassandra, IPFS, Redis. Table stakes clear. Anti-features identified with rationale. |
| Architecture | HIGH | Analysis based on direct codebase inspection. All component boundaries, data flows, and wire formats specified against actual source code. |
| Pitfalls | HIGH | Pitfalls derived from known past bugs (v1.0.0 PEX SIGSEGV, AEAD nonce desync history) and coroutine scheduling analysis. The nonce race is the real risk. |

**Overall confidence:** HIGH

### Gaps to Address

- **Send queue backpressure policy:** What happens when a slow peer's send queue grows unbounded? Need a queue depth limit and a disconnect-on-overflow policy. Address during Phase 1 planning.
- **Notification gap detection on SDK reconnect:** The SDK can re-subscribe after reconnect, but there is no built-in mechanism to detect what was missed during the disconnect window. `TimeRangeRequest` or `ListRequest` can serve as catch-up, but the SDK API for this needs design. Address during Phase 6 planning.
- **BlobFetch concurrency per peer:** If a peer receives 100 BlobNotify messages in rapid succession, how many concurrent BlobFetch requests should it send? Needs a concurrency limit (e.g., 4 in-flight fetches per peer). Address during Phase 2 planning.
- **Grace period interaction with bootstrap peers:** Bootstrap peers should probably never have their cursors compacted, even after disconnect + grace period expiry. Needs explicit handling. Address during Phase 4 planning.

## Sources

### Primary (HIGH confidence)
- Standalone Asio 1.38.0 documentation -- steady_timer, co_spawn, use_awaitable
- libmdbx GitHub repository -- cursor.to_first() B-tree traversal
- Direct codebase inspection: peer_manager, connection, storage, sync_protocol, SDK transport/client
- Project history: v1.0.0 PEX SIGSEGV root cause, AEAD nonce desync decisions, 78 phases of coroutine patterns

### Secondary (MEDIUM confidence)
- CouchDB Replication Protocol -- changes feed, continuous replication patterns
- Syncthing BEP v1 Protocol -- Index Update messages, reconnect behavior
- IPFS Bitswap Protocol -- wantlist, demand-driven fetch patterns
- AWS Exponential Backoff And Jitter -- Full Jitter algorithm for SDK reconnect
- gRPC Connection Backoff Protocol -- jittered exponential backoff spec
- RabbitMQ Heartbeats -- TCP keepalive vs application-level heartbeat comparison
- RFC 9771 Properties of AEAD Algorithms -- nonce reuse consequences

### Tertiary (LOW confidence)
- None. All findings are supported by primary or secondary sources.

---
*Research completed: 2026-04-02*
*Ready for roadmap: yes*
