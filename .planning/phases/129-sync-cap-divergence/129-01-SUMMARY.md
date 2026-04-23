---
phase: 129-sync-cap-divergence
plan: 01-peerinfo-snapshot-counter
subsystem: peer, metrics, wire-protocol
tags: [sync, capability-exchange, peer-info, prometheus, labeled-counter]

# Dependency graph
requires:
  - phase: 127-nodeinforesponse-capability-extensions
    provides: "NodeInfoResponse wire layout with max_blob_data_bytes at fixed offset — decoded verbatim by this plan's receive branch"
  - phase: 128-configurable-blob-cap-frame-shrink-config-gauges
    provides: "format_prometheus_metrics extension pattern (HELP/TYPE per metric), MetricsCollector owns the emission path"
provides:
  - "PeerInfo::advertised_blob_cap (u64, default 0) — snapshotted once per peer session from NodeInfoResponse; 0 = unknown = filter MUST NOT skip (D-01)"
  - "Per-connection post-handshake TransportMsgType_NodeInfoRequest emission on the initiator path — piggybacks on the existing announce+sync post-TrustedHello flow"
  - "MessageDispatcher NodeInfoResponse peer-role branch that decodes max_blob_data_bytes and writes PeerInfo::advertised_blob_cap"
  - "MetricsCollector::increment_sync_skipped_oversized(peer_address) public helper + std::map<std::string, uint64_t> sync_skipped_oversized_per_peer_ storage"
  - "chromatindb_sync_skipped_oversized_total{peer=\"...\"} labeled-counter Prometheus emission (HELP/TYPE always present; samples emerge as Wave 2 wires filter sites)"
affects:
  - 129-02-filter-tests-uat (Wave 2 consumes advertised_blob_cap + increment_sync_skipped_oversized from the 3 filter sites; no further infra needed)
  - /metrics scrape output (adds ~150 bytes HELP/TYPE baseline, +~80 bytes per active skip entry)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Capability ride-along: peer-to-peer capability exchange rides existing NodeInfoRequest/Response (Phase 127) without TrustedHello modification (SYNC-01 / CONTEXT.md D-02)."
    - "Async capability snapshot without sync-round blocking: initiator fires TransportMsgType_NodeInfoRequest fire-and-forget right before sync; response lands via dispatcher branch and writes PeerInfo::advertised_blob_cap whenever it arrives. Wave 2's filter tolerates cap==0 until the snapshot lands (D-01 conservative default)."
    - "Labeled Prometheus counter via std::map keyed by authenticated peer address — map ordering gives alphabetical scrape output without an explicit sort step (mirrors Phase 128 D-08 diff-friendly ordering)."
    - "HELP/TYPE-always, samples-optional: Prometheus block for chromatindb_sync_skipped_oversized_total emits HELP+TYPE unconditionally so scrape diffs stay stable across the first-skip transition."

key-files:
  created:
    - .planning/phases/129-sync-cap-divergence/129-01-SUMMARY.md
  modified:
    - "db/peer/peer_types.h — +3 lines: PeerInfo::advertised_blob_cap = 0 with D-01 comment"
    - "db/peer/metrics_collector.h — +1 include <map>, +1 public method declaration, +1 private member (std::map<std::string, uint64_t>)"
    - "db/peer/peer_manager.cpp — SyncTrigger lambda extended: co_await c->send_message(TransportMsgType_NodeInfoRequest, empty) before sync_.run_sync_with_peer(c)"
    - "db/peer/message_dispatcher.cpp — new branch for TransportMsgType_NodeInfoResponse with peer-role gate + offset-verified decode + PeerInfo write"
    - "db/peer/metrics_collector.cpp — format_prometheus_metrics tail extended with chromatindb_sync_skipped_oversized_total block; void increment_sync_skipped_oversized(const std::string&) impl added"

