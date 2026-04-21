---
title: Node daemon reconnect-loop bug — tries to reconnect to ephemeral client ports
area: db/net
created: 2026-04-21
resolved: 2026-04-21
status: resolved
resolved_in: 04255ec0
priority: high
scope: db
reported_in: phase-124 E2E (journalctl 2026-04-21)
related: 124-05-SUMMARY.md §"Blockers — B1"
---

## Summary

The `chromatindb` daemon attempts to reconnect to ephemeral client source ports
as if they were stable peer listen addresses. Observed on the host running phase
124's `cdb` commands.

## Observed behavior

From `sudo journalctl -u chromatindb -f` on 2026-04-21 ~10:11-10:14:

```
reconnecting to 192.168.1.173:56064 in 60s
reconnecting to 192.168.1.173:33926 in 60s
reconnecting to 192.168.1.173:48466 in 60s
reconnecting to 192.168.1.173:38230 in 60s
reconnecting to 192.168.1.173:33198 in 60s
reconnecting to 127.0.0.1:54886 in 2s
...
reconnecting to 127.0.0.1:54886 in 5s
...
reconnecting to 127.0.0.1:54886 in 22s
```

Ports in the `33000-56000` range (and `54886`) are ephemeral source ports, not
the node's listen port. The daemon should only retry stable peer listen
addresses exchanged via PEX / trust-relations — never the `remote_endpoint()`
of an inbound accept-side socket.

The backoff schedule (2s → 5s → 11s → 22s) suggests the same candidate is being
retried repeatedly, so this is not a single transient misclassification but a
persistent poisoning of the peer-candidates list.

## Suspected cause

When a peer connects INBOUND (node is the acceptor), `socket.remote_endpoint()`
returns the peer's **source** address (their OS-picked ephemeral port), not the
listen address they'd accept inbound connections on. If PEX or the
connection-manager feeds this endpoint into `peer_candidates` / the
reconnection queue instead of the peer's advertised listen address, the daemon
will keep trying to reconnect to a port that closed the moment the peer
disconnected.

CLI clients (`cdb`) connect outbound, get assigned a random ephemeral source
port by the kernel, then disconnect when the command finishes — exactly
matching the ports seen in the log. So the daemon is almost certainly
misidentifying transient `cdb` client connections as peers.

## Likely culprits

- `db/sync/peer_exchange.cpp` (or equivalent) — PEX handshake + peer-candidate
  ingestion path
- `db/net/connection_manager.cpp` (or equivalent) — where accepted connections
  get registered for reconnect tracking
- `/var/lib/chromatindb/peers.json` persistence — confirmed 2026-04-21 11:02:
  after a mdbx wipe + restart, daemon logged
  `loaded 3 persisted peers from /var/lib/chromatindb/peers.json` and
  immediately attempted `reconnecting to 192.168.1.173:39562 in 3s` and
  `reconnecting to 192.168.1.173:40244 in 3s`. Those ports are ephemeral
  source ports, so peers.json is persisting accept-side `remote_endpoint()`
  values. The bug has two halves: (a) the in-memory peer_candidates list
  accepts ephemeral addrs, AND (b) that list is serialized to peers.json
  on shutdown, so restart doesn't clear the poison.
- Any code path that does `peer_candidates.push_back(conn.remote_endpoint())`
  on an accept-side socket — AND the peers.json writer that snapshots the
  same list without direction filtering.

## Reproduce

1. Start `chromatindb` fresh
2. Run `cdb --node local info` (opens, does handshake, closes)
3. Wait ~2s, watch `journalctl -u chromatindb -f`
4. Expected: no reconnect lines for the client's source port
5. Observed: `reconnecting to 127.0.0.1:<port>` appears repeatedly

## Scope + priority

- **Scope:** `db/` only. Not a CLI bug. Phase 124 does not touch `db/`.
- **Priority:** high — contributed to the misdiagnosis of B1 during phase 124
  E2E (home node appeared broken / "bad response" partly because its
  peer-candidates was saturated with dead ephemeral addresses). Also wastes
  file descriptors and log volume on any node that talks to `cdb`.
- **Not blocking phase 124:** phase 124 is CLI-side wire adaptation.
  This belongs in a separate phase (likely v4.1.0 interrupt or v4.2.0 kickoff).

## Fix sketch

**Primary fix (user-directed, 2026-04-21):** reconnection should be **skipped
entirely for `Role::Client`** peers. The handshake already classifies the
remote peer as either `Role::Peer` or `Role::Client` (visible in logs as
`peer_role=peer` vs `peer_role=client`). `cdb` CLI connections land as
`Role::Client` — transient, ephemeral source port, never a reconnect
candidate. `Role::Peer` connections are the only ones that belong on the
reconnect queue or in `peers.json`.

Concretely, at every site that would enqueue a reconnect candidate or
serialize a peer to `peers.json`:

```cpp
if (conn.peer_role() != Role::Peer) {
    return;  // do not schedule reconnect; do not persist
}
```

Audit targets:
- Handshake completion path — where accepted connections are registered for
  reconnect tracking. Gate on `Role::Peer` only.
- `peers.json` serializer — skip any entry whose last-known role isn't
  `Role::Peer`. Purge existing poison on load (migration) or require a fresh
  handshake before re-adding.

**Secondary defense (belt-and-suspenders):** distinguish accept-side vs.
connect-side endpoints even within `Role::Peer`:
- For `connect`-initiated peers → their `remote_endpoint()` IS their listen
  address → safe.
- For `accept`-received peers → `remote_endpoint()` is **ephemeral** → only
  add to reconnect queue if PEX has given us their advertised listen
  address; otherwise drop on disconnect.

With the role gate in place, client-source-port poisoning becomes
structurally impossible; the accept-side filter just protects against PEX
bugs and misbehaving peers.
