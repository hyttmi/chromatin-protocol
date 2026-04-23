# Phase 129: Sync Cap Divergence — Context

**Gathered:** 2026-04-23 (inline, lean execution)
**Status:** Ready for planning

<domain>
## Phase Boundary

Nodes with divergent `blob_max_bytes` caps replicate cleanly. A node never offers a blob that the peer's advertised cap cannot accept. Operators can see partial-replication situations in `/metrics`.

**In scope:**
- PeerInfo snapshots the peer's advertised `max_blob_data_bytes` from NodeInfoResponse at handshake (session-constant, no mid-session renegotiation).
- Sync-out filter applies uniformly to three paths:
  1. PULL set-reconciliation announce (announce-side filter before building fingerprint set).
  2. PUSH `BlobNotify` fan-out (skip peer if `blob_size > peer.advertised_blob_cap`).
  3. Direct `BlobFetch` response (return empty/not-available instead of oversized blob).
- `chromatindb_sync_skipped_oversized_total{peer=...}` labeled counter increments once per skip.
- Unit tests covering the four cap-divergence scenarios (smaller / larger / equal / zero).
- UAT file for the 2-node integration scenario (VERI-05) — user runs via `cdb --node home` / `--node local`.

**Out of scope:**
- Mid-session cap renegotiation (REQUIREMENTS.md explicitly: cap is session-constant at handshake).
- Re-syncing blobs after a peer raises its cap post-reconnect (next connect snapshots the new cap, existing stored blobs do not backfill).
- CLI auto-tuning from advertised cap → Phase 130.
- Protocol doc refresh → Phase 131 DOCS-01.

</domain>

<decisions>
## Implementation Decisions

### Capability exchange (SYNC-01)
- **D-01:** Add `uint64_t advertised_blob_cap = 0;` field to `PeerInfo` (db/peer/peer_types.h). Default 0 means "unknown" — conservative interpretation: filter nothing (do not skip blobs when cap is unknown, to avoid silently halving replication if the peer is pre-129).
- **D-02:** When a peer sends `NodeInfoResponse` (in response to our `NodeInfoRequest`), decode the `max_blob_data_bytes` field (u64 BE at the offset Phase 127 established) and write it to the `PeerInfo::advertised_blob_cap` for that connection. This is a post-handshake exchange, not a handshake-protocol change — no TrustedHello modification.
- **D-03:** Peer-to-peer `NodeInfoRequest` is issued once per fresh connection, right after role-signalled handshake completes. The executor locates the correct insertion point in `db/peer/peer_manager.cpp` or `db/peer/connection_manager.cpp` (likely near where `SyncNamespaceAnnounce` is first sent).

### Filter semantics (SYNC-02, SYNC-03, SYNC-04)
- **D-04:** Filter rule is a single helper function: `should_skip_for_peer(blob_size, peer) -> bool { return peer.advertised_blob_cap > 0 && blob_size > peer.advertised_blob_cap; }`. Cap-unknown (0) means "do not skip" (D-01 conservatism).
- **D-05:** Filter applies to three enumerated sites; every skip increments `NodeMetrics::sync_skipped_oversized_total` with the peer's address as label.
- **D-06:** Counter is a labeled gauge-style counter (not a raw `NodeMetrics` u64) — emitted as `chromatindb_sync_skipped_oversized_total{peer="<address>"} <count>\n` in `format_prometheus_metrics`. Per-peer cardinality is bounded by `max_peers` config (default 32).
- **D-07:** On peer disconnect, the per-peer counter can either persist or reset. Decision: **persist** until process restart — aligns with the existing "monotonically increasing since startup, never reset" convention for other sync counters. Disconnect wipes the `PeerInfo`; on reconnect the counter starts fresh.

