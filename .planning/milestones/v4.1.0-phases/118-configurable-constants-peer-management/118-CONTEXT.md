# Phase 118: Configurable Constants + Peer Management - Context

**Gathered:** 2026-04-16
**Status:** Ready for planning

<domain>
## Phase Boundary

Move 5 hardcoded sync/peer constants to config.json with SIGHUP reload (where safe), and add peer management subcommands to the node binary (`chromatindb add-peer`, `remove-peer`, `list-peers`).

</domain>

<decisions>
## Implementation Decisions

### Configurable Constants — Which 5
- **D-01:** Only 5 of 12 hardcoded constants become configurable. The rest stay hardcoded to keep the node simple for operators.
- **D-02:** The 5 configurable constants:
  - `blob_transfer_timeout` — default **600s** (raised from 120s). Per-blob timeout during sync transfer.
  - `sync_timeout` — default 30s. Timeout waiting for sync protocol responses.
  - `pex_interval` — default 300s. How often to exchange peer lists.
  - `strike_threshold` — default 10. Strikes before banning a misbehaving peer.
  - `strike_cooldown` — default 300s. How long a banned peer stays banned.
- **D-03:** Constants that stay hardcoded (no operator business tuning these):
  - `KEEPALIVE_INTERVAL` (30s), `KEEPALIVE_TIMEOUT` (60s) — TCP liveness, rarely needs tuning
  - `MAX_HASHES_PER_REQUEST` (64) — protocol-level, both peers must agree
  - `MAX_PEERS_PER_EXCHANGE` (8), `MAX_DISCOVERED_PER_ROUND` (3), `MAX_PERSISTED_PEERS` (100), `MAX_PERSIST_FAILURES` (3) — PEX internals

### SIGHUP Reload Safety
- **D-04:** SIGHUP-reloadable (safe to change at runtime): `blob_transfer_timeout`, `sync_timeout`, `pex_interval`. These only affect new operations, not in-flight ones.
- **D-05:** Restart required: `strike_threshold`, `strike_cooldown`. Changing mid-strike accumulation could be surprising. Loaded once at startup.

### Config Validation
- **D-06:** Invalid config values rejected with clear error messages and range check details (CONF-03). Claude's discretion on exact ranges, but must reject obviously bad values (negative, zero where inappropriate, absurdly large).

### Peer Management — Command Mechanics
- **D-07:** Peer commands are subcommands on the `chromatindb` node binary (not `cdb`). Pattern: `chromatindb add-peer <addr>`, `chromatindb remove-peer <addr>`, `chromatindb list-peers`.
- **D-08:** `add-peer` and `remove-peer` edit the `bootstrap_peers` array in config.json, then send SIGHUP to the running node process (if running). Works offline too — just edits the file.
- **D-09:** `remove-peer` only removes from `bootstrap_peers` in config.json. PEX-discovered peers in `peers.json` are managed automatically by the node.
- **D-10:** `add-peer` and `remove-peer` trigger SIGHUP automatically after editing config (PEER-01, PEER-02 success criteria).

### Peer Management — list-peers
- **D-11:** `chromatindb list-peers` queries the running node via UDS (PeerInfoRequest type 55) for connected peer state, and reads config.json for bootstrap peers list.
- **D-12:** Merge display: connected peers from PeerInfoResponse + disconnected bootstrap peers from config.json.
- **D-13:** Uses existing PeerInfoResponse fields only — no wire format changes. Fields: address, is_bootstrap, syncing, peer_is_full, connected_duration_ms.
- **D-14:** If node is not running, fall back to showing config.json bootstrap peers only (all marked disconnected).

### Claude's Discretion
- Config field naming convention (snake_case in JSON, matching existing config.json style)
- Exact validation ranges for each constant
- How SIGHUP sender finds the node PID (pidfile or process lookup)
- list-peers output formatting (table, plain text, etc.)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Node Config
- `db/config/config.h` — Config struct, all current config fields
- `db/config/config.cpp` — JSON config parsing, validation
- `db/tests/config/test_config.cpp` — Config test patterns

### SIGHUP Reload
- `db/peer/peer_manager.cpp` — Existing SIGHUP handler (lines 467-523), reloads ACLs, rate limits, sync config, max_peers
- `db/peer/peer_manager.h` — PeerManager class, reload interface

### Hardcoded Constants
- `db/peer/peer_manager.h` — STRIKE_THRESHOLD (10), STRIKE_COOLDOWN_SEC (300), PEX_INTERVAL_SEC (300), MAX_HASHES_PER_REQUEST (64), BLOB_TRANSFER_TIMEOUT (120s)
- `db/peer/sync_orchestrator.h` — BLOB_TRANSFER_TIMEOUT (120s), MAX_HASHES_PER_REQUEST (64), SYNC_TIMEOUT (30s)
- `db/peer/connection_manager.cpp` — KEEPALIVE_INTERVAL (30s), KEEPALIVE_TIMEOUT (60s)

### Peer Info Protocol
- `db/PROTOCOL.md` — PeerInfoRequest/PeerInfoResponse (types 55/56, lines 1064-1098), trust-gating, per-peer entry format
- `db/peer/message_dispatcher.cpp` — PeerInfoRequest handler

### Node Main
- `db/main.cpp` — Node startup, argument parsing, signal handling

### Requirements
- `.planning/REQUIREMENTS.md` — CONF-01 through CONF-03, PEER-01 through PEER-03

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Config` struct (`config.h`): Already has 28+ configurable fields. Extend with 5 new ones.
- SIGHUP handler (`peer_manager.cpp`): Already reloads multiple config sections. Extend with new reloadable constants.
- `PeerInfoRequest`/`PeerInfoResponse` (type 55/56): Already implemented with UDS trust-gating. list-peers connects via UDS and uses this.
- `bootstrap_peers` config field: Already parsed as vector of address strings. add-peer/remove-peer modify this array.
- `peers.json` management (`pex_manager`): PEX-discovered peer persistence. Not modified by CLI — managed by node.

### Established Patterns
- Config fields use snake_case in JSON, parsed in `config.cpp` with nlohmann/json.
- SIGHUP reload: re-reads config.json, updates specific fields, logs changes.
- Signal handling: SIGHUP registered in main.cpp, dispatched to PeerManager.
- Node binary has existing subcommand detection in main.cpp for `--help`, `--version`.

### Integration Points
- `config.h` / `config.cpp` — 5 new fields with defaults matching current hardcoded values.
- `peer_manager.h` — Replace 3 constexpr constants with config-loaded members.
- `sync_orchestrator.h` — Replace 2 constexpr constants with config-loaded members.
- `peer_manager.cpp` SIGHUP handler — Extend to reload the 3 reloadable constants.
- `main.cpp` — New subcommand dispatch for `add-peer`, `remove-peer`, `list-peers`.
- `PROTOCOL.md` — No changes needed (existing PeerInfoResponse is sufficient).

</code_context>

<specifics>
## Specific Ideas

- blob_transfer_timeout default raised from 120s to 600s to handle large blobs (500 MiB) over slower links without spurious timeouts.
- Roadmap says "10 constants" but only 5 are operator-relevant. Update roadmap/requirements to reflect 5.

</specifics>

<deferred>
## Deferred Ideas

- **Extend PeerInfoResponse with strikes + direction** — Add strike_count and inbound/outbound direction to per-peer entries. Not needed now — add later if operators request it.
- **Blob size in ls output** — carried forward from Phase 117 deferral.

</deferred>

---

*Phase: 118-configurable-constants-peer-management*
*Context gathered: 2026-04-16*
