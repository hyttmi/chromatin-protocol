# Phase 109: New Features - Context

**Gathered:** 2026-04-13
**Status:** Ready for planning

<domain>
## Phase Boundary

Three features: (1) source exclusion for client notifications at both node and relay layers, (2) configurable blob size limit at the relay, (3) HTTP /health endpoint on the relay. Node code is NOT frozen for this phase — source exclusion requires a one-line fix in the node's on_blob_ingested() path.

</domain>

<decisions>
## Implementation Decisions

### Source Exclusion — Node Layer
- **D-01:** In `blob_push_manager.cpp` `on_blob_ingested()`, add source exclusion to the Notification(21) fan-out loop (lines 82-90). Currently only BlobNotify(59) has `if (peer->connection == source) continue;`. Add the same check for Notification(21): `if (peer->connection == source) continue;`.
- **D-02:** This is the root fix — if the node doesn't send the echo, relay doesn't need to filter it for UDS-connected clients.

### Source Exclusion — Relay Layer
- **D-03:** The relay must also suppress notification echo for relay-connected clients. When client A writes a blob via WsSession, the relay forwards Data(8) to the node via UDS. The node's WriteAck comes back with the request_id, so the relay knows which session wrote it. But the Notification(21) arrives with request_id=0 — the relay doesn't inherently know which session caused it.
- **D-04:** Solution: when the relay receives a WriteAck (type 30) for a Data(8) or Delete(17), extract the blob_hash from the WriteAck payload and record `{blob_hash -> writer_session_id}` in a short-lived map. When the corresponding Notification(21) arrives, extract the blob_hash from the notification payload, look up the writer session, and skip it during fan-out. Expire entries after 5 seconds (notifications arrive within milliseconds of WriteAck).
- **D-05:** New class: `WriteTracker` in `relay/core/write_tracker.h` — simple `unordered_map<array<uint8_t,32>, uint64_t>` (blob_hash -> session_id) with timestamp-based expiry.

### Blob Size Limit
- **D-06:** New config field `max_blob_size_bytes` (uint32_t, default 0 = no limit). SIGHUP-reloadable.
- **D-07:** Relay checks incoming Data(8) JSON `data` field size (after base64 decode length calculation) against the limit BEFORE forwarding to the node. Reject with JSON error `{"type": "error", "code": "blob_too_large", "max_size": N}`.
- **D-08:** This is a relay-only feature — the node already has MAX_BLOB_DATA_SIZE (100 MiB). The relay limit is meant to be lower (operator-configurable).

### Health Endpoint
- **D-09:** Add `/health` route to the existing MetricsCollector HTTP acceptor. No new listener needed.
- **D-10:** Response: `200 OK` with `{"status": "healthy", "relay": "ok", "node": "connected"}` when UDS is connected. `503 Service Unavailable` with `{"status": "degraded", "relay": "ok", "node": "disconnected"}` when UDS is down.
- **D-11:** Content-Type: `application/json`. Minimal JSON — no metrics, no version info. Keep it simple for load balancer probes.

### Claude's Discretion
- Whether WriteTracker should be a standalone class or inline in UdsMultiplexer
- Whether blob_too_large should be a new error code (7) in error_codes.h or relay-only
- Whether /health should include uptime or connection count

</decisions>

<canonical_refs>
## Canonical References

### Source exclusion — Node (one-line fix)
- `db/peer/blob_push_manager.cpp:42-91` — on_blob_ingested(): BlobNotify has source exclusion (line 65), Notification(21) does NOT (line 82-90)

### Source exclusion — Relay (WriteTracker)
- `relay/core/uds_multiplexer.cpp:505-534` — route_response(): WriteAck arrives here with request_id, has pending session info
- `relay/core/uds_multiplexer.cpp:568-597` — handle_notification(): fan-out to all subscribers, no source filtering
- `relay/core/request_router.h` — PendingRequest struct (has client_session_id)

### Blob size limit
- `relay/ws/ws_session.cpp` — handle_client_message() where incoming Data(8) is processed
- `relay/config/relay_config.h` — RelayConfig struct (add max_blob_size_bytes)
- `db/net/framing.h:16` — MAX_BLOB_DATA_SIZE = 100 MiB (node-side limit, for reference)

### Health endpoint
- `relay/core/metrics_collector.h/cpp` — existing HTTP acceptor, /metrics route
- `relay/core/uds_multiplexer.h` — connected_ state (for node health check)

### Requirements
- `FEAT-01` — Source exclusion for notifications
- `FEAT-02` — Configurable blob size limits
- `FEAT-03` — HTTP /health endpoint

</canonical_refs>

<code_context>
## Existing Code Insights

### Source Exclusion
- BlobNotify(59) already has perfect source exclusion: `if (peer->connection == source) continue;` (line 65)
- The `source` parameter is a `net::Connection::Ptr` passed through from the ingest call site
- Notification(21) loop (line 82-90) iterates the same `peers_` list but doesn't check source — literal copy-paste of the BlobNotify pattern minus the source check

### Relay Notification Fan-out
- `handle_notification()` gets the Notification from UDS with request_id=0 — no way to know the writer
- `route_response()` handles WriteAck with request_id > 0 — can resolve to PendingRequest with session_id
- WriteAck payload: [blob_hash:32][seq_num:8BE][status:1] — blob_hash is at offset 0

### Health Endpoint
- MetricsCollector already has a coroutine HTTP accept loop, parses GET requests, returns text
- Adding /health is a second route check in the same handler — minimal code

</code_context>

<specifics>
## Specific Ideas

- The node fix is literally one line: add `if (peer->connection == source) continue;` before the Notification send
- WriteTracker expiry can use the same steady_clock pattern as other relay timers
- Blob size check should happen BEFORE base64 decode — calculate decoded size from base64 length to avoid wasting memory on huge payloads

</specifics>

<deferred>
## Deferred Ideas

None — all three features are well-scoped

</deferred>

---

*Phase: 109-new-features*
*Context gathered: 2026-04-13*