### Test strategy (VERI-03, VERI-05)
- **D-08:** VERI-03 unit tests use mock `PeerInfo` with fabricated `advertised_blob_cap` and directly exercise the 3 filter sites. Cheap, in-process.
- **D-09:** VERI-05 is delegated to manual user-run UAT: the test file is a markdown script describing `cdb --node home` + `--node local` actions, expected outcomes, and the `/metrics` scrape-delta to check. Full in-process 2-node integration is explicitly deferred (too expensive for this phase's budget; the user already runs a real 2-node deployment).

### Minimal test surface
- **D-10:** Four scenarios for VERI-03 per REQUIREMENTS.md: (a) peer cap smaller → blob skipped + counter increments, (b) peer cap larger → blob sent normally, (c) peer cap equal → blob sent (boundary: `>` not `>=`), (d) peer cap zero → blob sent (conservative "unknown cap" interpretation per D-01). Same 4 scenarios × 3 filter sites → 12 test cases. Share a fixture to minimize duplication.

### Claude's Discretion
- Exact insertion point for post-handshake `NodeInfoRequest` (somewhere between handshake completion and first sync round).
- Gauge label quoting style (Prometheus convention for IP:port strings).
- Whether the skip helper lives in a new utility header or inline in `blob_push_manager.cpp` (YAGNI: inline first).

</decisions>

<canonical_refs>
## Canonical References

### Project-level
- `.planning/REQUIREMENTS.md` — SYNC-01, SYNC-02, SYNC-03, SYNC-04, METRICS-03, VERI-03, VERI-05 (authoritative)
- `.planning/ROADMAP.md` §"Phase 129" — goal + 4 success criteria
- `.planning/PROJECT.md` — YAGNI, no backward compat, feedback_delegate_tests_to_user

### Phase-level carryover
- `.planning/phases/127-nodeinforesponse-capability-extensions/127-CONTEXT.md` — wire layout of the 4 advertised fields Phase 129 now consumes
- `.planning/phases/127-nodeinforesponse-capability-extensions/127-01-SUMMARY.md` — exact byte offset of `max_blob_data_bytes` in NodeInfoResponse
- `.planning/phases/128-configurable-blob-cap-frame-shrink-config-gauges/128-04-SUMMARY.md` — the `format_prometheus_metrics` extension pattern this phase mirrors for the labeled counter

### Codebase anchors
- `db/peer/peer_types.h:32-64` — `PeerInfo` struct (add `advertised_blob_cap` here)
- `db/peer/peer_types.h:70-90` — `NodeMetrics` struct (add `sync_skipped_oversized_total` — or handle via labeled counter, see D-06)
- `db/peer/blob_push_manager.cpp:63-85` — BlobNotify fan-out loop (SYNC-03 site 2)
- `db/peer/blob_push_manager.cpp:143-172` — BlobFetch handler (SYNC-03 site 3)
- `db/sync/reconciliation.cpp` — PULL announce / fingerprint set builder (SYNC-03 site 1)
- `db/peer/message_dispatcher.cpp:662-739` — NodeInfoRequest handler (reference for NodeInfoResponse wire layout to decode on receive)
- `db/peer/peer_manager.cpp` — peer handshake flow + post-handshake bootstrap (locate `NodeInfoRequest` peer-initiation insertion point here)
- `db/peer/metrics_collector.cpp` — `format_prometheus_metrics` extension target for the labeled counter
- `cli/src/commands.cpp:2279-2282` — reference for how `max_blob_data_bytes` is decoded from NodeInfoResponse (read_u64 at correct offset)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **Phase 127's NodeInfoResponse encoder** — wire layout is fixed; the receive-side decode mirrors `cli/src/commands.cpp:2279` pattern (read_u64 at the post-storage_max offset).
- **`format_prometheus_metrics` builder** — Phase 128 hand-wrote 24 gauges as `out += "# HELP ...\n# TYPE ...\nname value\n"`. Same pattern extends to a labeled counter, just with `{peer="..."}` label between name and value.
- **Strand-confined counters** — Phase 0.8.0 established that sync counters run on `ioc_` strand, no atomics needed. Applies to the new per-peer counter map.

### Constraints
- **No cap-unknown regression.** A pre-129 peer that never sent us a usable NodeInfoResponse has `advertised_blob_cap = 0`. D-01 says we do NOT skip blobs in that case. This preserves replication with older/malformed peers at the cost of "silent rejection by peer's ingest" becoming the authoritative enforcement. Acceptable: the receiving peer still rejects oversized blobs at its own ingest via Phase 128 enforcement. Phase 129 is an optimization, not a correctness gate.
- **Per-peer counter cardinality.** Prometheus labels can explode if peer addresses churn. Bounded by `max_peers` (default 32). Safe.

</code_context>

<deferred>
## Deferred Ideas

- **Cap-change signal.** Propagating a peer's SIGHUP-changed cap to its connected peers mid-session. Spec explicitly makes this out-of-scope (session-constant). Deferred; re-evaluate if operators file pain on this.
- **In-process 2-node integration test.** Could be added with a dual-`PeerManager` fixture in `db/tests/`. Deferred to future phase if manual UAT ever misses a regression.
- **Counter reset semantics.** Currently persists across disconnect. If operators want per-session counters, add a `_since_peer_connect` suffix variant. Not requested yet.

</deferred>
