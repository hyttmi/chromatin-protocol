---
plan: 129-01-peerinfo-snapshot-counter
phase: 129
type: execute
wave: 1
depends_on: []
files_modified:
  - db/peer/peer_types.h
  - db/peer/peer_manager.cpp
  - db/peer/peer_manager.h
  - db/peer/message_dispatcher.cpp
  - db/peer/message_dispatcher.h
  - db/peer/metrics_collector.cpp
  - db/peer/metrics_collector.h
autonomous: true
requirements: [SYNC-01, METRICS-03, SYNC-04]
must_haves:
  truths:
    - "PeerInfo carries a u64 advertised_blob_cap field; set once per connection from the peer's NodeInfoResponse, never mutated afterwards within the same session"
    - "When we are the initiator, we send TransportMsgType_NodeInfoRequest to a peer right after handshake completes and before the first sync round; the response is decoded and max_blob_data_bytes is copied into PeerInfo::advertised_blob_cap"
    - "When cap is unknown (0) the filter MUST NOT skip — conservative default, see CONTEXT.md D-01"
    - "NodeMetrics gains a map keyed by peer-identifier to a u64 counter (or equivalent per-peer record); sync_skipped_oversized_total increments via a helper on each skip"
    - "format_prometheus_metrics emits one line per peer: chromatindb_sync_skipped_oversized_total{peer=\"<address>\"} <count>; emitted alphabetically by peer label for scrape stability"
    - "No modifications to TrustedHello wire format. Capability exchange rides the existing NodeInfoRequest/NodeInfoResponse flow from Phase 127."
  artifacts:
    - path: "db/peer/peer_types.h"
      provides: "PeerInfo::advertised_blob_cap field (u64)"
    - path: "db/peer/peer_manager.cpp"
      provides: "post-handshake NodeInfoRequest emission + NodeInfoResponse decode path that snapshots max_blob_data_bytes"
    - path: "db/peer/metrics_collector.cpp"
      provides: "chromatindb_sync_skipped_oversized_total{peer=...} emission"
---

<objective>
Lay the infrastructure Wave 2 consumes: extend PeerInfo with the peer's advertised cap, wire a post-handshake NodeInfoRequest/Response exchange between peers, store the decoded cap in PeerInfo, and expose a per-peer sync-skip counter via Prometheus.
</objective>

<tasks>

<task type="auto" tdd="false">
  <name>Task 1: PeerInfo + NodeMetrics fields, helper counter record</name>
  <files>db/peer/peer_types.h, db/peer/metrics_collector.h</files>
  <read_first>
    - db/peer/peer_types.h (current PeerInfo at line 32; NodeMetrics at ~line 70 — match style)
    - db/peer/metrics_collector.h (understand what the collector owns so we know where the per-peer counter map lives)
  </read_first>
  <action>
    Edit `db/peer/peer_types.h`:
    1. In `struct PeerInfo` (starts line 32), add just above the sync-related fields:
       ```cpp
       // Advertised blob cap — snapshotted from NodeInfoResponse post-handshake (SYNC-01).
       // 0 = unknown/pre-v4.2.0 peer; filter MUST NOT skip when 0 (CONTEXT.md D-01 conservative default).
       uint64_t advertised_blob_cap = 0;
       ```
    2. `NodeMetrics` does NOT gain a new field. The per-peer skip counter is managed at the metrics-collector layer via a `std::map<std::string, uint64_t>` keyed by peer address. Rationale: `NodeMetrics` is a flat counter struct; labeled/per-peer counters don't fit that shape.

    Edit `db/peer/metrics_collector.h`:
    1. Add a new private member to `MetricsCollector`:
       ```cpp
       // Per-peer sync-skip counter (chromatindb_sync_skipped_oversized_total{peer="..."})
       // Strand-confined to io_context; no mutex needed (same invariant as node_metrics_).
       std::map<std::string, uint64_t> sync_skipped_oversized_per_peer_;
       ```
    2. Add a public method declaration:
       ```cpp
       void increment_sync_skipped_oversized(const std::string& peer_address);
       ```
    3. Do NOT touch any other member; this is an additive change.
  </action>
  <verify>
    <automated>
      grep -c '^\s*uint64_t advertised_blob_cap = 0;' db/peer/peer_types.h
      grep -c 'sync_skipped_oversized_per_peer_' db/peer/metrics_collector.h
      grep -c 'void increment_sync_skipped_oversized' db/peer/metrics_collector.h
      cmake --build build-debug -j$(nproc) --target chromatindb_lib 2>&1 | tail -3
    </automated>
  </verify>
  <acceptance_criteria>
    - Exactly one `advertised_blob_cap` line in `peer_types.h` with value `= 0`.
    - `sync_skipped_oversized_per_peer_` declared as `std::map<std::string, uint64_t>` in `metrics_collector.h`.
    - `increment_sync_skipped_oversized` public method declared.
    - No other existing struct members removed or renamed.
    - `chromatindb_lib` target compiles; the added header-only changes must not break the build.
  </acceptance_criteria>
  <done>
    PeerInfo has the new field; MetricsCollector has the per-peer counter storage + public increment API.
  </done>