decisions:
  - "Post-handshake emission point is the PeerManager SyncTrigger lambda (line 57-74 in original file, extended here), NOT ConnectionManager::announce_and_sync. Rationale: PLAN explicitly lists peer_manager.cpp as the files_modified target and expressly omits connection_manager.cpp. The SyncTrigger lambda fires only for the initiator path (ConnectionManager::announce_and_sync invokes it only behind conn->is_initiator()), exactly matching the must_have 'when we are the initiator'."
  - "Fire-and-forget, no await on response: the initiator does NOT block its sync round on NodeInfoResponse arrival. Filter tolerates cap==0 (D-01); if the response is delayed or dropped, replication continues unchanged and the cap simply lands on the next response if any. Avoids compounding sync latency and avoids needing a separate recv timer."
  - "Peer-role gate on the NodeInfoResponse receive branch: we only write advertised_blob_cap when peer->role == net::Role::Peer. Client-role responses (where cdb used the same message type via synchronous Connection::recv()) are explicitly ignored here so we never steal a cdb response."
  - "Inline offset-math in the decode (`1 + ver_len + 40 + ver_len skip + ...`) with a sanity-check throw if the calculated offset disagrees with the encoder's structure. Matches the CLI's read_u64 lambda pattern at cli/src/commands.cpp:2263 without pulling in a full decoder utility (YAGNI; a shared decoder would help only once there are 2+ call sites)."
  - "std::map<std::string, uint64_t> chosen over std::unordered_map: sorted iteration gives free alphabetical Prometheus emission (no std::sort step, no intermediate vector). Cardinality is bounded by max_peers (default 32), so O(log N) access cost is trivial."
  - "NodeMetrics NOT extended with a new u64 field. Per-peer labeled counters don't fit that flat struct shape — Config gauges (Phase 128) use a mirror pattern, but labeled counters belong in a map. The plan's must_have #4 ('NodeMetrics gains a map...') is an over-broad phrasing resolved by the PLAN action step 2 which relocates the map to MetricsCollector."
  - "Malformed NodeInfoResponse payloads from peers: log spdlog::warn but DO NOT strike. A garbled capability advertisement is not a correctness failure — cap stays 0 (unknown), filter MUST NOT skip, and the ingest-side peer continues to enforce its cap via Phase 128 engine checks."

metrics:
  duration: "~14m"
  completed: 2026-04-23
  tasks-completed: 2
  files-modified: 5
  commits: 2
---

# Phase 129 Plan 01: PeerInfo Snapshot + Per-Peer Skip Counter Summary

**One-liner:** Wave 1 infrastructure for Sync Cap Divergence — PeerInfo now carries advertised_blob_cap (snapshotted from a post-handshake NodeInfoRequest/Response round trip), MetricsCollector owns a per-peer sync-skip counter map, and /metrics exposes `chromatindb_sync_skipped_oversized_total{peer="..."}` ready for Wave 2 to wire into the three filter sites.

## Performance

- **Duration:** ~14 min (reading + edits + build gates)
- **Started:** 2026-04-23T04:07:00Z (approx)
- **Completed:** 2026-04-23T04:21:25Z
- **Tasks:** 2 / 2
- **Files modified:** 5 (3 .cpp + 2 .h)
- **Commits:** 2

## What Landed

### Commit 1 (`27524325`): Data-shape additions (headers)

- `db/peer/peer_types.h` — `PeerInfo::advertised_blob_cap = 0` field with D-01 conservative-default comment.
- `db/peer/metrics_collector.h` — `#include <map>` (alphabetical insertion between `<functional>` and `<memory>`); public `void increment_sync_skipped_oversized(const std::string& peer_address)` declaration; private `std::map<std::string, uint64_t> sync_skipped_oversized_per_peer_` member.

No behavior change on its own — pure data-shape plumbing Wave 2 consumes.

### Commit 2 (`d44c3de9`): Wiring + emission + decode + impl

