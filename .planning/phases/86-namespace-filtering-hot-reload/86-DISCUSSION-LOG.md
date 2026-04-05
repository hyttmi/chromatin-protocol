# Phase 86: Namespace Filtering & Hot Reload - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-05
**Phase:** 86-namespace-filtering-hot-reload
**Areas discussed:** SIGHUP re-announce, Sync filtering scope, Wire format & type number, Announce timing, Empty set semantics, max_peers soft limit details

---

## SIGHUP Re-announce

| Option | Description | Selected |
|--------|-------------|----------|
| Re-announce immediately | Node re-sends SyncNamespaceAnnounce to all connected peers on SIGHUP | ✓ |
| New connections only | Existing peers keep old namespace set until reconnect | |

**User's choice:** Re-announce immediately
**Notes:** None

### Follow-up: Re-announce triggers re-sync?

| Option | Description | Selected |
|--------|-------------|----------|
| Passive filter update only | Just update the peer's announced set. Safety-net catches gaps. | ✓ |
| Trigger targeted re-sync | Reconciliation for newly-overlapping namespaces | |

**User's choice:** Passive filter update only
**Notes:** Safety-net cycle (600s) handles any gaps naturally

---

## Sync Filtering Scope

| Option | Description | Selected |
|--------|-------------|----------|
| Filter both BlobNotify and sync | Reconciliation scoped to namespace intersection | ✓ |
| BlobNotify only | Reconciliation still syncs everything | |

**User's choice:** Filter both BlobNotify and sync
**Notes:** Why sync blobs a peer doesn't replicate?

### Follow-up: Intersection vs receiver's set

| Option | Description | Selected |
|--------|-------------|----------|
| Intersection | Only namespaces both peers replicate | ✓ |
| Receiver's set | Sender sends anything receiver announced | |

**User's choice:** Intersection
**Notes:** None

---

## Wire Format & Type Number

### Format

| Option | Description | Selected |
|--------|-------------|----------|
| Raw binary | Like BlobNotify: [count:2 BE][N x namespace_id:32] | |
| FlatBuffers | Define in transport.fbs with structured schema | ✓ |

**User's choice:** FlatBuffers
**Notes:** None

### Type number

| Option | Description | Selected |
|--------|-------------|----------|
| 62 (next sequential) | Follows BlobFetchResponse=61. Added to relay blocklist. | ✓ |

**User's choice:** 62
**Notes:** None

### Envelope pattern

| Option | Description | Selected |
|--------|-------------|----------|
| Payload in transport envelope | Same pattern as all other messages | ✓ |

**User's choice:** Payload in transport envelope
**Notes:** Consistent with existing handle_message() dispatch

### Schema structure

| Option | Description | Selected |
|--------|-------------|----------|
| Vector of bytes ([uint8]) | Flat byte vector, manual offset arithmetic | |
| Vector of structs | Namespace { hash:[uint8:32] }, type-safe iteration | ✓ |

**User's choice:** Vector of structs
**Notes:** FlatBuffers handles alignment, cleaner iteration

---

## Announce Timing

| Option | Description | Selected |
|--------|-------------|----------|
| After auth, before sync | Both peers exchange announcements before sync-on-connect | ✓ |
| After sync-on-connect | Let first sync happen unfiltered, then exchange | |

**User's choice:** After auth, before sync
**Notes:** Reconciliation can use namespace intersection from the start

### Wait behavior

| Option | Description | Selected |
|--------|-------------|----------|
| Wait for both | Both sides send and wait to receive before proceeding | ✓ |
| Fire-and-forget | Send and proceed immediately | |

**User's choice:** Wait for both
**Notes:** Guarantees intersection is known before any sync traffic

---

## Empty Set Semantics

| Option | Description | Selected |
|--------|-------------|----------|
| No distinction | "Never announced" and "empty set" both mean replicate all | ✓ |
| Distinguish with timeout | Timeout-based detection for pre-v2.1.0 peers | |

**User's choice:** No distinction
**Notes:** All nodes upgrade together on KVM swarm — no mixed versions

### Backward compatibility confirmation

| Option | Description | Selected |
|--------|-------------|----------|
| All nodes upgrade together | Breaking change OK. No mixed-version concern. | ✓ |
| Add timeout fallback | Wait 5s, then treat as replicate-all | |

**User's choice:** All nodes upgrade together
**Notes:** Home KVM swarm, no production users

---

## max_peers Soft Limit

| Option | Description | Selected |
|--------|-------------|----------|
| Block new only | Refuse new connections until count drops naturally | ✓ |
| Graceful drain | Mark excess peers for disconnect after current sync | |

**User's choice:** Block new only
**Notes:** Log warning about over-limit state. No active disconnection.

---

## Claude's Discretion

- on_peer_connected coroutine flow for send/recv ordering
- PeerInfo struct additions for announced_namespaces
- Sync protocol Phase A/B/C namespace filtering implementation
- Relay blocklist update for type 62
- Test strategy and structure

## Deferred Ideas

None — discussion stayed within phase scope.
