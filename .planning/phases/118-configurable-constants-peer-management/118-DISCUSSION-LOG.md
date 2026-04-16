# Phase 118: Configurable Constants + Peer Management - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-16
**Phase:** 118-configurable-constants-peer-management
**Areas discussed:** Which 10 constants, Peer command mechanics, Reload safety, list-peers output

---

## Which Constants to Make Configurable

| Option | Description | Selected |
|--------|-------------|----------|
| All 12 | Make all 12 found constants configurable, update roadmap to 12 | |
| Drop 2 internals | Keep 10, drop MAX_PERSIST_FAILURES and MAX_DISCOVERED_PER_ROUND | |
| Drop keepalive pair | Keep 10, drop KEEPALIVE_INTERVAL + KEEPALIVE_TIMEOUT | |

**User's choice:** Even fewer — user prefers minimal knobs, wants the node simple to use. Concerned about nodes in same network needing same settings.

**Follow-up — which of 7 operator-relevant constants:**

| Option | Description | Selected |
|--------|-------------|----------|
| Timeouts only | blob_transfer_timeout + sync_timeout | ✓ |
| Strikes only | strike_threshold + strike_cooldown | ✓ |
| PEX interval | pex_interval | ✓ |
| Keepalive pair | keepalive_interval + keepalive_timeout | |

**User's choice:** Timeouts + Strikes + PEX interval = 5 constants. Keepalive stays hardcoded.

**Follow-up — blob_transfer_timeout default:**

User raised concern that 120s is too low for large blobs. Discussed per-blob timeout semantics, TCP keepalive overlap, and whether timeout is needed at all. Concluded: keep as configurable constant with raised default of 600s.

| Option | Description | Selected |
|--------|-------------|----------|
| Remove it | Rely on TCP keepalive + strikes | |
| Progress-based | Reset timer on data arrival | |
| Large wall-clock | Raise to 600s, make configurable | ✓ |

---

## Peer Command Mechanics

| Option | Description | Selected |
|--------|-------------|----------|
| Edit config + SIGHUP | chromatindb add-peer edits config.json, sends SIGHUP to running node | ✓ |
| UDS to running node | Send command to node via UDS. Requires node running. | |
| Config only, no signal | Edit config.json only, manual restart/SIGHUP | |

**User's choice:** Edit config + SIGHUP

**Follow-up — remove-peer scope:**

| Option | Description | Selected |
|--------|-------------|----------|
| Bootstrap only | Only remove from config.json bootstrap_peers | ✓ |
| Both sources | Remove from config.json AND peers.json | |
| Both + block | Remove from both, add to block list | |

**User's choice:** Bootstrap only. PEX peers managed by node.

---

## Reload Safety

| Option | Description | Selected |
|--------|-------------|----------|
| All 5 reloadable | All constants SIGHUP-reloadable, strikes apply to future evaluations only | |
| Strikes need restart | Timeouts + PEX interval reloadable, strike fields restart-only | ✓ |

**User's choice:** Strikes need restart. Avoids mid-accumulation surprises.

---

## list-peers Output

| Option | Description | Selected |
|--------|-------------|----------|
| Address + status | Minimal: address, connected/disconnected, source | |
| Detailed | Full: address, status, source, direction, strikes, last-seen | ✓ |
| Grouped by source | Two sections: Bootstrap Peers and Discovered Peers | |

**User's choice:** Detailed

**Follow-up — extend PeerInfoResponse wire format:**

| Option | Description | Selected |
|--------|-------------|----------|
| Extend response | Add strike_count + direction to PeerInfoResponse entries | |
| Use existing fields | Ship with current fields, add strikes/direction later | ✓ |

**User's choice:** Use existing fields (YAGNI). Current PeerInfoResponse has address, is_bootstrap, syncing, peer_is_full, connected_duration_ms.

---

## Claude's Discretion

- Config field naming convention
- Exact validation ranges for each constant
- SIGHUP PID discovery mechanism
- list-peers output formatting

## Deferred Ideas

- Extend PeerInfoResponse with strikes + direction — add later if operators need it
- Blob size in ls output — carried from Phase 117