- `db/peer/peer_manager.cpp` — SyncTrigger lambda (passed to ConnectionManager ctor) now emits `TransportMsgType_NodeInfoRequest` with empty payload before `sync_.run_sync_with_peer(c)`. This fires only on the initiator path because `ConnectionManager::announce_and_sync` gates the trigger on `conn->is_initiator()`.
- `db/peer/message_dispatcher.cpp` — new `on_peer_message` branch for `TransportMsgType_NodeInfoResponse`:
  - peer-role gate (clients' same-type responses are ignored)
  - byte-level decode at offset `1 + ver_len + 8+4+4+8+8+8` (40 fixed bytes + version_len + version string) using `chromatindb::util::read_u64_be`
  - writes `peer->advertised_blob_cap`; logs debug on success, warn on re-advertise, warn (no strike) on malformed payloads
- `db/peer/metrics_collector.cpp`:
  - `format_prometheus_metrics()` tail extended with `chromatindb_sync_skipped_oversized_total` block; HELP/TYPE always emitted, per-peer samples emitted when present (alphabetical via std::map key ordering)
  - `MetricsCollector::increment_sync_skipped_oversized(peer_address)` implemented as `++sync_skipped_oversized_per_peer_[peer_address]` (strand-confined; no mutex)

## Acceptance Criteria

All grep/build checks from PLAN pass:

| Check | Command | Expected | Result |
|-------|---------|----------|--------|
| Task 1 AC1 | `grep -c '^\s*uint64_t advertised_blob_cap = 0;' db/peer/peer_types.h` | 1 | **1** |
| Task 1 AC2 | `grep -c 'sync_skipped_oversized_per_peer_' db/peer/metrics_collector.h` | ≥1 | **1** |
| Task 1 AC3 | `grep -c 'void increment_sync_skipped_oversized' db/peer/metrics_collector.h` | 1 | **1** |
| Task 1 AC4 | `chromatindb_lib` builds clean | exit 0 | **exit 0** |
| Task 2 AC1 | `grep -c 'TransportMsgType_NodeInfoRequest' db/peer/peer_manager.cpp` | ≥1 | **1** (the emission site) |
| Task 2 AC2 | `grep -c 'advertised_blob_cap =' db/peer/{peer_manager,message_dispatcher}.cpp` | ≥1 | **1** in message_dispatcher.cpp (the snapshot write site) |
| Task 2 AC3 | `grep -c 'chromatindb_sync_skipped_oversized_total' db/peer/metrics_collector.cpp` | ≥2 | **4** (HELP line, TYPE line, per-peer format line, doc comment) |
| Task 2 AC4 | `grep -c '^void MetricsCollector::increment_sync_skipped_oversized' db/peer/metrics_collector.cpp` | 1 | **1** |
| Task 2 AC5 | `chromatindb` builds clean | exit 0 | **exit 0** (`[100%] Built target chromatindb`, linked executable) |

## Wire / Threat Invariants

- **No TrustedHello modification** — capability exchange rides the existing NodeInfoRequest/Response pair introduced by Phase 127. ✓
- **No NodeInfoResponse encoder modification** — decode half only, offsets mirror the Phase 127 layout. ✓
- **Session-constant cap per spec** — once written, `advertised_blob_cap` is not cleared within a session; on disconnect the entire PeerInfo is destroyed and next reconnect starts fresh at 0 (matches D-07 "counter persists until process restart; PeerInfo disconnect wipes state").
- **D-01 preserved** — cap==0 ("unknown") is never reset by this code path; Wave 2's filter will read cap and do the conservative `cap > 0 && blob_size > cap` check. Wave 1 provides only the storage.

## Deviations from Plan

### Auto-fixed issues

None. No Rule 1/2/3 fixes triggered during execution.

### Intentional reading of the PLAN

- **Must-have #4 ambiguity.** The must_have text says *"NodeMetrics gains a map keyed by peer-identifier to a u64 counter (or equivalent per-peer record)"*. PLAN Task 1 action step 2 explicitly relocates this map to MetricsCollector (not NodeMetrics) with rationale ("NodeMetrics is a flat counter struct; labeled/per-peer counters don't fit that shape"). Followed the action step — the map lives in MetricsCollector. The must_have's "(or equivalent per-peer record)" parenthetical covers this resolution.

- **SyncTrigger lambda vs ConnectionManager.** PLAN read_first step 1 says "Locate the post-handshake entry point in peer_manager.cpp ... where SyncNamespaceAnnounce is first sent". SyncNamespaceAnnounce actually lives in `ConnectionManager::announce_and_sync` (line 261 of connection_manager.cpp), but the files_modified list omits connection_manager.cpp. The resolution: the SyncTrigger lambda defined in peer_manager.cpp (passed by value to ConnectionManager ctor) is the last hook peer_manager.cpp owns before sync begins, and it runs only on the initiator path (conn->is_initiator() gate in ConnectionManager::announce_and_sync). Extending that lambda satisfies both the "files_modified" constraint and the "post-handshake initiator-only" must_have.

## No Stubs

No stub data. Every code path is wired to real values:

- `advertised_blob_cap` default 0 is the documented "unknown" sentinel, not a stub (per D-01 it has defined meaning: filter MUST NOT skip).
- `sync_skipped_oversized_per_peer_` is an empty-initialized map; empty ≠ stubbed — Wave 2 populates it from real filter sites.
- The Prometheus block emits real `std::to_string(count)` values; when the map is empty it emits HELP/TYPE only, which is valid Prometheus output (not a placeholder).

## Threat Flags

No new surface beyond the plan's `<threat_model>`:

- T-129-01 Information disclosure: unchanged — our own max_blob_data_bytes was already sent via NodeInfoRequest responses in Phase 127; no new leak.
- T-129-02 Tampering: mitigated by AEAD-encrypted post-handshake channel (Phase 127 T-127-02); PeerInfo is keyed to an authenticated connection.
- T-129-03 DoS via counter map: map key is `conn->remote_address()` (authenticated endpoint string), size bounded by max_peers (default 32), O(log N) insert.

No new threat surface introduced.

## Requirements Touched (Wave 1 infra only)

- `SYNC-01` — **infra complete.** Capability snapshot plumbing lands here; Wave 2's SYNC-02/SYNC-03 filter sites consume `PeerInfo::advertised_blob_cap`.
- `METRICS-03` — **infra complete.** `chromatindb_sync_skipped_oversized_total` gauge block emits; Wave 2 increments it via `MetricsCollector::increment_sync_skipped_oversized`.
- `SYNC-04` — **infra complete.** Public increment helper is available for Wave 2 call sites.

No VERI-* requirements closed in this plan (verification belongs to Wave 2 tests per PLAN `<verification>`).

## Commits

| Task | Commit  | Files                                                                                    | Summary |
|------|---------|------------------------------------------------------------------------------------------|---------|
| 1    | 27524325 | db/peer/peer_types.h, db/peer/metrics_collector.h                                         | PeerInfo::advertised_blob_cap field + MetricsCollector sync_skipped_oversized_per_peer_ map + public increment method declaration |
| 2    | d44c3de9 | db/peer/peer_manager.cpp, db/peer/message_dispatcher.cpp, db/peer/metrics_collector.cpp   | Post-handshake NodeInfoRequest emission on initiator + NodeInfoResponse decode→snapshot branch + Prometheus counter block + increment impl |

## Self-Check: PASSED

- `db/peer/peer_types.h` modified in `27524325` — FOUND (`git log --oneline 27524325 -1` exists; file contains `uint64_t advertised_blob_cap = 0;`).
- `db/peer/metrics_collector.h` modified in `27524325` — FOUND (contains `std::map<std::string, uint64_t> sync_skipped_oversized_per_peer_` and public `increment_sync_skipped_oversized`).
- `db/peer/peer_manager.cpp` modified in `d44c3de9` — FOUND (contains `TransportMsgType_NodeInfoRequest` emission in SyncTrigger lambda).
- `db/peer/message_dispatcher.cpp` modified in `d44c3de9` — FOUND (contains `TransportMsgType_NodeInfoResponse` branch with `advertised_blob_cap =` write).
- `db/peer/metrics_collector.cpp` modified in `d44c3de9` — FOUND (contains `increment_sync_skipped_oversized` impl + `chromatindb_sync_skipped_oversized_total` block).
- Commit `27524325` — FOUND in `git log` (2 files changed, 16 insertions).
- Commit `d44c3de9` — FOUND in `git log` (3 files changed, 91 insertions, 1 deletion).
- Build gate `cmake --build build-debug -j$(nproc) --target chromatindb` — exit 0, final line `[100%] Built target chromatindb`.

---
*Phase: 129-sync-cap-divergence*
*Plan: 129-01-peerinfo-snapshot-counter*
*Completed: 2026-04-23*