</task>

<task type="auto" tdd="false">
  <name>Task 2: Post-handshake NodeInfoRequest + NodeInfoResponse decode → snapshot cap</name>
  <files>db/peer/peer_manager.cpp, db/peer/peer_manager.h, db/peer/message_dispatcher.cpp, db/peer/message_dispatcher.h</files>
  <read_first>
    - db/peer/peer_manager.cpp (find where a peer connection completes handshake and transitions into sync-capable state; typically near on_connected / after role-signalling)
    - db/peer/peer_manager.h (understand which class/method owns the peer-outbound path)
    - db/peer/message_dispatcher.cpp:662-739 (reference for NodeInfoResponse wire layout — the encoder's byte offsets tell you where to read on decode)
    - db/peer/message_dispatcher.h (see what types_count/supported_types layout looks like; max_blob_data_bytes is at the offset established by Phase 127 CONTEXT.md D-02)
    - cli/src/commands.cpp around line 2279 (read_u64 decode pattern for max_blob_data_bytes — mirror exactly)
    - .planning/phases/127-nodeinforesponse-capability-extensions/127-01-SUMMARY.md (authoritative wire layout)
  </read_first>
  <action>
    The goal: after a peer connection has completed role-signalled handshake (role = Peer), the initiator side sends a TransportMsgType_NodeInfoRequest. When the NodeInfoResponse arrives, decode `max_blob_data_bytes` and store it in the `PeerInfo::advertised_blob_cap` for that connection.

    Implementation steps the executor figures out concretely from reading the code:

    1. **Peer-side NodeInfoRequest emission.** Locate the post-handshake entry point in `peer_manager.cpp` where an outbound peer connection is deemed ready (search for `on_connected`, `role = net::Role::Peer`, or where `SyncNamespaceAnnounce` is first sent — these are the same post-handshake sequence points). Send a `TransportMsgType_NodeInfoRequest` with an empty payload there. Use `conn->send_message(...)` pattern consistent with how other peer-initiated messages are emitted.

    2. **Response dispatcher branch.** In `message_dispatcher.cpp` (or wherever peer-inbound TransportMsgType_NodeInfoResponse is routed — if it is not currently routed for peer connections, ADD a branch that fires only for `role == Peer`), decode the payload. The field order from the encoder is (copy verbatim from 127-01 SUMMARY and CONTEXT.md):
       ```
       [version_len:1][version:N][uptime:8][peer_count:4][namespace_count:4]
       [total_blobs:8][storage_used:8][storage_max:8]
       [max_blob_data_bytes:8 BE]              <-- what you read
       [max_frame_bytes:4 BE]
       [rate_limit_bytes_per_sec:8 BE]
       [max_subscriptions_per_connection:4 BE]
       [types_count:1][supported_types:N]
       ```
       Read `max_blob_data_bytes` at the correct offset using `chromatindb::util::load_u64_be` (or mirror cli/src/commands.cpp:2279's lambda pattern).

    3. **Snapshot write.** Find the `PeerInfo` matching the source connection (PeerManager typically maintains `peers_` vector; search by `PeerInfo::connection == source`). Set `peer_info->advertised_blob_cap = decoded_value;`. Log at debug level: `spdlog::debug("peer {} advertised blob_max_bytes={}", addr, decoded_value);`.

    4. **Idempotency guard.** If `PeerInfo::advertised_blob_cap` is already non-zero (shouldn't happen within one session — session-constant per spec), log a warning and overwrite anyway (cheap consistency check, not a correctness concern).

    5. **MetricsCollector::increment_sync_skipped_oversized** implementation (in `.cpp`): simply `sync_skipped_oversized_per_peer_[peer_address]++;`. No mutex — strand-confined per existing NodeMetrics convention.

    6. **Prometheus emission.** In `format_prometheus_metrics`, after the existing counter block for `chromatindb_sync_*` (or wherever sync counters live), add:
       ```cpp
       // chromatindb_sync_skipped_oversized_total: labeled per-peer counter (METRICS-03 / SYNC-04)
       out += "# HELP chromatindb_sync_skipped_oversized_total Blobs skipped on sync-out because peer's advertised cap is smaller.\n"
              "# TYPE chromatindb_sync_skipped_oversized_total counter\n";
       // std::map already orders by key → alphabetical by peer address
       for (const auto& [peer_addr, count] : sync_skipped_oversized_per_peer_) {
           out += "chromatindb_sync_skipped_oversized_total{peer=\"" + peer_addr + "\"} " +
                  std::to_string(count) + "\n";
       }
       ```
       If `sync_skipped_oversized_per_peer_` is empty, emit only the HELP/TYPE lines (Prometheus convention — HELP/TYPE without samples is legal and diff-stable).

    7. Do NOT modify the existing `NodeInfoResponse` encoder — Phase 127/128 own that.
  </action>
  <verify>
    <automated>
      grep -c 'TransportMsgType_NodeInfoRequest' db/peer/peer_manager.cpp
      # ^ should be >= 1 (new emission site)
      grep -c 'advertised_blob_cap =' db/peer/peer_manager.cpp db/peer/message_dispatcher.cpp
      # ^ at least one assignment site decoded the cap
      grep -c 'chromatindb_sync_skipped_oversized_total' db/peer/metrics_collector.cpp
      # ^ >= 2 (HELP/TYPE lines + per-peer format line)
      grep -c '^void MetricsCollector::increment_sync_skipped_oversized' db/peer/metrics_collector.cpp
      # ^ exactly 1
      cmake --build build-debug -j$(nproc) --target chromatindb 2>&1 | tail -3
      # ^ build passes
    </automated>
  </verify>
  <acceptance_criteria>
    - Peer connection sends `TransportMsgType_NodeInfoRequest` exactly once per fresh connection, post-handshake.
    - Receive path for peer-sourced `TransportMsgType_NodeInfoResponse` decodes `max_blob_data_bytes` at the correct offset and writes it to `PeerInfo::advertised_blob_cap`.
    - `MetricsCollector::increment_sync_skipped_oversized` implemented; strand-confined, no mutex.
    - Prometheus scrape includes labeled counter — HELP/TYPE lines present even when the map is empty.
    - No modification to TrustedHello, NodeInfoResponse encoder, PeerInfo default-constructed members (beyond the new field), or other existing wire contracts.
    - `chromatindb` target builds clean.
  </acceptance_criteria>
  <done>
    Every peer-to-peer connection now performs a one-shot post-handshake capability exchange; the advertised cap lives in PeerInfo; Prometheus exposes the (empty-so-far) skip counter. Wave 2 can consume this infrastructure to do the actual filtering.
  </done>
</task>

</tasks>

<threat_model>
- **T-129-01 Information disclosure**: Our own `max_blob_data_bytes` is sent on every peer NodeInfoRequest. Already public-by-design per Phase 127 threat model (T-127-01). No change in posture.
- **T-129-02 Tampering**: NodeInfoResponse is AEAD-encrypted post-handshake (Phase 127 T-127-02). Peer cannot forge another peer's cap into our PeerInfo — each `PeerInfo` is keyed to an authenticated connection.
- **T-129-03 Denial of service via counter map**: A misbehaving peer could cause many skip events on its connection. Mitigation: skip counter is O(1) increment, map key is the peer's authenticated address (not attacker-influenced), map size bounded by `max_peers`.
</threat_model>

<verification>
- Wave 1 does NOT close any SYNC filter requirement on its own; it only provides infrastructure. VERI-03 and SYNC-02 verification happen in Wave 2.
- Build gate: `cmake --build build-debug -j$(nproc) --target chromatindb` passes.
- User-delegated gate: full test suite is NOT run per-commit (`feedback_delegate_tests_to_user.md`).
</verification>
